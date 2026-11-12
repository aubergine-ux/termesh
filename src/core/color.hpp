// Colour space conversion between 8-bit sRGB and linear floats.
//
// Images and terminals speak sRGB, a non-linear encoding that spends more of
// its 256 codes on dark tones. Lighting maths is only correct on linear
// values, so the renderer decodes textures to linear, shades, and encodes the
// result back to sRGB. Both directions go through lookup tables because they
// run per texel and per pixel.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "core/framebuffer.hpp"
#include "math/vec.hpp"

namespace termesh {

namespace detail {

inline float srgbDecode(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float srgbEncode(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline constexpr int kEncodeTableSize = 4096;

struct ColorTables {
    std::array<float, 256> toLinear;
    std::array<std::uint8_t, kEncodeTableSize> toSrgb;

    ColorTables() {
        for (int i = 0; i < 256; ++i) toLinear[i] = srgbDecode(static_cast<float>(i) / 255.0f);
        for (int i = 0; i < kEncodeTableSize; ++i) {
            const float linear = static_cast<float>(i) / (kEncodeTableSize - 1);
            toSrgb[i] = static_cast<std::uint8_t>(srgbEncode(linear) * 255.0f + 0.5f);
        }
    }
};

inline const ColorTables& colorTables() {
    static const ColorTables tables;
    return tables;
}

}  // namespace detail

inline float srgbToLinear(std::uint8_t c) { return detail::colorTables().toLinear[c]; }

inline std::uint8_t linearToSrgb(float c) {
    // The comparison is written so that NaN falls into the clamp as well.
    if (!(c > 0.0f)) return 0;
    if (c >= 1.0f) return 255;
    return detail::colorTables().toSrgb[static_cast<int>(c * (detail::kEncodeTableSize - 1) + 0.5f)];
}

inline Vec3 srgbToLinear(Color8 c) {
    const auto& t = detail::colorTables().toLinear;
    return {t[c.r], t[c.g], t[c.b]};
}

inline Color8 linearToSrgb(Vec3 c, std::uint8_t alpha = 255) {
    return {linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z), alpha};
}

// Perceptual brightness of an sRGB colour, 0..255 (Rec. 601 luma weights).
inline int luma(Color8 c) { return (c.r * 77 + c.g * 150 + c.b * 29) >> 8; }

}  // namespace termesh
