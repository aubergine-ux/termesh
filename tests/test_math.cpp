#include <doctest/doctest.h>

#include "math/mat.hpp"
#include "math/quat.hpp"
#include "math/vec.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

void checkVec(Vec3 actual, Vec3 expected, float eps = 1e-5f) {
    CHECK(actual.x == Approx(expected.x).epsilon(eps).scale(1.0));
    CHECK(actual.y == Approx(expected.y).epsilon(eps).scale(1.0));
    CHECK(actual.z == Approx(expected.z).epsilon(eps).scale(1.0));
}

void checkMat(const Mat4& actual, const Mat4& expected, float eps = 1e-4f) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            CAPTURE(r);
            CAPTURE(c);
            CHECK(actual(r, c) == Approx(expected(r, c)).epsilon(eps).scale(1.0));
        }
}

}  // namespace

TEST_CASE("vec: arithmetic and products") {
    const Vec3 a{1, 2, 3}, b{4, 5, 6};
    CHECK(a + b == Vec3{5, 7, 9});
    CHECK(b - a == Vec3{3, 3, 3});
    CHECK(a * 2.0f == Vec3{2, 4, 6});
    CHECK(2.0f * a == a * 2.0f);
    CHECK(-a == Vec3{-1, -2, -3});
    CHECK(dot(a, b) == 32.0f);
    CHECK(cross(Vec3{1, 0, 0}, Vec3{0, 1, 0}) == Vec3{0, 0, 1});
    CHECK(cross(a, b) == -cross(b, a));
    CHECK(dot(cross(a, b), a) == Approx(0.0f));
    CHECK(length(Vec3{3, 4, 0}) == Approx(5.0f));
    CHECK(cross(Vec2{1, 0}, Vec2{0, 1}) == 1.0f);
}

TEST_CASE("vec: normalize handles degenerate input") {
    CHECK(length(normalize(Vec3{10, -3, 2})) == Approx(1.0f));
    CHECK(normalize(Vec3{0, 0, 0}) == Vec3{0, 0, 1});
    CHECK(normalize(Vec3{0, 0, 0}, {1, 0, 0}) == Vec3{1, 0, 0});
}

TEST_CASE("vec: reflect and lerp") {
    checkVec(reflect({1, -1, 0}, {0, 1, 0}), {1, 1, 0});
    checkVec(lerp(Vec3{0, 0, 0}, Vec3{2, 4, 6}, 0.5f), {1, 2, 3});
    CHECK(saturate(1.5f) == 1.0f);
    CHECK(saturate(-0.5f) == 0.0f);
}

TEST_CASE("mat: identity and multiplication order") {
    const Mat4 t = translate({1, 2, 3});
    const Mat4 s = scale({2, 2, 2});
    checkMat(Mat4::identity() * t, t);
    // Right-to-left: scale first, then translate.
    checkVec(transformPoint(t * s, {1, 1, 1}), {3, 4, 5});
    // ...versus translate first, then scale.
    checkVec(transformPoint(s * t, {1, 1, 1}), {4, 6, 8});
    // Directions ignore translation.
    checkVec(transformDirection(t, {1, 0, 0}), {1, 0, 0});
}

TEST_CASE("mat: element access is (row, column)") {
    const Mat4 t = translate({7, 8, 9});
    CHECK(t(0, 3) == 7.0f);
    CHECK(t(1, 3) == 8.0f);
    CHECK(t(2, 3) == 9.0f);
    CHECK(transpose(t)(3, 0) == 7.0f);
}

TEST_CASE("mat: rotation is counter-clockwise about the axis") {
    checkVec(transformDirection(rotate({0, 0, 1}, radians(90.0f)), {1, 0, 0}), {0, 1, 0});
    checkVec(transformDirection(rotate({0, 1, 0}, radians(90.0f)), {0, 0, 1}), {1, 0, 0});
    checkVec(transformDirection(rotate({1, 0, 0}, radians(90.0f)), {0, 1, 0}), {0, 0, 1});
}

TEST_CASE("mat: inverse") {
    const Mat4 m = translate({1, -2, 3}) * rotate({1, 2, 3}, 0.7f) * scale({2, 3, 0.5f});
    checkMat(m * inverse(m), Mat4::identity());
    checkMat(inverse(m) * m, Mat4::identity());

    const Mat4 p = perspective(radians(60.0f), 1.5f, 0.1f, 50.0f);
    checkMat(p * inverse(p), Mat4::identity(), 1e-3f);

    // A singular matrix falls back to the identity instead of producing NaNs.
    checkMat(inverse(scale({1, 0, 1})), Mat4::identity());
}

TEST_CASE("mat: lookAt puts the target on the -Z axis") {
    const Vec3 eye{3, 4, 5}, target{-1, 0, 2};
    const Mat4 view = lookAt(eye, target, {0, 1, 0});
    checkVec(transformPoint(view, eye), {0, 0, 0});
    const Vec3 t = transformPoint(view, target);
    CHECK(t.x == Approx(0.0f).scale(1.0));
    CHECK(t.y == Approx(0.0f).scale(1.0));
    CHECK(t.z == Approx(-length(target - eye)));
    // World up stays in the upper half of the view.
    CHECK(transformDirection(view, {0, 1, 0}).y > 0.0f);
}

TEST_CASE("mat: perspective maps the frustum onto the NDC cube") {
    const float n = 0.5f, f = 20.0f;
    const Mat4 p = perspective(radians(90.0f), 2.0f, n, f);

    auto ndc = [&](Vec3 v) {
        const Vec4 clip = p * Vec4(v, 1.0f);
        return clip.xyz() / clip.w;
    };
    // Points on the near and far planes land on z = -1 and z = +1.
    CHECK(ndc({0, 0, -n}).z == Approx(-1.0f));
    CHECK(ndc({0, 0, -f}).z == Approx(1.0f));
    // With a 90 degree vertical FOV the top of the frustum is at y = distance.
    CHECK(ndc({0, 3, -3}).y == Approx(1.0f));
    // Aspect 2: the right edge is twice as far out.
    CHECK(ndc({6, 0, -3}).x == Approx(1.0f));
    // w carries the view-space distance.
    CHECK((p * Vec4(1, 2, -7, 1)).w == Approx(7.0f));
}

TEST_CASE("mat: normal matrix keeps normals perpendicular under non-uniform scale") {
    const Mat4 model = translate({5, 5, 5}) * rotate({0, 0, 1}, 0.3f) * scale({4, 1, 1});
    // A surface tangent and its normal.
    const Vec3 tangent = normalize(Vec3{1, 1, 0});
    const Vec3 normal = normalize(Vec3{-1, 1, 0});
    const Vec3 t2 = transformDirection(model, tangent);
    const Vec3 n2 = transformDirection(normalMatrix(model), normal);
    CHECK(dot(t2, n2) == Approx(0.0f).scale(1.0));
    // The naive transform does not.
    CHECK(std::abs(dot(t2, transformDirection(model, normal))) > 0.1f);
}

TEST_CASE("quat: axis-angle rotation matches the matrix") {
    const Vec3 axis = normalize(Vec3{1, 2, -1});
    const float angle = 1.1f;
    const Quat q = Quat::fromAxisAngle(axis, angle);
    const Vec3 v{0.3f, -2.0f, 1.5f};
    checkVec(rotate(q, v), transformDirection(rotate(axis, angle), v));
    checkMat(toMat4(q), rotate(axis, angle));
}

TEST_CASE("quat: composition order matches matrices") {
    const Quat a = Quat::fromAxisAngle({0, 1, 0}, 0.8f);
    const Quat b = Quat::fromAxisAngle({1, 0, 0}, -0.4f);
    checkMat(toMat4(a * b), toMat4(a) * toMat4(b));
    const Vec3 v{1, 2, 3};
    checkVec(rotate(a * b, v), rotate(a, rotate(b, v)));
}

TEST_CASE("quat: conjugate undoes the rotation") {
    const Quat q = Quat::fromAxisAngle({3, -1, 2}, 2.2f);
    const Vec3 v{1, 2, 3};
    checkVec(rotate(conjugate(q), rotate(q, v)), v);
    const Quat i = q * conjugate(q);
    CHECK(i.w == Approx(1.0f));
    CHECK(i.x == Approx(0.0f).scale(1.0));
}

TEST_CASE("quat: slerp") {
    const Quat a = Quat::fromAxisAngle({0, 1, 0}, 0.0f);
    const Quat b = Quat::fromAxisAngle({0, 1, 0}, radians(90.0f));
    const Quat mid = slerp(a, b, 0.5f);
    checkVec(rotate(mid, {0, 0, 1}), rotate(Quat::fromAxisAngle({0, 1, 0}, radians(45.0f)), {0, 0, 1}));
    checkVec(rotate(slerp(a, b, 0.0f), {1, 0, 0}), rotate(a, {1, 0, 0}));
    checkVec(rotate(slerp(a, b, 1.0f), {1, 0, 0}), rotate(b, {1, 0, 0}));

    // -b is the same rotation as b; slerp must still take the short way round.
    const Quat negB{-b.x, -b.y, -b.z, -b.w};
    checkVec(rotate(slerp(a, negB, 0.5f), {0, 0, 1}), rotate(mid, {0, 0, 1}));

    // Nearly identical inputs take the lerp path without dividing by zero.
    const Quat c = Quat::fromAxisAngle({0, 1, 0}, 1e-4f);
    const Quat r = slerp(a, c, 0.5f);
    CHECK(dot(r, r) == Approx(1.0f));
}
