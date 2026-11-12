#include <doctest/doctest.h>

#include "pipeline/clip.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

ClipVertex vertex(float x, float y, float z, float w, float v0 = 0.0f, float v1 = 0.0f) {
    ClipVertex v{};
    v.position = {x, y, z, w};
    v.varyings[0] = v0;
    v.varyings[1] = v1;
    return v;
}

bool insideFrustum(Vec4 p, float eps = 1e-4f) {
    for (unsigned plane = 1; plane < kClipAll; plane <<= 1)
        if (clipDistance(p, plane) < -eps) return false;
    return true;
}

// Twice the signed area of the polygon's projection onto the screen.
float projectedArea(const ClipVertex* v, int n) {
    float area = 0.0f;
    for (int i = 0; i < n; ++i) {
        const Vec4 a = v[i].position, b = v[(i + 1) % n].position;
        area += (a.x / a.w) * (b.y / b.w) - (b.x / b.w) * (a.y / a.w);
    }
    return area;
}

}  // namespace

TEST_CASE("clip: outcodes") {
    CHECK(clipOutcode({0, 0, 0, 1}) == 0u);
    CHECK(clipOutcode({1, 1, 1, 1}) == 0u);  // on the boundary counts as inside
    CHECK(clipOutcode({-2, 0, 0, 1}) == kClipLeft);
    CHECK(clipOutcode({2, 0, 0, 1}) == kClipRight);
    CHECK(clipOutcode({0, -2, 0, 1}) == kClipBottom);
    CHECK(clipOutcode({0, 2, 0, 1}) == kClipTop);
    CHECK(clipOutcode({0, 0, -2, 1}) == kClipNear);
    CHECK(clipOutcode({0, 0, 2, 1}) == kClipFar);
    CHECK(clipOutcode({3, 3, 0, 1}) == (kClipRight | kClipTop));
    // Behind the camera (negative w) a point is outside both planes of a pair.
    CHECK((clipOutcode({0, 0, 0, -1}) & kClipNear) != 0u);
}

TEST_CASE("clip: a triangle inside the frustum is untouched") {
    ClipVertex poly[kMaxClipVertices] = {vertex(-0.5f, -0.5f, 0, 1), vertex(0.5f, -0.5f, 0, 1), vertex(0, 0.5f, 0, 1)};
    const int n = clipPolygon(poly, 3, kClipAll, 2);
    REQUIRE(n == 3);
    CHECK(poly[0].position == Vec4{-0.5f, -0.5f, 0, 1});
    CHECK(poly[1].position == Vec4{0.5f, -0.5f, 0, 1});
    CHECK(poly[2].position == Vec4{0, 0.5f, 0, 1});
}

TEST_CASE("clip: a triangle outside one plane disappears") {
    ClipVertex poly[kMaxClipVertices] = {vertex(2, 0, 0, 1), vertex(3, 0, 0, 1), vertex(2, 1, 0, 1)};
    CHECK(clipPolygon(poly, 3, kClipAll, 0) == 0);
}

TEST_CASE("clip: one vertex behind the near plane makes a quad") {
    // z = -w is the near plane. Vertex c is behind the camera entirely.
    ClipVertex poly[kMaxClipVertices] = {vertex(-0.5f, 0, 0, 1, 0.0f), vertex(0.5f, 0, 0, 1, 1.0f),
                                         vertex(0, 0.5f, -3, -1, 2.0f)};
    const int n = clipPolygon(poly, 3, kClipNear, 1);
    REQUIRE(n == 4);
    for (int i = 0; i < n; ++i) {
        CAPTURE(i);
        CHECK(clipDistance(poly[i].position, kClipNear) >= -1e-5f);
        CHECK(poly[i].position.w > 0.0f);  // safe to divide by
    }
}

TEST_CASE("clip: two vertices behind the near plane leave a triangle") {
    ClipVertex poly[kMaxClipVertices] = {vertex(0, 0, 0, 1), vertex(1, 0, -3, -1), vertex(-1, 0, -3, -1)};
    const int n = clipPolygon(poly, 3, kClipNear, 0);
    REQUIRE(n == 3);
    for (int i = 0; i < n; ++i) CHECK(clipDistance(poly[i].position, kClipNear) >= -1e-5f);
}

TEST_CASE("clip: new vertices lie on the plane with linearly interpolated varyings") {
    // Edge from x = 0 (inside) to x = 4 (outside); the right plane x = w = 1
    // cuts it a quarter of the way along.
    ClipVertex poly[kMaxClipVertices] = {vertex(0, 0, 0, 1, 10.0f, -1.0f), vertex(4, 0, 0, 1, 50.0f, 3.0f),
                                         vertex(0, 0.5f, 0, 1, 10.0f, -1.0f)};
    const int n = clipPolygon(poly, 3, kClipRight, 2);
    REQUIRE(n == 4);

    int onPlane = 0;
    for (int i = 0; i < n; ++i) {
        if (std::abs(clipDistance(poly[i].position, kClipRight)) < 1e-5f) {
            ++onPlane;
            CHECK(poly[i].position.x == Approx(1.0f));
            CHECK(poly[i].varyings[0] == Approx(20.0f));  // 10 + (50 - 10) / 4
            CHECK(poly[i].varyings[1] == Approx(0.0f));   // -1 + (3 - -1) / 4
        }
    }
    CHECK(onPlane == 2);
}

TEST_CASE("clip: a huge triangle is cut down to the frustum") {
    ClipVertex poly[kMaxClipVertices] = {vertex(-50, -40, 0.2f, 1), vertex(60, -30, -0.4f, 1), vertex(5, 70, 0.9f, 1)};
    const float before = projectedArea(poly, 3);
    const int n = clipPolygon(poly, 3, kClipAll, 0);
    REQUIRE(n >= 3);
    REQUIRE(n <= kMaxClipVertices);
    for (int i = 0; i < n; ++i) {
        CAPTURE(i);
        CHECK(insideFrustum(poly[i].position));
    }
    // It covered the whole viewport, so what is left is the viewport: a 2x2
    // square in NDC (projectedArea returns twice the area).
    const float after = projectedArea(poly, n);
    CHECK(std::abs(after) == Approx(8.0f).epsilon(1e-3));
    // Clipping must not flip the winding - back-face culling depends on it.
    CHECK((before > 0.0f) == (after > 0.0f));
}

TEST_CASE("clip: only the requested planes are applied") {
    ClipVertex poly[kMaxClipVertices] = {vertex(0, 0, 0, 1), vertex(4, 0, 0, 1), vertex(0, 4, 0, 1)};
    const int n = clipPolygon(poly, 3, kClipRight, 0);
    REQUIRE(n == 4);
    bool stillAboveTop = false;
    for (int i = 0; i < n; ++i)
        if (poly[i].position.y > 1.0f) stillAboveTop = true;
    CHECK(stillAboveTop);
}

TEST_CASE("clip: shared edges get identical intersection points") {
    // Two triangles sharing the edge a-b, wound in opposite directions along
    // it. If the clipper computed the crossing point differently for each, a
    // crack would open between them along the clip plane.
    const ClipVertex a = vertex(0.3f, -0.7f, 0.1f, 1), b = vertex(2.9f, 0.4f, -0.2f, 1.3f);
    ClipVertex left[kMaxClipVertices] = {a, b, vertex(0, 0.9f, 0, 1)};
    ClipVertex right[kMaxClipVertices] = {b, a, vertex(0.5f, -3.0f, 0, 1)};
    const int nl = clipPolygon(left, 3, kClipRight, 0);
    const int nr = clipPolygon(right, 3, kClipRight, 0);

    auto crossing = [](const ClipVertex* v, int n, Vec4& out) {
        for (int i = 0; i < n; ++i)
            if (std::abs(clipDistance(v[i].position, kClipRight)) < 1e-5f && v[i].position.y < 0.3f &&
                v[i].position.y > -0.69f) {
                out = v[i].position;
                return true;
            }
        return false;
    };
    Vec4 pl, pr;
    REQUIRE(crossing(left, nl, pl));
    REQUIRE(crossing(right, nr, pr));
    CHECK(pl == pr);  // bit-for-bit
}
