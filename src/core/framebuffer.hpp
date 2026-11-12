// Framebuffer: the colour and depth images the rasterizer draws into.
//
// There are two kinds, differing only in what a colour is:
//
//   HdrFramebuffer  linear light as floats, with no upper limit. This is
//                   what the renderer shades into, so that a lamp can be a
//                   hundred times brighter than a wall and effects such as
//                   bloom and tone mapping have real intensities to work on.
//   Framebuffer     8-bit sRGB: the finished picture, ready for a terminal
//                   or an image file.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "math/vec.hpp"

namespace termesh {

// 8-bit sRGB colour. Alpha doubles as a coverage flag: 0 means "background,
// nothing was drawn here", which the Braille backend uses to decide which
// dots to light.
struct Color8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    constexpr bool operator==(const Color8&) const = default;
};

template <typename PixelType>
class BasicFramebuffer {
public:
    using Pixel = PixelType;

    BasicFramebuffer() = default;
    BasicFramebuffer(int width, int height) { resize(width, height); }

    void resize(int width, int height) {
        width_ = std::max(width, 0);
        height_ = std::max(height, 0);
        const std::size_t n = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        color_.resize(n);
        depth_.resize(n);
    }

    // Depth is in [0, 1] with 0 at the near plane, so "clear" means 1: the
    // far plane, behind everything that can be drawn.
    void clear(Pixel background) {
        std::fill(color_.begin(), color_.end(), background);
        std::fill(depth_.begin(), depth_.end(), 1.0f);
    }

    int width() const { return width_; }
    int height() const { return height_; }

    // Row 0 is the top of the image.
    Pixel* colorRow(int y) { return color_.data() + static_cast<std::size_t>(y) * width_; }
    const Pixel* colorRow(int y) const { return color_.data() + static_cast<std::size_t>(y) * width_; }
    float* depthRow(int y) { return depth_.data() + static_cast<std::size_t>(y) * width_; }
    const float* depthRow(int y) const { return depth_.data() + static_cast<std::size_t>(y) * width_; }

    Pixel& at(int x, int y) { return colorRow(y)[x]; }
    const Pixel& at(int x, int y) const { return colorRow(y)[x]; }

    const std::vector<Pixel>& pixels() const { return color_; }
    std::vector<Pixel>& pixels() { return color_; }

private:
    int width_ = 0, height_ = 0;
    std::vector<Pixel> color_;
    std::vector<float> depth_;
};

using Framebuffer = BasicFramebuffer<Color8>;

// Linear RGB in xyz. As with Color8, w is coverage: 0 where nothing has been
// drawn, 1 where there is geometry.
using HdrFramebuffer = BasicFramebuffer<Vec4>;

}  // namespace termesh
