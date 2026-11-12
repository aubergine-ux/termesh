// PhysicsWorld: rigid bodies - spheres and boxes - falling, colliding and
// coming to rest.
//
// Each step does what every rigid-body engine does, in its simplest form:
//
//   1. apply gravity to the velocities
//   2. find every pair of bodies that overlap, and where (contacts)
//   3. change velocities so that no contact is being pushed further in
//      (the solver: sequential impulses)
//   4. move the bodies by their new velocities
//
// ARCHITECTURE.md walks through each part.
#pragma once

#include <vector>

#include "math/mat.hpp"
#include "math/quat.hpp"
#include "math/vec.hpp"

namespace termesh {

enum class ShapeType { Sphere, Box };

struct Body {
    ShapeType shape = ShapeType::Sphere;
    float radius = 0.5f;                 // spheres
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};  // boxes: half the size along each of its own axes

    Vec3 position;
    Quat orientation;
    Vec3 velocity;         // of the centre, in metres per second
    Vec3 angularVelocity;  // axis times radians per second

    // 1 / mass and 1 / moment of inertia (about each of the body's own
    // axes). Stored inverted because that is how they are used - and so
    // that a body that cannot be moved is simply one where both are zero.
    float inverseMass = 1.0f;
    Vec3 inverseInertia{1.0f, 1.0f, 1.0f};

    float restitution = 0.25f;  // bounciness: 0 = none, 1 = loses nothing
    float friction = 0.6f;

    // A body that has been still for a while stops being simulated until
    // something disturbs it.
    bool asleep = false;
    float quietTime = 0.0f;

    bool isStatic() const { return inverseMass == 0.0f; }
    // Body space -> world space.
    Mat4 transform() const { return translate(position) * toMat4(orientation); }
    // Velocity of the point of the body at `worldPoint`.
    Vec3 velocityAt(Vec3 worldPoint) const { return velocity + cross(angularVelocity, worldPoint - position); }
    float boundingRadius() const { return shape == ShapeType::Sphere ? radius : length(halfExtents); }
};

// Two bodies touching at a point.
struct Contact {
    int a = -1, b = -1;  // indices of the bodies; b is -1 for the ground
    Vec3 point;          // where, in world space
    Vec3 normal;         // unit vector from a towards b
    float depth = 0.0f;  // how far they overlap along the normal
};

class PhysicsWorld {
public:
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    // An immovable floor: everything below y = groundHeight is solid.
    bool hasGround = true;
    float groundHeight = 0.0f;
    // The floor reaches this far from the origin along x and z, and things
    // can fall off its edge. 0 means it goes on for ever.
    float groundExtent = 0.0f;
    float groundFriction = 0.7f, groundRestitution = 0.3f;

    // Add a body; `density` (kg per cubic metre) fixes its mass, and 0
    // makes it immovable. Returns its index.
    int addSphere(Vec3 position, float radius, float density = 1000.0f);
    int addBox(Vec3 position, Vec3 halfExtents, Quat orientation = {}, float density = 1000.0f);

    std::vector<Body>& bodies() { return bodies_; }
    const std::vector<Body>& bodies() const { return bodies_; }
    void clear() { bodies_.clear(), contacts_.clear(), accumulator_ = 0.0f; }

    // Advance by `dt` seconds. The simulation itself always moves in fixed
    // steps of kTimeStep, so it behaves the same at any frame rate; what is
    // left over is carried to the next call.
    void advance(float dt);
    // One fixed step.
    void step();

    // The contacts found by the last step.
    const std::vector<Contact>& contacts() const { return contacts_; }
    // True if any body is awake.
    bool moving() const;
    // Total kinetic energy, for tests: it must never grow by itself.
    float kineticEnergy() const;

    static constexpr float kTimeStep = 1.0f / 120.0f;

private:
    void findContacts();
    void solve();
    void integrate();
    void updateSleep();

    std::vector<Body> bodies_;
    std::vector<Contact> contacts_;
    float accumulator_ = 0.0f;
};

// Contact generation, one function per pair of shapes. Each appends to
// `out` and returns how many contacts it added. Normals point from the
// first body to the second.
int collideSphereSphere(const Body& a, const Body& b, int ia, int ib, std::vector<Contact>& out);
int collideSphereBox(const Body& sphere, const Body& box, int isphere, int ibox, std::vector<Contact>& out);
int collideBoxBox(const Body& a, const Body& b, int ia, int ib, std::vector<Contact>& out);
int collideGround(const Body& body, int index, float height, float extent, std::vector<Contact>& out);

}  // namespace termesh
