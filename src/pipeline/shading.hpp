// Shading: how much light leaves a surface towards the eye.
//
// Three models share the same inputs - lights, shadows, an environment and
// a description of the surface - and differ in the maths between:
//
//   Blinn-Phong   the classic: a cosine falloff plus a tunable highlight.
//                 Cheap, and what flat, Gouraud and Phong shading use.
//   PBR           a microfacet model (Cook-Torrance with the GGX
//                 distribution), the one glTF materials are defined in
//                 terms of. Conserves energy; metals and rough surfaces
//                 look right without per-material tweaking.
//   Toon          Blinn-Phong with the smooth falloff snapped to a few flat
//                 bands, for a hand-drawn look.
#pragma once

#include <algorithm>
#include <cmath>

#include "pipeline/shadow.hpp"
#include "scene/environment.hpp"
#include "scene/light.hpp"

// The lighting functions run once per pixel, inside the rasterizer's inner
// loop. Left to itself the compiler keeps the larger ones as separate
// functions, and the call - with a dozen floats to pass each way - costs
// more than a tenth of the frame.
//
// The opposite holds for the models that are not the default: inlined, they
// would triple the size of the fragment shader, and the compiler would run
// out of registers on the path that matters most. They stay out of line.
#if defined(_MSC_VER)
#define TERMESH_FORCE_INLINE __forceinline
#define TERMESH_NO_INLINE __declspec(noinline)
#else
#define TERMESH_FORCE_INLINE inline __attribute__((always_inline))
#define TERMESH_NO_INLINE __attribute__((noinline))
#endif

namespace termesh {

// A light, digested for the per-pixel loop: everything that is the same for
// every pixel of a frame is worked out once here (see prepareLight) instead
// of once per pixel.
struct ShadingLight {
    LightType type = LightType::Directional;
    Vec3 radiance;  // colour * intensity
    Vec3 toLight;   // directional: unit vector towards the light
    Vec3 position;  // point / spot
    Vec3 axis;      // spot: unit vector along the beam
    float specular = 1.0f;
    float range = 0.0f;
    float cosInner = 1.0f, cosOuter = 0.0f;
    const ShadowMap* shadow = nullptr;  // null: casts no shadow
};

inline ShadingLight prepareLight(const Light& light, const ShadowMap* shadow) {
    ShadingLight out;
    out.type = light.type;
    out.radiance = light.color * light.intensity;
    out.axis = normalize(light.direction, {0.0f, -1.0f, 0.0f});
    out.toLight = -out.axis;
    out.position = light.position;
    out.specular = light.specular;
    out.range = light.range;
    out.cosOuter = std::cos(light.outerCone);
    out.cosInner = std::cos(std::min(light.innerCone, light.outerCone - 1e-3f));
    out.shadow = shadow;
    return out;
}

// Everything a shading function needs to know about the surroundings. All
// directions and positions are in world space.
struct LightingContext {
    const ShadingLight* lights = nullptr;
    int lightCount = 0;
    const Environment* environment = nullptr;
};

// The surface at one point.
struct Surface {
    Vec3 position;
    Vec3 normal;  // unit
    Vec3 view;    // unit, from the surface towards the eye
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusion = 1.0f;  // 1 = fully open to ambient light
};

// Light arriving at a point from one source.
struct Incoming {
    Vec3 direction;  // unit, from the surface towards the light
    Vec3 radiance;
};

// Point and spot lights: the direction and brightness depend on where the
// surface is. Returns false if the light does not reach the point at all.
inline bool incomingPunctual(const ShadingLight& light, Vec3 position, Incoming& out) {
    const Vec3 toLight = light.position - position;
    const float distance2 = std::max(lengthSquared(toLight), 1e-8f);
    const float distance = std::sqrt(distance2);
    out.direction = toLight / distance;

    // Inverse-square falloff, optionally windowed so it reaches exactly zero
    // at `range` rather than trailing off for ever.
    float attenuation = 1.0f / distance2;
    if (light.range > 0.0f) {
        const float ratio = distance / light.range;
        if (ratio >= 1.0f) return false;
        const float window = 1.0f - ratio * ratio * ratio * ratio;
        attenuation *= window * window;
    }
    if (light.type == LightType::Spot) {
        // Compare the angle between the beam's axis and the ray to this
        // point with the cone's - as cosines, which need no trigonometry.
        const float cosAngle = dot(-out.direction, light.axis);
        if (cosAngle <= light.cosOuter) return false;
        const float t = saturate((cosAngle - light.cosOuter) / (light.cosInner - light.cosOuter));
        attenuation *= t * t * (3.0f - 2.0f * t);
    }
    out.radiance = light.radiance * attenuation;
    return true;
}

// The light reaching `position` from one source, shadows included. Returns
// false if there is none. `nDotL` receives the cosine between the normal
// and the light.
TERMESH_FORCE_INLINE bool incomingLight(const ShadingLight& light, Vec3 position, Vec3 n, bool useShadows,
                                        Incoming& out, float& nDotL) {
    // Directional lights - the common case - are the same everywhere.
    out.direction = light.toLight;
    out.radiance = light.radiance;
    if (light.type != LightType::Directional && !incomingPunctual(light, position, out)) return false;
    nDotL = dot(n, out.direction);
    if (nDotL <= 0.0f) return false;  // facing away
    if (useShadows && light.shadow) {
        const float visible = light.shadow->sample(position, nDotL);
        if (visible <= 0.0f) return false;
        out.radiance *= visible;
    }
    return true;
}

// ------------------------------------------------------------- Blinn-Phong ---

// Light arriving at a surface point, before it meets the material:
// the final colour is  albedo * diffuse + specularColor * specular.
struct Lighting {
    Vec3 diffuse;
    Vec3 specular;
};

// Convert glTF-style roughness (0 = mirror, 1 = chalk) to a Blinn-Phong
// exponent. This is the usual mapping from microfacet roughness.
inline float shininessFromRoughness(float roughness) {
    const float a = std::max(roughness * roughness, 0.03f);
    return std::clamp(2.0f / (a * a) - 2.0f, 1.0f, 1024.0f);
}

// n: unit surface normal.  v: unit vector from the surface to the eye.
// Shadows are optional because flat and Gouraud shading evaluate this per
// triangle or per vertex, where a shadow lookup would be far too coarse.
TERMESH_FORCE_INLINE Lighting blinnPhong(const LightingContext& ctx, Vec3 position, Vec3 n, Vec3 v, float roughness,
                                         bool useShadows) {
    Lighting out;
    // Ambient light from the environment.
    out.diffuse = ctx.environment->irradiance(n);
    out.specular = {0.0f, 0.0f, 0.0f};
    const float shininess = shininessFromRoughness(roughness);

    // Highlights are finished in a second, tiny loop. std::pow is a function
    // call, and a call in the middle of the main loop would make the
    // compiler write every value the loop is juggling out to memory and
    // read it back, for every light. Queueing the two numbers each
    // highlight needs keeps the main loop entirely in registers.
    constexpr int kBatch = 4;
    Vec3 queuedRadiance[kBatch];
    float queuedCos[kBatch];
    int queued = 0;
    auto finishHighlights = [&] {
        // The (s + 8) / 8 factor keeps the highlight's total energy
        // constant, so glossy surfaces get small bright highlights and
        // rough ones wide dim ones.
        for (int k = 0; k < queued; ++k)
            out.specular += queuedRadiance[k] * ((shininess + 8.0f) * 0.125f * std::pow(queuedCos[k], shininess));
        queued = 0;
    };

    for (int i = 0; i < ctx.lightCount; ++i) {
        const ShadingLight& light = ctx.lights[i];
        Incoming in;
        float nDotL;
        if (!incomingLight(light, position, n, useShadows, in, nDotL)) continue;

        // Diffuse (Lambert): brightness falls off with the cosine of the
        // angle between the normal and the light.
        out.diffuse += in.radiance * nDotL;

        // Specular (Blinn-Phong): the highlight is brightest where the
        // normal lines up with the half vector between the light and the
        // eye.
        if (light.specular > 0.0f) {
            const Vec3 h = normalize(in.direction + v);
            queuedCos[queued] = std::max(dot(n, h), 0.0f);
            queuedRadiance[queued] = in.radiance * (light.specular * nDotL);
            if (++queued == kBatch) finishHighlights();
        }
    }
    finishHighlights();

    // Environment reflections: look the environment up in the mirror
    // direction. Without this metals, which have no diffuse colour at all,
    // would be black wherever the highlight is not.
    const Vec3 r = reflect(-v, n);
    out.specular += ctx.environment->specular(r, roughness) * (1.6f - roughness);
    return out;
}

// The Blinn-Phong colour of a surface: how Phong shading combines the
// lighting with the material.
TERMESH_FORCE_INLINE Vec3 combineBlinnPhong(const Lighting& light, Vec3 albedo, float metallic, float occlusion) {
    // Metals have no diffuse reflection; their colour tints the specular
    // reflection instead. Non-metals reflect about 4% specularly.
    const Vec3 diffuseColor = albedo * (1.0f - metallic);
    const Vec3 specularColor = lerp(Vec3(0.04f), albedo, metallic);
    return diffuseColor * light.diffuse * occlusion + specularColor * light.specular;
}

// --------------------------------------------------------------------- PBR ---
//
// The microfacet idea: a surface is made of tiny perfect mirrors, tilted at
// random. Light from L reaches the eye at V only off the facets that happen
// to face exactly half-way between the two (the half vector H). So the
// specular reflection is the product of three questions:
//
//   D  how many facets face H?            (the distribution: GGX)
//   G  how many of those can both see the light and be seen by the eye,
//      rather than being hidden behind their neighbours?   (Smith)
//   F  how much of the light does a facet reflect rather than absorb,
//      at this angle?                      (Fresnel, Schlick's approximation)
//
//                 D * G * F
//   specular = ----------------          (Cook and Torrance, 1982)
//               4 (N.L) (N.V)
//
// Whatever is not reflected enters the surface; in a non-metal it scatters
// and comes back out as the diffuse colour, in a metal it is absorbed.

// GGX / Trowbridge-Reitz. `a` is roughness squared. Small `a` gives a tall
// narrow peak (a tight highlight); large `a` a low wide one.
inline float distributionGgx(float nDotH, float a) {
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

// Smith's shadowing-masking term, already divided by the 4 (N.L) (N.V) of
// the Cook-Torrance denominator (the "visibility" form).
inline float visibilitySmithGgx(float nDotL, float nDotV, float a) {
    const float a2 = a * a;
    const float gl = nDotV * std::sqrt(nDotL * nDotL * (1.0f - a2) + a2);
    const float gv = nDotL * std::sqrt(nDotV * nDotV * (1.0f - a2) + a2);
    return 0.5f / std::max(gl + gv, 1e-5f);
}

// Schlick's approximation to the Fresnel equations: reflectance rises from
// f0 face-on to 100% at a grazing angle, which is why even a matt floor
// shows reflections when you look along it.
inline Vec3 fresnelSchlick(float cosTheta, Vec3 f0) {
    const float x = 1.0f - saturate(cosTheta);
    const float x5 = x * x * x * x * x;
    return f0 + (Vec3(1.0f) - f0) * x5;
}

// How much of the (pre-blurred) environment a surface reflects, as a scale
// and offset applied to f0. The exact answer is an integral usually baked
// into a lookup texture; this is Karis's curve fit to that texture, which
// is within a few percent of it.
inline Vec3 environmentBrdf(Vec3 f0, float roughness, float nDotV) {
    const Vec4 c0{-1.0f, -0.0275f, -0.572f, 0.022f};
    const Vec4 c1{1.0f, 0.0425f, 1.04f, -0.04f};
    const Vec4 r = c0 * roughness + c1;
    const float a004 = std::min(r.x * r.x, std::exp2(-9.28f * nDotV)) * r.x + r.y;
    const float scale = -1.04f * a004 + r.z, offset = 1.04f * a004 + r.w;
    return f0 * scale + Vec3(offset);
}

TERMESH_NO_INLINE inline Vec3 shadePbr(const LightingContext& ctx, const Surface& s) {
    const Vec3 n = s.normal, v = s.view;
    const float nDotV = std::max(dot(n, v), 1e-4f);
    // Below this, the highlight of a point light becomes a single pixel
    // spike; clamping keeps mirrors stable.
    const float roughness = std::clamp(s.roughness, 0.045f, 1.0f);
    const float a = roughness * roughness;

    // f0: reflectance looking straight at the surface. About 4% for
    // anything that is not a metal; for metals it is the base colour.
    const Vec3 f0 = lerp(Vec3(0.04f), s.albedo, s.metallic);
    const Vec3 diffuseColor = s.albedo * (1.0f - s.metallic);

    Vec3 color{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < ctx.lightCount; ++i) {
        const ShadingLight& light = ctx.lights[i];
        Incoming in;
        float nDotL;
        if (!incomingLight(light, s.position, n, true, in, nDotL)) continue;

        const Vec3 h = normalize(in.direction + v);
        const float nDotH = std::max(dot(n, h), 0.0f);
        const Vec3 f = fresnelSchlick(dot(v, h), f0);
        const float specular = distributionGgx(nDotH, a) * visibilitySmithGgx(nDotL, nDotV, a);

        // Light colours here are radiance / pi (so that a white surface lit
        // head-on by a white light of intensity 1 comes out white), which
        // puts the pi on the specular term instead of under the diffuse one.
        const Vec3 diffuse = diffuseColor * (Vec3(1.0f) - f);
        color += (diffuse + f * (specular * kPi * light.specular)) * in.radiance * nDotL;
    }

    // Image-based lighting: the same split, with the environment as the
    // light. Diffuse sees the whole hemisphere; specular sees a blurred
    // patch around the mirror direction.
    const Vec3 r = reflect(-v, n);
    const Vec3 specularAmount = environmentBrdf(f0, roughness, nDotV);
    color += diffuseColor * ctx.environment->irradiance(n) * s.occlusion;
    color += specularAmount * ctx.environment->specular(r, roughness) * s.occlusion;
    return color;
}

// -------------------------------------------------------------------- toon ---

// A step from 0 to 1 at `edge`, softened over `width` so the boundary
// between two bands is anti-aliased rather than jagged.
inline float softStep(float edge, float width, float x) {
    const float t = saturate((x - edge) / width + 0.5f);
    return t * t * (3.0f - 2.0f * t);
}

// Cel shading: the same ingredients as Blinn-Phong, but each is snapped to
// a few flat levels. Smooth gradients are what make a render look "3D";
// removing them gives the look of ink and flat paint.
TERMESH_NO_INLINE inline Vec3 shadeToon(const LightingContext& ctx, const Surface& s) {
    const Vec3 n = s.normal, v = s.view;
    // One flat ambient tone, whichever way the surface faces.
    const Vec3 ambient =
        (ctx.environment->irradiance({0.0f, 1.0f, 0.0f}) + ctx.environment->irradiance({0.0f, -1.0f, 0.0f})) * 0.5f;
    Vec3 light = ambient * 1.2f;
    Vec3 highlight{0.0f, 0.0f, 0.0f};
    const float shininess = shininessFromRoughness(s.roughness);

    for (int i = 0; i < ctx.lightCount; ++i) {
        const ShadingLight& l = ctx.lights[i];
        Incoming in;
        float nDotL;
        if (!incomingLight(l, s.position, n, true, in, nDotL)) continue;
        // Shadows have already dimmed the radiance; fold that back into a
        // single "how lit is this point" number to pick the band from.
        const float full = std::max({l.radiance.x, l.radiance.y, l.radiance.z, 1e-6f});
        const float reach = std::max({in.radiance.x, in.radiance.y, in.radiance.z});
        const float lit = nDotL * std::min(reach / full, 1.0f);

        // Three tones: shadow, mid, lit.
        const float band = 0.45f * softStep(0.04f, 0.03f, lit) + 0.55f * softStep(0.5f, 0.03f, lit);
        light += l.radiance * band;

        // A highlight with a hard edge: it is either there or it is not.
        if (l.specular > 0.0f) {
            const Vec3 h = normalize(in.direction + v);
            const float spec = std::pow(std::max(dot(n, h), 0.0f), shininess);
            highlight += l.radiance * (l.specular * (1.0f - s.roughness) * 0.5f * softStep(0.5f, 0.05f, spec));
        }
    }
    const Vec3 base = s.albedo * (1.0f - 0.5f * s.metallic);
    return base * light + highlight;
}

}  // namespace termesh
