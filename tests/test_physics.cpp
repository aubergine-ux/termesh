// Rigid-body physics: contact generation, the solver, and the demo.
#include <doctest/doctest.h>

#include <cmath>

#include "physics/demo.hpp"
#include "physics/world.hpp"

using namespace termesh;

namespace {

void run(PhysicsWorld& world, float seconds) {
    const int steps = static_cast<int>(std::lround(seconds / PhysicsWorld::kTimeStep));
    for (int i = 0; i < steps; ++i) world.step();
}

Quat aboutY(float degrees) { return Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, radians(degrees)); }

}  // namespace

TEST_CASE("physics: mass and inertia come from the shape") {
    PhysicsWorld world;
    const Body& sphere = world.bodies()[static_cast<std::size_t>(world.addSphere({}, 0.5f, 1000.0f))];
    CHECK(1.0f / sphere.inverseMass == doctest::Approx(1000.0f * 4.0f / 3.0f * kPi * 0.125f));
    const Body& box = world.bodies()[static_cast<std::size_t>(world.addBox({}, {0.5f, 1.0f, 1.5f}))];
    CHECK(1.0f / box.inverseMass == doctest::Approx(6000.0f));
    // About x the box is 2 x 3: m (4 + 9) / 12.
    CHECK(1.0f / box.inverseInertia.x == doctest::Approx(6000.0f * 13.0f / 12.0f));
    const Body& fixed = world.bodies()[static_cast<std::size_t>(world.addBox({}, {1, 1, 1}, {}, 0.0f))];
    CHECK(fixed.isStatic());
}

TEST_CASE("physics: free fall matches the textbook") {
    PhysicsWorld world;
    world.hasGround = false;
    world.addSphere({0.0f, 10.0f, 0.0f}, 0.5f);
    run(world, 1.0f);
    const Body& body = world.bodies()[0];
    // v = g t, y = y0 - g t^2 / 2 (a little less: there is some drag).
    CHECK(body.velocity.y == doctest::Approx(-9.81f).epsilon(0.03));
    CHECK(body.position.y == doctest::Approx(10.0f - 4.905f).epsilon(0.03));
}

TEST_CASE("physics: sphere contacts") {
    PhysicsWorld world;
    world.addSphere({0.0f, 0.0f, 0.0f}, 1.0f);
    world.addSphere({1.5f, 0.0f, 0.0f}, 1.0f);
    world.addSphere({5.0f, 0.0f, 0.0f}, 1.0f);
    std::vector<Contact> contacts;
    REQUIRE(collideSphereSphere(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
    CHECK(contacts[0].depth == doctest::Approx(0.5f));
    CHECK(contacts[0].normal.x == doctest::Approx(1.0f));
    CHECK(contacts[0].point.x == doctest::Approx(0.75f));
    CHECK(collideSphereSphere(world.bodies()[0], world.bodies()[2], 0, 2, contacts) == 0);

    // On the ground.
    contacts.clear();
    Body low = world.bodies()[0];
    low.position = {0.0f, 0.8f, 0.0f};
    REQUIRE(collideGround(low, 0, 0.0f, 0.0f, contacts) == 1);
    CHECK(contacts[0].depth == doctest::Approx(0.2f));
    CHECK(contacts[0].normal.y == doctest::Approx(-1.0f));
    CHECK(contacts[0].b == -1);
}

TEST_CASE("physics: sphere against box") {
    PhysicsWorld world;
    world.addSphere({0.0f, 1.3f, 0.0f}, 0.5f);
    world.addBox({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    std::vector<Contact> contacts;
    REQUIRE(collideSphereBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
    CHECK(contacts[0].depth == doctest::Approx(0.2f));
    CHECK(contacts[0].normal.y == doctest::Approx(-1.0f));  // from the sphere down into the box
    CHECK(contacts[0].point.y == doctest::Approx(1.0f));

    // Near a corner the normal points at the corner.
    contacts.clear();
    world.bodies()[0].position = {1.2f, 1.2f, 1.2f};
    REQUIRE(collideSphereBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
    CHECK(contacts[0].normal.x == doctest::Approx(-0.57735f).epsilon(0.01));
    CHECK(contacts[0].point.x == doctest::Approx(1.0f));

    // With its centre inside, it is pushed out through the nearest face.
    contacts.clear();
    world.bodies()[0].position = {0.9f, 0.1f, 0.0f};
    REQUIRE(collideSphereBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
    CHECK(contacts[0].normal.x == doctest::Approx(-1.0f));
    CHECK(contacts[0].depth == doctest::Approx(0.6f));

    // A turned box.
    contacts.clear();
    world.bodies()[1].orientation = aboutY(45.0f);
    world.bodies()[0].position = {1.6f, 0.0f, 0.0f};  // the box's corner now reaches 1.414 along x
    REQUIRE(collideSphereBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
    CHECK(contacts[0].depth == doctest::Approx(0.5f - (1.6f - std::sqrt(2.0f))).epsilon(0.01));

    contacts.clear();
    world.bodies()[0].position = {3.0f, 0.0f, 0.0f};
    CHECK(collideSphereBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 0);
}

TEST_CASE("physics: box against box") {
    PhysicsWorld world;
    world.addBox({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    world.addBox({0.0f, 1.9f, 0.0f}, {1.0f, 1.0f, 1.0f});
    std::vector<Contact> contacts;

    SUBCASE("face on face: four contacts") {
        REQUIRE(collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 4);
        for (const Contact& contact : contacts) {
            CHECK(contact.normal.y == doctest::Approx(1.0f));
            CHECK(contact.depth == doctest::Approx(0.1f));
            CHECK(std::abs(contact.point.x) == doctest::Approx(1.0f));
            CHECK(std::abs(contact.point.z) == doctest::Approx(1.0f));
            CHECK(contact.point.y == doctest::Approx(0.95f));
        }
    }
    SUBCASE("offset: the contact area is the overlap of the two faces") {
        world.bodies()[1].position.x = 1.5f;
        REQUIRE(collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 4);
        float lo = 10.0f, hi = -10.0f;
        for (const Contact& contact : contacts) lo = std::min(lo, contact.point.x), hi = std::max(hi, contact.point.x);
        CHECK(lo == doctest::Approx(0.5f));
        CHECK(hi == doctest::Approx(1.0f));
    }
    SUBCASE("turned about the vertical: an octagon") {
        world.bodies()[1].orientation = aboutY(45.0f);
        const int count = collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts);
        CHECK(count == 8);
        for (const Contact& contact : contacts) CHECK(contact.normal.y == doctest::Approx(1.0f));
    }
    SUBCASE("apart") {
        world.bodies()[1].position.y = 2.1f;
        CHECK(collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 0);
        // Close along every box axis, but separated diagonally.
        world.bodies()[1].position = {1.9f, 1.9f, 0.0f};
        world.bodies()[1].orientation = Quat::fromAxisAngle({0.0f, 0.0f, 1.0f}, radians(45.0f));
        CHECK(collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 0);
    }
    SUBCASE("a corner into a face: one contact") {
        // Stand the upper box on a corner: turn its diagonal (1, 1, 1)
        // to point straight up.
        world.bodies()[1].orientation =
            Quat::fromAxisAngle(normalize(Vec3{1.0f, 0.0f, -1.0f}), std::acos(1.0f / std::sqrt(3.0f)));
        world.bodies()[1].position = {0.0f, 1.0f + std::sqrt(3.0f) - 0.05f, 0.0f};
        REQUIRE(collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
        CHECK(contacts[0].normal.y == doctest::Approx(1.0f));
        CHECK(contacts[0].depth == doctest::Approx(0.05f).epsilon(0.05));
        CHECK(contacts[0].point.x == doctest::Approx(0.0f).epsilon(0.01));
    }
    SUBCASE("edge across edge") {
        // Two edges crossing at right angles, each box turned 45 degrees
        // about a different horizontal axis.
        world.bodies()[0].orientation = Quat::fromAxisAngle({0.0f, 0.0f, 1.0f}, radians(45.0f));
        world.bodies()[1].orientation = Quat::fromAxisAngle({1.0f, 0.0f, 0.0f}, radians(45.0f));
        world.bodies()[1].position = {0.0f, 2.0f * std::sqrt(2.0f) - 0.1f, 0.0f};
        REQUIRE(collideBoxBox(world.bodies()[0], world.bodies()[1], 0, 1, contacts) == 1);
        CHECK(contacts[0].normal.y == doctest::Approx(1.0f));
        CHECK(contacts[0].depth == doctest::Approx(0.1f).epsilon(0.02));
        CHECK(contacts[0].point.x == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(contacts[0].point.z == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(contacts[0].point.y == doctest::Approx(std::sqrt(2.0f) - 0.05f).epsilon(0.01));
    }
}

TEST_CASE("physics: a ball bounces lower each time and comes to rest") {
    PhysicsWorld world;
    world.addSphere({0.0f, 3.0f, 0.0f}, 0.5f);
    world.bodies()[0].restitution = 0.5f;
    world.groundRestitution = 0.5f;

    float peak = 0.0f, previousPeak = 3.0f;
    bool rising = false;
    int bounces = 0;
    for (int i = 0; i < 120 * 6; ++i) {
        world.step();
        const Body& ball = world.bodies()[0];
        CHECK(ball.position.y > 0.45f);  // never through the floor
        if (ball.velocity.y > 0.0f) rising = true, peak = std::max(peak, ball.position.y);
        else if (rising) {
            // Just past the top of a bounce.
            CHECK(peak < previousPeak);
            previousPeak = peak;
            peak = 0.0f;
            rising = false;
            ++bounces;
        }
    }
    CHECK(bounces >= 2);
    const Body& ball = world.bodies()[0];
    CHECK(ball.position.y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(ball.asleep);
    CHECK(!world.moving());
}

TEST_CASE("physics: a crate lands flat and stays put") {
    PhysicsWorld world;
    // Dropped at an angle, so it has to tumble onto a face.
    world.addBox({0.0f, 2.0f, 0.0f}, {0.5f, 0.5f, 0.5f},
                 normalize(Quat::fromAxisAngle({1.0f, 0.0f, 0.3f}, radians(25.0f))));
    run(world, 5.0f);
    const Body& crate = world.bodies()[0];
    CHECK(crate.position.y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(crate.asleep);
    // One of its axes points straight up (either way).
    float upright = 0.0f;
    for (Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}})
        upright = std::max(upright, std::abs(rotate(crate.orientation, axis).y));
    CHECK(upright > 0.999f);
}

TEST_CASE("physics: a stack of crates stands") {
    PhysicsWorld world;
    for (int i = 0; i < 4; ++i) world.addBox({0.0f, 0.5f + static_cast<float>(i) * 1.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    run(world, 6.0f);
    for (int i = 0; i < 4; ++i) {
        const Body& crate = world.bodies()[static_cast<std::size_t>(i)];
        CHECK(crate.position.y == doctest::Approx(0.5f + static_cast<float>(i)).epsilon(0.03));
        CHECK(std::abs(crate.position.x) < 0.05f);
        CHECK(std::abs(crate.position.z) < 0.05f);
    }
    CHECK(!world.moving());
}

TEST_CASE("physics: collisions conserve momentum") {
    PhysicsWorld world;
    world.hasGround = false;
    world.gravity = {};
    world.addSphere({-2.0f, 0.0f, 0.0f}, 0.5f, 1000.0f);
    world.addSphere({2.0f, 0.1f, 0.0f}, 0.7f, 400.0f);
    world.bodies()[0].velocity = {3.0f, 0.0f, 0.0f};
    world.bodies()[1].velocity = {-2.0f, 0.0f, 0.0f};
    const auto momentum = [&] {
        Vec3 total;
        for (const Body& body : world.bodies()) total += body.velocity / body.inverseMass;
        return total;
    };
    const Vec3 before = momentum();
    const float energyBefore = world.kineticEnergy();
    run(world, 2.0f);
    const Vec3 after = momentum();
    // (Less the little that drag takes.)
    CHECK(after.x == doctest::Approx(before.x).epsilon(0.05));
    CHECK(std::abs(after.y - before.y) < std::abs(before.x) * 0.02f);
    // They did collide, and no energy came from nowhere.
    CHECK(world.bodies()[0].velocity.x < 2.0f);
    CHECK(world.kineticEnergy() < energyBefore);
    // ...and they are moving apart.
    CHECK(world.bodies()[1].position.x - world.bodies()[0].position.x > 1.2f);
}

TEST_CASE("physics: a head-on collision between equal elastic balls swaps their speeds") {
    PhysicsWorld world;
    world.hasGround = false;
    world.gravity = {};
    world.addSphere({-1.5f, 0.0f, 0.0f}, 0.5f);
    world.addSphere({1.5f, 0.0f, 0.0f}, 0.5f);
    for (Body& body : world.bodies()) body.restitution = 1.0f, body.friction = 0.0f;
    world.bodies()[0].velocity = {4.0f, 0.0f, 0.0f};
    run(world, 1.0f);
    CHECK(world.bodies()[0].velocity.x == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(std::abs(world.bodies()[0].velocity.x) < 0.2f);
    CHECK(world.bodies()[1].velocity.x == doctest::Approx(4.0f).epsilon(0.06));
}

TEST_CASE("physics: friction turns sliding into rolling") {
    PhysicsWorld world;
    world.addSphere({0.0f, 0.5f, 0.0f}, 0.5f);
    world.bodies()[0].velocity = {4.0f, 0.0f, 0.0f};
    run(world, 2.0f);
    const Body& ball = world.bodies()[0];
    CHECK(ball.velocity.x > 1.0f);
    CHECK(ball.velocity.x < 4.0f);  // some of the motion went into spin
    // Rolling without slipping: the contact point is still, so v = w r. A
    // ball moving along +x rolls about -z.
    CHECK(-ball.angularVelocity.z * 0.5f == doctest::Approx(ball.velocity.x).epsilon(0.1));

    // A crate on the same floor just slides to a stop.
    PhysicsWorld other;
    other.addBox({0.0f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f});
    other.bodies()[0].velocity = {3.0f, 0.0f, 0.0f};
    run(other, 4.0f);
    CHECK(std::abs(other.bodies()[0].velocity.x) < 0.05f);
    CHECK(other.bodies()[0].position.x > 0.2f);
    CHECK(other.bodies()[0].position.x < 3.0f);
}

TEST_CASE("physics: things fall off the edge of a floor that has one") {
    PhysicsWorld world;
    world.groundExtent = 2.0f;
    world.addSphere({0.0f, 0.5f, 0.0f}, 0.5f);
    world.addBox({1.0f, 0.5f, 1.0f}, {0.4f, 0.4f, 0.4f});
    world.bodies()[0].velocity = {5.0f, 0.0f, 0.0f};  // rolling for the edge
    run(world, 3.0f);
    CHECK(world.bodies()[0].position.x > 2.0f);
    CHECK(world.bodies()[0].position.y < -5.0f);
    // The crate that stayed where it was is still on the floor.
    CHECK(world.bodies()[1].position.y == doctest::Approx(0.4f).epsilon(0.05));
}

TEST_CASE("physics: a static body is a wall") {
    PhysicsWorld world;
    world.gravity = {};
    world.hasGround = false;
    const int wall = world.addBox({3.0f, 0.0f, 0.0f}, {0.5f, 3.0f, 3.0f}, {}, 0.0f);
    world.addSphere({0.0f, 0.0f, 0.0f}, 0.5f);
    world.bodies()[1].velocity = {6.0f, 0.0f, 0.0f};
    world.bodies()[1].restitution = 0.8f;
    run(world, 1.5f);
    CHECK(world.bodies()[1].velocity.x < -2.0f);  // it came back
    CHECK(world.bodies()[1].position.x < 2.0f);   // and never got through
    const Body& fixed = world.bodies()[static_cast<std::size_t>(wall)];
    CHECK(fixed.position.x == 3.0f);
    CHECK(lengthSquared(fixed.velocity) == 0.0f);
}

TEST_CASE("physics: advance() takes fixed steps whatever the frame rate") {
    PhysicsWorld coarse, fine;
    coarse.addSphere({0.0f, 5.0f, 0.0f}, 0.5f);
    fine.addSphere({0.0f, 5.0f, 0.0f}, 0.5f);
    for (int i = 0; i < 30; ++i) coarse.advance(1.0f / 30.0f);
    for (int i = 0; i < 240; ++i) fine.advance(1.0f / 240.0f);
    CHECK(coarse.bodies()[0].position.y == doctest::Approx(fine.bodies()[0].position.y).epsilon(0.01));
    // A long pause is not made up for all at once.
    PhysicsWorld paused;
    paused.addSphere({0.0f, 100.0f, 0.0f}, 0.5f);
    paused.advance(10.0f);
    CHECK(paused.bodies()[0].position.y > 99.9f);
}

TEST_CASE("physics: the demo settles, with nothing through the floor or flung away") {
    Scene scene;
    auto demo = std::make_shared<PhysicsDemo>(scene, 24);
    scene.driver = demo;
    // A floor, the bodies, and the balls waiting to be thrown.
    CHECK(scene.objects.size() == 1 + 24 + PhysicsDemo::kMaxThrown);
    CHECK(scene.camera.has_value());
    CHECK(!scene.lights.empty());
    CHECK(scene.playing());

    const float energyAtStart = demo->world().kineticEnergy();
    CHECK(energyAtStart < 1e4f);
    bool moved = false;
    for (int i = 0; i < 60 * 30 && scene.playing(); ++i) moved = scene.update(1.0f / 60.0f) || moved;
    CHECK(moved);
    CHECK(!scene.playing());  // everything came to rest within half a minute

    int visible = 0;
    for (std::size_t i = 0; i < demo->world().bodies().size(); ++i) {
        const Body& body = demo->world().bodies()[i];
        if (body.position.y < -50.0f) continue;  // parked, or fell off the edge
        ++visible;
        CHECK(body.position.y > 0.0f);
        CHECK(body.position.y < 12.0f);
        CHECK(std::abs(body.position.x) < 40.0f);
        CHECK(std::isfinite(body.orientation.w));
    }
    CHECK(visible >= 20);

    // The scene's objects are where the bodies are.
    const Bounds bounds = scene.bounds();
    CHECK(bounds.min.y > -1.0f);
    CHECK(bounds.max.y < 12.0f);

    // Throwing a ball wakes things up again...
    demo->poke({0.0f, 2.0f, 12.0f}, {0.0f, -0.1f, -1.0f});
    CHECK(scene.playing());
    const std::size_t objects = scene.objects.size();
    for (int i = 0; i < 60 * 2; ++i) scene.update(1.0f / 60.0f);
    CHECK(scene.objects.size() == objects);  // thrown balls are reused, not added

    // ...and reset puts everything back exactly.
    demo->reset();
    Scene fresh;
    PhysicsDemo again(fresh, 24);
    for (std::size_t i = 0; i < demo->world().bodies().size(); ++i) {
        CHECK(demo->world().bodies()[i].position.x == again.world().bodies()[i].position.x);
        CHECK(demo->world().bodies()[i].position.y == again.world().bodies()[i].position.y);
    }
    CHECK(scene.playing());
}
