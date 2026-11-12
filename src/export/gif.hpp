// A small animated GIF encoder.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/image.hpp"

namespace termesh {

// GIF frames are limited to 256 colours. One entry is kept for
// "transparent", which is how unchanged pixels are skipped between frames.
inline constexpr int kGifColors = 255;
inline constexpr int kGifTransparent = 255;

struct GifPalette {
    std::array<Color8, kGifColors> colors{};
    int count = 0;
    // Nearest palette entry for every 15-bit RGB value (5 bits per channel).
    std::vector<std::uint8_t> lookup;

    std::uint8_t nearest(Color8 c) const {
        return lookup[(static_cast<std::size_t>(c.r >> 3) << 10) | (static_cast<std::size_t>(c.g >> 3) << 5) |
                      (c.b >> 3)];
    }
};

// Choose a palette for a set of frames with the median cut algorithm.
GifPalette buildGifPalette(const std::vector<Image>& frames);

class GifWriter {
public:
    GifWriter() = default;
    ~GifWriter() { close(); }
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;

    // Start a looping animation. `delayCs` is the time per frame in
    // hundredths of a second (the unit GIF uses).
    bool open(const std::string& path, int width, int height, const GifPalette& palette, int delayCs);

    // Append a frame; the image must match the size given to open().
    bool addFrame(const Image& frame);

    // Finish the file. Returns false if any write failed.
    bool close();

private:
    void writeLzw(const std::vector<std::uint8_t>& indices);

    std::FILE* file_ = nullptr;
    int width_ = 0, height_ = 0, delayCs_ = 4;
    const GifPalette* palette_ = nullptr;
    std::vector<std::uint8_t> previous_;  // palette indices of the last frame
    std::vector<std::uint8_t> current_, encoded_;
    bool failed_ = false;
};

}  // namespace termesh
