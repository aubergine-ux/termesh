// Cameras: the matrices the renderer needs, and an orbit controller that
// produces them.
#pragma once

#include <algorithm>

#include "math/mat.hpp"
#include "math/quat.hpp"
#include "scene/model.hpp"

namespace termesh {

// Everything the renderer needs to know about the viewer.
struct Camera {
    Mat4 view;
    Mat4 projection;
    Vec3 position;  // world space
};

// A camera that orbits a target point, turntable style: yaw spins around the
// world's up axis and pitch tilts towards the poles.
class OrbitCamera {
public:
    Vec3 target{0.0f, 0.0f, 0.0f};
    float distance = 3.0f;
    float yaw = 0.0f;    // radians, around world +Y
    float pitch = 0.0f;  // radians, positive looks down on the model
    float fovY = radians(40.0f);

    // Position the camera so a model with these bounds fills the view.
    void frame(const Bounds& bounds) {
        target = bounds.center();
        sceneCenter_ = bounds.center();
        sceneRadius_ = std::max(bounds.radius(), 1e-4f);
        // The bounding sphere just touches the edges of the view cone.
        distance = sceneRadius_ / std::sin(fovY * 0.5f) * 1.02f;
        yaw = radians(30.0f);
        pitch = radians(15.0f);
    }

    // The orientation is built from two quaternions: pitch about the
    // camera's own X axis, then yaw about the world's Y axis. Composing them
    // this way keeps the horizon level however far you orbit.
    Quat orientation() const {
        return Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, yaw) *
               Quat::fromAxisAngle({1.0f, 0.0f, 0.0f}, -pitch);
    }

    Vec3 position() const { return target + rotate(orientation(), {0.0f, 0.0f, distance}); }

    void orbit(float deltaYaw, float deltaPitch) {
        yaw += deltaYaw;
        // Stop just short of the poles, where "up" becomes ambiguous.
        const float limit = radians(89.0f);
        pitch = std::clamp(pitch + deltaPitch, -limit, limit);
    }

    // Slide the target in the camera's image plane. Units are fractions of
    // the view height, so a drag feels the same at any zoom level.
    void pan(float right, float up) {
        const Quat q = orientation();
        const float viewHeight = 2.0f * distance * std::tan(fovY * 0.5f);
        target += rotate(q, {1.0f, 0.0f, 0.0f}) * (right * viewHeight);
        target += rotate(q, {0.0f, 1.0f, 0.0f}) * (up * viewHeight);
    }

    // factor < 1 moves closer.
    void zoom(float factor) {
        distance = std::clamp(distance * factor, sceneRadius_ * 0.02f, sceneRadius_ * 100.0f);
    }

    // `aspect` is the width / height of the image as displayed.
    Camera camera(float aspect) const {
        Camera cam;
        cam.position = position();
        // The view matrix is the inverse of the camera's placement in the
        // world: undo the translation, then undo the rotation.
        cam.view = toMat4(conjugate(orientation())) * translate(-cam.position);

        // Fit the near and far planes around the model. A tight depth range
        // spends the depth buffer's precision where the geometry is.
        const float toCenter = length(sceneCenter_ - cam.position);
        const float zFar = toCenter + sceneRadius_ * 1.05f;
        const float zNear = std::max(toCenter - sceneRadius_ * 1.05f, zFar * 1e-3f);
        cam.projection = perspective(fovY, aspect, zNear, zFar);
        return cam;
    }

private:
    Vec3 sceneCenter_{0.0f, 0.0f, 0.0f};
    float sceneRadius_ = 1.0f;
};

}  // namespace termesh
