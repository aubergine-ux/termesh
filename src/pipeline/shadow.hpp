// Shadow mapping.
//
// A point is in shadow if something sits between it and the light. Finding
// that out by tracing a ray per pixel would be far too slow, so instead the
// scene is drawn once more *from the light's point of view*, keeping only
// depth. That depth image - the shadow map - records, for every direction
// from the light, how far its rays get before hitting something.
//
// When the camera's pixels are shaded, each surface point is transformed
// into the light's view and its distance compared with the stored one:
// further than what the light saw means something is in the way.
//
//        light                        the map stores the depth of A;
//          \                          B projects to the same texel but is
//           \   A  <- lit             further from the light, so B is in
//            \  |                     shadow.
//             \ B  <- shadowed
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/framebuffer.hpp"
#include "math/mat.hpp"
#include "scene/light.hpp"

namespace termesh {

struct ShadowMap {
    Mat4 viewProjection;  // world space -> the light's clip space
    // Only the depth half is used; a byte per pixel of colour is the price
    // of sharing the rasterizer with everything else.
    BasicFramebuffer<std::uint8_t> target;

    // Width, in shadow-map texels, of the box the comparison is averaged
    // over. 1 gives hard (bilinear) edges; larger gives a wider penumbra.
    int kernel = 3;
    // How much nearer to the light than the stored depth a point is taken
    // to be, in world units. Without it a surface shadows itself in stripes
    // ("shadow acne"), because the map's texels are coarser than the
    // screen's pixels.
    float biasWorld = 0.0015f;
    // Converting that distance to depth-buffer units. For an orthographic
    // map depth is linear, so it is one constant. For a perspective map
    // (spot lights) depth is squeezed towards 1 far from the light - the
    // same distance is a smaller and smaller depth difference - so the
    // conversion is perspectiveScale / w^2, with w the distance from the
    // light. perspectiveScale = 0 means orthographic.
    float depthPerWorld = 1.0f;
    float perspectiveScale = 0.0f;
    // How the light's projection scales x and y (view units to clip units):
    // needed to size point splats in the shadow pass.
    float projectionScale = 1.0f;

    bool valid() const { return target.width() > 0; }

    // Fraction of the light reaching `world`: 1 fully lit, 0 fully shadowed.
    // `nDotL` is the cosine between the surface normal and the light; the
    // bias grows as the light grazes the surface, where acne is worst.
    //
    // Soft edges come from percentage-closer filtering: the depth
    // comparison is made at several neighbouring texels and the *results*
    // (not the depths) are averaged. A box of kernel x kernel texels placed
    // at the exact sub-texel position touches (kernel + 1)^2 texels; the
    // ones on its border are only partly covered and are weighted by how
    // much, which is what makes the shadow edge move smoothly instead of in
    // texel-sized steps.
    float sample(Vec3 world, float nDotL) const {
        const Vec4 clip = viewProjection * Vec4(world, 1.0f);
        if (clip.w <= 0.0f) return 1.0f;  // behind a spot light
        const float invW = 1.0f / clip.w;
        const float depth = clip.z * invW * 0.5f + 0.5f;
        if (depth >= 1.0f) return 1.0f;  // beyond the light's reach

        const int size = target.width();
        const float x = (clip.x * invW * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f * static_cast<float>(kernel);
        const float y = (0.5f - clip.y * invW * 0.5f) * static_cast<float>(size) - 0.5f * static_cast<float>(kernel);
        const float fx = std::floor(x), fy = std::floor(y);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        if (x0 < 0 || y0 < 0 || x0 + kernel >= size || y0 + kernel >= size) return 1.0f;  // outside the map
        const float tx = x - fx, ty = y - fy;

        const float depthBias = biasWorld * (perspectiveScale > 0.0f ? perspectiveScale * invW * invW : depthPerWorld);
        const float reference = depth - depthBias * (1.0f + 3.0f * (1.0f - std::clamp(nDotL, 0.0f, 1.0f)));
        float lit = 0.0f;
        for (int j = 0; j <= kernel; ++j) {
            const float wy = j == 0 ? 1.0f - ty : (j == kernel ? ty : 1.0f);
            const float* row = target.depthRow(y0 + j) + x0;
            float rowLit = 0.0f;
            for (int i = 0; i <= kernel; ++i) {
                const float wx = i == 0 ? 1.0f - tx : (i == kernel ? tx : 1.0f);
                if (reference <= row[i]) rowLit += wx;
            }
            lit += rowLit * wy;
        }
        return lit / static_cast<float>(kernel * kernel);
    }
};

// Point a shadow map at the scene from a light: an orthographic box around
// the scene for a directional light (parallel rays), a perspective frustum
// matching the cone for a spot light. The scene is given as a bounding
// sphere. Point lights, which would need six such views, are not supported.
inline void aimShadowMap(ShadowMap& map, const Light& light, Vec3 center, float radius) {
    const Vec3 direction = normalize(light.direction, {0.0f, -1.0f, 0.0f});
    // Any up vector works as long as it is not parallel to the direction.
    const Vec3 up = std::abs(direction.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    radius = std::max(radius, 1e-4f);
    map.biasWorld = 0.0045f * radius;

    if (light.type == LightType::Spot) {
        const float distance = length(center - light.position);
        const float zFar = distance + radius;
        // A near plane very close to the light would spend nearly all of the
        // depth buffer's precision on the first few percent of the range.
        const float zNear = std::max(distance - radius, zFar * 0.01f);
        const float fov = std::clamp(2.0f * light.outerCone + 0.05f, 0.1f, 3.0f);
        const Mat4 projection = perspective(fov, 1.0f, zNear, zFar);
        map.viewProjection = projection * lookAt(light.position, light.position + direction, up);
        // depth = 0.5 * (a + b / w) + 0.5 with b = -projection.c[3].z, so a
        // step of one world unit along the ray changes depth by 0.5 b / w^2.
        map.perspectiveScale = -0.5f * projection.c[3].z;
        map.projectionScale = projection.c[1].y;
        return;
    }
    // Stand far enough back along the rays that the whole scene is in front.
    const Vec3 eye = center - direction * (radius * 2.0f);
    const float zNear = radius * 0.5f, zFar = radius * 3.5f;
    map.viewProjection = orthographic(-radius, radius, -radius, radius, zNear, zFar) * lookAt(eye, center, up);
    map.perspectiveScale = 0.0f;
    map.depthPerWorld = 1.0f / (zFar - zNear);
    map.projectionScale = 1.0f / radius;
}

}  // namespace termesh
