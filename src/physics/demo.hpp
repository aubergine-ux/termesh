// The physics demo: a wall of crates, things falling on it, and balls to
// throw. `termesh --demo physics`.
#pragma once

#include <cstdint>
#include <vector>

#include "physics/world.hpp"
#include "stage/scene.hpp"

namespace termesh {

// Owns a PhysicsWorld and keeps a scene's objects where its bodies are.
class PhysicsDemo : public SceneDriver {
public:
    // Fills `scene` with a floor and `bodies` crates and balls (and a
    // camera, a sun and render settings to suit). The scene must outlive
    // this object, and should hold a shared_ptr to it as its driver.
    PhysicsDemo(Scene& scene, int bodies, std::uint32_t seed = 1);

    bool update(float dt) override;
    bool active() const override { return world_.moving(); }
    void reset() override;
    // Throw a ball from `origin` along `direction`.
    void poke(Vec3 origin, Vec3 direction) override;
    const char* hint() const override { return "Enter  Backspace           throw a ball; start the simulation again"; }

    PhysicsWorld& world() { return world_; }

    static constexpr int kDefaultBodies = 28;
    static constexpr int kMaxBodies = 200;
    static constexpr int kMaxThrown = 12;

private:
    struct Link {
        int body;    // index in the world
        int object;  // index in the scene
        Mat4 fit;    // model space -> the body's own space
    };

    void place();  // put every body at its starting position
    void sync();   // copy the bodies' positions to the scene's objects
    int addCrate(Vec3 halfExtents, Vec3 tint);
    int addBall(float radius, Vec3 tint, float density);

    Scene& scene_;
    PhysicsWorld world_;
    std::vector<Link> links_;
    std::vector<Body> start_;  // the bodies as they were placed, for reset()
    int thrown_ = 0, firstThrown_ = -1;
    std::uint32_t seed_;
};

}  // namespace termesh
