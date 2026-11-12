// Mesh simplification, level-of-detail selection and the quality governor.
#include <doctest/doctest.h>

#include <set>
#include <vector>

#include "accel/model_accel.hpp"
#include "accel/simplify.hpp"
#include "anim/skinning.hpp"
#include "app/quality.hpp"
#include "pipeline/renderer.hpp"
#include "pipeline/resolve.hpp"
#include "scene/primitives.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

// A flat grid of (n x n) squares in the xy plane, with its own vertices.
// `offset` shifts it; `uvOffset` gives it a different patch of texture.
void addGrid(Mesh& mesh, int n, Vec3 offset, Vec2 uvOffset) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            Vertex v;
            v.position = offset + Vec3{static_cast<float>(x), static_cast<float>(y), 0.0f};
            v.uv = uvOffset + Vec2{static_cast<float>(x), static_cast<float>(y)} * (1.0f / static_cast<float>(n));
            mesh.vertices.push_back(v);
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t a = base + static_cast<std::uint32_t>(y * (n + 1) + x), b = a + 1;
            const std::uint32_t c = a + static_cast<std::uint32_t>(n + 1), d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
        }
}

Bounds boundsOf(const Mesh& mesh, const std::vector<std::uint32_t>& indices) {
    Bounds box = emptyBounds();
    for (const std::uint32_t i : indices) include(box, mesh.vertices[i].position);
    return box;
}

std::size_t covered(const Framebuffer& fb) {
    std::size_t n = 0;
    for (const Color8& p : fb.pixels()) n += p.a != 0;
    return n;
}

}  // namespace

// ---------------------------------------------------------- simplification ---

TEST_CASE("simplify: each level is a smaller, valid mesh over the same vertices") {
    const Model model = makeDemoModel("knot");
    const Mesh& mesh = model.meshes[0];
    const std::size_t original = mesh.indices.size() / 3;
    const std::vector<MeshLod> lods = buildLods(mesh);
    REQUIRE(lods.size() >= 3);

    std::size_t previous = original;
    float previousError = 0.0f;
    for (const MeshLod& lod : lods) {
        const std::size_t triangles = lod.indices.size() / 3;
        // Roughly half of the level before.
        CHECK(triangles < previous);
        CHECK(triangles <= previous * 6 / 10);
        CHECK(triangles >= previous * 3 / 10);
        CHECK(lod.indices.size() % 3 == 0);

        // Indices are in range, and no triangle has collapsed to a line.
        for (std::size_t t = 0; t < triangles; ++t) {
            const std::uint32_t a = lod.indices[t * 3], b = lod.indices[t * 3 + 1], c = lod.indices[t * 3 + 2];
            REQUIRE(a < mesh.vertices.size());
            REQUIRE(b < mesh.vertices.size());
            REQUIRE(c < mesh.vertices.size());
            const Vec3 pa = mesh.vertices[a].position, pb = mesh.vertices[b].position, pc = mesh.vertices[c].position;
            REQUIRE(lengthSquared(cross(pb - pa, pc - pa)) > 0.0f);
        }

        // The vertex list is exactly the vertices used, in ascending order.
        std::set<std::uint32_t> used(lod.indices.begin(), lod.indices.end());
        CHECK(lod.vertices == std::vector<std::uint32_t>(used.begin(), used.end()));
        CHECK(lod.vertices.size() < mesh.vertices.size());

        // The error only grows, and stays small next to the model.
        CHECK(lod.error >= previousError);
        CHECK(lod.error < model.bounds().radius() * 0.1f);

        // The simplified shape occupies about the same space.
        const Bounds box = boundsOf(mesh, lod.indices), whole = model.bounds();
        CHECK(length(box.min - whole.min) < whole.radius() * 0.08f);
        CHECK(length(box.max - whole.max) < whole.radius() * 0.08f);

        previous = triangles;
        previousError = lod.error;
    }
    CHECK(lods.front().error > 0.0f);  // a curved surface cannot be simplified for free
}

TEST_CASE("simplify: the result is the same every time") {
    const Model model = makeDemoModel("sphere");
    const std::vector<MeshLod> a = buildLods(model.meshes[0]), b = buildLods(model.meshes[0]);
    REQUIRE(a.size() == b.size());
    REQUIRE_FALSE(a.empty());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].indices == b[i].indices);
        CHECK(a[i].error == b[i].error);
    }
}

TEST_CASE("simplify: a flat sheet loses triangles but not its outline") {
    Mesh sheet;
    addGrid(sheet, 24, {0, 0, 0}, {0, 0});  // 1152 triangles
    const std::vector<MeshLod> lods = buildLods(sheet);
    REQUIRE(lods.size() >= 3);
    for (const MeshLod& lod : lods) {
        // Flat: removing interior vertices costs nothing at all.
        CHECK(lod.error == Approx(0.0f).scale(1.0));
        // The border must not be eaten into: the sheet still spans its
        // full extent, which means all four corners survived.
        const Bounds box = boundsOf(sheet, lod.indices);
        CHECK(box.min == Vec3{0, 0, 0});
        CHECK(box.max == Vec3{24, 24, 0});
        // ...and it is still the same area: no holes, no overlaps.
        double area = 0.0;
        for (std::size_t t = 0; t * 3 < lod.indices.size(); ++t) {
            const Vec3 a = sheet.vertices[lod.indices[t * 3]].position;
            const Vec3 b = sheet.vertices[lod.indices[t * 3 + 1]].position;
            const Vec3 c = sheet.vertices[lod.indices[t * 3 + 2]].position;
            area += 0.5 * length(cross(b - a, c - a));
        }
        CHECK(area == Approx(24.0 * 24.0).epsilon(0.001));
    }
    CHECK(lods.back().indices.size() / 3 < 1152 / 6);
}

TEST_CASE("simplify: texture seams stay closed and unmixed") {
    // Two sheets that meet along x = 16, with separate vertices and
    // different texture coordinates there: a seam.
    Mesh mesh;
    addGrid(mesh, 16, {0, 0, 0}, {0, 0});
    const std::uint32_t firstOfSecond = static_cast<std::uint32_t>(mesh.vertices.size());
    addGrid(mesh, 16, {16, 0, 0}, {5, 5});
    // Bend the second sheet a little so that the fold along the seam matters.
    for (std::uint32_t i = firstOfSecond; i < mesh.vertices.size(); ++i)
        mesh.vertices[i].position.z = (mesh.vertices[i].position.x - 16.0f) * 0.5f;

    const std::vector<MeshLod> lods = buildLods(mesh);
    REQUIRE_FALSE(lods.empty());
    for (const MeshLod& lod : lods) {
        std::set<float> seamLeft, seamRight;  // y of the seam vertices each sheet still uses
        for (std::size_t t = 0; t * 3 < lod.indices.size(); ++t) {
            const std::uint32_t corner[3] = {lod.indices[t * 3], lod.indices[t * 3 + 1], lod.indices[t * 3 + 2]};
            // A triangle belongs to one sheet or the other, never both:
            // mixing them would stretch one texture patch across to the next.
            const bool second = corner[0] >= firstOfSecond;
            REQUIRE((corner[1] >= firstOfSecond) == second);
            REQUIRE((corner[2] >= firstOfSecond) == second);
            for (const std::uint32_t v : corner)
                if (mesh.vertices[v].position.x == 16.0f)
                    (second ? seamRight : seamLeft).insert(mesh.vertices[v].position.y);
        }
        // Both sheets kept exactly the same points along the seam, so there
        // is no gap between them.
        CHECK(seamLeft == seamRight);
        CHECK(seamLeft.count(0.0f) == 1);
        CHECK(seamLeft.count(16.0f) == 1);
    }
    CHECK(lods.back().indices.size() < mesh.indices.size() / 4);
}

TEST_CASE("simplify: small or awkward input") {
    // Too small to bother with.
    CHECK(buildLods(makeDemoModel("cube").meshes[0]).empty());
    Mesh empty;
    CHECK(buildLods(empty).empty());

    // Degenerate triangles in the input are dropped, not divided by.
    Mesh mesh;
    addGrid(mesh, 12, {0, 0, 0}, {0, 0});
    mesh.indices.insert(mesh.indices.end(), {0, 0, 1, 5, 5, 5});
    const std::vector<MeshLod> lods = buildLods(mesh);
    REQUIRE_FALSE(lods.empty());
    for (std::size_t t = 0; t * 3 < lods[0].indices.size(); ++t) {
        const std::uint32_t a = lods[0].indices[t * 3], b = lods[0].indices[t * 3 + 1], c = lods[0].indices[t * 3 + 2];
        REQUIRE(a != b);
        REQUIRE(b != c);
        REQUIRE(a != c);
    }

    // The options are honoured.
    const Model knotModel = makeDemoModel("knot");
    const Mesh& knot = knotModel.meshes[0];
    SimplifyOptions options;
    options.maxLevels = 2;
    CHECK(buildLods(knot, options).size() == 2);
    options.maxLevels = 6;
    options.minTriangles = 3000;
    for (const MeshLod& lod : buildLods(knot, options)) CHECK(lod.indices.size() / 3 >= 3000);
    options.minTriangles = 64;
    options.ratio = 0.25f;
    const std::vector<MeshLod> quartered = buildLods(knot, options);
    REQUIRE_FALSE(quartered.empty());
    CHECK(quartered[0].indices.size() / 3 <= knot.indices.size() / 3 * 3 / 10);
}

// --------------------------------------------------------------- selection ---

TEST_CASE("lod: the renderer draws coarser meshes as they get smaller on screen") {
    const Model model = makeDemoModel("knot");
    ModelAccel accel(model);
    accel.buildLods(model);
    REQUIRE_FALSE(accel.lods(0).empty());
    const DrawItem item{&model, Mat4::identity(), &accel};
    const std::span<const DrawItem> items(&item, 1);

    ThreadPool pool(4);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    const Camera camera = orbit.camera(1.0f);

    RenderSettings settings;
    Framebuffer full(160, 160), same(160, 160), coarse(160, 160);
    const RenderStats reference = renderer.render(items, camera, settings, full);
    CHECK(reference.trianglesSimplified == 0);

    // A tolerance too small for any level changes nothing at all.
    settings.lodTolerance = 1e-6f;
    renderer.render(items, camera, settings, same);
    CHECK(full.pixels() == same.pixels());

    // A generous one draws far fewer triangles for nearly the same picture.
    settings.lodTolerance = 2.0f;
    const RenderStats simplified = renderer.render(items, camera, settings, coarse);
    CHECK(simplified.trianglesSimplified > model.triangleCount() / 2);
    CHECK(simplified.trianglesDrawn < reference.trianglesDrawn / 2);
    CHECK(static_cast<double>(covered(coarse)) == Approx(static_cast<double>(covered(full))).epsilon(0.05));

    // The same tolerance allows more simplification from further away, and
    // at a lower resolution.
    orbit.zoom(4.0f);
    settings.lodTolerance = 0.5f;
    const RenderStats near = renderer.render(items, camera, settings, coarse);
    const RenderStats far = renderer.render(items, orbit.camera(1.0f), settings, coarse);
    CHECK(far.trianglesSimplified > near.trianglesSimplified);
    Framebuffer small(40, 40);
    const RenderStats lowRes = renderer.render(items, camera, settings, small);
    CHECK(lowRes.trianglesSimplified > near.trianglesSimplified);

    // Wireframe shows the real mesh, whatever the tolerance.
    settings.lodTolerance = 100.0f;
    settings.wireframe = true;
    CHECK(renderer.render(items, camera, settings, coarse).trianglesSimplified == 0);

    // Shadows are cast by the same simplified triangles that receive them.
    settings.wireframe = false;
    settings.shadows = true;
    settings.lodTolerance = 2.0f;
    CHECK(renderer.render(items, camera, settings, coarse).trianglesSimplified > 0);

    // A scaled-down copy is simplified as if it were further away.
    settings.shadows = false;
    settings.lodTolerance = 0.5f;
    const DrawItem tiny{&model, scale({0.2f, 0.2f, 0.2f}), &accel};
    CHECK(renderer.render(std::span<const DrawItem>(&tiny, 1), camera, settings, coarse).trianglesSimplified >
          near.trianglesSimplified);

    // Without levels (or without an acceleration structure) the tolerance is ignored.
    const DrawItem plain{&model, Mat4::identity(), nullptr};
    settings.lodTolerance = 100.0f;
    CHECK(renderer.render(std::span<const DrawItem>(&plain, 1), camera, settings, coarse).trianglesSimplified == 0);
}

TEST_CASE("lod: levels built from the rest pose follow the animation") {
    Model model = makeDemoModel("tentacle");
    ModelAccel accel(model);
    accel.buildLods(model);
    REQUIRE_FALSE(accel.lods(0).empty());

    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(model.framingBounds());
    RenderSettings settings;
    settings.lodTolerance = 4.0f;
    const DrawItem item{&model, Mat4::identity(), &accel};

    Pose pose = restPose(model.rig);
    Framebuffer rest(96, 96), bent(96, 96);
    const RenderStats a = renderer.render(std::span<const DrawItem>(&item, 1), orbit.camera(1.0f), settings, rest);
    CHECK(a.trianglesSimplified > 0);

    sampleAnimation(model.rig.animations[1], model.rig.animations[1].duration * 0.5f, pose);
    applyPose(model, pose);
    accel.refresh(model);
    const RenderStats b = renderer.render(std::span<const DrawItem>(&item, 1), orbit.camera(1.0f), settings, bent);
    CHECK(b.trianglesSimplified > 0);
    CHECK(rest.pixels() != bent.pixels());  // the simplified mesh moved with the bones
    CHECK(covered(bent) > 100);
}

TEST_CASE("model accel: installing levels built elsewhere") {
    const Model model = makeDemoModel("knot");
    ModelAccel accel(model);
    CHECK(accel.lods(0).empty());

    std::vector<std::vector<MeshLod>> wrong(3);  // for a model with three meshes
    accel.setLods(std::move(wrong));
    CHECK(accel.lods(0).empty());

    std::vector<std::vector<MeshLod>> built;
    built.push_back(buildLods(model.meshes[0]));
    const std::size_t count = built[0].size();
    accel.setLods(std::move(built));
    CHECK(accel.lods(0).size() == count);
    // Levels survive a refresh: they are lists of triangles, not positions.
    accel.refresh(model);
    CHECK(accel.lods(0).size() == count);
}

// ----------------------------------------------------------------- upscale ---

TEST_CASE("upscale: bilinear enlargement") {
    ThreadPool pool(2);
    Image flat(5, 3);
    for (Color8& p : flat.pixels) p = {40, 80, 120, 200};
    Image out;
    upscale(flat, 17, 11, out, pool);
    REQUIRE(out.width == 17);
    REQUIRE(out.height == 11);
    for (const Color8& p : out.pixels) REQUIRE(p == Color8{40, 80, 120, 200});

    // A two-pixel ramp becomes a smooth one that never overshoots.
    Image ramp(2, 1);
    ramp.pixels = {{0, 0, 0, 255}, {200, 100, 50, 255}};
    upscale(ramp, 8, 1, out, pool);
    CHECK(out.row(0)[0] == Color8{0, 0, 0, 255});
    CHECK(out.row(0)[7] == Color8{200, 100, 50, 255});
    for (int x = 1; x < 8; ++x) CHECK(out.row(0)[x].r >= out.row(0)[x - 1].r);
    CHECK(out.row(0)[3].r > 50);
    CHECK(out.row(0)[4].r < 150);

    // Same size in, same picture out; nothing in, nothing out.
    upscale(ramp, 2, 1, out, pool);
    CHECK(out.pixels == ramp.pixels);
    upscale(Image(), 4, 4, out, pool);
    CHECK(out.width == 4);
}

// ---------------------------------------------------------------- governor ---

TEST_CASE("governor: the ladder goes from subtle to drastic") {
    CHECK(QualityGovernor::level(0).lodScale == 1.0f);
    CHECK(QualityGovernor::level(0).ssaaDrop == 0);
    CHECK(QualityGovernor::level(0).renderScale == 1.0f);
    for (int i = 1; i < QualityGovernor::kLevelCount; ++i) {
        const auto &a = QualityGovernor::level(i - 1), &b = QualityGovernor::level(i);
        CHECK(b.lodScale >= a.lodScale);
        CHECK(b.ssaaDrop >= a.ssaaDrop);
        CHECK(b.renderScale <= a.renderScale);
    }
    // Out-of-range indices are clamped.
    CHECK(QualityGovernor::level(-3).renderScale == 1.0f);
    CHECK(QualityGovernor::level(99).renderScale ==
          QualityGovernor::level(QualityGovernor::kLevelCount - 1).renderScale);
}

TEST_CASE("governor: drops quickly when frames are slow, recovers slowly when they are fast") {
    QualityGovernor governor;
    const double budget = 33.0;
    CHECK(governor.current() == 0);

    // A few slow frames are not enough...
    for (int i = 0; i < QualityGovernor::kSlowFramesToDrop - 1; ++i) CHECK_FALSE(governor.frame(50.0, budget, 3));
    CHECK(governor.current() == 0);
    // ...and one good frame in between starts the count again.
    governor.frame(20.0, budget, 3);
    for (int i = 0; i < QualityGovernor::kSlowFramesToDrop - 1; ++i) governor.frame(50.0, budget, 3);
    CHECK(governor.current() == 0);
    CHECK(governor.frame(50.0, budget, 3));
    CHECK(governor.current() == 1);

    // Frames inside the budget, but not by much, change nothing.
    for (int i = 0; i < 500; ++i) governor.frame(25.0, budget, 3);
    CHECK(governor.current() == 1);

    // Plenty of time to spare, for long enough, restores a level.
    for (int i = 0; i < QualityGovernor::kFastFramesToRecover - 1; ++i) CHECK_FALSE(governor.frame(5.0, budget, 3));
    CHECK(governor.frame(5.0, budget, 3));
    CHECK(governor.current() == 0);
    // Already at full quality: nothing more to restore.
    for (int i = 0; i < 200; ++i) governor.frame(5.0, budget, 3);
    CHECK(governor.current() == 0);
}

TEST_CASE("governor: bounded, and skips steps that would change nothing") {
    QualityGovernor governor;
    // Hopelessly slow: goes to the bottom of the ladder and stays there.
    for (int i = 0; i < 1000; ++i) governor.frame(1000.0, 16.0, 3);
    CHECK(governor.current() == QualityGovernor::kLevelCount - 1);

    // With no supersampling to give up (a pixel backend), the rungs that
    // only drop supersamples are skipped on the way down and on the way up.
    QualityGovernor pixels;
    std::vector<int> visited;
    for (int i = 0; i < 1000; ++i)
        if (pixels.frame(1000.0, 16.0, 1)) visited.push_back(pixels.current());
    for (std::size_t i = 0; i < visited.size(); ++i) {
        const auto& now = QualityGovernor::level(visited[i]);
        const auto& before = QualityGovernor::level(i ? visited[i - 1] : 0);
        CHECK((now.lodScale != before.lodScale || now.renderScale != before.renderScale));
    }
    CHECK(visited.size() < static_cast<std::size_t>(QualityGovernor::kLevelCount - 1));
    CHECK(pixels.current() == QualityGovernor::kLevelCount - 1);
    int recoveries = 0;
    for (int i = 0; i < 2000; ++i) recoveries += pixels.frame(0.1, 16.0, 1);
    CHECK(pixels.current() == 0);
    CHECK(recoveries == static_cast<int>(visited.size()));

    // No target frame rate: no adaptation.
    QualityGovernor off;
    for (int i = 0; i < 100; ++i) CHECK_FALSE(off.frame(1000.0, 0.0, 3));
    CHECK(off.current() == 0);
}
