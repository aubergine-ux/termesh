// Image: a plain 2D array of colours, and a non-owning view of one.
#pragma once

#include <cstddef>
#include <vector>

#include "core/framebuffer.hpp"

namespace termesh {

// A read-only window onto pixels owned by someone else (an Image or a
// Framebuffer). This is what the output backends consume.
struct ImageView {
    const Color8* data = nullptr;
    int width = 0, height = 0;

    const Color8* row(int y) const { return data + static_cast<std::size_t>(y) * width; }
    const Color8& at(int x, int y) const { return row(y)[x]; }
};

struct Image {
    int width = 0, height = 0;
    std::vector<Color8> pixels;

    Image() = default;
    Image(int w, int h) { resize(w, h); }

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    }
    Color8* row(int y) { return pixels.data() + static_cast<std::size_t>(y) * width; }
    const Color8* row(int y) const { return pixels.data() + static_cast<std::size_t>(y) * width; }
    ImageView view() const { return {pixels.data(), width, height}; }
};

inline ImageView viewOf(const Framebuffer& fb) {
    return {fb.pixels().data(), fb.width(), fb.height()};
}

}  // namespace termesh
