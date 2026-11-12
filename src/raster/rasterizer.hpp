// The triangle rasterizer: coverage, depth testing and perspective-correct
// attribute interpolation for one screen tile.
//
// This is a header because the fragment shader is a template parameter: the
// compiler inlines the shader straight into the pixel loop.
#pragma once

#include <algorithm>
#include <cstdint>

#include "core/framebuffer.hpp"
#include "pipeline/clip.hpp"
#include "raster/simd.hpp"

namespace termesh {

// ------------------------------------------------------------ fixed point ---
//
// Screen positions are snapped to a 1/16 pixel grid and stored as integers
// ("28.4 fixed point"). All coverage maths is then exact integer arithmetic.
// With floats, the two triangles sharing an edge round their edge equations
// differently and pixels along the seam get drawn twice or not at all.
inline constexpr int kSubpixelBits = 4;
inline constexpr int kSubpixelScale = 1 << kSubpixelBits;

// Edge function values are stored in 32 bits. For points inside the viewport
// they are bounded by 256 * width * height, which caps the framebuffer at a
// little over 8 million pixels (3840 x 2160 just fits).
inline constexpr std::int64_t kMaxFramebufferPixels = (std::int64_t{1} << 31) / 256 - 1;

// A triangle ready for rasterization, produced by the renderer's primitive
// assembly. Vertices are ordered so that the triangle's area (as computed by
// edgeFunction) is positive.
struct RasterTriangle {
    std::int32_t x[3], y[3];  // screen position, 28.4 fixed point, y down
    float z[3];               // depth in [0, 1], 0 = near plane
    float invW[3];            // 1 / clip-space w, for perspective correction
    float varyings[3][kMaxVaryings];
    float edgeScale[3];       // 1 / (edge length in subpixels * 16); see Fragment
    float lod;                // texture level-of-detail bias for the whole triangle
    float tangentSign;        // handedness of the tangent frame (+1 / -1)
    std::uint16_t material;
    std::uint8_t edgeMask;    // bit i set: the edge opposite vertex i is a real mesh edge
    bool backFacing;
};

// What the fragment shader receives for one pixel.
struct Fragment {
    float varyings[kMaxVaryings];  // interpolated with perspective correction
    float edgeDistance[3];         // distance in pixels to each triangle edge
};

// A rectangle of pixels; x1 and y1 are exclusive.
struct TileRect {
    int x0, y0, x1, y1;
};

// Twice the signed area of the triangle (a, b, p). Positive when p is on the
// interior side of the directed edge a -> b for a positively-oriented
// triangle.
constexpr std::int64_t edgeFunction(std::int32_t ax, std::int32_t ay, std::int32_t bx,
                                    std::int32_t by, std::int32_t px, std::int32_t py) {
    return static_cast<std::int64_t>(bx - ax) * (py - ay) -
           static_cast<std::int64_t>(by - ay) * (px - ax);
}

// The top-left fill rule. A pixel centre lying exactly on an edge shared by
// two triangles must be drawn by exactly one of them. By convention it goes
// to the triangle for which that edge is a "top" edge (horizontal, with the
// interior below it) or a "left" edge. With y pointing down and the interior
// on the positive side, a top edge runs right-to-left and a left edge runs
// downwards.
constexpr bool isTopLeftEdge(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by) {
    const std::int32_t dx = bx - ax, dy = by - ay;
    return (dy == 0 && dx < 0) || dy > 0;
}

// Rasterize `count` triangles into the part of the framebuffer covered by
// `tile`. Tiles never overlap, so different threads can run this on different
// tiles of the same framebuffer at once.
//
// Shader requirements:
//     int  varyingCount() const;         how many varyings to interpolate
//     bool wantsEdgeDistance() const;    fill in Fragment::edgeDistance?
//     bool operator()(const RasterTriangle&, const Fragment&, Color8& out) const;
//                                         return false to discard the pixel
template <typename Shader>
void rasterizeTile(Framebuffer& fb, const TileRect& tile, const RasterTriangle* const* triangles,
                   std::size_t count, const Shader& shader) {
    using simd::Float4;
    using simd::Int4;

    const int varyingCount = shader.varyingCount();
    const bool wantsEdgeDistance = shader.wantsEdgeDistance();

    for (std::size_t t = 0; t < count; ++t) {
        const RasterTriangle& tri = *triangles[t];

        // Bounding box of the triangle in whole pixels, clipped to the tile.
        // A pixel is "in" the box if its centre can be inside the triangle.
        const int minX = std::max(std::min({tri.x[0], tri.x[1], tri.x[2]}) >> kSubpixelBits, tile.x0);
        const int minY = std::max(std::min({tri.y[0], tri.y[1], tri.y[2]}) >> kSubpixelBits, tile.y0);
        const int maxX = std::min(std::max({tri.x[0], tri.x[1], tri.x[2]}) >> kSubpixelBits, tile.x1 - 1);
        const int maxY = std::min(std::max({tri.y[0], tri.y[1], tri.y[2]}) >> kSubpixelBits, tile.y1 - 1);
        if (minX > maxX || minY > maxY) continue;

        // --- Edge functions -------------------------------------------------
        //
        // Edge i is the one opposite vertex i. Its edge function
        //
        //     E_i(p) = (b - a) x (p - a)
        //
        // is positive on the interior side, zero on the edge and negative
        // outside, so a point is inside the triangle when all three are
        // positive. Each E_i is linear in p, which means it never has to be
        // evaluated from scratch after the first pixel: moving one pixel
        // right adds a constant (stepX), one pixel down adds another (stepY).
        const int a[3] = {1, 2, 0}, b[3] = {2, 0, 1};
        std::int64_t rowStart[3];
        std::int32_t stepX[3], stepY[3], bias[3];

        // The first sample point: the centre of pixel (minX, minY).
        const std::int32_t startX = minX * kSubpixelScale + kSubpixelScale / 2;
        const std::int32_t startY = minY * kSubpixelScale + kSubpixelScale / 2;

        for (int i = 0; i < 3; ++i) {
            const std::int32_t ax = tri.x[a[i]], ay = tri.y[a[i]];
            const std::int32_t bx = tri.x[b[i]], by = tri.y[b[i]];
            stepX[i] = -(by - ay) * kSubpixelScale;
            stepY[i] = (bx - ax) * kSubpixelScale;
            // Fill rule: a pixel is covered when E > 0, or E == 0 on a
            // top-left edge. Subtracting 1 from the other edges folds both
            // cases into a single "E >= 0" test, which in turn is just a
            // check of the sign bit.
            bias[i] = isTopLeftEdge(ax, ay, bx, by) ? 0 : -1;
            rowStart[i] = edgeFunction(ax, ay, bx, by, startX, startY) + bias[i];
        }

        // --- Barycentric coordinates ---------------------------------------
        //
        // E_i is proportional to the distance from edge i, and edge i is the
        // one opposite vertex i, so E_i grows from 0 on that edge to the full
        // triangle area at vertex i. Dividing by the area therefore gives the
        // barycentric weight of vertex i:  l_i = E_i / area,  l0 + l1 + l2 = 1.
        const std::int64_t area =
            edgeFunction(tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2]);
        const float invArea = 1.0f / static_cast<float>(area);

        // Depth as a plane equation over the barycentrics:
        //     z = z0 + l1 * (z1 - z0) + l2 * (z2 - z0)
        // Depth here is NDC z (z/w), and it can be interpolated linearly in
        // screen space *without* perspective correction, because projection
        // maps planes to planes: a flat triangle in 3D is still flat in
        // (screen x, screen y, z/w) space.
        const Float4 z0 = Float4::splat(tri.z[0]);
        const Float4 dz1 = Float4::splat((tri.z[1] - tri.z[0]) * invArea);
        const Float4 dz2 = Float4::splat((tri.z[2] - tri.z[0]) * invArea);

        // Lane offsets: the four pixels of a SIMD group sit 0, 1, 2 and 3
        // steps to the right of the group's first pixel.
        const Int4 lanes0 = Int4::set(0, stepX[0], 2 * stepX[0], 3 * stepX[0]);
        const Int4 lanes1 = Int4::set(0, stepX[1], 2 * stepX[1], 3 * stepX[1]);
        const Int4 lanes2 = Int4::set(0, stepX[2], 2 * stepX[2], 3 * stepX[2]);
        // The fill-rule bias must not leak into the barycentrics.
        const Float4 unbias1 = Float4::splat(static_cast<float>(bias[1]));
        const Float4 unbias2 = Float4::splat(static_cast<float>(bias[2]));

        for (int y = minY; y <= maxY; ++y) {
            Color8* colorRow = fb.colorRow(y);
            float* depthRow = fb.depthRow(y);
            std::int64_t e0 = rowStart[0], e1 = rowStart[1], e2 = rowStart[2];

            for (int x = minX; x <= maxX; x += 4) {
                // Edge functions for four pixels at once.
                const Int4 w0 = Int4::splat(static_cast<std::int32_t>(e0)) + lanes0;
                const Int4 w1 = Int4::splat(static_cast<std::int32_t>(e1)) + lanes1;
                const Int4 w2 = Int4::splat(static_cast<std::int32_t>(e2)) + lanes2;
                e0 += 4 * static_cast<std::int64_t>(stepX[0]);
                e1 += 4 * static_cast<std::int64_t>(stepX[1]);
                e2 += 4 * static_cast<std::int64_t>(stepX[2]);

                // Coverage: OR-ing the three values leaves the sign bit clear
                // only where all three are non-negative. Lanes past the right
                // edge of the bounding box are masked off.
                const int lanesInBox = (1 << std::min(4, maxX - x + 1)) - 1;
                int mask = simd::nonNegativeMask(w0 | w1 | w2) & lanesInBox;
                if (!mask) continue;

                // Depth test for the covered pixels.
                const Float4 f1 = Float4::fromInt(w1) - unbias1;
                const Float4 f2 = Float4::fromInt(w2) - unbias2;
                const Float4 z = z0 + f1 * dz1 + f2 * dz2;
                if (lanesInBox == 0xF) {
                    mask &= simd::lessThanMask(z, Float4::load(depthRow + x));
                } else {
                    // A partial group at the right edge of the box: only read
                    // the pixels that belong to this tile, since another
                    // thread may be writing the ones beyond it.
                    float stored[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    for (int lane = 0; lane <= maxX - x; ++lane) stored[lane] = depthRow[x + lane];
                    mask &= simd::lessThanMask(z, Float4::load(stored));
                }
                if (!mask) continue;

                // Shade the survivors one at a time.
                float edge1[4], edge2[4], depth[4];
                f1.store(edge1);
                f2.store(edge2);
                z.store(depth);

                for (int lane = 0; lane < 4; ++lane) {
                    if (!(mask & (1 << lane))) continue;

                    const float l1 = edge1[lane] * invArea;
                    const float l2 = edge2[lane] * invArea;
                    const float l0 = 1.0f - l1 - l2;

                    // --- Perspective-correct interpolation -----------------
                    //
                    // l0, l1, l2 are barycentrics of the *projected*
                    // triangle. Using them directly to blend vertex
                    // attributes would be wrong: projection squeezes the far
                    // end of a triangle into fewer pixels than the near end,
                    // so the halfway point on screen is not the halfway
                    // point on the surface. (That is the "swimming" texture
                    // look of early 3D hardware.)
                    //
                    // The fix comes from one fact: for any attribute u that
                    // is linear across the triangle in 3D, the quantities
                    // u/w and 1/w are linear in *screen* space. So
                    // interpolate both with the screen-space weights and
                    // divide:
                    //
                    //            l0*u0/w0 + l1*u1/w1 + l2*u2/w2
                    //     u  =  --------------------------------
                    //              l0/w0  +  l1/w1  +  l2/w2
                    //
                    // Rearranged, that is an ordinary weighted sum with
                    // adjusted weights  b_i = (l_i / w_i) / sum(l_j / w_j),
                    // which only need computing once per pixel however many
                    // attributes there are.
                    const float q0 = l0 * tri.invW[0];
                    const float q1 = l1 * tri.invW[1];
                    const float q2 = l2 * tri.invW[2];
                    const float norm = 1.0f / (q0 + q1 + q2);
                    const float b0 = q0 * norm, b1 = q1 * norm, b2 = q2 * norm;

                    Fragment frag;
                    for (int k = 0; k < varyingCount; ++k)
                        frag.varyings[k] = b0 * tri.varyings[0][k] + b1 * tri.varyings[1][k] +
                                           b2 * tri.varyings[2][k];

                    frag.edgeDistance[0] = frag.edgeDistance[1] = frag.edgeDistance[2] = 0.0f;
                    if (wantsEdgeDistance) {
                        // An edge function divided by its edge's length is
                        // the perpendicular distance from that edge - used to
                        // draw wireframe lines of constant width.
                        const float area_f = static_cast<float>(area);
                        frag.edgeDistance[0] = (area_f - edge1[lane] - edge2[lane]) * tri.edgeScale[0];
                        frag.edgeDistance[1] = edge1[lane] * tri.edgeScale[1];
                        frag.edgeDistance[2] = edge2[lane] * tri.edgeScale[2];
                    }

                    Color8 color;
                    if (!shader(tri, frag, color)) continue;  // discarded (alpha cut-out)
                    depthRow[x + lane] = depth[lane];
                    colorRow[x + lane] = color;
                }
            }

            rowStart[0] += stepY[0];
            rowStart[1] += stepY[1];
            rowStart[2] += stepY[2];
        }
    }
}

}  // namespace termesh
