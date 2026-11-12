#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "anim/player.hpp"
#include "anim/pose.hpp"
#include "anim/skinning.hpp"
#include "app/options.hpp"
#include "scene/loader.hpp"
#include "support.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

void checkVec(Vec3 actual, Vec3 expected, float epsilon = 1e-4f) {
    CHECK(actual.x == Approx(expected.x).epsilon(epsilon));
    CHECK(actual.y == Approx(expected.y).epsilon(epsilon));
    CHECK(actual.z == Approx(expected.z).epsilon(epsilon));
}

AnimationChannel makeChannel(AnimationPath path, Interpolation interpolation, int components, std::vector<float> times,
                             std::vector<float> values) {
    AnimationChannel channel;
    channel.node = 0;
    channel.path = path;
    channel.interpolation = interpolation;
    channel.components = components;
    channel.times = std::move(times);
    channel.values = std::move(values);
    return channel;
}

// A rig of `count` nodes in a chain: node i is the parent of node i + 1.
Rig makeChain(int count) {
    Rig rig;
    rig.nodes.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        rig.nodes[static_cast<std::size_t>(i)].parent = i - 1;
        rig.order.push_back(i);
    }
    return rig;
}

const char* const kTriangleBounds = ",\"min\":[0,0,0],\"max\":[1,2,0]";

}  // namespace

// -------------------------------------------------------------------- math ---

TEST_CASE("quat: a rotation matrix converts back to the same quaternion") {
    const Quat cases[] = {
        Quat{},
        Quat::fromAxisAngle({1, 0, 0}, 0.3f),
        Quat::fromAxisAngle({0, 1, 0}, 2.9f),  // w small: exercises the other branches
        Quat::fromAxisAngle({0, 0, 1}, 3.1f),
        Quat::fromAxisAngle({1, 0, 0}, 3.1f),
        Quat::fromAxisAngle({1, 2, 3}, 1.7f),
        Quat::fromAxisAngle({-2, 1, 0.5f}, 4.0f),
    };
    for (const Quat& q : cases) {
        const Quat r = quatFromMat4(toMat4(q));
        // q and -q are the same rotation.
        CHECK(std::abs(dot(q, r)) == Approx(1.0f).epsilon(1e-4));
    }
}

TEST_CASE("transform: scale, then rotate, then translate") {
    Transform t;
    t.translation = {1, 2, 3};
    t.rotation = Quat::fromAxisAngle({0, 0, 1}, kPi * 0.5f);
    t.scale = {2, 2, 2};
    // (1,0,0) -> scaled (2,0,0) -> rotated (0,2,0) -> translated (1,4,3)
    checkVec(transformPoint(t.matrix(), {1, 0, 0}), {1, 4, 3});
}

// ---------------------------------------------------------------- sampling ---

TEST_CASE("channel: step holds, linear blends, both clamp outside the keys") {
    float v[3];
    const auto step =
        makeChannel(AnimationPath::Translation, Interpolation::Step, 3, {1, 2, 4}, {0, 0, 0, 10, 0, 0, 20, 0, 0});
    REQUIRE(sampleChannel(step, 1.5f, v));
    CHECK(v[0] == Approx(0));
    REQUIRE(sampleChannel(step, 2.0f, v));
    CHECK(v[0] == Approx(10));
    REQUIRE(sampleChannel(step, 3.99f, v));
    CHECK(v[0] == Approx(10));

    auto linear = step;
    linear.interpolation = Interpolation::Linear;
    REQUIRE(sampleChannel(linear, 1.5f, v));
    CHECK(v[0] == Approx(5));
    REQUIRE(sampleChannel(linear, 3.0f, v));  // half-way through an uneven interval
    CHECK(v[0] == Approx(15));

    // Before the first key and after the last, the nearest key wins.
    REQUIRE(sampleChannel(linear, -5.0f, v));
    CHECK(v[0] == Approx(0));
    REQUIRE(sampleChannel(linear, 99.0f, v));
    CHECK(v[0] == Approx(20));
}

TEST_CASE("channel: a single key and an empty channel") {
    float v[3] = {7, 7, 7};
    const auto one = makeChannel(AnimationPath::Scale, Interpolation::Linear, 3, {0.5f}, {1, 2, 3});
    REQUIRE(sampleChannel(one, 0.0f, v));
    CHECK(v[1] == Approx(2));
    REQUIRE(sampleChannel(one, 9.0f, v));
    CHECK(v[2] == Approx(3));

    CHECK_FALSE(sampleChannel(makeChannel(AnimationPath::Scale, Interpolation::Linear, 3, {}, {}), 0.0f, v));
    // Fewer values than the keys need: refused rather than read out of bounds.
    CHECK_FALSE(sampleChannel(makeChannel(AnimationPath::Scale, Interpolation::Linear, 3, {0, 1}, {1, 2, 3}), 0.5f, v));
}

TEST_CASE("channel: rotations take the short arc and stay unit length") {
    const Quat a = Quat::fromAxisAngle({0, 0, 1}, 0.0f);
    const Quat b = Quat::fromAxisAngle({0, 0, 1}, kPi * 0.5f);
    // The same end rotation, written with the opposite sign: a naive lerp
    // would go the long way round.
    const auto channel = makeChannel(AnimationPath::Rotation, Interpolation::Linear, 4, {0, 1},
                                     {a.x, a.y, a.z, a.w, -b.x, -b.y, -b.z, -b.w});
    float v[4];
    REQUIRE(sampleChannel(channel, 0.5f, v));
    const Quat q{v[0], v[1], v[2], v[3]};
    CHECK(dot(q, q) == Approx(1.0f));
    checkVec(rotate(q, {1, 0, 0}), {std::cos(kPi * 0.25f), std::sin(kPi * 0.25f), 0});
}

TEST_CASE("channel: cubic splines follow the Hermite curve") {
    // Layout per key: in-tangent, value, out-tangent.
    // Zero tangents give the classic ease-in / ease-out curve 3u^2 - 2u^3.
    const auto ease = makeChannel(AnimationPath::Translation, Interpolation::CubicSpline, 3, {0, 2},
                                  {0, 0, 0, /**/ 0, 0, 0, /**/ 0, 0, 0, /**/ 0, 0, 0, /**/ 8, 0, 0, /**/ 0, 0, 0});
    float v[3];
    REQUIRE(sampleChannel(ease, 1.0f, v));
    CHECK(v[0] == Approx(4.0f));
    REQUIRE(sampleChannel(ease, 0.5f, v));  // u = 0.25 -> 3/16 - 2/64 = 0.15625
    CHECK(v[0] == Approx(8.0f * 0.15625f));
    REQUIRE(sampleChannel(ease, 0.0f, v));
    CHECK(v[0] == Approx(0.0f));
    REQUIRE(sampleChannel(ease, 2.0f, v));
    CHECK(v[0] == Approx(8.0f));

    // Tangents equal to the straight-line slope reproduce a straight line.
    // The slope is per second: 8 units over 2 seconds = 4.
    const auto line = makeChannel(AnimationPath::Translation, Interpolation::CubicSpline, 3, {0, 2},
                                  {0, 0, 0, /**/ 0, 0, 0, /**/ 4, 0, 0, /**/ 4, 0, 0, /**/ 8, 0, 0, /**/ 0, 0, 0});
    for (float t : {0.25f, 0.5f, 1.0f, 1.7f}) {
        REQUIRE(sampleChannel(line, t, v));
        CHECK(v[0] == Approx(4.0f * t));
    }
}

TEST_CASE("animation: only the animated properties of a pose change") {
    Rig rig = makeChain(2);
    rig.nodes[1].rest.translation = {0, 5, 0};
    rig.nodes[0].weights = {0.0f, 0.0f};

    Animation animation;
    animation.duration = 1.0f;
    animation.channels.push_back(
        makeChannel(AnimationPath::Translation, Interpolation::Linear, 3, {0, 1}, {0, 0, 0, 2, 0, 0}));
    animation.channels.push_back(makeChannel(AnimationPath::Weights, Interpolation::Linear, 2, {0, 1}, {0, 1, 1, 0}));
    // A channel pointing at a node that does not exist is ignored.
    animation.channels.push_back(
        makeChannel(AnimationPath::Scale, Interpolation::Linear, 3, {0, 1}, {1, 1, 1, 2, 2, 2}));
    animation.channels.back().node = 42;

    Pose pose = restPose(rig);
    sampleAnimation(animation, 0.5f, pose);
    checkVec(pose.local[0].translation, {1, 0, 0});
    CHECK(pose.local[0].scale == Vec3{1, 1, 1});
    checkVec(pose.local[1].translation, {0, 5, 0});  // untouched
    REQUIRE(pose.weights[0].size() == 2);
    CHECK(pose.weights[0][0] == Approx(0.5f));
    CHECK(pose.weights[0][1] == Approx(0.5f));
}

TEST_CASE("pose: world matrices accumulate down the hierarchy") {
    Rig rig = makeChain(3);
    rig.nodes[0].rest.translation = {1, 0, 0};
    rig.nodes[1].rest.rotation = Quat::fromAxisAngle({0, 0, 1}, kPi * 0.5f);
    rig.nodes[2].rest.translation = {1, 0, 0};

    std::vector<Mat4> world;
    computeWorldMatrices(rig, restPose(rig), world);
    REQUIRE(world.size() == 3);
    checkVec(transformPoint(world[0], {0, 0, 0}), {1, 0, 0});
    // Node 2 sits one unit along node 1's x axis, which now points up.
    checkVec(transformPoint(world[2], {0, 0, 0}), {1, 1, 0});

    // The order does not have to be the storage order, only parents first.
    Rig reversed;
    reversed.nodes.resize(2);
    reversed.nodes[0].parent = 1;
    reversed.nodes[0].rest.translation = {0, 1, 0};
    reversed.nodes[1].rest.translation = {5, 0, 0};
    reversed.order = {1, 0};
    computeWorldMatrices(reversed, restPose(reversed), world);
    checkVec(transformPoint(world[0], {0, 0, 0}), {5, 1, 0});
}

TEST_CASE("pose: blending") {
    Rig rig = makeChain(1);
    rig.nodes[0].weights = {0.0f};
    Pose a = restPose(rig), b = restPose(rig), out;
    b.local[0].translation = {4, 0, 0};
    b.local[0].scale = {3, 3, 3};
    b.local[0].rotation = Quat::fromAxisAngle({0, 1, 0}, kPi * 0.5f);
    b.weights[0] = {1.0f};

    blendPoses(a, b, 0.25f, out);
    checkVec(out.local[0].translation, {1, 0, 0});
    checkVec(out.local[0].scale, {1.5f, 1.5f, 1.5f});
    CHECK(out.weights[0][0] == Approx(0.25f));
    // A quarter of a 90 degree turn about y.
    const float angle = kPi * 0.125f;
    checkVec(rotate(out.local[0].rotation, {0, 0, 1}), {std::sin(angle), 0, std::cos(angle)});

    blendPoses(a, b, 0.0f, out);
    checkVec(out.local[0].translation, {0, 0, 0});
    blendPoses(a, b, 1.0f, out);
    checkVec(out.local[0].translation, {4, 0, 0});
}

// ---------------------------------------------------------------- skinning ---

namespace {

// Two bones stacked along y, the second starting at y = 1, and a mesh of
// three vertices: two bound to the first bone, one (at y = 2) to the second.
Model makeSkinnedModel() {
    Model model;
    model.rig = makeChain(3);  // node 0: lower bone, node 1: upper bone, node 2: the mesh
    model.rig.nodes[2].parent = -1;
    model.rig.nodes[1].rest.translation = {0, 1, 0};

    Skin skin;
    skin.joints = {0, 1};
    skin.inverseBind = {Mat4::identity(), translate({0, -1, 0})};
    model.rig.skins.push_back(skin);

    Mesh mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    mesh.rig.node = 2;
    mesh.rig.skin = 0;
    mesh.rig.positions = {{0, 0, 0}, {1, 0, 0}, {0, 2, 0}};
    mesh.rig.normals = {{0, 0, 1}, {0, 0, 1}, {0, 1, 0}};
    mesh.rig.tangents = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    mesh.rig.joints = {{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}};
    mesh.rig.weights = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
    model.meshes.push_back(mesh);
    model.materials.emplace_back();
    return model;
}

}  // namespace

TEST_CASE("skinning: the rest pose reproduces the rest shape") {
    Model model = makeSkinnedModel();
    applyPose(model, restPose(model.rig));
    checkVec(model.meshes[0].vertices[0].position, {0, 0, 0});
    checkVec(model.meshes[0].vertices[1].position, {1, 0, 0});
    checkVec(model.meshes[0].vertices[2].position, {0, 2, 0});
}

TEST_CASE("skinning: vertices follow their bone") {
    Model model = makeSkinnedModel();
    Pose pose = restPose(model.rig);
    pose.local[1].rotation = Quat::fromAxisAngle({0, 0, 1}, kPi * 0.5f);
    applyPose(model, pose);

    const auto& v = model.meshes[0].vertices;
    checkVec(v[0].position, {0, 0, 0});  // on the lower bone: unmoved
    checkVec(v[1].position, {1, 0, 0});
    // One unit above the upper bone's pivot (0,1,0), swung 90 degrees about z.
    checkVec(v[2].position, {-1, 1, 0});
    checkVec(v[2].normal, {-1, 0, 0});  // normals turn with the bone
    checkVec(v[0].normal, {0, 0, 1});

    // Moving the root moves everything, children included.
    pose.local[0].translation = {0, 0, 5};
    applyPose(model, pose);
    checkVec(v[0].position, {0, 0, 5});
    checkVec(v[2].position, {-1, 1, 5});
}

TEST_CASE("skinning: a vertex shared between two bones lands between them") {
    Model model = makeSkinnedModel();
    model.meshes[0].rig.joints[2] = {0, 1, 0, 0};
    model.meshes[0].rig.weights[2] = {0.5f, 0.5f, 0, 0};
    Pose pose = restPose(model.rig);
    pose.local[1].rotation = Quat::fromAxisAngle({0, 0, 1}, kPi * 0.5f);
    applyPose(model, pose);
    // Half of "stays at (0,2,0)" plus half of "goes to (-1,1,0)".
    checkVec(model.meshes[0].vertices[2].position, {-0.5f, 1.5f, 0});
}

TEST_CASE("skinning: rigid meshes follow their node, with a correct normal matrix") {
    Model model;
    model.rig = makeChain(1);
    model.rig.nodes[0].rest.translation = {0, 0, 3};
    model.rig.nodes[0].rest.scale = {1, 4, 1};  // non-uniform
    Mesh mesh;
    mesh.vertices.resize(1);
    mesh.rig.node = 0;
    mesh.rig.positions = {{1, 1, 0}};
    mesh.rig.normals = {normalize(Vec3{1, 1, 0})};
    mesh.rig.tangents = {{1, 0, 0}};
    model.meshes.push_back(mesh);

    applyPose(model, restPose(model.rig));
    checkVec(model.meshes[0].vertices[0].position, {1, 4, 3});
    // Stretching along y tilts the surface towards y, so its normal tilts
    // towards x: (1, 1/4) normalised, not (1, 4).
    checkVec(model.meshes[0].vertices[0].normal, normalize(Vec3{1, 0.25f, 0}));
}

TEST_CASE("skinning: morph targets add weighted offsets before the transform") {
    Model model;
    model.rig = makeChain(1);
    model.rig.nodes[0].rest.translation = {10, 0, 0};
    model.rig.nodes[0].weights = {0.0f, 0.0f};
    Mesh mesh;
    mesh.vertices.resize(2);
    mesh.rig.node = 0;
    mesh.rig.positions = {{0, 0, 0}, {1, 0, 0}};
    mesh.rig.normals = {{0, 0, 1}, {0, 0, 1}};
    mesh.rig.tangents = {{1, 0, 0}, {1, 0, 0}};
    mesh.rig.targets.resize(2);
    mesh.rig.targets[0].positions = {{0, 1, 0}, {0, 2, 0}};
    mesh.rig.targets[1].positions = {{0, 0, 4}, {0, 0, 4}};
    mesh.rig.targets[1].normals = {{0, 1, -1}, {0, 0, 0}};  // (0,0,1) + (0,1,-1) = (0,1,0)
    model.meshes.push_back(mesh);

    Pose pose = restPose(model.rig);
    applyPose(model, pose);
    checkVec(model.meshes[0].vertices[1].position, {11, 0, 0});

    pose.weights[0] = {0.5f, 1.0f};
    applyPose(model, pose);
    checkVec(model.meshes[0].vertices[0].position, {10, 0.5f, 4});
    checkVec(model.meshes[0].vertices[1].position, {11, 1.0f, 4});
    checkVec(model.meshes[0].vertices[0].normal, {0, 1, 0});
    checkVec(model.meshes[0].vertices[1].normal, {0, 0, 1});

    // More weights than targets, or fewer, must not read out of bounds.
    pose.weights[0] = {1.0f, 0.0f, 9.0f};
    applyPose(model, pose);
    checkVec(model.meshes[0].vertices[0].position, {10, 1, 0});
    pose.weights[0] = {};
    applyPose(model, pose);
    checkVec(model.meshes[0].vertices[0].position, {10, 0, 0});
}

TEST_CASE("skinning: the result does not depend on the thread count") {
    // Enough vertices for several chunks.
    Model model = makeSkinnedModel();
    Mesh& mesh = model.meshes[0];
    const std::size_t count = 10000;
    mesh.vertices.assign(count, {});
    mesh.rig.positions.resize(count);
    mesh.rig.normals.assign(count, {0, 0, 1});
    mesh.rig.tangents.assign(count, {1, 0, 0});
    mesh.rig.joints.assign(count, {0, 1, 0, 0});
    mesh.rig.weights.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(count);
        mesh.rig.positions[i] = {f, 2.0f * f, 0};
        mesh.rig.weights[i] = {1.0f - f, f, 0, 0};
    }
    Pose pose = restPose(model.rig);
    pose.local[1].rotation = Quat::fromAxisAngle({0, 0, 1}, 1.0f);

    Model serial = model, threaded = model;
    ThreadPool pool(4);
    applyPose(serial, pose);
    applyPose(threaded, pose, &pool);
    bool same = true;
    for (std::size_t i = 0; i < count; ++i)
        same = same && serial.meshes[0].vertices[i].position == threaded.meshes[0].vertices[i].position;
    CHECK(same);
}

TEST_CASE("skinning: mismatched data is ignored, not trusted") {
    Model model = makeSkinnedModel();
    const Pose pose = restPose(model.rig);

    Model wrongPose = model;
    Pose shortPose = pose;
    shortPose.local.pop_back();
    applyPose(wrongPose, shortPose);  // pose of a different rig: no-op
    checkVec(wrongPose.meshes[0].vertices[2].position, {0, 0, 0});

    Model badSkin = model;
    badSkin.meshes[0].rig.skin = 7;  // no such skin: falls back to the node transform
    applyPose(badSkin, pose);
    checkVec(badSkin.meshes[0].vertices[2].position, {0, 2, 0});

    Model staticModel;
    staticModel.meshes.emplace_back().vertices.resize(1);
    applyPose(staticModel, pose);  // no rig at all
    checkVec(staticModel.meshes[0].vertices[0].position, {0, 0, 0});
}

// ------------------------------------------------------------------ player ---

namespace {

// Two clips that slide node 0 along x: "slow" covers 0..10 in 2 s, "fast"
// covers 100..200 in 1 s.
Rig makeTwoClipRig() {
    Rig rig = makeChain(1);
    Animation slow;
    slow.name = "slow";
    slow.duration = 2.0f;
    slow.channels.push_back(
        makeChannel(AnimationPath::Translation, Interpolation::Linear, 3, {0, 2}, {0, 0, 0, 10, 0, 0}));
    Animation fast;
    fast.duration = 1.0f;
    fast.channels.push_back(
        makeChannel(AnimationPath::Translation, Interpolation::Linear, 3, {0, 1}, {100, 0, 0, 200, 0, 0}));
    rig.animations = {slow, fast};
    return rig;
}

}  // namespace

TEST_CASE("player: plays, loops, pauses and seeks") {
    const Rig rig = makeTwoClipRig();
    AnimationPlayer player(rig);
    CHECK(player.clipCount() == 2);
    CHECK(player.clip() == 0);
    CHECK(player.clipName(0) == "slow");
    CHECK(player.clipName(1) == "animation 2");  // unnamed clips are numbered
    CHECK(player.duration() == Approx(2.0f));
    CHECK(player.pose().local[0].translation.x == Approx(0.0f));

    CHECK(player.advance(0.5f));
    CHECK(player.time() == Approx(0.5f));
    CHECK(player.pose().local[0].translation.x == Approx(2.5f));

    // Past the end it wraps around.
    player.advance(2.0f);
    CHECK(player.time() == Approx(0.5f));

    player.pause();
    CHECK_FALSE(player.playing());
    CHECK_FALSE(player.advance(1.0f));  // nothing changed
    CHECK(player.time() == Approx(0.5f));

    // Scrubbing works while paused, and wraps in both directions.
    player.seek(1.0f);
    CHECK(player.pose().local[0].translation.x == Approx(5.0f));
    player.seek(-0.5f);
    CHECK(player.time() == Approx(1.5f));
    player.seek(4.5f);
    CHECK(player.time() == Approx(0.5f));
}

TEST_CASE("player: speed, reverse and non-looping playback") {
    const Rig rig = makeTwoClipRig();
    AnimationPlayer player(rig);
    player.setSpeed(2.0f);
    player.advance(0.25f);
    CHECK(player.time() == Approx(0.5f));

    player.setSpeed(-1.0f);
    player.advance(1.0f);  // 0.5 - 1.0 wraps to 1.5
    CHECK(player.time() == Approx(1.5f));

    player.setLooping(false);
    player.setSpeed(1.0f);
    player.advance(10.0f);
    CHECK(player.time() == Approx(2.0f));  // holds the last frame
    CHECK(player.pose().local[0].translation.x == Approx(10.0f));
    CHECK_FALSE(player.advance(1.0f));  // and stays there
    player.seek(-3.0f);
    CHECK(player.time() == Approx(0.0f));  // clamps instead of wrapping
}

TEST_CASE("player: switching clips, with and without a cross-fade") {
    const Rig rig = makeTwoClipRig();
    AnimationPlayer player(rig);
    player.advance(1.0f);  // slow clip at x = 5

    // A hard cut starts the new clip from its beginning.
    player.setClip(1);
    CHECK(player.clip() == 1);
    CHECK_FALSE(player.fading());
    CHECK(player.pose().local[0].translation.x == Approx(100.0f));

    // Out-of-range indices are clamped.
    player.setClip(99);
    CHECK(player.clip() == 1);
    player.setClip(-4);
    CHECK(player.clip() == 0);

    // Cross-fade: half-way through, the pose is half of each clip. Pause so
    // the clips themselves stand still and only the fade advances.
    player.seek(1.0f);  // slow clip at x = 5
    player.pause();
    player.setClip(1, 1.0f);
    CHECK(player.fading());
    CHECK(player.pose().local[0].translation.x == Approx(5.0f));  // still all of the old clip
    CHECK(player.advance(0.5f));                                  // the fade runs even while paused
    CHECK(player.pose().local[0].translation.x == Approx(52.5f));
    player.advance(0.6f);
    CHECK_FALSE(player.fading());
    CHECK(player.pose().local[0].translation.x == Approx(100.0f));
}

TEST_CASE("player: a rig without animations stays in its rest pose") {
    Rig rig = makeChain(1);
    rig.nodes[0].rest.translation = {3, 0, 0};
    AnimationPlayer player(rig);
    CHECK(player.clip() == -1);
    CHECK(player.clipCount() == 0);
    CHECK_FALSE(player.playing());
    CHECK(player.duration() == 0.0f);
    player.setClip(0, 0.5f);
    player.seek(2.0f);
    player.advance(1.0f);
    CHECK(player.pose().local[0].translation.x == Approx(3.0f));
}

// ------------------------------------------------------------------ loader ---

TEST_CASE("gltf: a skinned, animated file plays back") {
    // The same two-bone arrangement as above, read from a file. Node 0 holds
    // the mesh and skin, node 1 is the lower bone, node 2 the upper one,
    // which the animation swings 90 degrees about z over one second.
    GltfBuffer buffer;
    const int positions = buffer.floats({0, 0, 0, 1, 0, 0, 0, 2, 0}, 3, "VEC3", kTriangleBounds);
    const std::uint16_t indexData[] = {0, 1, 2};
    const int indices = buffer.add(indexData, sizeof(indexData), 5123, 3, "SCALAR");
    const std::uint8_t jointData[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    const int joints = buffer.add(jointData, sizeof(jointData), 5121, 3, "VEC4");
    // Weights that do not sum to one are normalised by the loader.
    const int weights = buffer.floats({1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0}, 4, "VEC4");
    const int inverseBind = buffer.floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0,  0, 1,  //
                                           1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1},
                                          16, "MAT4");
    const int times = buffer.floats({0, 1}, 1, "SCALAR", ",\"min\":[0],\"max\":[1]");
    const float s = std::sqrt(0.5f);
    const int rotations = buffer.floats({0, 0, 0, 1, 0, 0, s, s}, 4, "VEC4");

    const std::string gltf =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],
        "nodes":[{"mesh":0,"skin":0,"name":"body"},{"children":[2],"name":"lower"},
                 {"translation":[0,1,0],"name":"upper"}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":)" +
        std::to_string(positions) + R"(,"JOINTS_0":)" + std::to_string(joints) + R"(,"WEIGHTS_0":)" +
        std::to_string(weights) + R"(},"indices":)" + std::to_string(indices) + R"(}]}],
        "skins":[{"joints":[1,2],"inverseBindMatrices":)" +
        std::to_string(inverseBind) + R"(}],
        "animations":[{"name":"bend","channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}],
                       "samplers":[{"input":)" +
        std::to_string(times) + R"(,"output":)" + std::to_string(rotations) + R"(,"interpolation":"LINEAR"}]}],)" +
        buffer.json() + "}";

    TempDir dir;
    LoadResult result = loadModel(dir.write("bend.gltf", gltf));
    REQUIRE_MESSAGE(result.ok, result.error);
    Model& model = result.model;

    REQUIRE(model.animated());
    REQUIRE(model.rig.animations.size() == 1);
    CHECK(model.rig.animations[0].name == "bend");
    CHECK(model.rig.animations[0].duration == Approx(1.0f));
    REQUIRE(model.rig.nodes.size() == 3);
    CHECK(model.rig.nodes[2].name == "upper");
    CHECK(model.rig.nodes[2].parent == 1);
    REQUIRE(model.rig.skins.size() == 1);
    CHECK(model.rig.skins[0].joints == std::vector<int>{1, 2});
    REQUIRE(model.meshes.size() == 1);
    CHECK(model.meshes[0].rig.weights[1] == Vec4{1, 0, 0, 0});

    // Loaded in its rest pose.
    checkVec(model.meshes[0].vertices[2].position, {0, 2, 0});

    AnimationPlayer player(model.rig);
    player.seek(0.5f);
    applyPose(model, player.pose());
    checkVec(model.meshes[0].vertices[2].position, {-std::sqrt(0.5f), 1.0f + std::sqrt(0.5f), 0});
    checkVec(model.meshes[0].vertices[1].position, {1, 0, 0});

    // The camera frames everything the animation reaches, not just frame 0.
    const Bounds motion = model.framingBounds();
    CHECK(motion.min.x < -0.9f);
    CHECK(motion.max.y == Approx(2.0f));

    CHECK(findAnimation(model, "") == 0);
    CHECK(findAnimation(model, "BEND") == 0);
    CHECK(findAnimation(model, "1") == 0);
    CHECK(findAnimation(model, "2") == -1);
    CHECK(findAnimation(model, "0") == -1);
    CHECK(findAnimation(model, "walk") == -1);
}

TEST_CASE("gltf: animated morph targets, with default weights from the mesh") {
    GltfBuffer buffer;
    const int positions = buffer.floats({0, 0, 0, 1, 0, 0, 0, 2, 0}, 3, "VEC3", kTriangleBounds);
    const int deltas = buffer.floats({0, 0, 1, 0, 0, 1, 0, 0, 1}, 3, "VEC3", ",\"min\":[0,0,1],\"max\":[0,0,1]");
    const int times = buffer.floats({0, 1}, 1, "SCALAR", ",\"min\":[0],\"max\":[1]");
    const int values = buffer.floats({0, 1}, 1, "SCALAR");

    const std::string gltf = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"mesh":0,"translation":[0,10,0]}],
        "meshes":[{"weights":[0.25],"primitives":[{"attributes":{"POSITION":)" +
                             std::to_string(positions) + R"(},"targets":[{"POSITION":)" + std::to_string(deltas) +
                             R"(}]}]}],
        "animations":[{"channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}],
                       "samplers":[{"input":)" +
                             std::to_string(times) + R"(,"output":)" + std::to_string(values) + R"(}]}],)" +
                             buffer.json() + "}";

    TempDir dir;
    LoadResult result = loadModel(dir.write("morph.gltf", gltf));
    REQUIRE_MESSAGE(result.ok, result.error);
    Model& model = result.model;
    REQUIRE(model.animated());
    REQUIRE(model.meshes[0].rig.targets.size() == 1);

    // Rest pose: the mesh's default weight of 0.25, then the node transform.
    checkVec(model.meshes[0].vertices[0].position, {0, 10, 0.25f});

    AnimationPlayer player(model.rig);
    player.seek(0.5f);
    applyPose(model, player.pose());
    checkVec(model.meshes[0].vertices[0].position, {0, 10, 0.5f});
    checkVec(model.meshes[0].vertices[2].position, {0, 12, 0.5f});
}

TEST_CASE("gltf: a file without animations keeps no rig") {
    GltfBuffer buffer;
    const int positions = buffer.floats({0, 0, 0, 1, 0, 0, 0, 2, 0}, 3, "VEC3", kTriangleBounds);
    // A node given as a matrix: scale 2, then 90 degrees about z, then
    // translate by (5,0,0). Column-major.
    const std::string gltf = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"mesh":0,"matrix":[0,2,0,0, -2,0,0,0, 0,0,2,0, 5,0,0,1]}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                             std::to_string(positions) + "}}]}]," + buffer.json() + "}";
    TempDir dir;
    const LoadResult result = loadModel(dir.write("static.gltf", gltf));
    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK_FALSE(result.model.animated());
    CHECK(result.model.rig.empty());
    CHECK(result.model.meshes[0].rig.empty());
    // (1,0,0) -> (0,2,0) + (5,0,0);  (0,2,0) -> (-4,0,0) + (5,0,0)
    checkVec(result.model.meshes[0].vertices[1].position, {5, 2, 0});
    checkVec(result.model.meshes[0].vertices[2].position, {1, 0, 0});
}

TEST_CASE("gltf: malformed animation channels are skipped with a warning") {
    GltfBuffer buffer;
    const int positions = buffer.floats({0, 0, 0, 1, 0, 0, 0, 2, 0}, 3, "VEC3", kTriangleBounds);
    const int times = buffer.floats({0, 1}, 1, "SCALAR", ",\"min\":[0],\"max\":[1]");
    const int good = buffer.floats({0, 0, 0, 2, 0, 0}, 3, "VEC3");
    const int wrongType = buffer.floats({0, 0, 0, 1, 0, 0, 0, 1}, 4, "VEC4");  // a quaternion as a translation
    const int backwards = buffer.floats({1, 0}, 1, "SCALAR", ",\"min\":[0],\"max\":[1]");

    const std::string gltf = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"mesh":0}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                             std::to_string(positions) + R"(}}]}],
        "animations":[{"name":"mixed","channels":[
            {"sampler":0,"target":{"node":0,"path":"translation"}},
            {"sampler":1,"target":{"node":0,"path":"translation"}},
            {"sampler":2,"target":{"node":0,"path":"translation"}}],
          "samplers":[{"input":)" +
                             std::to_string(times) + R"(,"output":)" + std::to_string(good) + R"(},{"input":)" +
                             std::to_string(times) + R"(,"output":)" + std::to_string(wrongType) + R"(},{"input":)" +
                             std::to_string(backwards) + R"(,"output":)" + std::to_string(good) + R"(}]}],)" +
                             buffer.json() + "}";
    TempDir dir;
    LoadResult result = loadModel(dir.write("mixed.gltf", gltf));
    REQUIRE_MESSAGE(result.ok, result.error);
    REQUIRE(result.model.rig.animations.size() == 1);
    CHECK(result.model.rig.animations[0].channels.size() == 1);  // only the good one
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("mixed") != std::string::npos);

    AnimationPlayer player(result.model.rig);
    player.seek(0.5f);
    applyPose(result.model, player.pose());
    checkVec(result.model.meshes[0].vertices[0].position, {1, 0, 0});
}
