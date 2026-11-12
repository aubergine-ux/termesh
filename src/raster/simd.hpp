// Four-lane SIMD wrappers: just enough for the rasterizer's coverage and
// depth tests, with SSE2, NEON and plain scalar implementations.
//
// The rasterizer processes four horizontally adjacent pixels at once. Each
// type here holds four values, one per pixel; lane 0 is the leftmost pixel.
#pragma once

#include <cstdint>

#if defined(TERMESH_NO_SIMD)
#define TERMESH_SIMD_SCALAR 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define TERMESH_SIMD_SSE2 1
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define TERMESH_SIMD_NEON 1
#include <arm_neon.h>
#else
#define TERMESH_SIMD_SCALAR 1
#endif

namespace termesh::simd {

#if defined(TERMESH_SIMD_SSE2)

inline constexpr const char* kName = "SSE2";

struct Int4 {
    __m128i v;
    static Int4 splat(std::int32_t x) { return {_mm_set1_epi32(x)}; }
    static Int4 set(std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) {
        return {_mm_setr_epi32(a, b, c, d)};
    }
    void store(std::int32_t* out) const { _mm_storeu_si128(reinterpret_cast<__m128i*>(out), v); }
};
inline Int4 operator+(Int4 a, Int4 b) { return {_mm_add_epi32(a.v, b.v)}; }
inline Int4 operator|(Int4 a, Int4 b) { return {_mm_or_si128(a.v, b.v)}; }

struct Float4 {
    __m128 v;
    static Float4 splat(float x) { return {_mm_set1_ps(x)}; }
    static Float4 load(const float* p) { return {_mm_loadu_ps(p)}; }
    static Float4 fromInt(Int4 i) { return {_mm_cvtepi32_ps(i.v)}; }
    void store(float* out) const { _mm_storeu_ps(out, v); }
};
inline Float4 operator+(Float4 a, Float4 b) { return {_mm_add_ps(a.v, b.v)}; }
inline Float4 operator-(Float4 a, Float4 b) { return {_mm_sub_ps(a.v, b.v)}; }
inline Float4 operator*(Float4 a, Float4 b) { return {_mm_mul_ps(a.v, b.v)}; }
inline Float4 operator/(Float4 a, Float4 b) { return {_mm_div_ps(a.v, b.v)}; }
// Lane-wise minimum and maximum. If a lane of `a` is NaN the result is `b`,
// the same as the scalar expressions the fallback uses.
inline Float4 min(Float4 a, Float4 b) { return {_mm_min_ps(a.v, b.v)}; }
inline Float4 max(Float4 a, Float4 b) { return {_mm_max_ps(a.v, b.v)}; }

// Bit i is set when lane i is >= 0. movemask collects the four sign bits, so
// "non-negative" is simply the complement.
inline int nonNegativeMask(Int4 a) { return ~_mm_movemask_ps(_mm_castsi128_ps(a.v)) & 0xF; }

// Bit i is set when a[i] < b[i].
inline int lessThanMask(Float4 a, Float4 b) { return _mm_movemask_ps(_mm_cmplt_ps(a.v, b.v)); }

#elif defined(TERMESH_SIMD_NEON)

inline constexpr const char* kName = "NEON";

struct Int4 {
    int32x4_t v;
    static Int4 splat(std::int32_t x) { return {vdupq_n_s32(x)}; }
    static Int4 set(std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) {
        const std::int32_t lanes[4] = {a, b, c, d};
        return {vld1q_s32(lanes)};
    }
    void store(std::int32_t* out) const { vst1q_s32(out, v); }
};
inline Int4 operator+(Int4 a, Int4 b) { return {vaddq_s32(a.v, b.v)}; }
inline Int4 operator|(Int4 a, Int4 b) { return {vorrq_s32(a.v, b.v)}; }

struct Float4 {
    float32x4_t v;
    static Float4 splat(float x) { return {vdupq_n_f32(x)}; }
    static Float4 load(const float* p) { return {vld1q_f32(p)}; }
    static Float4 fromInt(Int4 i) { return {vcvtq_f32_s32(i.v)}; }
    void store(float* out) const { vst1q_f32(out, v); }
};
inline Float4 operator+(Float4 a, Float4 b) { return {vaddq_f32(a.v, b.v)}; }
inline Float4 operator-(Float4 a, Float4 b) { return {vsubq_f32(a.v, b.v)}; }
inline Float4 operator*(Float4 a, Float4 b) { return {vmulq_f32(a.v, b.v)}; }
inline Float4 operator/(Float4 a, Float4 b) { return {vdivq_f32(a.v, b.v)}; }
// Written as compare-and-select so that a NaN lane of `a` gives `b`, as on
// the other backends (NEON's own min and max would give NaN).
inline Float4 min(Float4 a, Float4 b) { return {vbslq_f32(vcltq_f32(a.v, b.v), a.v, b.v)}; }
inline Float4 max(Float4 a, Float4 b) { return {vbslq_f32(vcgtq_f32(a.v, b.v), a.v, b.v)}; }

namespace detail {
// NEON has no movemask. Shift each lane's top bit down to bit 0, move it to
// that lane's position (1, 2, 4, 8) and sum the lanes.
inline int moveMask(uint32x4_t m) {
    const std::int32_t shifts[4] = {0, 1, 2, 3};
    return static_cast<int>(vaddvq_u32(vshlq_u32(vshrq_n_u32(m, 31), vld1q_s32(shifts))));
}
}  // namespace detail

inline int nonNegativeMask(Int4 a) { return detail::moveMask(vcgezq_s32(a.v)); }
inline int lessThanMask(Float4 a, Float4 b) { return detail::moveMask(vcltq_f32(a.v, b.v)); }

#else  // scalar fallback

inline constexpr const char* kName = "scalar";

struct Int4 {
    std::int32_t v[4];
    static Int4 splat(std::int32_t x) { return {{x, x, x, x}}; }
    static Int4 set(std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d) { return {{a, b, c, d}}; }
    void store(std::int32_t* out) const {
        for (int i = 0; i < 4; ++i) out[i] = v[i];
    }
};
// Lane arithmetic wraps like the SIMD instructions do, so go through unsigned.
inline Int4 operator+(Int4 a, Int4 b) {
    Int4 r;
    for (int i = 0; i < 4; ++i)
        r.v[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(a.v[i]) + static_cast<std::uint32_t>(b.v[i]));
    return r;
}
inline Int4 operator|(Int4 a, Int4 b) {
    Int4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] | b.v[i];
    return r;
}

struct Float4 {
    float v[4];
    static Float4 splat(float x) { return {{x, x, x, x}}; }
    static Float4 load(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
    static Float4 fromInt(Int4 i) {
        return {{static_cast<float>(i.v[0]), static_cast<float>(i.v[1]), static_cast<float>(i.v[2]),
                 static_cast<float>(i.v[3])}};
    }
    void store(float* out) const {
        for (int i = 0; i < 4; ++i) out[i] = v[i];
    }
};
inline Float4 operator+(Float4 a, Float4 b) {
    return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}
inline Float4 operator-(Float4 a, Float4 b) {
    return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
inline Float4 operator*(Float4 a, Float4 b) {
    return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}
inline Float4 operator/(Float4 a, Float4 b) {
    return {{a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]}};
}
inline Float4 min(Float4 a, Float4 b) {
    Float4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i];
    return r;
}
inline Float4 max(Float4 a, Float4 b) {
    Float4 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i];
    return r;
}

inline int nonNegativeMask(Int4 a) {
    int m = 0;
    for (int i = 0; i < 4; ++i) m |= (a.v[i] >= 0) << i;
    return m;
}
inline int lessThanMask(Float4 a, Float4 b) {
    int m = 0;
    for (int i = 0; i < 4; ++i) m |= (a.v[i] < b.v[i]) << i;
    return m;
}

#endif

}  // namespace termesh::simd
