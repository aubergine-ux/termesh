// The BVH, frustum culling and picking.
#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "accel/model_accel.hpp"
#include "pipeline/renderer.hpp"
#include "scene/primitives.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

Camera lookFrom(Vec3 eye, Vec3 target, float zNear = 0.5f, float zFar = 50.0f) {
    Camera camera;
    camera.position = eye;
    camera.view = lookAt(eye, target, {0, 1, 0});
    camera.projection = perspective(radians(40.0f), 1.0f, zNear, zFar);
    return camera;
}

// The slow, obviously correct answer to compare the tree against.
bool bruteForce(const Mesh& mesh, const Ray& ray, float tMax, bool cull, BvhHit& hit) {
    bool found = false;
    for (std::uint32_t tri = 0; tri * 3 + 2 < mesh.indices.size(); ++tri) {
        const Vec3 a = mesh.vertices[mesh.indices[tri * 3]].position;
        const Vec3 b = mesh.vertices[mesh.indices[tri * 3 + 1]].position;
        const Vec3 c = mesh.vertices[mesh.indices[tri * 3 + 2]].position;
        float t, u, v;
        if (!rayHitsTriangle(ray, a, b, c, t, u, v) || t >= tMax) continue;
        if (cull && dot(cross(b - a, c - a), ray.direction) >= 0.0f) continue;
        tMax = t;
        hit = {t, tri, u, v};
        found = true;
    }
    return found;
}

}  // namespace

// --------------------------------------------------------------- primitives ---

TEST_CASE("ray: triangle intersection") {
    const Vec3 a{0, 0, 0}, b{1, 0, 0}, c{0, 1, 0};
    float t, u, v;

    REQUIRE(rayHitsTriangle({{0.25f, 0.25f, 5}, {0, 0, -1}}, a, b, c, t, u, v));
    CHECK(t == Approx(5.0f));
    CHECK(u == Approx(0.25f));  // weight of b
    CHECK(v == Approx(0.25f));  // weight of c

    // The direction's length scales t, nothing else.
    REQUIRE(rayHitsTriangle({{0.25f, 0.25f, 5}, {0, 0, -10}}, a, b, c, t, u, v));
    CHECK(t == Approx(0.5f));

    CHECK_FALSE(rayHitsTriangle({{0.8f, 0.8f, 5}, {0, 0, -1}}, a, b, c, t, u, v));    // past the long edge
    CHECK_FALSE(rayHitsTriangle({{-0.1f, 0.2f, 5}, {0, 0, -1}}, a, b, c, t, u, v));   // off to the side
    CHECK_FALSE(rayHitsTriangle({{0.25f, 0.25f, 5}, {0, 0, 1}}, a, b, c, t, u, v));   // pointing away
    CHECK_FALSE(rayHitsTriangle({{0.25f, 0.25f, 5}, {1, 0, 0}}, a, b, c, t, u, v));   // parallel to it
    CHECK(rayHitsTriangle({{0.25f, 0.25f, -5}, {0, 0, 1}}, a, b, c, t, u, v));        // from behind: still a hit
    CHECK_FALSE(rayHitsTriangle({{0.25f, 0.25f, 5}, {0, 0, -1}}, a, a, a, t, u, v));  // a degenerate triangle
}

TEST_CASE("ray: box intersection") {
    const Bounds box{{-1, -1, -1}, {1, 1, 1}};
    auto hits = [&](Vec3 origin, Vec3 direction, float tMax = 100.0f) {
        return rayHitsBox({origin, direction}, {1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z}, box, tMax);
    };
    CHECK(hits({0, 0, 5}, {0, 0, -1}));
    CHECK(hits({0, 0, 0}, {1, 2, 3}));               // starting inside
    CHECK(hits({-5, -5, -5}, {1, 1, 1}));            // through opposite corners
    CHECK_FALSE(hits({0, 0, 5}, {0, 0, 1}));         // pointing away
    CHECK_FALSE(hits({3, 0, 5}, {0, 0, -1}));        // passing beside it
    CHECK_FALSE(hits({0, 0, 5}, {0, 0, -1}, 3.0f));  // stops short
    CHECK(hits({0, 0, 5}, {0, 0, -1}, 4.5f));
    // Axis-parallel rays have zero components, hence infinite inverses.
    CHECK(hits({0.5f, 0.5f, 5}, {0, 0, -1}));
    CHECK_FALSE(hits({1.5f, 0.5f, 5}, {0, 0, -1}));
}

TEST_CASE("frustum: boxes inside, outside and across the edge of the view") {
    const Camera camera = lookFrom({0, 0, 10}, {0, 0, 0});
    const Frustum frustum = Frustum::fromMatrix(camera.projection * camera.view);
    using Side = Frustum::Side;

    CHECK(frustum.classify({{-1, -1, -1}, {1, 1, 1}}) == Side::Inside);
    CHECK(frustum.classify({{50, -1, -1}, {52, 1, 1}}) == Side::Outside);                  // far to the right
    CHECK(frustum.classify({{-1, -1, 20}, {1, 1, 22}}) == Side::Outside);                  // behind the camera
    CHECK(frustum.classify({{-1, -1, -100}, {1, 1, -90}}) == Side::Outside);               // beyond the far plane
    CHECK(frustum.classify({{2, -1, -1}, {30, 1, 1}}) == Side::Intersecting);              // sticking out of the side
    CHECK(frustum.classify({{-1, -1, 5}, {1, 1, 12}}) == Side::Intersecting);              // through the near plane
    CHECK(frustum.classify({{-500, -500, -500}, {500, 500, 500}}) == Side::Intersecting);  // containing the frustum

    // A matrix that includes a model transform gives planes in model space.
    const Mat4 model = translate({50, 0, 0});
    const Frustum local = Frustum::fromMatrix(camera.projection * camera.view * model);
    CHECK(local.classify({{-1, -1, -1}, {1, 1, 1}}) == Side::Outside);    // the model sits 50 to the right
    CHECK(local.classify({{-51, -1, -1}, {-49, 1, 1}}) == Side::Inside);  // ...so this box is back at the origin
}

TEST_CASE("bounds: transforming a box") {
    const Bounds box{{-1, -2, -3}, {1, 2, 3}};
    const Bounds moved = transformBounds(box, translate({10, 0, 0}) * scale({2, 1, 1}));
    CHECK(moved.min == Vec3{8, -2, -3});
    CHECK(moved.max == Vec3{12, 2, 3});
    // A rotation makes the box grow: it must still contain every corner.
    const Bounds turned = transformBounds(box, rotate({0, 0, 1}, radians(45.0f)));
    CHECK(turned.max.x == Approx(std::sqrt(0.5f) * 3.0f));
    CHECK(turned.max.z == Approx(3.0f));
}

// --------------------------------------------------------------------- BVH ---

TEST_CASE("bvh: structure") {
    const Model model = makeDemoModel("knot");
    const Mesh& mesh = model.meshes[0];
    Bvh bvh;
    bvh.build(mesh);
    const std::uint32_t triangles = static_cast<std::uint32_t>(mesh.indices.size() / 3);

    // Every triangle appears exactly once.
    REQUIRE(bvh.triangleCount() == triangles);
    std::vector<std::uint32_t> sorted = bvh.order();
    std::sort(sorted.begin(), sorted.end());
    for (std::uint32_t i = 0; i < triangles; ++i) REQUIRE(sorted[i] == i);

    // The root box is the mesh's box.
    const Bounds whole = model.bounds();
    CHECK(bvh.bounds().min == whole.min);
    CHECK(bvh.bounds().max == whole.max);

    // Every node's box contains its triangles; leaves are small; inner
    // nodes' children lie inside their parent.
    std::size_t leafTriangles = 0;
    for (const Bvh::Node& node : bvh.nodes()) {
        if (node.count == 0) {
            for (std::uint32_t child : {node.first, node.first + 1}) {
                const Bounds& c = bvh.nodes()[child].bounds;
                REQUIRE(c.min.x >= node.bounds.min.x);
                REQUIRE(c.min.y >= node.bounds.min.y);
                REQUIRE(c.min.z >= node.bounds.min.z);
                REQUIRE(c.max.x <= node.bounds.max.x);
                REQUIRE(c.max.y <= node.bounds.max.y);
                REQUIRE(c.max.z <= node.bounds.max.z);
            }
            continue;
        }
        REQUIRE(node.count <= 4);
        leafTriangles += node.count;
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i)
            for (int k = 0; k < 3; ++k) {
                const Vec3 p = mesh.vertices[mesh.indices[bvh.order()[i] * 3 + static_cast<std::uint32_t>(k)]].position;
                REQUIRE(p.x >= node.bounds.min.x);
                REQUIRE(p.x <= node.bounds.max.x);
                REQUIRE(p.y >= node.bounds.min.y);
                REQUIRE(p.y <= node.bounds.max.y);
                REQUIRE(p.z >= node.bounds.min.z);
                REQUIRE(p.z <= node.bounds.max.z);
            }
    }
    CHECK(leafTriangles == triangles);
    // A binary tree with leaves of 1-4 triangles: far fewer than 2 nodes per triangle.
    CHECK(bvh.nodeCount() < static_cast<std::size_t>(triangles) * 2);
}

TEST_CASE("bvh: rays find the same triangle as testing every one") {
    const Model model = makeDemoModel("knot");
    const Mesh& mesh = model.meshes[0];
    Bvh bvh;
    bvh.build(mesh);
    const Bounds bounds = model.bounds();
    const float r = bounds.radius();

    std::mt19937 random(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int hits = 0;
    for (int i = 0; i < 400; ++i) {
        // From somewhere around the model, aimed somewhere inside its box.
        const Vec3 origin = bounds.center() + normalize(Vec3{unit(random), unit(random), unit(random)}) * (r * 2.0f);
        const Vec3 target = bounds.center() + Vec3{unit(random), unit(random), unit(random)} * (r * 0.6f);
        const Ray ray{origin, target - origin};
        for (const bool cull : {false, true}) {
            BvhHit fast, slow;
            const bool a = bvh.intersect(mesh, ray, 10.0f, cull, fast);
            const bool b = bruteForce(mesh, ray, 10.0f, cull, slow);
            REQUIRE(a == b);
            if (!a) continue;
            ++hits;
            CHECK(fast.t == Approx(slow.t));
            // Two triangles can share the nearest point exactly (a ray
            // through a shared edge); the distance is what must agree.
            if (fast.triangle != slow.triangle) CHECK(std::abs(fast.t - slow.t) < 1e-5f);
        }
    }
    CHECK(hits > 100);  // the test is not vacuous

    // tMax limits the search.
    const Ray through{bounds.center() + Vec3{0, 0, r * 3}, {0, 0, -1}};
    BvhHit hit;
    if (bvh.intersect(mesh, through, 100.0f, false, hit))
        CHECK_FALSE(bvh.intersect(mesh, through, hit.t * 0.5f, false, hit));
}

TEST_CASE("bvh: degenerate input") {
    Bvh bvh;
    Mesh empty;
    bvh.build(empty);
    CHECK(bvh.empty());
    BvhHit hit;
    CHECK_FALSE(bvh.intersect(empty, {{0, 0, 5}, {0, 0, -1}}, 100.0f, false, hit));
    std::vector<std::uint64_t> bits;
    bvh.markVisible(Frustum::fromMatrix(Mat4::identity()), bits);  // nothing to mark, nothing to crash on

    // Five thousand copies of one triangle: no split separates them, so the
    // builder has to fall back to halving - and must not recurse for ever.
    Mesh stacked;
    for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        Vertex v;
        v.position = p;
        stacked.vertices.push_back(v);
    }
    for (int i = 0; i < 5000; ++i) stacked.indices.insert(stacked.indices.end(), {0, 1, 2});
    bvh.build(stacked);
    CHECK(bvh.triangleCount() == 5000);
    REQUIRE(bvh.intersect(stacked, {{0.2f, 0.2f, 5}, {0, 0, -1}}, 100.0f, false, hit));
    CHECK(hit.t == Approx(5.0f));

    // A single triangle is a tree of one leaf.
    Mesh one = stacked;
    one.indices = {0, 1, 2};
    bvh.build(one);
    CHECK(bvh.nodeCount() == 1);
    CHECK(bvh.intersect(one, {{0.2f, 0.2f, 5}, {0, 0, -1}}, 100.0f, false, hit));
}

TEST_CASE("bvh: culling never drops a triangle the camera could see") {
    const Model model = makeDemoModel("knot");
    const Mesh& mesh = model.meshes[0];
    Bvh bvh;
    bvh.build(mesh);
    const std::size_t triangles = mesh.indices.size() / 3;
    const Bounds bounds = model.bounds();

    std::mt19937 random(11);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::size_t culledTotal = 0;
    for (int view = 0; view < 40; ++view) {
        // Cameras close in, looking at random parts of the model.
        const Vec3 target = bounds.center() + Vec3{unit(random), unit(random), unit(random)} * (bounds.radius() * 0.5f);
        const Vec3 eye = target + normalize(Vec3{unit(random), unit(random), unit(random)}) * (bounds.radius() * 0.45f);
        const Camera camera = lookFrom(eye, target, 0.05f, 20.0f);
        const Mat4 viewProjection = camera.projection * camera.view;

        std::vector<std::uint64_t> visible((triangles + 63) / 64, 0);
        bvh.markVisible(Frustum::fromMatrix(viewProjection), visible);

        for (std::size_t tri = 0; tri < triangles; ++tri) {
            const bool marked = (visible[tri >> 6] >> (tri & 63)) & 1;
            if (marked) continue;
            ++culledTotal;
            // An unmarked triangle must be one the clipper would have
            // rejected anyway: all three corners beyond the same plane.
            unsigned outside = kClipAll;
            for (int k = 0; k < 3; ++k)
                outside &= clipOutcode(
                    viewProjection *
                    Vec4(mesh.vertices[mesh.indices[tri * 3 + static_cast<std::size_t>(k)]].position, 1.0f));
            REQUIRE(outside != 0);
        }
    }
    // ...and from this close, a good share of the model is out of view.
    CHECK(culledTotal > triangles * 40 / 4);
}

// ---------------------------------------------------------------- renderer ---

TEST_CASE("culling: the picture is the same with and without it") {
    const Model model = makeDemoModel("knot");
    const ModelAccel accel(model);
    ThreadPool pool(4);
    Renderer renderer(pool);
    const Bounds bounds = model.bounds();

    const DrawItem plain{&model, Mat4::identity(), nullptr};
    const DrawItem accelerated{&model, Mat4::identity(), &accel};

    struct View {
        Vec3 eye, target;
    };
    const float r = bounds.radius();
    const View views[] = {
        {bounds.center() + Vec3{0, 0, r * 3}, bounds.center()},            // all in view
        {bounds.center() + Vec3{r * 0.6f, 0, r * 0.9f}, bounds.center()},  // close: partly in view
        {bounds.center() + Vec3{0, r * 0.3f, r * 0.5f}, bounds.center() + Vec3{r, 0, 0}},
        {bounds.center() + Vec3{0, 0, r * 3}, bounds.center() + Vec3{r * 30, 0, 0}},  // looking away
    };
    bool culledSomeTriangles = false, culledAMesh = false;
    for (const View& view : views) {
        const Camera camera = lookFrom(view.eye, view.target, 0.02f, 40.0f);
        for (const bool shadows : {false, true}) {
            RenderSettings settings;
            settings.shadows = shadows;
            Framebuffer a(160, 160), b(160, 160);
            const RenderStats without = renderer.render(std::span<const DrawItem>(&plain, 1), camera, settings, a);
            const RenderStats with = renderer.render(std::span<const DrawItem>(&accelerated, 1), camera, settings, b);
            CHECK(a.pixels() == b.pixels());
            CHECK(with.trianglesDrawn == without.trianglesDrawn);
            CHECK(without.trianglesCulled == 0);
            culledSomeTriangles = culledSomeTriangles || (with.trianglesCulled > 0 && with.meshesCulled == 0);
            culledAMesh = culledAMesh || with.meshesCulled > 0;
        }
    }
    CHECK(culledSomeTriangles);
    CHECK(culledAMesh);
}

TEST_CASE("culling: an acceleration structure for a different model is ignored") {
    const Model knot = makeDemoModel("knot"), cube = makeDemoModel("cube");
    Model two = cube;
    two.meshes.push_back(cube.meshes[0]);  // two meshes; the knot has one
    const ModelAccel wrong(knot);
    REQUIRE(wrong.meshCount() != two.meshes.size());

    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(two.bounds());
    Framebuffer a(96, 96), b(96, 96);
    const DrawItem plain{&two, Mat4::identity(), nullptr}, mismatched{&two, Mat4::identity(), &wrong};
    renderer.render(std::span<const DrawItem>(&plain, 1), orbit.camera(1.0f), {}, a);
    renderer.render(std::span<const DrawItem>(&mismatched, 1), orbit.camera(1.0f), {}, b);
    CHECK(a.pixels() == b.pixels());
}

TEST_CASE("highlight: only the selected mesh changes colour") {
    const Model cube = makeDemoModel("cube");
    const Camera camera = lookFrom({0, 2, 12}, {0, 0, 0});
    ThreadPool pool(2);
    Renderer renderer(pool);
    const DrawItem items[] = {{&cube, translate({-2, 0, 0}), nullptr}, {&cube, translate({2, 0, 0}), nullptr}};

    RenderSettings settings;
    Framebuffer plain(128, 128), selected(128, 128);
    renderer.render(items, camera, settings, plain);
    settings.highlightItem = 1;
    settings.highlightMesh = 0;
    renderer.render(items, camera, settings, selected);

    int changedLeft = 0, changedRight = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            if (plain.at(x, y) == selected.at(x, y)) continue;
            (x < 64 ? changedLeft : changedRight)++;
        }
    CHECK(changedLeft == 0);
    CHECK(changedRight > 200);
}

// ----------------------------------------------------------------- picking ---

TEST_CASE("picking: the ray through a pixel") {
    const Camera camera = lookFrom({0, 0, 10}, {0, 0, 0}, 1.0f, 30.0f);
    const Ray centre = cameraRay(camera, 0.0f, 0.0f);
    // Starts on the near plane and ends on the far plane, straight ahead.
    CHECK(centre.origin.z == Approx(9.0f));
    CHECK(centre.origin.x == Approx(0.0f));
    CHECK((centre.origin + centre.direction).z == Approx(-20.0f).epsilon(0.001));
    // The top right of the image is up and to the right.
    const Ray corner = cameraRay(camera, 1.0f, 1.0f);
    CHECK(corner.direction.x > 0.0f);
    CHECK(corner.direction.y > 0.0f);
}

TEST_CASE("picking: what is under the pointer") {
    const Model cube = makeDemoModel("cube");
    const Bounds box = cube.bounds();
    const float half = (box.max.z - box.min.z) * 0.5f;
    const ModelAccel accel(cube);
    const Camera camera = lookFrom({0, 0, 12}, {0, 0, 0});
    PickResult hit;

    SUBCASE("one model") {
        const DrawItem item{&cube, Mat4::identity(), &accel};
        const std::span<const DrawItem> items(&item, 1);
        REQUIRE(pick(items, camera, 0.0f, 0.0f, true, hit));
        CHECK(hit.item == 0);
        CHECK(hit.mesh == 0);
        CHECK(hit.distance == Approx(12.0f - half));
        CHECK(hit.position.z == Approx(half));
        CHECK(hit.position.x == Approx(0.0f).scale(1.0));
        // The corner of the image is empty space.
        CHECK_FALSE(pick(items, camera, 0.95f, 0.95f, true, hit));
    }
    SUBCASE("the result does not depend on having an acceleration structure") {
        const DrawItem fast{&cube, rotate({0, 1, 0}, 0.5f), &accel}, slow{&cube, rotate({0, 1, 0}, 0.5f), nullptr};
        for (const float x : {-0.1f, 0.0f, 0.07f})
            for (const float y : {-0.08f, 0.0f, 0.1f}) {
                PickResult a, b;
                const bool hitA = pick(std::span<const DrawItem>(&fast, 1), camera, x, y, true, a);
                const bool hitB = pick(std::span<const DrawItem>(&slow, 1), camera, x, y, true, b);
                REQUIRE(hitA == hitB);
                if (hitA) CHECK(a.distance == Approx(b.distance));
            }
    }
    SUBCASE("the nearest of several models wins, wherever it is in the list") {
        const DrawItem items[] = {{&cube, translate({0, 0, -5}), &accel},
                                  {&cube, translate({0, 0, 3}), &accel},
                                  {&cube, translate({20, 0, 0}), &accel}};
        REQUIRE(pick(items, camera, 0.0f, 0.0f, true, hit));
        CHECK(hit.item == 1);
        CHECK(hit.position.z == Approx(3.0f + half));
    }
    SUBCASE("transforms: scaled, and mirrored") {
        const DrawItem big{&cube, scale({3, 3, 3}), &accel};
        REQUIRE(pick(std::span<const DrawItem>(&big, 1), camera, 0.0f, 0.0f, true, hit));
        CHECK(hit.distance == Approx(12.0f - 3.0f * half));
        // Mirroring turns the cube inside out; its faces must still be solid.
        const DrawItem mirrored{&cube, scale({-1, 1, 1}), &accel};
        REQUIRE(pick(std::span<const DrawItem>(&mirrored, 1), camera, 0.0f, 0.0f, true, hit));
        CHECK(hit.distance == Approx(12.0f - half));
    }
    SUBCASE("back faces are passed through only when culling is on") {
        // From inside the cube every face is a back face.
        const Camera inside = lookFrom({0, 0, 0}, {0, 0, -5}, 0.01f, 30.0f);
        const DrawItem item{&cube, Mat4::identity(), &accel};
        CHECK_FALSE(pick(std::span<const DrawItem>(&item, 1), inside, 0.0f, 0.0f, true, hit));
        REQUIRE(pick(std::span<const DrawItem>(&item, 1), inside, 0.0f, 0.0f, false, hit));
        CHECK(hit.distance == Approx(half));
    }
    SUBCASE("an empty list, and items without a model") {
        CHECK_FALSE(pick(std::span<const DrawItem>(), camera, 0.0f, 0.0f, true, hit));
        const DrawItem nothing{};
        CHECK_FALSE(pick(std::span<const DrawItem>(&nothing, 1), camera, 0.0f, 0.0f, true, hit));
    }
}

TEST_CASE("model accel: follows vertices that move") {
    Model cube = makeDemoModel("cube");
    ModelAccel accel(cube);
    REQUIRE(accel.meshCount() == 1);
    REQUIRE(accel.bvh(0) != nullptr);
    const Camera camera = lookFrom({0, 0, 12}, {0, 0, 0});
    const DrawItem item{&cube, Mat4::identity(), &accel};

    // Move every vertex far to the right, as an animation frame might.
    for (Vertex& v : cube.meshes[0].vertices) v.position.x += 40.0f;
    accel.refresh(cube);
    CHECK(accel.bounds(0).min.x > 30.0f);
    CHECK(accel.bvh(0) == nullptr);  // out of date until asked for

    // Picking still gives the right answer while the tree is out of date...
    PickResult hit;
    CHECK_FALSE(pick(std::span<const DrawItem>(&item, 1), camera, 0.0f, 0.0f, true, hit));
    const Camera moved = lookFrom({40, 0, 12}, {40, 0, 0});
    CHECK(pick(std::span<const DrawItem>(&item, 1), moved, 0.0f, 0.0f, true, hit));

    // ...and after it has been rebuilt.
    accel.updateTrees(cube);
    REQUIRE(accel.bvh(0) != nullptr);
    CHECK(accel.bvh(0)->bounds().min.x > 30.0f);
    CHECK(pick(std::span<const DrawItem>(&item, 1), moved, 0.0f, 0.0f, true, hit));

    // The renderer culls by the refreshed boxes: nothing is drawn when the
    // camera looks at where the cube used to be.
    ThreadPool pool(2);
    Renderer renderer(pool);
    Framebuffer fb(64, 64);
    const RenderStats stats = renderer.render(std::span<const DrawItem>(&item, 1), camera, {}, fb);
    CHECK(stats.meshesCulled == 1);
    CHECK(stats.trianglesDrawn == 0);
}
