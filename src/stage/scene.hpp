// Scene: several models, each with its own place, plus the lights, the
// environment and the camera that go with them.
//
// A scene can be put together in code, from the files named on the command
// line, or read from a scene file (see scene_file.hpp).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "accel/model_accel.hpp"
#include "anim/player.hpp"
#include "core/json.hpp"
#include "scene/environment.hpp"
#include "scene/light.hpp"
#include "stage/camera_path.hpp"

namespace termesh {

// One model in the scene.
struct SceneObject {
    std::string name;
    std::string source;  // where it came from: a path, or "demo:<name>"
    Model model;
    Mat4 transform;    // model space -> world space
    ModelAccel accel;  // boxes, BVHs and (optionally) levels of detail
    // The model's animation, if it has any. Null for a static model.
    std::unique_ptr<AnimationPlayer> player;
    bool visible = true;
};

class Scene {
public:
    Scene() = default;
    // Objects refer to each other's storage (players point at rigs), so a
    // scene is moved around but never copied.
    Scene(Scene&&) = default;
    Scene& operator=(Scene&&) = default;

    std::string name;
    // Each object lives at a fixed address for as long as the scene does.
    std::vector<std::unique_ptr<SceneObject>> objects;

    std::vector<Light> lights;  // empty: the renderer's default rig
    Environment environment;
    bool hasEnvironment = false;  // the scene chose one (rather than the default)
    std::optional<bool> skybox;   // unset: show the environment if it is a panorama
    float skyBlur = 0.0f;

    std::optional<CameraPose> camera;  // a fixed view to start from
    CameraPath cameraPath;             // empty: none

    // The "render" block of a scene file, for the application to apply as
    // its defaults. Null when there was none.
    Json renderSettings;

    // Add a model. Builds its acceleration data and, if it is animated,
    // its player. The returned reference stays valid.
    SceneObject& add(Model model, std::string objectName, const Mat4& transform = Mat4::identity(),
                     ThreadPool* pool = nullptr);

    // Everything visible, ready for the renderer.
    std::vector<DrawItem> drawItems() const;
    // Which object a draw item came from (items skip invisible objects).
    int objectOfItem(int item) const;
    int itemOfObject(int object) const;

    // A world-space box around the visible objects, including everywhere
    // their animations reach.
    Bounds bounds() const;

    std::size_t triangleCount() const;
    std::size_t vertexCount() const;
    bool animated() const;
    // Whether any animation is currently running (playing or cross-fading).
    bool playing() const;

    // Move every animation on by `dt` seconds and re-pose the models that
    // changed. Returns true if anything moved.
    bool update(float dt, ThreadPool* pool = nullptr);
    // Re-pose everything at its current time (after seeking, say).
    void pose(ThreadPool* pool = nullptr);

    // Build levels of detail for every object.
    void buildLods(ThreadPool* pool = nullptr);

    // Stand the objects side by side: each scaled to the same size, resting
    // on the ground, in a row along x. For models that were not made to
    // share a scene (several files on a command line).
    void arrangeInRow();
};

}  // namespace termesh
