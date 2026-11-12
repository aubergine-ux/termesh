// Lighting: the Blinn-Phong reflection model shared by all three shading
// modes. Flat shading evaluates it once per triangle, Gouraud once per
// vertex, Phong once per pixel - the maths is the same, only the frequency
// changes.
#pragma once

#include <algorithm>
#include <cmath>

#include "math/vec.hpp"

namespace termesh {

// The light rig, in world space. Directions point *towards* the light.
struct Lights {
    Vec3 keyDirection{0.0f, 0.0f, 1.0f};
    Vec3 keyColor{2.0f, 1.94f, 1.86f};
    Vec3 fillDirection{0.0f, 0.0f, 1.0f};
    Vec3 fillColor{0.25f, 0.3f, 0.4f};
    // Hemisphere ambient: surfaces facing up pick up the sky colour, surfaces
    // facing down the ground colour. Much less flat than a constant ambient.
    Vec3 skyColor{0.42f, 0.47f, 0.55f};
    Vec3 groundColor{0.14f, 0.12f, 0.11f};
};

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
inline Lighting computeLighting(const Lights& lights, Vec3 n, Vec3 v, float roughness) {
    Lighting out;

    // Diffuse (Lambert): brightness falls off with the cosine of the angle
    // between the normal and the light.
    const float keyCos = std::max(dot(n, lights.keyDirection), 0.0f);
    const float fillCos = std::max(dot(n, lights.fillDirection), 0.0f);
    const Vec3 ambient = lerp(lights.groundColor, lights.skyColor, n.y * 0.5f + 0.5f);
    out.diffuse = ambient + lights.keyColor * keyCos + lights.fillColor * fillCos;

    // Specular (Blinn-Phong): the highlight is brightest where the normal
    // lines up with the half vector between the light and the eye. The
    // (s + 8) / 8 factor keeps the highlight's total energy constant, so
    // glossy surfaces get small bright highlights and rough ones wide dim
    // ones.
    out.specular = {0.0f, 0.0f, 0.0f};
    if (keyCos > 0.0f) {
        const float shininess = shininessFromRoughness(roughness);
        const Vec3 h = normalize(lights.keyDirection + v);
        const float nh = std::max(dot(n, h), 0.0f);
        out.specular = lights.keyColor * ((shininess + 8.0f) * 0.125f * std::pow(nh, shininess) * keyCos);
    }

    // A stand-in for environment reflections: look up the hemisphere colour
    // in the mirror direction. Without it metals, which have no diffuse
    // colour at all, would be black wherever the highlight is not.
    const Vec3 r = reflect(-v, n);
    const Vec3 environment = lerp(lights.groundColor, lights.skyColor, r.y * 0.5f + 0.5f);
    out.specular += environment * (1.6f - roughness);

    return out;
}

// Tone mapping: squeeze the unbounded range of computed light into the
// displayable [0, 1]. A hard clamp turns every bright area into a flat white
// patch; this curve (Narkowicz's fit of the ACES filmic curve) instead rolls
// highlights off smoothly, the way a camera does.
inline float toneMap(float x) {
    x = std::max(x, 0.0f);
    return std::min((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 1.0f);
}
inline Vec3 toneMap(Vec3 c) { return {toneMap(c.x), toneMap(c.y), toneMap(c.z)}; }

}  // namespace termesh
