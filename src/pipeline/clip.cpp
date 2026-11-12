// Why clip at all, and why here?
//
// The perspective divide (x/w, y/w, z/w) only makes sense for points in front
// of the camera. A vertex behind the camera has negative w, so dividing flips
// it through the origin and it reappears on the wrong side of the screen; a
// vertex at w = 0 divides by zero. A triangle with one vertex in front and one
// behind therefore cannot be projected - it first has to be cut along the near
// plane so that only the visible part remains.
//
// The cut is made in clip space, *before* the divide, for two reasons:
//
//   1. Every frustum plane has a trivially simple equation there. The frustum
//      is the set of points with -w <= x, y, z <= w, so "inside the left
//      plane" is just w + x >= 0, and so on (see clipDistance).
//
//   2. Clip space is still linear. A point part-way along an edge in 3D is
//      the same fraction of the way along that edge in clip space, so both
//      positions and vertex attributes can be interpolated with a plain lerp.
//      After the divide that is no longer true - which is the whole reason
//      perspective-correct interpolation exists in the rasterizer.
//
// Clipping against the other five planes is not strictly required (the
// rasterizer could scissor instead), but it keeps every screen coordinate
// inside the viewport, and that bound is what lets the rasterizer do its
// coverage maths in 32-bit integers.
#include "pipeline/clip.hpp"

namespace termesh {

namespace {

ClipVertex interpolate(const ClipVertex& a, const ClipVertex& b, float t, int varyingCount) {
    ClipVertex r;
    r.position = a.position + (b.position - a.position) * t;
    for (int i = 0; i < varyingCount; ++i) r.varyings[i] = a.varyings[i] + (b.varyings[i] - a.varyings[i]) * t;
    return r;
}

// Sutherland-Hodgman: clip a convex polygon against one plane.
//
// Walk the polygon's edges. For each edge from `prev` to `cur`:
//
//     prev inside,  cur inside   -> emit cur
//     prev inside,  cur outside  -> emit the intersection
//     prev outside, cur inside   -> emit the intersection, then cur
//     prev outside, cur outside  -> emit nothing
//
// The output is the part of the polygon on the inside of the plane, still
// convex and with its winding order preserved.
int clipAgainstPlane(const ClipVertex* in, int count, ClipVertex* out, unsigned plane, int varyingCount) {
    int n = 0;
    const ClipVertex* prev = &in[count - 1];
    float prevDist = clipDistance(prev->position, plane);

    for (int i = 0; i < count; ++i) {
        const ClipVertex* cur = &in[i];
        const float curDist = clipDistance(cur->position, plane);

        if ((prevDist >= 0.0f) != (curDist >= 0.0f)) {
            // The edge crosses the plane. The distance function is linear
            // along the edge, so it reaches zero at this fraction of the way.
            // Always interpolating from the inside vertex means the two
            // triangles sharing this edge compute bit-identical intersection
            // points, whichever direction each one walks it.
            if (prevDist >= 0.0f) {
                const float t = prevDist / (prevDist - curDist);
                out[n++] = interpolate(*prev, *cur, t, varyingCount);
            } else {
                const float t = curDist / (curDist - prevDist);
                out[n++] = interpolate(*cur, *prev, t, varyingCount);
            }
        }
        if (curDist >= 0.0f) out[n++] = *cur;

        prev = cur;
        prevDist = curDist;
    }
    return n;
}

}  // namespace

int clipPolygon(ClipVertex* vertices, int count, unsigned planes, int varyingCount) {
    ClipVertex scratch[kMaxClipVertices];
    ClipVertex* in = vertices;
    ClipVertex* out = scratch;

    // Clip against each requested plane in turn, ping-ponging between the
    // caller's buffer and the scratch buffer.
    for (unsigned plane = 1; plane < kClipAll; plane <<= 1) {
        if (!(planes & plane)) continue;
        count = clipAgainstPlane(in, count, out, plane, varyingCount);
        if (count < 3) return 0;
        ClipVertex* t = in;
        in = out;
        out = t;
    }

    if (in != vertices)
        for (int i = 0; i < count; ++i) vertices[i] = in[i];
    return count;
}

}  // namespace termesh
