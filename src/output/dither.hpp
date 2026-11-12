// Dithering: making a few colours look like many.
//
// Most terminal output modes cannot show the colour the renderer computed:
// sixel has a 253-colour palette, a 256-colour terminal has a fixed cube,
// ASCII has a dozen glyph densities and Braille has exactly two states per
// dot. Rounding each pixel to the nearest available value turns smooth
// gradients into flat bands. Dithering trades that banding for fine noise:
// neighbouring pixels are rounded in different directions so that their
// *average* is right.
//
//   ordered           add a small position-dependent offset (a "Bayer"
//                     threshold pattern) before rounding. Each pixel is
//                     independent, so the pattern holds still from frame to
//                     frame: the right choice for animation.
//   Floyd-Steinberg   round a pixel, then push the rounding error onto the
//                     neighbours that have not been visited yet. Sharper
//                     and closer to the true image, but the noise depends
//                     on everything before it and crawls when the picture
//                     moves: the right choice for stills.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/image.hpp"

namespace termesh {

enum class DitherMode {
    Auto,  // whatever suits the backend: see each backend's notes
    None,
    Ordered,
    FloydSteinberg,
};

inline const char* ditherModeName(DitherMode mode) {
    switch (mode) {
        case DitherMode::None: return "none";
        case DitherMode::Ordered: return "ordered";
        case DitherMode::FloydSteinberg: return "floyd-steinberg";
        default: return "auto";
    }
}

inline std::optional<DitherMode> ditherModeFromName(std::string_view name) {
    if (name == "auto") return DitherMode::Auto;
    if (name == "none" || name == "off") return DitherMode::None;
    if (name == "ordered" || name == "bayer") return DitherMode::Ordered;
    if (name == "fs" || name == "floyd-steinberg" || name == "diffusion") return DitherMode::FloydSteinberg;
    return std::nullopt;
}

// What Auto means for a backend that is better with dithering than without:
// stable noise while the picture moves, error diffusion for a single frame.
inline DitherMode resolveDither(DitherMode mode, bool still) {
    if (mode != DitherMode::Auto) return mode;
    return still ? DitherMode::FloydSteinberg : DitherMode::Ordered;
}

// The 4x4 Bayer matrix as thresholds in (-0.5, 0.5). Its sixteen values are
// arranged so that any 2x2 block contains a spread of low and high ones: the
// pattern has no visible structure coarser than a pixel or two.
inline float bayerThreshold(int x, int y) {
    static constexpr int matrix[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return (static_cast<float>(matrix[y & 3][x & 3]) + 0.5f) / 16.0f - 0.5f;
}

// Reduce `src` to the colours `quantize` can produce, writing into `dst`.
//
//   quantize   Color8 -> Color8: the nearest representable colour
//   spread     roughly how far apart representable levels are, in 0-255
//              units; sets the amplitude of the ordered pattern
//
// Pixels with alpha 0 (background) pass through untouched and take no part
// in error diffusion, so the empty area around a model stays perfectly flat.
template <typename Quantize>
void ditherImage(ImageView src, DitherMode mode, float spread, Quantize&& quantize, Image& dst) {
    dst.resize(src.width, src.height);
    auto clampByte = [](float v) { return static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); };

    if (mode != DitherMode::FloydSteinberg) {
        for (int y = 0; y < src.height; ++y) {
            const Color8* in = src.row(y);
            Color8* out = dst.row(y);
            for (int x = 0; x < src.width; ++x) {
                if (in[x].a == 0) {
                    out[x] = in[x];
                    continue;
                }
                const float offset = mode == DitherMode::Ordered ? bayerThreshold(x, y) * spread : 0.0f;
                Color8 c = quantize(Color8{clampByte(static_cast<float>(in[x].r) + offset),
                                           clampByte(static_cast<float>(in[x].g) + offset),
                                           clampByte(static_cast<float>(in[x].b) + offset), in[x].a});
                c.a = in[x].a;
                out[x] = c;
            }
        }
        return;
    }

    // Floyd-Steinberg. The error of each pixel is shared out to the four
    // neighbours still to come, in these proportions:
    //
    //             *    7/16
    //     3/16   5/16  1/16
    //
    // Rows are walked alternately left-to-right and right-to-left
    // ("serpentine"), which stops the error from always drifting the same
    // way and forming diagonal streaks. Only two rows of error are ever
    // needed: this one and the next.
    const std::size_t width = static_cast<std::size_t>(src.width);
    std::vector<float> current((width + 2) * 3, 0.0f), next((width + 2) * 3, 0.0f);
    for (int y = 0; y < src.height; ++y) {
        const Color8* in = src.row(y);
        Color8* out = dst.row(y);
        const bool reverse = y & 1;
        std::fill(next.begin(), next.end(), 0.0f);
        for (int i = 0; i < src.width; ++i) {
            const int x = reverse ? src.width - 1 - i : i;
            if (in[x].a == 0) {
                out[x] = in[x];
                continue;
            }
            const std::size_t e = (static_cast<std::size_t>(x) + 1) * 3;  // +1: room for x - 1
            const float wanted[3] = {static_cast<float>(in[x].r) + current[e],
                                     static_cast<float>(in[x].g) + current[e + 1],
                                     static_cast<float>(in[x].b) + current[e + 2]};
            Color8 c = quantize(Color8{clampByte(wanted[0]), clampByte(wanted[1]), clampByte(wanted[2]), in[x].a});
            c.a = in[x].a;
            out[x] = c;

            const float error[3] = {wanted[0] - static_cast<float>(c.r), wanted[1] - static_cast<float>(c.g),
                                    wanted[2] - static_cast<float>(c.b)};
            const std::size_t ahead = reverse ? e - 3 : e + 3, behind = reverse ? e + 3 : e - 3;
            for (std::size_t k = 0; k < 3; ++k) {
                current[ahead + k] += error[k] * (7.0f / 16.0f);
                next[behind + k] += error[k] * (3.0f / 16.0f);
                next[e + k] += error[k] * (5.0f / 16.0f);
                next[ahead + k] += error[k] * (1.0f / 16.0f);
            }
        }
        current.swap(next);
    }
}

// The same for a single channel: reduce `values` (each in [0, 1], or
// negative for "nothing here") to integer levels 0 .. levels - 1.
inline void ditherLevels(const std::vector<float>& values, int width, int height, int levels, DitherMode mode,
                         std::vector<int>& out) {
    out.assign(values.size(), -1);
    const float top = static_cast<float>(levels - 1);
    auto round = [&](float v) { return std::clamp(static_cast<int>(v * top + 0.5f), 0, levels - 1); };

    if (mode != DitherMode::FloydSteinberg) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * width + x;
                if (values[i] < 0.0f) continue;
                const float offset = mode == DitherMode::Ordered ? bayerThreshold(x, y) / top : 0.0f;
                out[i] = round(values[i] + offset);
            }
        return;
    }

    std::vector<float> current(static_cast<std::size_t>(width) + 2, 0.0f), next(current.size(), 0.0f);
    for (int y = 0; y < height; ++y) {
        const bool reverse = y & 1;
        std::fill(next.begin(), next.end(), 0.0f);
        for (int k = 0; k < width; ++k) {
            const int x = reverse ? width - 1 - k : k;
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            if (values[i] < 0.0f) continue;
            const std::size_t e = static_cast<std::size_t>(x) + 1;
            const float wanted = values[i] + current[e];
            out[i] = round(wanted);
            const float error = wanted - static_cast<float>(out[i]) / top;
            const std::size_t ahead = reverse ? e - 1 : e + 1, behind = reverse ? e + 1 : e - 1;
            current[ahead] += error * (7.0f / 16.0f);
            next[behind] += error * (3.0f / 16.0f);
            next[e] += error * (5.0f / 16.0f);
            next[ahead] += error * (1.0f / 16.0f);
        }
        current.swap(next);
    }
}

}  // namespace termesh
