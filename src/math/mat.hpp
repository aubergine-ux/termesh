// Mat4: a 4x4 matrix, plus the transforms a 3D pipeline needs.
//
// Conventions used everywhere in termesh (they match OpenGL and glTF):
//
//   * Vectors are columns and transforms apply right-to-left:
//         clip = projection * view * model * position
//   * Storage is column-major: `c[i]` is the i-th column.
//   * World and view space are right-handed. The camera looks down -Z, +X is
//     right and +Y is up.
//   * Clip space is OpenGL-style: a point is inside the view frustum when
//         -w <= x <= w,  -w <= y <= w,  -w <= z <= w.
#pragma once

#include "math/vec.hpp"

namespace termesh {

struct Mat4 {
    Vec4 c[4];  // columns

    // Default-constructs to the identity.
    constexpr Mat4()
        : c{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}} {}
    constexpr Mat4(Vec4 c0, Vec4 c1, Vec4 c2, Vec4 c3) : c{c0, c1, c2, c3} {}

    static constexpr Mat4 identity() { return Mat4{}; }

    // Element access in the usual maths notation: m(row, column).
    constexpr float& operator()(int row, int col) { return c[col][row]; }
    constexpr float operator()(int row, int col) const { return c[col][row]; }

    constexpr Vec4 row(int r) const { return {c[0][r], c[1][r], c[2][r], c[3][r]}; }
    constexpr bool operator==(const Mat4&) const = default;
};

constexpr Vec4 operator*(const Mat4& m, Vec4 v) {
    return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z + m.c[3] * v.w;
}

constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    return {a * b.c[0], a * b.c[1], a * b.c[2], a * b.c[3]};
}

// Transform a position (w = 1). Only valid for affine matrices: the result's
// w is assumed to stay 1 and is dropped.
constexpr Vec3 transformPoint(const Mat4& m, Vec3 p) { return (m * Vec4(p, 1.0f)).xyz(); }

// Transform a direction (w = 0): translation has no effect.
constexpr Vec3 transformDirection(const Mat4& m, Vec3 d) { return (m * Vec4(d, 0.0f)).xyz(); }

constexpr Mat4 transpose(const Mat4& m) { return {m.row(0), m.row(1), m.row(2), m.row(3)}; }

constexpr Mat4 translate(Vec3 t) {
    Mat4 m;
    m.c[3] = Vec4(t, 1.0f);
    return m;
}

constexpr Mat4 scale(Vec3 s) {
    Mat4 m;
    m.c[0].x = s.x;
    m.c[1].y = s.y;
    m.c[2].z = s.z;
    return m;
}

// Rotation of `angle` radians about a unit `axis` (Rodrigues' formula),
// counter-clockwise when looking down the axis towards the origin.
inline Mat4 rotate(Vec3 axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const Vec3 a = normalize(axis);
    return {
        {t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y, 0.0f},
        {t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x, 0.0f},
        {t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
}

// View matrix for a camera at `eye` looking at `target`.
//
// The camera's basis vectors (right, up, back) become the *rows* of the
// rotation part: multiplying by the transpose of an orthonormal basis is the
// same as projecting onto it, which is what "expressing a point in camera
// coordinates" means. The translation column then moves the eye to the origin.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 back = normalize(eye - target);   // camera looks down -Z
    const Vec3 right = normalize(cross(up, back), {1.0f, 0.0f, 0.0f});
    const Vec3 trueUp = cross(back, right);
    return {
        {right.x, trueUp.x, back.x, 0.0f},
        {right.y, trueUp.y, back.y, 0.0f},
        {right.z, trueUp.z, back.z, 0.0f},
        {-dot(right, eye), -dot(trueUp, eye), -dot(back, eye), 1.0f},
    };
}

// Perspective projection.
//
//   fovY    vertical field of view in radians
//   aspect  width / height of the image as it appears on screen
//   zNear   distance to the near plane (> 0)
//   zFar    distance to the far plane (> zNear)
//
// A view-space point (x, y, z) with z < 0 in front of the camera maps to
//
//     clip.x = f / aspect * x
//     clip.y = f * y                      f = 1 / tan(fovY / 2)
//     clip.z = A * z + B
//     clip.w = -z
//
// Dividing by w is the "perspective" part: x and y shrink in proportion to
// distance. A and B are chosen so that z = -zNear lands on NDC -1 and
// z = -zFar on NDC +1. Note that depth is *not* linear in view-space z after
// the divide - it is linear in 1/z - which is exactly why depth can be
// interpolated linearly across the screen (see raster/rasterizer.cpp).
inline Mat4 perspective(float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 m;
    m.c[0] = {f / aspect, 0.0f, 0.0f, 0.0f};
    m.c[1] = {0.0f, f, 0.0f, 0.0f};
    m.c[2] = {0.0f, 0.0f, (zFar + zNear) / (zNear - zFar), -1.0f};
    m.c[3] = {0.0f, 0.0f, 2.0f * zFar * zNear / (zNear - zFar), 0.0f};
    return m;
}

// Orthographic projection of the box [l,r] x [b,t] x [-n,-f] onto the NDC cube.
constexpr Mat4 orthographic(float l, float r, float b, float t, float n, float f) {
    Mat4 m;
    m.c[0].x = 2.0f / (r - l);
    m.c[1].y = 2.0f / (t - b);
    m.c[2].z = -2.0f / (f - n);
    m.c[3] = {-(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1.0f};
    return m;
}

// General 4x4 inverse by cofactor expansion. Returns the identity for a
// singular matrix rather than propagating infinities through the pipeline.
inline Mat4 inverse(const Mat4& m) {
    const float a00 = m(0, 0), a01 = m(0, 1), a02 = m(0, 2), a03 = m(0, 3);
    const float a10 = m(1, 0), a11 = m(1, 1), a12 = m(1, 2), a13 = m(1, 3);
    const float a20 = m(2, 0), a21 = m(2, 1), a22 = m(2, 2), a23 = m(2, 3);
    const float a30 = m(3, 0), a31 = m(3, 1), a32 = m(3, 2), a33 = m(3, 3);

    // 2x2 sub-determinants of the top two and bottom two rows.
    const float s0 = a00 * a11 - a10 * a01, s1 = a00 * a12 - a10 * a02;
    const float s2 = a00 * a13 - a10 * a03, s3 = a01 * a12 - a11 * a02;
    const float s4 = a01 * a13 - a11 * a03, s5 = a02 * a13 - a12 * a03;
    const float c5 = a22 * a33 - a32 * a23, c4 = a21 * a33 - a31 * a23;
    const float c3 = a21 * a32 - a31 * a22, c2 = a20 * a33 - a30 * a23;
    const float c1 = a20 * a32 - a30 * a22, c0 = a20 * a31 - a30 * a21;

    const float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::abs(det) < 1e-30f) return Mat4::identity();
    const float inv = 1.0f / det;

    Mat4 r;
    r(0, 0) = (a11 * c5 - a12 * c4 + a13 * c3) * inv;
    r(0, 1) = (-a01 * c5 + a02 * c4 - a03 * c3) * inv;
    r(0, 2) = (a31 * s5 - a32 * s4 + a33 * s3) * inv;
    r(0, 3) = (-a21 * s5 + a22 * s4 - a23 * s3) * inv;
    r(1, 0) = (-a10 * c5 + a12 * c2 - a13 * c1) * inv;
    r(1, 1) = (a00 * c5 - a02 * c2 + a03 * c1) * inv;
    r(1, 2) = (-a30 * s5 + a32 * s2 - a33 * s1) * inv;
    r(1, 3) = (a20 * s5 - a22 * s2 + a23 * s1) * inv;
    r(2, 0) = (a10 * c4 - a11 * c2 + a13 * c0) * inv;
    r(2, 1) = (-a00 * c4 + a01 * c2 - a03 * c0) * inv;
    r(2, 2) = (a30 * s4 - a31 * s2 + a33 * s0) * inv;
    r(2, 3) = (-a20 * s4 + a21 * s2 - a23 * s0) * inv;
    r(3, 0) = (-a10 * c3 + a11 * c1 - a12 * c0) * inv;
    r(3, 1) = (a00 * c3 - a01 * c1 + a02 * c0) * inv;
    r(3, 2) = (-a30 * s3 + a31 * s1 - a32 * s0) * inv;
    r(3, 3) = (a20 * s3 - a21 * s1 + a22 * s0) * inv;
    return r;
}

// Matrix for transforming surface normals.
//
// Normals are not ordinary directions: under a non-uniform scale, transforming
// a normal with the model matrix tilts it away from perpendicular. The matrix
// that keeps normals perpendicular to their surface is the inverse transpose
// of the model matrix's upper-left 3x3. (Translation is cleared first so it
// cannot leak in through the transpose.)
inline Mat4 normalMatrix(const Mat4& model) {
    Mat4 linear = model;
    linear.c[3] = {0.0f, 0.0f, 0.0f, 1.0f};
    linear.c[0].w = linear.c[1].w = linear.c[2].w = 0.0f;
    return transpose(inverse(linear));
}

}  // namespace termesh
