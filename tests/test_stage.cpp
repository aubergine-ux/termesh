// JSON, camera paths, scenes and scene files.
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "app/options.hpp"
#include "core/json.hpp"
#include "scene/primitives.hpp"
#include "stage/camera_path.hpp"
#include "stage/scene.hpp"
#include "stage/scene_file.hpp"
#include "support.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

void checkVec(Vec3 actual, Vec3 expected, float epsilon = 1e-3f) {
    CHECK(actual.x == Approx(expected.x).scale(1.0).epsilon(epsilon));
    CHECK(actual.y == Approx(expected.y).scale(1.0).epsilon(epsilon));
    CHECK(actual.z == Approx(expected.z).scale(1.0).epsilon(epsilon));
}

Json parsed(const std::string& text) {
    Json json;
    std::string error;
    REQUIRE_MESSAGE(Json::parse(text, json, error), error);
    return json;
}

std::string parseError(const std::string& text) {
    Json json;
    std::string error;
    CHECK_FALSE(Json::parse(text, json, error));
    return error;
}

}  // namespace

// -------------------------------------------------------------------- JSON ---

TEST_CASE("json: values of every type") {
    const Json json = parsed(R"({"n": null, "t": true, "f": false, "i": 42, "x": -1.5e2, "s": "hi",
                                 "a": [1, "two", [3]], "o": {"k": "v"}})");
    REQUIRE(json.isObject());
    CHECK(json["n"].isNull());
    CHECK(json["t"].boolean() == true);
    CHECK(json["f"].isBool());
    CHECK(json["f"].boolean(true) == false);
    CHECK(json["i"].number() == 42.0);
    CHECK(json["x"].number() == -150.0);
    CHECK(json["s"].string() == "hi");
    REQUIRE(json["a"].isArray());
    CHECK(json["a"].size() == 3);
    CHECK(json["a"][1].string() == "two");
    CHECK(json["a"][2][0].number() == 3.0);
    CHECK(json["o"]["k"].string() == "v");
    // Members keep the order of the file.
    CHECK(json.members()[0].first == "n");
    CHECK(json.members()[7].first == "o");

    // Missing things are null, and asking a value for the wrong type gives
    // the fallback - so lookups chain without checks.
    CHECK(json["nope"].isNull());
    CHECK(json["nope"]["deeper"][3]["still"].isNull());
    CHECK(json["s"].number(7.0) == 7.0);
    CHECK(json["i"].string("fallback") == "fallback");
    CHECK(json["a"][99].isNull());
    CHECK(json.has("i"));
    CHECK_FALSE(json.has("n"));  // present but null counts as absent

    // Scalars at the top level are documents too.
    CHECK(parsed("3.25").number() == 3.25);
    CHECK(parsed("  \"x\"  ").string() == "x");
    CHECK(parsed("[]").size() == 0);
    CHECK(parsed("{}").members().empty());
}

TEST_CASE("json: strings and escapes") {
    CHECK(parsed(R"("a\"b\\c\/d\n\t")").string() == "a\"b\\c/d\n\t");
    CHECK(parsed(R"("\u0041\u00e9\u20ac")").string() == "A\xc3\xa9\xe2\x82\xac");  // A, e-acute, euro sign
    CHECK(parsed(R"("\ud83d\ude00")").string() == "\xf0\x9f\x98\x80");             // a surrogate pair
    CHECK(parsed(R"("\ud83d")").string() == "\xef\xbf\xbd");                       // half of one: replaced
    CHECK(parsed("\"caf\xc3\xa9\"").string() == "caf\xc3\xa9");                    // raw UTF-8 passes through
}

TEST_CASE("json: comments and trailing commas are allowed") {
    const Json json =
        parsed("// a scene\n{\n  \"a\": [1, 2, 3,],  // three numbers\n  \"b\": {\"c\": 1,},\n}\n// the end\n");
    CHECK(json["a"].size() == 3);
    CHECK(json["b"]["c"].number() == 1.0);
    // A double slash inside a string is not a comment.
    CHECK(parsed(R"({"url": "http://example"})")["url"].string() == "http://example");
}

TEST_CASE("json: errors say where") {
    CHECK(parseError("{\n  \"a\": 1\n  \"b\": 2\n}").find("line 3") != std::string::npos);
    CHECK(parseError("{\"a\": }").find("expected a value") != std::string::npos);
    CHECK(parseError("[1, 2").find("end of file") != std::string::npos);
    CHECK(parseError("\"abc").find("unterminated") != std::string::npos);
    CHECK(parseError("{\"a\": 1} extra").find("after the end") != std::string::npos);
    CHECK_FALSE(parseError("").empty());
    CHECK_FALSE(parseError("{a: 1}").empty());  // names need quotes
    CHECK_FALSE(parseError("01x").empty());
    CHECK_FALSE(parseError("1.").empty());
    CHECK_FALSE(parseError("-").empty());
    CHECK_FALSE(parseError("\"\\q\"").empty());  // unknown escape
    CHECK_FALSE(parseError("\"\\u12\"").empty());
    CHECK_FALSE(parseError("tru").empty());
    CHECK_FALSE(parseError("[1 2]").empty());

    // Nesting deep enough to overflow the stack is refused instead.
    CHECK(parseError(std::string(5000, '[')).find("nested too deeply") != std::string::npos);
    CHECK(parsed(std::string(40, '[') + std::string(40, ']')).isArray());
}

// ------------------------------------------------------------- camera path ---

namespace {

CameraPath squarePath(bool loop, bool smooth) {
    CameraPath path;
    path.loop = loop;
    path.smooth = smooth;
    const Vec3 corners[4] = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}};
    for (int i = 0; i < 4; ++i) {
        CameraKey key;
        key.time = static_cast<float>(i) * 2.0f;
        key.pose.position = corners[i];
        key.pose.target = {5, 0, 5};
        key.pose.fov = radians(40.0f + 10.0f * static_cast<float>(i));
        path.keys.push_back(key);
    }
    path.finish();
    return path;
}

}  // namespace

TEST_CASE("camera path: passes through its keys, whichever interpolation") {
    for (const bool smooth : {false, true})
        for (const bool loop : {false, true}) {
            const CameraPath path = squarePath(loop, smooth);
            for (const CameraKey& key : path.keys) {
                const CameraPose pose = path.evaluate(key.time);
                checkVec(pose.position, key.pose.position);
                CHECK(pose.fov == Approx(key.pose.fov));
            }
        }
}

TEST_CASE("camera path: linear moves in straight lines, smooth rounds the corners") {
    const CameraPath linear = squarePath(false, false), smooth = squarePath(false, true);
    checkVec(linear.evaluate(1.0f).position, {5, 0, 0});
    checkVec(linear.evaluate(3.0f).position, {10, 0, 5});
    CHECK(linear.evaluate(1.0f).fov == Approx(radians(45.0f)));

    // The curve leaves the straight line between two keys, bulging outwards
    // as it sets up for the turn ahead.
    const Vec3 mid = smooth.evaluate(3.0f).position;
    CHECK(mid.x > 10.0f);
    CHECK(mid.z == Approx(5.0f).epsilon(0.05));

    // No jumps: a small step in time is a small step in space, everywhere.
    Vec3 previous = smooth.evaluate(0.0f).position;
    for (int i = 1; i <= 600; ++i) {
        const Vec3 p = smooth.evaluate(6.0f * static_cast<float>(i) / 600.0f).position;
        REQUIRE(length(p - previous) < 0.2f);
        previous = p;
    }
}

TEST_CASE("camera path: looping and the ends") {
    // Not looping: time is clamped to the path.
    const CameraPath open = squarePath(false, true);
    CHECK(open.duration == Approx(6.0f));
    checkVec(open.evaluate(-5.0f).position, {0, 0, 0});
    checkVec(open.evaluate(100.0f).position, {0, 0, 10});

    // Looping: one more leg, back to the start, and then round again.
    const CameraPath loop = squarePath(true, false);
    CHECK(loop.duration == Approx(8.0f));
    checkVec(loop.evaluate(7.0f).position, {0, 0, 5});  // half-way along the leg home
    checkVec(loop.evaluate(8.0f).position, {0, 0, 0});
    checkVec(loop.evaluate(9.0f).position, {5, 0, 0});
    checkVec(loop.evaluate(8.0f * 5.0f + 3.0f).position, {10, 0, 5});
    checkVec(loop.evaluate(-1.0f).position, {0, 0, 5});

    // A smooth loop is smooth across the join as well.
    const CameraPath round = squarePath(true, true);
    CHECK(length(round.evaluate(7.99f).position - round.evaluate(8.01f).position) < 0.2f);

    // An explicit duration is kept if it is long enough.
    CameraPath slow = squarePath(true, false);
    slow.duration = 20.0f;
    slow.finish();
    CHECK(slow.duration == Approx(20.0f));
    checkVec(slow.evaluate(13.0f).position, {0, 0, 5});
}

TEST_CASE("camera path: degenerate paths") {
    CameraPath empty;
    empty.finish();
    CHECK(empty.empty());
    CHECK(empty.evaluate(3.0f).fov == Approx(radians(40.0f)));  // the default pose

    CameraPath one;
    one.keys.push_back({5.0f, {{1, 2, 3}, {0, 0, 0}, radians(60.0f)}});
    one.loop = true;
    one.finish();
    checkVec(one.evaluate(0.0f).position, {1, 2, 3});
    checkVec(one.evaluate(99.0f).position, {1, 2, 3});

    // Keys given out of order are sorted.
    CameraPath shuffled;
    shuffled.smooth = false;
    shuffled.keys.push_back({4.0f, {{4, 0, 0}, {}, radians(40.0f)}});
    shuffled.keys.push_back({0.0f, {{0, 0, 0}, {}, radians(40.0f)}});
    shuffled.finish();
    checkVec(shuffled.evaluate(1.0f).position, {1, 0, 0});

    // A spline can overshoot; the field of view stays usable.
    CameraPath wide;
    for (int i = 0; i < 4; ++i)
        wide.keys.push_back({static_cast<float>(i), {{}, {0, 0, -1}, radians(i % 2 ? 150.0f : 5.0f)}});
    wide.finish();
    for (int i = 0; i <= 30; ++i) {
        const float fov = wide.evaluate(static_cast<float>(i) * 0.1f).fov;
        CHECK(fov >= radians(5.0f) - 1e-4f);
        CHECK(fov <= radians(150.0f) + 1e-4f);
    }
}

TEST_CASE("camera path: turntable") {
    const Bounds bounds{{-1, 0, -1}, {3, 2, 1}};
    const float elevation = radians(20.0f);
    const CameraPath path = makeTurntable(bounds, 10.0f, elevation, 1.0f, radians(40.0f));
    REQUIRE(path.loop);
    CHECK(path.duration == Approx(10.0f));

    const Vec3 centre = bounds.center();
    const float radius = length(path.keys[0].pose.position - centre);
    CHECK(radius > bounds.radius());  // outside the scene
    for (int i = 0; i <= 100; ++i) {
        const CameraPose pose = path.evaluate(static_cast<float>(i) * 0.1f);
        // Always looking at the centre, from the same distance and height.
        checkVec(pose.target, centre);
        CHECK(length(pose.position - centre) == Approx(radius).epsilon(0.01));
        CHECK(pose.position.y - centre.y == Approx(radius * std::sin(elevation)).epsilon(0.02));
    }
    // Half-way round is the opposite side.
    const Vec3 start = path.evaluate(0.0f).position - centre, half = path.evaluate(5.0f).position - centre;
    CHECK(start.x == Approx(-half.x).scale(1.0).epsilon(0.02));
    CHECK(start.z == Approx(-half.z).epsilon(0.02));

    // Twice the distance is twice as far away.
    const CameraPath further = makeTurntable(bounds, 10.0f, elevation, 2.0f, radians(40.0f));
    CHECK(length(further.keys[0].pose.position - centre) == Approx(radius * 2.0f));
}

TEST_CASE("camera path: from a pose to matrices and to an orbit camera") {
    const Bounds bounds{{-1, -1, -1}, {1, 1, 1}};
    CameraPose pose;
    pose.position = {3, 2, 4};
    pose.target = {0.5f, 0, 0};
    pose.fov = radians(50.0f);

    const Camera camera = cameraFromPose(pose, 1.5f, bounds);
    checkVec(camera.position, pose.position);
    // The target is dead centre, in front of the camera, inside the depth range.
    const Vec4 clip = camera.projection * camera.view * Vec4(pose.target, 1.0f);
    CHECK(clip.x / clip.w == Approx(0.0f).scale(1.0));
    CHECK(clip.y / clip.w == Approx(0.0f).scale(1.0));
    CHECK(clip.w > 0.0f);
    CHECK(std::abs(clip.z / clip.w) < 1.0f);
    // Every corner of the scene is between the near and far planes.
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, i & 4 ? 1.0f : -1.0f};
        const Vec4 c = camera.projection * camera.view * Vec4(corner, 1.0f);
        CHECK(std::abs(c.z) <= c.w);
    }

    // The orbit camera stands in the same place, looking the same way.
    const OrbitCamera orbit = orbitFromPose(pose, bounds);
    checkVec(orbit.position(), pose.position);
    checkVec(orbit.target, pose.target);
    CHECK(orbit.fovY == Approx(pose.fov));
    const Camera same = orbit.camera(1.5f);
    const Vec4 again = same.projection * same.view * Vec4(pose.target, 1.0f);
    CHECK(again.x / again.w == Approx(0.0f).scale(1.0));

    // Straight down is a legitimate direction for a path, though not for an
    // orbit camera, which stops short of the pole.
    pose.position = {0, 10, 0};
    pose.target = {0, 0, 0};
    const Camera down = cameraFromPose(pose, 1.0f, bounds);
    const Vec4 below = down.projection * down.view * Vec4(0, 0, 0, 1);
    CHECK(below.w > 0.0f);
    CHECK(below.x / below.w == Approx(0.0f).scale(1.0));
    CHECK(orbitFromPose(pose, bounds).pitch <= radians(89.0f) + 1e-4f);
}

// ------------------------------------------------------------------- scene ---

TEST_CASE("scene: objects, draw items and bounds") {
    Scene scene;
    CHECK(scene.drawItems().empty());
    CHECK(scene.bounds().radius() == 0.0f);
    CHECK_FALSE(scene.animated());

    SceneObject& cube = scene.add(makeDemoModel("cube"), "cube", translate({10, 0, 0}));
    SceneObject& knot = scene.add(makeDemoModel("knot"), "knot");
    SceneObject& worm = scene.add(makeDemoModel("tentacle"), "worm", translate({0, 0, -20}));
    REQUIRE(scene.objects.size() == 3);
    // References handed out by add() stay valid as the scene grows.
    CHECK(&cube == scene.objects[0].get());
    CHECK(cube.accel.meshCount() == cube.model.meshes.size());
    CHECK(cube.player == nullptr);
    REQUIRE(worm.player != nullptr);
    CHECK(scene.animated());
    CHECK(scene.playing());
    CHECK(scene.triangleCount() ==
          cube.model.triangleCount() + knot.model.triangleCount() + worm.model.triangleCount());

    std::vector<DrawItem> items = scene.drawItems();
    REQUIRE(items.size() == 3);
    CHECK(items[0].model == &cube.model);
    CHECK(items[0].accel == &cube.accel);
    CHECK(items[0].transform == translate({10, 0, 0}));

    // The box covers every object where it actually stands.
    const Bounds all = scene.bounds();
    CHECK(all.max.x > 10.0f);
    CHECK(all.min.z < -19.0f);

    // Hidden objects are neither drawn nor measured, and item numbers skip them.
    knot.visible = false;
    items = scene.drawItems();
    REQUIRE(items.size() == 2);
    CHECK(items[1].model == &worm.model);
    CHECK(scene.objectOfItem(0) == 0);
    CHECK(scene.objectOfItem(1) == 2);
    CHECK(scene.objectOfItem(2) == -1);
    CHECK(scene.itemOfObject(2) == 1);
    CHECK(scene.itemOfObject(1) == -1);
    CHECK(scene.itemOfObject(7) == -1);
}

TEST_CASE("scene: update moves the animated models and their boxes") {
    Scene scene;
    scene.add(makeDemoModel("cube"), "cube");
    SceneObject& worm = scene.add(makeDemoModel("tentacle"), "worm");
    const Vec3 tipBefore = worm.model.meshes[0].vertices.back().position;
    const Bounds boxBefore = worm.accel.bounds(0);

    CHECK(scene.update(0.6f));
    const Vec3 tipAfter = worm.model.meshes[0].vertices.back().position;
    CHECK(length(tipAfter - tipBefore) > 0.05f);
    CHECK(length(worm.accel.bounds(0).max - boxBefore.max) > 0.01f);
    CHECK(worm.player->time() == Approx(0.6f));

    // Paused: nothing moves, and update says so.
    worm.player->pause();
    CHECK_FALSE(scene.update(0.5f));
    CHECK_FALSE(scene.playing());

    // pose() applies a seek straight away, paused or not.
    worm.player->seek(0.0f);
    scene.pose();
    CHECK(length(worm.model.meshes[0].vertices.back().position - tipAfter) > 0.05f);
    worm.player->seek(0.6f);
    scene.pose();
    checkVec(worm.model.meshes[0].vertices.back().position, tipAfter, 1e-3f);
}

TEST_CASE("scene: arranging unrelated models in a row") {
    Scene scene;
    // Wildly different sizes and positions, as files from different sources are.
    Model tiny = makeDemoModel("cube");
    for (Vertex& v : tiny.meshes[0].vertices) v.position = v.position * 0.01f + Vec3{500, 500, 500};
    scene.add(std::move(tiny), "tiny");
    scene.add(makeDemoModel("knot"), "knot");
    scene.add(makeDemoModel("sphere"), "sphere");
    scene.arrangeInRow();

    float previousRight = -1e9f;
    for (const auto& object : scene.objects) {
        const Bounds box = transformBounds(object->model.bounds(), object->transform);
        // The same size, standing on the ground, centred on the row's line...
        CHECK(box.radius() == Approx(1.0f).epsilon(0.01));
        CHECK(box.min.y == Approx(0.0f).scale(1.0).epsilon(0.01));
        CHECK(box.center().z == Approx(0.0f).scale(1.0).epsilon(0.01));
        // ...and not overlapping its neighbour.
        CHECK(box.min.x > previousRight);
        previousRight = box.max.x;
    }
    // The row as a whole is centred.
    CHECK(scene.bounds().center().x == Approx(0.0f).scale(1.0).epsilon(0.02));
}

// -------------------------------------------------------------- scene file ---

TEST_CASE("scene file: a complete scene") {
    const SceneLoadResult loaded = loadSceneText(R"({
        "name": "test",
        "models": [
            {"demo": "cube", "name": "box", "position": [1, 2, 3], "rotation": [0, 90, 0], "scale": 2},
            {"demo": "sphere", "scale": [1, 2, 1], "visible": false,
             "material": {"color": "#ff0000", "metallic": 1, "roughness": 0.2, "opacity": 0.5}},
            {"demo": "tentacle", "animation": "curl", "animationSpeed": 2, "animationTime": 0.5, "paused": true}
        ],
        "lights": [
            {"type": "directional", "direction": [0, -2, 0], "color": [1, 0.5, 0.25], "intensity": 3, "shadows": true},
            {"type": "spot", "position": [0, 5, 0], "target": [0, 0, 5], "angle": 40, "blend": 0.25, "range": 20},
            {"type": "point", "position": [1, 1, 1], "specular": 0}
        ],
        "environment": {"name": "sky", "intensity": 2, "rotation": 90, "skybox": false, "blur": 0.4},
        "camera": {"position": [0, 1, 10], "target": [0, 1, 0], "fov": 55},
        "render": {"shading": "pbr", "shadows": true}
    })",
                                                 "");
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    CHECK(loaded.warnings.empty());
    const Scene& scene = loaded.scene;
    CHECK(scene.name == "test");
    REQUIRE(scene.objects.size() == 3);

    // Placement: scale, then turn, then move. The cube's +x face ends up
    // facing -z after a quarter turn about y.
    const SceneObject& box = *scene.objects[0];
    CHECK(box.name == "box");
    CHECK(box.source == "demo:cube");
    checkVec(transformPoint(box.transform, {0, 0, 0}), {1, 2, 3});
    checkVec(transformPoint(box.transform, {1, 0, 0}), {1, 2, 1});
    checkVec(transformPoint(box.transform, {0, 1, 0}), {1, 4, 3});

    const SceneObject& ball = *scene.objects[1];
    CHECK(ball.name == "sphere");  // named after its model when no name is given
    CHECK_FALSE(ball.visible);
    checkVec(transformPoint(ball.transform, {1, 1, 1}), {1, 2, 1});
    const Material& red = ball.model.materials[0];
    CHECK(red.baseColor.x == Approx(1.0f));
    CHECK(red.baseColor.y == Approx(0.0f));
    CHECK(red.baseColor.w == Approx(0.5f));
    CHECK(red.alphaMode == AlphaMode::Blend);
    CHECK(red.metallic == 1.0f);
    CHECK(red.roughness == Approx(0.2f));
    CHECK(red.baseColorTexture == -1);  // the colour replaced the texture

    const SceneObject& worm = *scene.objects[2];
    REQUIRE(worm.player);
    CHECK(worm.player->clipName(worm.player->clip()) == "curl");
    CHECK(worm.player->speed() == 2.0f);
    CHECK(worm.player->time() == Approx(0.5f));
    CHECK_FALSE(worm.player->playing());

    REQUIRE(scene.lights.size() == 3);
    CHECK(scene.lights[0].type == LightType::Directional);
    CHECK(scene.lights[0].castsShadow);
    CHECK(scene.lights[0].intensity == 3.0f);
    CHECK(scene.lights[0].color.y == Approx(0.5f));
    CHECK(scene.lights[1].type == LightType::Spot);
    checkVec(scene.lights[1].direction, {0, -5, 5});  // from "target"
    CHECK(scene.lights[1].outerCone == Approx(radians(40.0f)));
    CHECK(scene.lights[1].innerCone == Approx(radians(30.0f)));
    CHECK(scene.lights[1].range == 20.0f);
    CHECK(scene.lights[2].type == LightType::Point);
    CHECK(scene.lights[2].specular == 0.0f);

    CHECK(scene.hasEnvironment);
    CHECK(scene.environment.name == "sky");
    CHECK(scene.environment.intensity == 2.0f);
    CHECK(scene.environment.rotation == Approx(radians(90.0f)));
    REQUIRE(scene.skybox.has_value());
    CHECK_FALSE(*scene.skybox);
    CHECK(scene.skyBlur == Approx(0.4f));

    REQUIRE(scene.camera.has_value());
    checkVec(scene.camera->position, {0, 1, 10});
    CHECK(scene.camera->fov == Approx(radians(55.0f)));
    CHECK(scene.cameraPath.empty());
    CHECK(scene.renderSettings["shading"].string() == "pbr");
}

TEST_CASE("scene file: camera turntables and paths") {
    const SceneLoadResult turntable = loadSceneText(
        R"({"models": [{"demo": "cube"}], "camera": {"fov": 30, "turntable": {"seconds": 6, "elevation": 30}}})", "");
    REQUIRE_MESSAGE(turntable.ok, turntable.error);
    CHECK_FALSE(turntable.scene.camera.has_value());
    REQUIRE_FALSE(turntable.scene.cameraPath.empty());
    CHECK(turntable.scene.cameraPath.loop);
    CHECK(turntable.scene.cameraPath.duration == Approx(6.0f));
    CHECK(turntable.scene.cameraPath.keys[0].pose.fov == Approx(radians(30.0f)));

    const SceneLoadResult path = loadSceneText(R"({"models": [{"demo": "cube"}], "camera": {"path": {
        "loop": true, "interpolation": "linear",
        "keys": [{"time": 0, "position": [0, 0, 5], "target": [0, 0, 0], "fov": 40},
                 {"time": 2, "position": [5, 0, 0]},
                 {"position": [0, 0, -5], "fov": 60}]}}})",
                                               "");
    REQUIRE_MESSAGE(path.ok, path.error);
    const CameraPath& p = path.scene.cameraPath;
    REQUIRE(p.keys.size() == 3);
    CHECK(p.loop);
    CHECK_FALSE(p.smooth);
    // A key inherits what it leaves out from the one before, and with no
    // time comes one second later.
    checkVec(p.keys[1].pose.target, {0, 0, 0});
    CHECK(p.keys[1].pose.fov == Approx(radians(40.0f)));
    CHECK(p.keys[2].time == Approx(3.0f));
    CHECK(p.keys[2].pose.fov == Approx(radians(60.0f)));
    checkVec(p.evaluate(1.0f).position, {2.5f, 0, 2.5f});
}

TEST_CASE("scene file: models from files, relative to the scene") {
    TempDir dir;
    dir.write("tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const std::string scenePath =
        dir.write("scene.json",
                  R"({"models": [{"file": "tri.obj"}, {"file": "tri.obj", "name": "second", "position": [5, 0, 0]}]})");

    const SceneLoadResult loaded = loadSceneFile(scenePath);
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    CHECK(loaded.scene.name == "scene.json");  // unnamed scenes take the file's name
    REQUIRE(loaded.scene.objects.size() == 2);
    CHECK(loaded.scene.objects[0]->name == "tri.obj");
    CHECK(loaded.scene.objects[1]->name == "second");
    CHECK(loaded.scene.objects[0]->model.triangleCount() == 1);
    CHECK(loaded.scene.bounds().max.x == Approx(6.0f));

    const SceneLoadResult missing = loadSceneFile(dir.file("nope.json"));
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("nope.json") != std::string::npos);

    const SceneLoadResult badModel = loadSceneFile(dir.write("bad.json", R"({"models": [{"file": "gone.obj"}]})"));
    CHECK_FALSE(badModel.ok);
    CHECK(badModel.error.find("models[0].file") != std::string::npos);
    CHECK(badModel.error.find("bad.json") != std::string::npos);
}

TEST_CASE("scene file: mistakes are reported with the path to them") {
    auto error = [](const std::string& text) {
        const SceneLoadResult result = loadSceneText(text, "");
        CHECK_FALSE(result.ok);
        return result.error;
    };
    auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };

    CHECK(has(error("{"), "line 1"));
    CHECK(has(error("[]"), "top level"));
    CHECK(has(error("{}"), "models"));
    CHECK(has(error(R"({"models": []})"), "at least one model"));
    CHECK(has(error(R"({"models": [{}]})"), "models[0]"));
    CHECK(has(error(R"({"models": [{"demo": "teapot"}]})"), "teapot"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}, {"demo": "cube", "position": [1, 2]}]})"), "models[1].position"));
    CHECK(has(error(R"({"models": [{"demo": "cube", "scale": "big"}]})"), "models[0].scale"));
    CHECK(has(error(R"({"models": [{"demo": "cube", "material": {"color": "red"}}]})"), "models[0].material.color"));
    CHECK(has(error(R"({"models": [{"demo": "tentacle", "animation": "dance"}]})"), "dance"));
    CHECK(has(error(R"({"models": [{"demo": "tentacle", "animation": 9}]})"), "models[0].animation"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "lights": [{"type": "laser"}]})"), "lights[0].type"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "lights": [{"direction": [0, 0, 0]}]})"), "lights[0]"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "lights": [{"intensity": -1}]})"), "lights[0]"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "environment": {"name": "mars"}})"), "mars"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "environment": {"file": "no.hdr"}})"), "environment.file"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "camera": {"fov": 400}})"), "camera.fov"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "camera": {"turntable": {}, "path": {"keys": [{}]}}})"), "both"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "camera": {"path": {"keys": []}}})"), "camera.path.keys"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "camera": {"path": {"keys": [{"time": 2}, {"time": 1}]}}})"),
              "keys[1].time"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "camera": {"turntable": {"seconds": 0}}})"), "seconds"));
    CHECK(has(error(R"({"models": [{"demo": "cube"}], "render": 5})"), "render"));

    // Things that are odd but harmless are warnings, not errors.
    const SceneLoadResult odd =
        loadSceneText(R"({"models": [{"demo": "cube", "animation": "spin"}], "lightz": []})", "");
    REQUIRE_MESSAGE(odd.ok, odd.error);
    REQUIRE(odd.warnings.size() == 2);
    CHECK(has(odd.warnings[0], "no animations"));
    CHECK(has(odd.warnings[1], "lightz"));
}

TEST_CASE("scene file: colours") {
    Vec3 c;
    Json hex, list, bad;
    std::string error;
    REQUIRE(Json::parse("\"#ff8000\"", hex, error));
    REQUIRE(parseSceneColor(hex, c));
    CHECK(c.x == Approx(1.0f));
    CHECK(c.y == Approx(0.2158f).epsilon(0.01));  // sRGB 128 is about 0.22 in linear light
    CHECK(c.z == 0.0f);
    REQUIRE(Json::parse("[0.25, 0.5, 2]", list, error));
    REQUIRE(parseSceneColor(list, c));
    CHECK(c == Vec3{0.25f, 0.5f, 2.0f});  // given directly in linear light; above 1 is allowed
    for (const char* text : {"\"#ff80\"", "\"#gggggg\"", "\"red\"", "[1, 2]", "[1, -1, 0]", "5", "null"}) {
        REQUIRE(Json::parse(text, bad, error));
        CHECK_FALSE(parseSceneColor(bad, c));
    }
}

TEST_CASE("scene file: the examples that ship with termesh load") {
    for (const char* name : {"showroom.json", "flythrough.json"}) {
        const SceneLoadResult loaded = loadSceneFile(std::string(TERMESH_SOURCE_DIR) + "/examples/" + name);
        CAPTURE(name);
        REQUIRE_MESSAGE(loaded.ok, loaded.error);
        CHECK(loaded.warnings.empty());
        CHECK(loaded.scene.objects.size() >= 3);
        CHECK_FALSE(loaded.scene.lights.empty());
        CHECK_FALSE(loaded.scene.cameraPath.empty());
        Options options;
        std::string error;
        CHECK_MESSAGE(applySceneRenderSettings(loaded.scene.renderSettings, options, error), error);
    }
}

// ----------------------------------------------------- scenes and options ---

TEST_CASE("options: a scene's render settings sit underneath the command line") {
    Json render;
    std::string error;
    REQUIRE(Json::parse(R"({"shading": "toon", "shadows": true, "ssao": true, "outline": false, "exposure": 1.5,
                            "background": "#204060", "tonemap": "filmic", "shadowSoftness": 5, "lod": 2,
                            "wireframe": true, "dither": "ordered", "fov": 60})",
                        render, error));
    Options fromScene;
    REQUIRE_MESSAGE(applySceneRenderSettings(render, fromScene, error), error);
    CHECK(fromScene.shading == ShadingMode::Toon);
    CHECK(fromScene.shadows);
    CHECK(fromScene.ssao);
    REQUIRE(fromScene.outline.has_value());
    CHECK_FALSE(*fromScene.outline);
    CHECK(fromScene.exposure == Approx(1.5f));
    CHECK(fromScene.background == Color8{0x20, 0x40, 0x60, 255});
    CHECK(fromScene.toneMapper == ToneMapper::Filmic);
    CHECK(fromScene.shadowSoftness == 5);
    CHECK(*fromScene.lodTolerance == 2.0f);
    CHECK(fromScene.wireframe);
    CHECK(fromScene.dither == DitherMode::Ordered);
    CHECK(fromScene.fov == 60.0f);

    // The command line is read on top: what it says wins, what it does not
    // mention keeps the scene's value.
    const char* argv[] = {"termesh", "scene.json", "--shading", "pbr", "--exposure", "0"};
    const ParseResult parsed = parseOptions(6, argv, fromScene);
    REQUIRE(parsed.ok);
    CHECK(parsed.options.shading == ShadingMode::Pbr);
    CHECK(parsed.options.exposure == 0.0f);
    CHECK(parsed.options.shadows);
    CHECK(parsed.options.toneMapper == ToneMapper::Filmic);
    CHECK(parsed.options.inputs == std::vector<std::string>{"scene.json"});

    // Nothing to apply is fine; nonsense is not.
    Options untouched;
    CHECK(applySceneRenderSettings(Json(), untouched, error));
    for (const char* text : {R"({"shading": "plaid"})", R"({"shadows": "yes"})", R"({"exposure": 99})",
                             R"({"background": "blue"})", R"({"sparkle": true})", R"({"shadowSize": 3})"}) {
        REQUIRE(Json::parse(text, render, error));
        Options o;
        CAPTURE(text);
        CHECK_FALSE(applySceneRenderSettings(render, o, error));
        CHECK(error.find("render.") == 0);
    }
}

TEST_CASE("options: several models, or one scene") {
    const char* two[] = {"termesh", "a.glb", "b.obj"};
    const ParseResult models = parseOptions(3, two);
    REQUIRE(models.ok);
    CHECK(models.options.inputs == std::vector<std::string>{"a.glb", "b.obj"});
    CHECK(models.options.modelPath == "a.glb");

    CHECK(isSceneFile("scene.json"));
    CHECK(isSceneFile("dir/My Scene.JSON"));
    CHECK_FALSE(isSceneFile("model.glb"));
    CHECK_FALSE(isSceneFile("json"));

    const char* mixed[] = {"termesh", "scene.json", "extra.glb"};
    CHECK_FALSE(parseOptions(3, mixed).ok);
    const char* withDemo[] = {"termesh", "a.glb", "--demo", "knot"};
    CHECK_FALSE(parseOptions(4, withDemo).ok);

    const char* path[] = {"termesh", "scene.json", "--camera-time", "2.5", "--no-path"};
    const ParseResult camera = parseOptions(5, path);
    REQUIRE(camera.ok);
    CHECK(camera.options.cameraTime == Approx(2.5f));
    CHECK_FALSE(camera.options.followPath);
    const char* negative[] = {"termesh", "scene.json", "--camera-time", "-1"};
    CHECK_FALSE(parseOptions(4, negative).ok);
}
