#include "physics/demo.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "scene/primitives.hpp"

namespace termesh {

namespace {

// A small deterministic generator, so the demo is the same every time.
struct Random {
    std::uint32_t state;
    float next() {  // 0 .. 1
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

const Vec3 kTints[] = {{1.0f, 1.0f, 1.0f},   {0.55f, 0.75f, 1.0f}, {1.0f, 0.6f, 0.55f},
                       {0.65f, 1.0f, 0.65f}, {1.0f, 0.9f, 0.5f},   {0.85f, 0.65f, 1.0f}};

constexpr float kFloorSize = 9.0f;  // half the floor's width

}  // namespace

PhysicsDemo::PhysicsDemo(Scene& scene, int bodies, std::uint32_t seed) : scene_(scene), seed_(seed) {
    bodies = std::clamp(bodies, 1, kMaxBodies);
    scene.name = "physics";

    // The floor is only something to look at: the world's ground plane is
    // what things land on.
    Model floor = makeDemoModel("plane");
    // A darker floor than the plane's own: it fills most of the picture, and
    // the things on it should be what catches the eye.
    for (Material& material : floor.materials) material.baseColor = {0.5f, 0.52f, 0.58f, 1.0f};
    scene.add(std::move(floor), "floor", scale({kFloorSize, 1.0f, kFloorSize}));
    world_.groundExtent = kFloorSize;

    // A third of the bodies make a wall to knock down; the rest fall on it.
    Random random{seed * 2654435761u + 12345u};
    const int wall = std::max(bodies / 3, 1);
    for (int i = 0; i < bodies; ++i) {
        const Vec3 tint = kTints[static_cast<std::size_t>(i) % std::size(kTints)];
        if (i < wall) {
            addCrate({0.5f, 0.5f, 0.5f}, tint);
        } else if (i % 2 == 0) {
            addBall(random.range(0.3f, 0.55f), tint, 600.0f);
        } else {
            const float s = random.range(0.3f, 0.5f);
            addCrate({s, s * random.range(0.6f, 1.4f), s}, tint);
        }
    }
    // Balls for throwing wait out of sight until they are needed.
    firstThrown_ = static_cast<int>(links_.size());
    for (int i = 0; i < kMaxThrown; ++i) addBall(0.45f, {1.0f, 0.35f, 0.3f}, 2500.0f);

    place();
    start_ = world_.bodies();
    sync();

    // Somewhere to look from, and a sun to throw shadows.
    CameraPose camera;
    camera.position = {8.5f, 5.5f, 10.5f};
    camera.target = {0.0f, 1.6f, 0.0f};
    scene.camera = camera;
    Light sun;
    sun.type = LightType::Directional;
    sun.direction = normalize(Vec3{-0.5f, -1.0f, -0.35f});
    sun.intensity = 1.7f;
    sun.castsShadow = true;
    scene.lights.push_back(sun);
    Light fill;
    fill.type = LightType::Directional;
    fill.direction = normalize(Vec3{0.6f, -0.4f, 0.7f});
    fill.intensity = 0.35f;
    fill.specular = 0.0f;
    scene.lights.push_back(fill);
}

int PhysicsDemo::addCrate(Vec3 halfExtents, Vec3 tint) {
    Model model = makeDemoModel("cube");
    for (Material& material : model.materials)
        material.baseColor = {material.baseColor.x * tint.x, material.baseColor.y * tint.y,
                              material.baseColor.z * tint.z, material.baseColor.w};
    const int body = world_.addBox({}, halfExtents);
    scene_.add(std::move(model), "crate " + std::to_string(links_.size() + 1));
    // The cube model spans -1 .. 1.
    links_.push_back({body, static_cast<int>(scene_.objects.size()) - 1, scale(halfExtents)});
    return body;
}

int PhysicsDemo::addBall(float radius, Vec3 tint, float density) {
    Model model = makeDemoModel("sphere");
    for (Material& material : model.materials)
        material.baseColor = {material.baseColor.x * tint.x, material.baseColor.y * tint.y,
                              material.baseColor.z * tint.z, material.baseColor.w};
    // Whatever size the sphere model is, make it this radius about its centre.
    const Bounds bounds = model.bounds();
    const float modelRadius = std::max((bounds.max.x - bounds.min.x) * 0.5f, 1e-6f);
    const int body = world_.addSphere({}, radius, density);
    world_.bodies()[static_cast<std::size_t>(body)].restitution = 0.45f;
    scene_.add(std::move(model), "ball " + std::to_string(links_.size() + 1));
    links_.push_back(
        {body, static_cast<int>(scene_.objects.size()) - 1,
         scale({radius / modelRadius, radius / modelRadius, radius / modelRadius}) * translate(-bounds.center())});
    return body;
}

void PhysicsDemo::place() {
    Random random{seed_ * 747796405u + 2891336453u};
    std::vector<Body>& bodies = world_.bodies();
    const int count = firstThrown_;
    const int wall = std::max(count / 3, 1);

    // The wall: rows of crates, each row half a crate along from the last,
    // like bricks.
    const int perRow = std::max(static_cast<int>(std::ceil(std::sqrt(static_cast<float>(wall) * 1.6f))), 1);
    for (int i = 0; i < wall; ++i) {
        Body& body = bodies[static_cast<std::size_t>(links_[static_cast<std::size_t>(i)].body)];
        const int row = i / perRow, column = i % perRow;
        const float stagger = row % 2 ? 0.5f : 0.0f;
        body.position = {(static_cast<float>(column) - static_cast<float>(perRow - 1) * 0.5f + stagger) * 1.02f,
                         0.5f + static_cast<float>(row) * 1.0f, 0.0f};
        body.orientation = {};
    }
    // The rest start in the air above it, in a loose spiral, so that they
    // arrive one after another.
    for (int i = wall; i < count; ++i) {
        Body& body = bodies[static_cast<std::size_t>(links_[static_cast<std::size_t>(i)].body)];
        const float k = static_cast<float>(i - wall);
        const float angle = k * 2.4f, reach = 0.6f + 0.22f * k;
        body.position = {std::cos(angle) * std::min(reach, 4.0f), 5.0f + k * 0.9f,
                         std::sin(angle) * std::min(reach, 4.0f)};
        body.orientation = normalize(Quat{random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f),
                                          random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f)});
        body.angularVelocity = {random.range(-2.0f, 2.0f), random.range(-2.0f, 2.0f), random.range(-2.0f, 2.0f)};
    }
    // Thrown balls: parked far below the floor, asleep, where nothing
    // reaches them.
    for (int i = 0; i < kMaxThrown; ++i) {
        Body& body = bodies[static_cast<std::size_t>(links_[static_cast<std::size_t>(firstThrown_ + i)].body)];
        body.position = {static_cast<float>(i) * 3.0f, -100.0f, 0.0f};
        body.asleep = true;
    }
    for (Body& body : bodies) body.velocity = {};
}

void PhysicsDemo::sync() {
    for (const Link& link : links_) {
        SceneObject& object = *scene_.objects[static_cast<std::size_t>(link.object)];
        const Body& body = world_.bodies()[static_cast<std::size_t>(link.body)];
        object.transform = body.transform() * link.fit;
        // The parked balls are not part of the picture (or of its bounds).
        object.visible = body.position.y > -50.0f;
    }
}

bool PhysicsDemo::update(float dt) {
    if (!world_.moving()) return false;
    world_.advance(dt);
    // Anything that has left the floor and fallen a long way is put to
    // rest, so the simulation can end.
    for (Body& body : world_.bodies()) {
        if (body.position.y < -30.0f && !body.asleep) {
            body.asleep = true;
            body.velocity = {};
            body.angularVelocity = {};
            body.position.y = -100.0f;
        }
    }
    sync();
    return true;
}

void PhysicsDemo::reset() {
    world_.bodies() = start_;
    thrown_ = 0;
    sync();
}

void PhysicsDemo::poke(Vec3 origin, Vec3 direction) {
    // Reuse the oldest ball once they have all been thrown.
    const Link& link = links_[static_cast<std::size_t>(firstThrown_ + thrown_ % kMaxThrown)];
    ++thrown_;
    Body& body = world_.bodies()[static_cast<std::size_t>(link.body)];
    const Vec3 aim = normalize(direction, {0.0f, 0.0f, -1.0f});
    body.position = origin + aim * 1.5f;
    body.position.y = std::max(body.position.y, body.radius + 0.05f);
    // Fast, and slightly upwards so that it arrives level.
    body.velocity = aim * 16.0f + Vec3{0.0f, 2.0f, 0.0f};
    body.angularVelocity = {};
    body.asleep = false;
    body.quietTime = 0.0f;
    sync();
}

}  // namespace termesh
