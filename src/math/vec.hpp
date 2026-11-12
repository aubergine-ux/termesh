// Vectors: Vec2, Vec3, Vec4.
//
// Plain aggregates of floats with the usual component-wise operators. Nothing
// here is clever on purpose: the compiler vectorises these well enough, and
// the hot inner loop of the rasterizer uses the explicit SIMD wrappers in
// raster/simd.hpp instead.
#pragma once

#include <algorithm>
#include <cmath>

namespace termesh {

inline constexpr float kPi = 3.14159265358979323846f;

constexpr float radians(float degrees) { return degrees * (kPi / 180.0f); }
constexpr float degrees(float radians) { return radians * (180.0f / kPi); }

template <typename T>
constexpr T lerp(const T& a, const T& b, float t) {
    return a + (b - a) * t;
}

constexpr float saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// ---------------------------------------------------------------- Vec2 ----

struct Vec2 {
    float x = 0.0f, y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr explicit Vec2(float s) : x(s), y(s) {}

    constexpr float& operator[](int i) { return i == 0 ? x : y; }
    constexpr float operator[](int i) const { return i == 0 ? x : y; }
    constexpr bool operator==(const Vec2&) const = default;
};

constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator*(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }
constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
constexpr Vec2 operator*(float s, Vec2 a) { return a * s; }
constexpr Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }
constexpr Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }
constexpr Vec2& operator+=(Vec2& a, Vec2 b) { return a = a + b; }
constexpr Vec2& operator-=(Vec2& a, Vec2 b) { return a = a - b; }
constexpr Vec2& operator*=(Vec2& a, float s) { return a = a * s; }

constexpr float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
// The 2D "cross product": the z component of the 3D cross product of the two
// vectors lifted into the plane. It is twice the signed area of the triangle
// (0, a, b) and is the building block of the rasterizer's edge functions.
constexpr float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline float length(Vec2 a) { return std::sqrt(dot(a, a)); }

// ---------------------------------------------------------------- Vec3 ----

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}
    constexpr Vec3(Vec2 xy, float z_) : x(xy.x), y(xy.y), z(z_) {}

    constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr bool operator==(const Vec3&) const = default;
    constexpr Vec2 xy() const { return {x, y}; }
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr Vec3 operator*(float s, Vec3 a) { return a * s; }
constexpr Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
constexpr Vec3& operator+=(Vec3& a, Vec3 b) { return a = a + b; }
constexpr Vec3& operator-=(Vec3& a, Vec3 b) { return a = a - b; }
constexpr Vec3& operator*=(Vec3& a, float s) { return a = a * s; }
constexpr Vec3& operator*=(Vec3& a, Vec3 b) { return a = a * b; }

constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
constexpr float lengthSquared(Vec3 a) { return dot(a, a); }
// False if any component is infinite or not a number.
inline bool isFinite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// Normalise, returning `fallback` for vectors too short to have a meaningful
// direction (degenerate triangles, zero normals in sloppy model files, ...).
inline Vec3 normalize(Vec3 a, Vec3 fallback = {0.0f, 0.0f, 1.0f}) {
    const float len2 = dot(a, a);
    if (len2 < 1e-20f) return fallback;
    return a * (1.0f / std::sqrt(len2));
}

// Mirror the incident direction `i` about the unit normal `n`.
constexpr Vec3 reflect(Vec3 i, Vec3 n) { return i - n * (2.0f * dot(i, n)); }

constexpr Vec3 min(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
constexpr Vec3 max(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
constexpr Vec3 saturate(Vec3 a) { return {saturate(a.x), saturate(a.y), saturate(a.z)}; }

// ---------------------------------------------------------------- Vec4 ----

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr explicit Vec4(float s) : x(s), y(s), z(s), w(s) {}
    constexpr Vec4(Vec3 xyz, float w_) : x(xyz.x), y(xyz.y), z(xyz.z), w(w_) {}

    constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    constexpr bool operator==(const Vec4&) const = default;
    constexpr Vec3 xyz() const { return {x, y, z}; }
    constexpr Vec2 xy() const { return {x, y}; }
};

constexpr Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
constexpr Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
constexpr Vec4 operator*(Vec4 a, Vec4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
constexpr Vec4 operator*(Vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
constexpr Vec4 operator*(float s, Vec4 a) { return a * s; }
constexpr Vec4 operator/(Vec4 a, float s) { return {a.x / s, a.y / s, a.z / s, a.w / s}; }
constexpr Vec4 operator-(Vec4 a) { return {-a.x, -a.y, -a.z, -a.w}; }
constexpr Vec4& operator+=(Vec4& a, Vec4 b) { return a = a + b; }

constexpr float dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

}  // namespace termesh
