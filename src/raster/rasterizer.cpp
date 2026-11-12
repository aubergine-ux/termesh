#include "raster/rasterizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace termesh {

namespace {

// Vertex positions are snapped to a 1/16 pixel grid and all coverage maths is
// done in integers. Floating-point edge functions round differently for the
// two triangles sharing an edge, which leaves cracks or double-drawn pixels;
// integers are exact, so adjacent triangles tile perfectly.
constexpr int kSubpixelBits = 4;
constexpr int kSubpixelScale = 1 << kSubpixelBits;

struct Point {
    std::int32_t x, y;
};

Point snap(Vec3 v) {
    return {static_cast<std::int32_t>(std::lround(v.x * kSubpixelScale)),
            static_cast<std::int32_t>(std::lround(v.y * kSubpixelScale))};
}

// Edge function: twice the signed area of triangle (a, b, p). Its sign says
// which side of the directed line a->b the point p is on.
std::int64_t edge(Point a, Point b, Point p) {
    return static_cast<std::int64_t>(b.x - a.x) * (p.y - a.y) -
           static_cast<std::int64_t>(b.y - a.y) * (p.x - a.x);
}

// The top-left fill rule. A pixel centre lying exactly on an edge shared by
// two triangles must be drawn by exactly one of them. The convention: it
// belongs to the triangle for which that edge is a "top" edge (horizontal,
// with the interior below it) or a "left" edge. With y pointing down and the
// interior on the positive side of every edge function, those are the edges
// going right-to-left along the top, or downwards on the left.
bool isTopLeft(Point a, Point b) {
    const std::int32_t dx = b.x - a.x, dy = b.y - a.y;
    return (dy == 0 && dx < 0) || dy > 0;
}

}  // namespace

void fillTriangle(Framebuffer& fb, Vec3 a, Vec3 b, Vec3 c, Color8 color) {
    Point p0 = snap(a), p1 = snap(b), p2 = snap(c);
    float z0 = a.z, z1 = b.z, z2 = c.z;

    // Orient the triangle so that the interior is on the positive side of all
    // three edge functions.
    std::int64_t area = edge(p0, p1, p2);
    if (area == 0) return;  // degenerate: no pixels
    if (area < 0) {
        std::swap(p1, p2);
        std::swap(z1, z2);
        area = -area;
    }

    // Bounding box in whole pixels, clipped to the framebuffer.
    const int minX = std::max(std::min({p0.x, p1.x, p2.x}) >> kSubpixelBits, 0);
    const int minY = std::max(std::min({p0.y, p1.y, p2.y}) >> kSubpixelBits, 0);
    const int maxX = std::min(std::max({p0.x, p1.x, p2.x}) >> kSubpixelBits, fb.width() - 1);
    const int maxY = std::min(std::max({p0.y, p1.y, p2.y}) >> kSubpixelBits, fb.height() - 1);

    // A pixel is inside when all three edge functions are > 0, or == 0 on a
    // top-left edge. Subtracting 1 from the non-top-left edges turns both
    // cases into a single ">= 0" test.
    const std::int64_t bias0 = isTopLeft(p1, p2) ? 0 : -1;
    const std::int64_t bias1 = isTopLeft(p2, p0) ? 0 : -1;
    const std::int64_t bias2 = isTopLeft(p0, p1) ? 0 : -1;

    const float invArea = 1.0f / static_cast<float>(area);

    for (int y = minY; y <= maxY; ++y) {
        Color8* colorRow = fb.colorRow(y);
        float* depthRow = fb.depthRow(y);
        for (int x = minX; x <= maxX; ++x) {
            // Sample at the pixel centre.
            const Point p{x * kSubpixelScale + kSubpixelScale / 2,
                          y * kSubpixelScale + kSubpixelScale / 2};
            const std::int64_t w0 = edge(p1, p2, p);
            const std::int64_t w1 = edge(p2, p0, p);
            const std::int64_t w2 = edge(p0, p1, p);
            if ((w0 + bias0) < 0 || (w1 + bias1) < 0 || (w2 + bias2) < 0) continue;

            // Barycentric weights: each edge function is proportional to the
            // distance from the opposite vertex's edge, so dividing by the
            // total area gives weights that sum to one.
            const float l1 = static_cast<float>(w1) * invArea;
            const float l2 = static_cast<float>(w2) * invArea;
            const float z = z0 + l1 * (z1 - z0) + l2 * (z2 - z0);

            if (z >= depthRow[x]) continue;  // something nearer is already there
            depthRow[x] = z;
            colorRow[x] = color;
        }
    }
}

}  // namespace termesh
