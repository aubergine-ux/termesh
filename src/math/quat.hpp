// Quat: unit quaternions for rotations.
//
// A unit quaternion q = (v * sin(a/2), cos(a/2)) represents a rotation of
// angle `a` about the unit axis `v`. Compared with Euler angles they compose
// without gimbal lock, and compared with matrices they are cheap to
// renormalise and easy to interpolate - which is why the orbit camera stores
// its orientation as one.
#pragma once

#include "math/mat.hpp"
#include "math/vec.hpp"

namespace termesh {

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;  // identity rotation

    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr bool operator==(const Quat&) const = default;

    static Quat fromAxisAngle(Vec3 axis, float angle) {
        const Vec3 a = normalize(axis) * std::sin(angle * 0.5f);
        return {a.x, a.y, a.z, std::cos(angle * 0.5f)};
    }
};

// Hamilton product. `a * b` applies b first, then a - the same order as
// matrix multiplication.
constexpr Quat operator*(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

constexpr float dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// For unit quaternions the conjugate is the inverse rotation.
constexpr Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat normalize(Quat q) {
    const float len2 = dot(q, q);
    if (len2 < 1e-20f) return {};
    const float inv = 1.0f / std::sqrt(len2);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Rotate a vector: v' = q v q*. Expanded into the equivalent
// v + 2w(u x v) + 2 u x (u x v), with u the quaternion's vector part, which
// avoids building two full quaternion products.
constexpr Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

constexpr Mat4 toMat4(Quat q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {
        {1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f},
        {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f},
        {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
}

// Spherical linear interpolation: constant angular velocity from a to b.
inline Quat slerp(Quat a, Quat b, float t) {
    float cosTheta = dot(a, b);
    // q and -q are the same rotation; pick the sign that takes the short way.
    if (cosTheta < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosTheta = -cosTheta;
    }
    float wa, wb;
    if (cosTheta > 0.9995f) {
        // Nearly parallel: sin(theta) is too small to divide by, and a plain
        // lerp is indistinguishable from the arc at this angle.
        wa = 1.0f - t;
        wb = t;
    } else {
        const float theta = std::acos(cosTheta);
        const float invSin = 1.0f / std::sin(theta);
        wa = std::sin((1.0f - t) * theta) * invSin;
        wb = std::sin(t * theta) * invSin;
    }
    return normalize(Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

}  // namespace termesh
