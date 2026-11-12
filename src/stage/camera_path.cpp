#include "stage/camera_path.hpp"

#include <algorithm>
#include <cmath>

namespace termesh {

namespace {

// Catmull-Rom spline: the curve from p1 to p2 as t goes from 0 to 1, shaped
// by the points before and after. It passes exactly through every key and
// leaves each one heading the way the neighbours on either side suggest
// (parallel to the line from the previous key to the next), which is what
// makes a camera glide through its keyframes rather than stop at each.
template <typename T>
T catmullRom(const T& p0, const T& p1, const T& p2, const T& p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
           0.5f;
}

}  // namespace

void CameraPath::finish() {
    if (keys.empty()) return;
    std::stable_sort(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.time < b.time; });
    const float last = keys.back().time;
    if (duration > last) return;
    duration = last;
    if (loop && keys.size() > 1) duration += (last - keys.front().time) / static_cast<float>(keys.size() - 1);
    if (loop && duration <= last) duration = last + 1.0f;  // every key at the same instant
}

CameraPose CameraPath::evaluate(float time) const {
    if (keys.empty()) return {};
    const std::size_t count = keys.size();
    if (count == 1) return keys[0].pose;

    if (loop && duration > 0.0f) {
        time = std::fmod(time - keys.front().time, duration - keys.front().time);
        if (time < 0.0f) time += duration - keys.front().time;
        time += keys.front().time;
    } else {
        time = std::clamp(time, keys.front().time, keys.back().time);
    }

    // The segment containing `time`: from key `index` to the next. For a
    // loop the last segment runs from the last key back round to the first.
    std::size_t index = count - 1;
    for (std::size_t i = 0; i + 1 < count; ++i)
        if (time < keys[i + 1].time) {
            index = i;
            break;
        }
    if (index == count - 1 && !loop) return keys.back().pose;
    const float start = keys[index].time;
    const float end = index + 1 < count ? keys[index + 1].time : duration;
    const float t = end > start ? std::clamp((time - start) / (end - start), 0.0f, 1.0f) : 0.0f;

    // Keys around the segment. Off the ends, a loop wraps round and an open
    // path repeats its end key (so the curve arrives and leaves gently).
    auto key = [&](long offset) -> const CameraPose& {
        long i = static_cast<long>(index) + offset;
        const long n = static_cast<long>(count);
        if (loop) i = ((i % n) + n) % n;
        else i = std::clamp(i, 0L, n - 1);
        return keys[static_cast<std::size_t>(i)].pose;
    };
    const CameraPose &p0 = key(-1), &p1 = key(0), &p2 = key(1), &p3 = key(2);

    CameraPose out;
    if (smooth) {
        out.position = catmullRom(p0.position, p1.position, p2.position, p3.position, t);
        out.target = catmullRom(p0.target, p1.target, p2.target, p3.target, t);
        out.fov = catmullRom(p0.fov, p1.fov, p2.fov, p3.fov, t);
    } else {
        out.position = lerp(p1.position, p2.position, t);
        out.target = lerp(p1.target, p2.target, t);
        out.fov = lerp(p1.fov, p2.fov, t);
    }
    // A spline can overshoot its keys; a field of view must stay sensible.
    out.fov = std::clamp(out.fov, radians(5.0f), radians(150.0f));
    return out;
}

CameraPath makeTurntable(const Bounds& bounds, float seconds, float elevation, float distance, float fov) {
    constexpr int kKeys = 16;  // enough for the spline to be indistinguishable from a circle
    const float radius = std::max(bounds.radius(), 1e-4f) / std::sin(fov * 0.5f) * 1.02f * std::max(distance, 0.05f);
    CameraPath path;
    path.loop = true;
    path.duration = std::max(seconds, 0.1f);
    for (int i = 0; i < kKeys; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / kKeys;
        CameraKey key;
        key.time = path.duration * static_cast<float>(i) / kKeys;
        key.pose.target = bounds.center();
        key.pose.position = bounds.center() + Vec3{std::sin(angle) * std::cos(elevation), std::sin(elevation),
                                                   std::cos(angle) * std::cos(elevation)} *
                                                  radius;
        key.pose.fov = fov;
        path.keys.push_back(key);
    }
    return path;
}

Camera cameraFromPose(const CameraPose& pose, float aspect, const Bounds& bounds) {
    Camera camera;
    camera.position = pose.position;
    // Looking straight up or down, "up" needs to be something else.
    const Vec3 forward = normalize(pose.target - pose.position, {0.0f, 0.0f, -1.0f});
    const Vec3 up = std::abs(forward.y) > 0.999f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    camera.view = lookAt(pose.position, pose.position + forward, up);

    const float radius = std::max(bounds.radius(), 1e-4f);
    const float toCenter = length(bounds.center() - pose.position);
    const float zFar = toCenter + radius * 1.05f;
    const float zNear = std::max(toCenter - radius * 1.05f, zFar * 1e-3f);
    camera.projection = perspective(pose.fov, aspect, zNear, zFar);
    return camera;
}

OrbitCamera orbitFromPose(const CameraPose& pose, const Bounds& bounds) {
    OrbitCamera orbit;
    orbit.fovY = pose.fov;
    orbit.frame(bounds);  // sets the limits for zooming and the depth range
    const Vec3 offset = pose.position - pose.target;
    orbit.target = pose.target;
    orbit.distance = std::max(length(offset), 1e-4f);
    orbit.pitch = std::asin(std::clamp(offset.y / orbit.distance, -1.0f, 1.0f));
    orbit.yaw = std::atan2(offset.x, offset.z);
    const float limit = radians(89.0f);
    orbit.pitch = std::clamp(orbit.pitch, -limit, limit);
    return orbit;
}

}  // namespace termesh
