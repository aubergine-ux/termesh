#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "pipeline/renderer.hpp"
#include "raster/rasterizer.hpp"
#include "scene/primitives.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

struct ScreenVertex {
    float x, y, z = 0.5f, w = 1.0f, varying = 0.0f;
};

// Build a RasterTriangle the way the renderer's setup stage does.
bool makeTriangle(ScreenVertex a, ScreenVertex b, ScreenVertex c, RasterTriangle& tri) {
    ScreenVertex v[3] = {a, b, c};
    auto fixed = [](float f) { return static_cast<std::int32_t>(std::floor(f * kSubpixelScale + 0.5f)); };
    if (edgeFunction(fixed(a.x), fixed(a.y), fixed(b.x), fixed(b.y), fixed(c.x), fixed(c.y)) < 0)
        std::swap(v[1], v[2]);
    tri = {};
    for (int i = 0; i < 3; ++i) {
        tri.x[i] = fixed(v[i].x);
        tri.y[i] = fixed(v[i].y);
        tri.z[i] = v[i].z;
        tri.invW[i] = 1.0f / v[i].w;
        tri.varyings[i][0] = v[i].varying;
    }
    return edgeFunction(tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2]) != 0;
}

// Writes a fixed colour, with the interpolated varying in the green channel.
struct TestShader {
    Color8 color{255, 255, 255, 255};

    int varyingCount() const { return 1; }
    float depthSlopeBias() const { return 0.0f; }
    bool shadeEdge(const RasterTriangle&, const Fragment& frag, Color8& out) const {
        out = color;
        out.g = static_cast<std::uint8_t>(std::clamp(frag.varyings[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        return true;
    }
    bool operator()(const RasterTriangle&, const Fragment& frag, Color8& out) const {
        out = color;
        out.g = static_cast<std::uint8_t>(std::clamp(frag.varyings[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        return true;
    }
};

void draw(Framebuffer& fb, const RasterTriangle& tri, const TestShader& shader = {}) {
    const RasterTriangle* list[1] = {&tri};
    rasterizeTile(fb, {0, 0, fb.width(), fb.height()}, list, 1, shader);
}

int countCovered(const Framebuffer& fb) {
    int n = 0;
    for (const Color8& p : fb.pixels()) n += p.a != 0;
    return n;
}

Framebuffer blank(int w, int h) {
    Framebuffer fb(w, h);
    fb.clear({0, 0, 0, 0});
    return fb;
}

}  // namespace

TEST_CASE("raster: an axis-aligned square covers exactly its pixels") {
    Framebuffer fb = blank(16, 16);
    RasterTriangle a, b;
    REQUIRE(makeTriangle({2, 3}, {10, 3}, {10, 9}, a));
    REQUIRE(makeTriangle({2, 3}, {10, 9}, {2, 9}, b));
    draw(fb, a);
    draw(fb, b);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const bool expected = x >= 2 && x < 10 && y >= 3 && y < 9;
            CHECK((fb.at(x, y).a != 0) == expected);
        }
}

TEST_CASE("raster: triangles sharing an edge never overlap or leave gaps") {
    // Split random quads along each diagonal. Whichever way a quad is cut,
    // the two halves must cover every pixel of it exactly once - that is what
    // the top-left fill rule and exact integer edge functions buy.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> jitter(-6.0f, 6.0f);

    for (int iteration = 0; iteration < 200; ++iteration) {
        const ScreenVertex q[4] = {{10 + jitter(rng), 10 + jitter(rng)},
                                   {50 + jitter(rng), 10 + jitter(rng)},
                                   {50 + jitter(rng), 50 + jitter(rng)},
                                   {10 + jitter(rng), 50 + jitter(rng)}};
        std::vector<int> hits[2];
        for (int split = 0; split < 2; ++split) {
            hits[split].assign(64 * 64, 0);
            const int tris[2][2][3] = {{{0, 1, 2}, {0, 2, 3}}, {{0, 1, 3}, {1, 2, 3}}};
            for (int t = 0; t < 2; ++t) {
                Framebuffer fb = blank(64, 64);
                RasterTriangle tri;
                if (!makeTriangle(q[tris[split][t][0]], q[tris[split][t][1]], q[tris[split][t][2]], tri))
                    continue;
                draw(fb, tri);
                for (int i = 0; i < 64 * 64; ++i) hits[split][i] += fb.pixels()[i].a != 0;
            }
        }
        int overlaps = 0, mismatches = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            overlaps += hits[0][i] > 1 || hits[1][i] > 1;
            mismatches += (hits[0][i] != 0) != (hits[1][i] != 0);
        }
        CAPTURE(iteration);
        CHECK(overlaps == 0);
        CHECK(mismatches == 0);
    }
}

TEST_CASE("raster: a fan of thin triangles tiles a disc with no double hits") {
    const int slices = 37;
    std::vector<int> hits(96 * 96, 0);
    for (int i = 0; i < slices; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / slices;
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / slices;
        Framebuffer fb = blank(96, 96);
        RasterTriangle tri;
        if (!makeTriangle({48.3f, 47.6f}, {48.3f + 40 * std::cos(a0), 47.6f + 40 * std::sin(a0)},
                          {48.3f + 40 * std::cos(a1), 47.6f + 40 * std::sin(a1)}, tri))
            continue;
        draw(fb, tri);
        for (int p = 0; p < 96 * 96; ++p) hits[p] += fb.pixels()[p].a != 0;
    }
    int doubles = 0, holes = 0;
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 96; ++x) {
            const int h = hits[y * 96 + x];
            doubles += h > 1;
            // Well inside the disc every pixel must be hit.
            const float dx = x + 0.5f - 48.3f, dy = y + 0.5f - 47.6f;
            if (dx * dx + dy * dy < 38.0f * 38.0f) holes += h == 0;
        }
    CHECK(doubles == 0);
    CHECK(holes == 0);
}

TEST_CASE("raster: winding order does not matter to coverage") {
    Framebuffer cw = blank(32, 32), ccw = blank(32, 32);
    RasterTriangle a, b;
    REQUIRE(makeTriangle({3.2f, 4.1f}, {28.7f, 9.3f}, {12.5f, 27.8f}, a));
    REQUIRE(makeTriangle({3.2f, 4.1f}, {12.5f, 27.8f}, {28.7f, 9.3f}, b));
    draw(cw, a);
    draw(ccw, b);
    CHECK(countCovered(cw) > 100);
    CHECK(cw.pixels() == ccw.pixels());
}

TEST_CASE("raster: the nearer triangle wins whatever the draw order") {
    RasterTriangle nearTri, farTri;
    REQUIRE(makeTriangle({2, 2, 0.2f}, {30, 2, 0.2f}, {2, 30, 0.2f}, nearTri));
    REQUIRE(makeTriangle({2, 2, 0.8f}, {30, 2, 0.8f}, {2, 30, 0.8f}, farTri));
    TestShader red, blue;
    red.color = {255, 0, 0, 255};
    blue.color = {0, 0, 255, 255};

    Framebuffer first = blank(32, 32), second = blank(32, 32);
    draw(first, nearTri, red);
    draw(first, farTri, blue);
    draw(second, farTri, blue);
    draw(second, nearTri, red);

    CHECK(first.at(8, 8).r == 255);
    CHECK(first.at(8, 8).b == 0);
    CHECK(second.at(8, 8).r == 255);
    CHECK(first.depthRow(8)[8] == Approx(0.2f));
}

TEST_CASE("raster: depth is interpolated across the triangle") {
    Framebuffer fb = blank(64, 8);
    RasterTriangle tri;
    // Depth runs from 0 at x = 0 to 1 at x = 64.
    REQUIRE(makeTriangle({0, 0, 0.0f}, {64, 0, 1.0f}, {0, 8, 0.0f}, tri));
    draw(fb, tri);
    CHECK(fb.depthRow(0)[0] == Approx(0.5f / 64.0f).epsilon(1e-3));
    CHECK(fb.depthRow(0)[32] == Approx(32.5f / 64.0f).epsilon(1e-3));
}

TEST_CASE("raster: varyings are perspective-correct") {
    // A strip receding into the distance: w = 1 at the left edge, w = 4 at
    // the right, with the varying going 0 -> 1. Halfway across the *screen*
    // is much less than halfway along the *surface*:
    //
    //     u = (0.5 * 0/1 + 0.5 * 1/4) / (0.5 * 1/1 + 0.5 * 1/4) = 0.2
    Framebuffer fb = blank(64, 16);
    RasterTriangle a, b;
    const ScreenVertex tl{0, 0, 0.5f, 1, 0}, tr{64, 0, 0.5f, 4, 1}, br{64, 16, 0.5f, 4, 1}, bl{0, 16, 0.5f, 1, 0};
    REQUIRE(makeTriangle(tl, tr, br, a));
    REQUIRE(makeTriangle(tl, br, bl, b));
    draw(fb, a);
    draw(fb, b);

    auto expected = [](float screenFraction) {
        const float l = screenFraction;
        return (l * 1.0f / 4.0f) / ((1.0f - l) * 1.0f + l * 1.0f / 4.0f);
    };
    for (int x : {0, 16, 31, 32, 48, 63}) {
        CAPTURE(x);
        const float want = expected((x + 0.5f) / 64.0f);
        // Both triangles of the quad must agree, so check a row in each.
        CHECK(fb.at(x, 2).g / 255.0f == Approx(want).epsilon(0.02).scale(1.0));
        CHECK(fb.at(x, 13).g / 255.0f == Approx(want).epsilon(0.02).scale(1.0));
    }
    // And it is clearly not the affine answer.
    CHECK(fb.at(32, 8).g / 255.0f < 0.25f);
}

TEST_CASE("raster: splitting the screen into tiles does not change the image") {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> pos(0.0f, 100.0f), depth(0.1f, 0.9f);
    std::vector<RasterTriangle> tris;
    for (int i = 0; i < 60; ++i) {
        RasterTriangle tri;
        if (makeTriangle({pos(rng), pos(rng), depth(rng)}, {pos(rng), pos(rng), depth(rng)},
                         {pos(rng), pos(rng), depth(rng)}, tri))
            tris.push_back(tri);
    }
    std::vector<const RasterTriangle*> list;
    for (const auto& t : tris) list.push_back(&t);

    Framebuffer whole = blank(100, 100), tiled = blank(100, 100);
    const TestShader shader;
    rasterizeTile(whole, {0, 0, 100, 100}, list.data(), list.size(), shader);
    // Odd-sized tiles, so SIMD groups get cut at awkward places.
    for (int y = 0; y < 100; y += 13)
        for (int x = 0; x < 100; x += 7)
            rasterizeTile(tiled, {x, y, std::min(x + 7, 100), std::min(y + 13, 100)}, list.data(),
                          list.size(), shader);
    CHECK(whole.pixels() == tiled.pixels());
}

TEST_CASE("lines: an edge is an unbroken run of pixels, one per step along its major axis") {
    Framebuffer fb = blank(64, 64);
    RasterTriangle tri;
    REQUIRE(makeTriangle({4.5f, 10.5f}, {58.5f, 30.5f}, {20.5f, 60.5f}, tri));
    tri.edgeMask = 0b111;
    const RasterTriangle* list[1] = {&tri};
    rasterizeTileEdges(fb, {0, 0, 64, 64}, list, 1, TestShader{}, 1);

    // The edge from (4.5, 10.5) to (58.5, 30.5) is x-major: every column
    // from 4 to 57 must contain a pixel near the ideal line.
    for (int x = 4; x <= 57; ++x) {
        const int expected = static_cast<int>(std::floor(10.5f + (x + 0.5f - 4.5f) * 20.0f / 54.0f));
        CAPTURE(x);
        CHECK(fb.at(x, expected).a != 0);
    }
    // The edge from (20.5, 60.5) to (4.5, 10.5) is y-major: one pixel per row.
    for (int y = 11; y <= 59; ++y) {
        int hits = 0;
        for (int x = 0; x < 24; ++x) hits += fb.at(x, y).a != 0;
        CAPTURE(y);
        CHECK(hits >= 1);
    }
}

TEST_CASE("lines: shared edges, edge flags, width and tiling") {
    // Two triangles sharing the edge p-q, described in opposite directions.
    const ScreenVertex p{10.3f, 12.7f}, q{50.1f, 41.2f};
    RasterTriangle left, right;
    REQUIRE(makeTriangle(p, q, {12.0f, 50.0f}, left));
    REQUIRE(makeTriangle(q, p, {52.0f, 8.0f}, right));
    const TestShader shader;

    // Flag only the edge that joins p and q (edge e is opposite vertex e).
    auto fixed = [](float f) { return static_cast<std::int32_t>(std::floor(f * kSubpixelScale + 0.5f)); };
    auto isEndpoint = [&](const RasterTriangle& tri, int v) {
        return (tri.x[v] == fixed(p.x) && tri.y[v] == fixed(p.y)) || (tri.x[v] == fixed(q.x) && tri.y[v] == fixed(q.y));
    };
    auto sharedEdgeOnly = [&](RasterTriangle tri) {
        tri.edgeMask = 0;
        for (int e = 0; e < 3; ++e)
            if (isEndpoint(tri, (e + 1) % 3) && isEndpoint(tri, (e + 2) % 3)) tri.edgeMask = static_cast<std::uint8_t>(1 << e);
        return tri;
    };
    const RasterTriangle a = sharedEdgeOnly(left), b = sharedEdgeOnly(right);
    REQUIRE(a.edgeMask != 0);
    REQUIRE(b.edgeMask != 0);

    Framebuffer fa = blank(64, 64), fb2 = blank(64, 64);
    const RasterTriangle* la[1] = {&a};
    const RasterTriangle* lb[1] = {&b};
    rasterizeTileEdges(fa, {0, 0, 64, 64}, la, 1, shader, 1);
    rasterizeTileEdges(fb2, {0, 0, 64, 64}, lb, 1, shader, 1);
    CHECK(countCovered(fa) >= 39);
    CHECK(fa.pixels() == fb2.pixels());  // both triangles light the same pixels

    SUBCASE("edges that are not flagged are not drawn") {
        RasterTriangle none = left;
        none.edgeMask = 0;
        Framebuffer f = blank(64, 64);
        const RasterTriangle* l[1] = {&none};
        rasterizeTileEdges(f, {0, 0, 64, 64}, l, 1, shader, 1);
        CHECK(countCovered(f) == 0);
    }
    SUBCASE("a wider line covers proportionally more pixels") {
        Framebuffer wide = blank(64, 64);
        rasterizeTileEdges(wide, {0, 0, 64, 64}, la, 1, shader, 3);
        CHECK(countCovered(wide) == 3 * countCovered(fa));
    }
    SUBCASE("drawing tile by tile gives the same picture") {
        RasterTriangle all = left;
        all.edgeMask = 0b111;
        const RasterTriangle* l[1] = {&all};
        Framebuffer whole = blank(64, 64), tiled = blank(64, 64);
        rasterizeTileEdges(whole, {0, 0, 64, 64}, l, 1, shader, 2);
        for (int y = 0; y < 64; y += 9)
            for (int x = 0; x < 64; x += 11)
                rasterizeTileEdges(tiled, {x, y, std::min(x + 11, 64), std::min(y + 9, 64)}, l, 1, shader, 2);
        CHECK(whole.pixels() == tiled.pixels());
    }
    SUBCASE("a line behind existing geometry is hidden") {
        Framebuffer f = blank(64, 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) f.depthRow(y)[x] = 0.1f;  // something nearer everywhere
        rasterizeTileEdges(f, {0, 0, 64, 64}, la, 1, shader, 1);
        CHECK(countCovered(f) == 0);
    }
}

TEST_CASE("renderer: wireframe outlines are closed") {
    // Every row crossing the cube's silhouette must have a line pixel at its
    // left and right extremes - the case the old "shade pixels near an edge"
    // approach got wrong, leaving dotted outlines.
    const Model model = makeDemoModel("cube");
    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    const Camera camera = orbit.camera(160.0f / 120.0f);

    RenderSettings solid, wire;
    wire.wireframe = true;
    Framebuffer filled(160, 120), lines(160, 120);
    renderer.render(model, camera, solid, filled);
    renderer.render(model, camera, wire, lines);

    int rowsChecked = 0, broken = 0;
    for (int y = 0; y < 120; ++y) {
        int first = -1, last = -1;
        for (int x = 0; x < 160; ++x)
            if (filled.at(x, y).a != 0) {
                if (first < 0) first = x;
                last = x;
            }
        if (first < 0 || last - first < 6) continue;
        ++rowsChecked;
        // Allow a pixel of slack: lines are centred on the edge, fills stop at it.
        bool leftOk = false, rightOk = false;
        for (int d = -1; d <= 1; ++d) {
            if (first + d >= 0 && first + d < 160) leftOk = leftOk || lines.at(first + d, y).a != 0;
            if (last + d >= 0 && last + d < 160) rightOk = rightOk || lines.at(last + d, y).a != 0;
        }
        broken += !leftOk || !rightOk;
    }
    CHECK(rowsChecked > 50);
    CHECK(broken == 0);
}

TEST_CASE("renderer: output does not depend on the number of threads") {
    const Model model = makeDemoModel("knot");
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    const Camera camera = orbit.camera(160.0f / 100.0f);

    for (ShadingMode mode : {ShadingMode::Flat, ShadingMode::Gouraud, ShadingMode::Phong}) {
        RenderSettings settings;
        settings.shading = mode;
        ThreadPool one(1), many(8);
        Renderer a(one), b(many);
        Framebuffer fa(160, 100), fb(160, 100);
        a.render(model, camera, settings, fa);
        b.render(model, camera, settings, fb);
        CAPTURE(shadingModeName(mode));
        CHECK(fa.pixels() == fb.pixels());
    }
}

TEST_CASE("renderer: culling, clipping and background") {
    const Model model = makeDemoModel("cube");
    ThreadPool pool(2);
    Renderer renderer(pool);
    Framebuffer fb(120, 90);
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    RenderSettings settings;

    SUBCASE("a framed cube: three faces visible, corners empty") {
        const RenderStats stats = renderer.render(model, orbit.camera(120.0f / 90.0f), settings, fb);
        CHECK(stats.trianglesSubmitted == 12);
        CHECK(stats.trianglesDrawn == 6);  // the other three faces are back-facing
        CHECK(fb.at(60, 45).a == 255);
        CHECK(fb.at(0, 0).a == 0);
        CHECK(fb.at(119, 89).a == 0);
    }
    SUBCASE("disabling culling draws the back faces too") {
        settings.cullBackfaces = false;
        const RenderStats stats = renderer.render(model, orbit.camera(120.0f / 90.0f), settings, fb);
        CHECK(stats.trianglesDrawn == 12);
    }
    SUBCASE("zoomed inside the cube: triangles cross the near plane and get clipped") {
        orbit.zoom(0.1f);
        settings.cullBackfaces = false;
        const RenderStats stats = renderer.render(model, orbit.camera(120.0f / 90.0f), settings, fb);
        CHECK(stats.trianglesDrawn > 0);
        // The view is filled edge to edge: no background anywhere.
        CHECK(countCovered(fb) == 120 * 90);
    }
    SUBCASE("wireframe leaves the face interiors as background") {
        settings.wireframe = true;
        renderer.render(model, orbit.camera(120.0f / 90.0f), settings, fb);
        const int covered = countCovered(fb);
        CHECK(covered > 100);
        CHECK(covered < 120 * 90 / 4);
    }
}
