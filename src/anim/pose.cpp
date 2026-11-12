#include "anim/pose.hpp"

#include <algorithm>

namespace termesh {

Pose restPose(const Rig& rig) {
    Pose pose;
    pose.local.resize(rig.nodes.size());
    pose.weights.resize(rig.nodes.size());
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        pose.local[i] = rig.nodes[i].rest;
        pose.weights[i] = rig.nodes[i].weights;
    }
    return pose;
}

// Keyframe interpolation.
//
// Find the two keyframes around `time`, and how far between them it lies
// (u in [0, 1)). Then:
//
//   Step         hold the earlier keyframe's value.
//   Linear       blend the two values; rotations take the arc (slerp).
//   CubicSpline  a Hermite curve: each keyframe stores the value and the
//                slope of the curve arriving at it and leaving it, so motion
//                can ease in and out instead of changing speed abruptly at
//                every keyframe.
//
//                p(u) = (2u³ - 3u² + 1) p0 + (u³ - 2u² + u) dt m0
//                     + (-2u³ + 3u²)    p1 + (u³ - u²)      dt m1
//
//                where m0 is the out-tangent of the earlier keyframe, m1 the
//                in-tangent of the later one and dt the time between them
//                (tangents are stored per second, the curve runs on u).
bool sampleChannel(const AnimationChannel& channel, float time, float* out) {
    const std::size_t keys = channel.times.size();
    const std::size_t n = static_cast<std::size_t>(channel.components);
    const bool cubic = channel.interpolation == Interpolation::CubicSpline;
    const std::size_t stride = cubic ? n * 3 : n;
    if (keys == 0 || n == 0 || channel.values.size() < keys * stride) return false;

    // For cubic splines the value sits between the two tangents.
    auto value = [&](std::size_t key) { return channel.values.data() + key * stride + (cubic ? n : 0); };
    const bool rotation = channel.path == AnimationPath::Rotation && n == 4;

    auto finish = [&] {
        if (!rotation) return;
        const Quat q = normalize(Quat{out[0], out[1], out[2], out[3]});
        out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
    };

    if (keys == 1 || time <= channel.times.front()) {
        std::copy_n(value(0), n, out);
        finish();
        return true;
    }
    if (time >= channel.times.back()) {
        std::copy_n(value(keys - 1), n, out);
        finish();
        return true;
    }

    // The first keyframe later than `time`; the one before it is at or
    // before `time`.
    const std::size_t next = static_cast<std::size_t>(
        std::upper_bound(channel.times.begin(), channel.times.end(), time) - channel.times.begin());
    const std::size_t prev = next - 1;
    const float t0 = channel.times[prev], t1 = channel.times[next];
    const float dt = t1 - t0;
    const float u = dt > 0.0f ? (time - t0) / dt : 0.0f;
    const float* p0 = value(prev);
    const float* p1 = value(next);

    switch (channel.interpolation) {
        case Interpolation::Step: std::copy_n(p0, n, out); break;
        case Interpolation::Linear:
            if (rotation) {
                const Quat q = slerp({p0[0], p0[1], p0[2], p0[3]}, {p1[0], p1[1], p1[2], p1[3]}, u);
                out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
            } else {
                for (std::size_t i = 0; i < n; ++i) out[i] = p0[i] + (p1[i] - p0[i]) * u;
            }
            break;
        case Interpolation::CubicSpline: {
            const float* m0 = p0 + n;  // out-tangent of the earlier keyframe
            const float* m1 = p1 - n;  // in-tangent of the later one
            const float u2 = u * u, u3 = u2 * u;
            const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f, h10 = u3 - 2.0f * u2 + u;
            const float h01 = -2.0f * u3 + 3.0f * u2, h11 = u3 - u2;
            for (std::size_t i = 0; i < n; ++i)
                out[i] = h00 * p0[i] + h10 * dt * m0[i] + h01 * p1[i] + h11 * dt * m1[i];
            break;
        }
    }
    finish();
    return true;
}

void sampleAnimation(const Animation& animation, float time, Pose& pose) {
    std::vector<float> weights;
    for (const AnimationChannel& channel : animation.channels) {
        if (channel.node < 0 || static_cast<std::size_t>(channel.node) >= pose.local.size()) continue;
        Transform& local = pose.local[static_cast<std::size_t>(channel.node)];
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};

        switch (channel.path) {
            case AnimationPath::Translation:
                if (channel.components == 3 && sampleChannel(channel, time, v)) local.translation = {v[0], v[1], v[2]};
                break;
            case AnimationPath::Rotation:
                if (channel.components == 4 && sampleChannel(channel, time, v))
                    local.rotation = {v[0], v[1], v[2], v[3]};
                break;
            case AnimationPath::Scale:
                if (channel.components == 3 && sampleChannel(channel, time, v)) local.scale = {v[0], v[1], v[2]};
                break;
            case AnimationPath::Weights:
                weights.resize(static_cast<std::size_t>(std::max(channel.components, 0)));
                if (!weights.empty() && sampleChannel(channel, time, weights.data()))
                    pose.weights[static_cast<std::size_t>(channel.node)] = weights;
                break;
        }
    }
}

void blendPoses(const Pose& a, const Pose& b, float t, Pose& out) {
    const std::size_t count = std::min(a.local.size(), b.local.size());
    out.local.resize(count);
    out.weights.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.local[i].translation = lerp(a.local[i].translation, b.local[i].translation, t);
        out.local[i].rotation = slerp(a.local[i].rotation, b.local[i].rotation, t);
        out.local[i].scale = lerp(a.local[i].scale, b.local[i].scale, t);

        const std::vector<float>& wa = a.weights[i];
        const std::vector<float>& wb = b.weights[i];
        std::vector<float>& w = out.weights[i];
        w.resize(std::max(wa.size(), wb.size()));
        for (std::size_t k = 0; k < w.size(); ++k) {
            const float x = k < wa.size() ? wa[k] : 0.0f;
            const float y = k < wb.size() ? wb[k] : 0.0f;
            w[k] = x + (y - x) * t;
        }
    }
}

void computeWorldMatrices(const Rig& rig, const Pose& pose, std::vector<Mat4>& world) {
    world.resize(rig.nodes.size());
    // `order` lists parents before children, so a parent's world matrix is
    // always ready by the time a child needs it.
    for (const int index : rig.order) {
        const std::size_t i = static_cast<std::size_t>(index);
        const Mat4 local = pose.local[i].matrix();
        const int parent = rig.nodes[i].parent;
        world[i] = parent >= 0 ? world[static_cast<std::size_t>(parent)] * local : local;
    }
}

}  // namespace termesh
