// CameraPath: a camera that moves by itself.
//
// A path is a list of keyframes - where the camera is, what it is looking
// at, and its field of view, at given times - and a way of filling in the
// moments between them. That is the same idea as model animation (see
// anim/pose.hpp) applied to the viewer instead of the viewed.
#pragma once

#include <vector>

#include "scene/camera.hpp"

namespace termesh {

// A camera described by where it is and what it looks at.
struct CameraPose {
    Vec3 position{0.0f, 0.0f, 5.0f};
    Vec3 target{0.0f, 0.0f, 0.0f};
    float fov = radians(40.0f);  // vertical field of view
};

struct CameraKey {
    float time = 0.0f;  // seconds
    CameraPose pose;
};

struct CameraPath {
    std::vector<CameraKey> keys;  // in ascending order of time
    // Smooth passes a curve through the keys; otherwise the camera moves in
    // straight lines between them, turning abruptly at each.
    bool smooth = true;
    // A looping path returns from its last key to its first.
    bool loop = false;
    // Total length in seconds. For a loop this includes the time taken to
    // return to the start, so it is longer than the last key's time.
    float duration = 0.0f;

    bool empty() const { return keys.empty(); }

    // Fill in `duration` if it was not given: the last key's time, plus -
    // for a loop - one more step of the average length.
    void finish();

    // The camera at a moment. Times beyond the path wrap around if it
    // loops, and stop at the ends otherwise.
    CameraPose evaluate(float time) const;
};

// A turntable: one circle around the bounds at a fixed height, looping.
//   elevation   angle above the horizontal, in radians
//   distance    1 frames the bounds exactly; larger stands further back
CameraPath makeTurntable(const Bounds& bounds, float seconds, float elevation, float distance, float fov);

// The matrices for a pose. The near and far planes are fitted around
// `bounds` so that depth precision is spent on the scene.
Camera cameraFromPose(const CameraPose& pose, float aspect, const Bounds& bounds);

// The orbit camera that shows the same view as a pose, so that control can
// pass from a path (or a fixed camera in a scene file) to the user without
// a jump.
OrbitCamera orbitFromPose(const CameraPose& pose, const Bounds& bounds);

}  // namespace termesh
