// Clipping triangles against the view frustum, in homogeneous clip space.
#pragma once

#include "math/vec.hpp"

namespace termesh {

// Per-vertex values interpolated across a triangle. What each slot means
// depends on the shading mode; the clipper just treats them as numbers.
inline constexpr int kMaxVaryings = 12;

struct ClipVertex {
    Vec4 position;  // clip space, before the perspective divide
    float varyings[kMaxVaryings];
};

// Clipping a triangle against six planes can add one vertex per plane.
inline constexpr int kMaxClipVertices = 9;

// The six frustum planes as bit flags.
enum ClipPlane : unsigned {
    kClipLeft = 1u << 0,    // x >= -w
    kClipRight = 1u << 1,   // x <=  w
    kClipBottom = 1u << 2,  // y >= -w
    kClipTop = 1u << 3,     // y <=  w
    kClipNear = 1u << 4,    // z >= -w
    kClipFar = 1u << 5,     // z <=  w
    kClipAll = (1u << 6) - 1,
};

// Signed distance-like value of a clip-space point from one frustum plane:
// positive inside, negative outside, zero on the plane.
constexpr float clipDistance(Vec4 p, unsigned plane) {
    switch (plane) {
        case kClipLeft: return p.w + p.x;
        case kClipRight: return p.w - p.x;
        case kClipBottom: return p.w + p.y;
        case kClipTop: return p.w - p.y;
        case kClipNear: return p.w + p.z;
        default: return p.w - p.z;  // kClipFar
    }
}

// Which planes a point is outside of. For a triangle:
//   - if the AND of its three outcodes is non-zero, all three vertices are
//     beyond the same plane and the triangle is invisible;
//   - if the OR is zero, it is entirely inside and needs no clipping;
//   - otherwise it straddles the planes set in the OR.
constexpr unsigned clipOutcode(Vec4 p) {
    unsigned code = 0;
    if (p.w + p.x < 0.0f) code |= kClipLeft;
    if (p.w - p.x < 0.0f) code |= kClipRight;
    if (p.w + p.y < 0.0f) code |= kClipBottom;
    if (p.w - p.y < 0.0f) code |= kClipTop;
    if (p.w + p.z < 0.0f) code |= kClipNear;
    if (p.w - p.z < 0.0f) code |= kClipFar;
    return code;
}

// Clip a convex polygon against the planes in `planes` (a mask of ClipPlane
// bits). `vertices` holds `count` vertices on entry and must have room for
// kMaxClipVertices; the clipped polygon is written back to it and its vertex
// count returned (0 if nothing is left). Only the first `varyingCount`
// varyings are interpolated.
int clipPolygon(ClipVertex* vertices, int count, unsigned planes, int varyingCount);

}  // namespace termesh
