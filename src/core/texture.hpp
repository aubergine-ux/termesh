// Texture: an 8-bit RGBA image with a mip chain and filtered sampling.
#pragma once

#include <string>
#include <vector>

#include "core/framebuffer.hpp"
#include "math/vec.hpp"

namespace termesh {

enum class WrapMode { Repeat, Clamp, Mirror };

class Texture {
public:
    Texture() = default;

    // Build a texture from RGBA pixels (row 0 at the top). `srgb` says whether
    // the colour channels are sRGB-encoded (base colour, emissive) or already
    // linear data (normal maps, roughness). The mip chain is generated here.
    Texture(int width, int height, std::vector<Color8> pixels, bool srgb);

    int width() const { return levels_.empty() ? 0 : levels_[0].width; }
    int height() const { return levels_.empty() ? 0 : levels_[0].height; }
    int levelCount() const { return static_cast<int>(levels_.size()); }
    bool empty() const { return levels_.empty(); }
    bool srgb() const { return srgb_; }

    // Change how the colour channels are interpreted, rebuilding the mips
    // (which are filtered in linear space) if it actually changes.
    void setSrgb(bool srgb);

    void setWrap(WrapMode u, WrapMode v) {
        wrapU_ = u;
        wrapV_ = v;
    }

    // Sample with trilinear filtering. Returns linear RGB plus alpha.
    //
    // `lodBias` is 0.5 * log2 of the area, in units of the whole texture, that
    // one screen pixel covers; the texture adds its own resolution to turn
    // that into a mip level. (0, 0) is the top-left corner, matching glTF.
    Vec4 sample(Vec2 uv, float lodBias) const;

    // Bilinear sample of one mip level.
    Vec4 sampleLevel(Vec2 uv, int level) const;

    // Raw access to the base level.
    const std::vector<Color8>& pixels() const { return levels_[0].pixels; }

    // True if every pixel is a shade of grey - used to recognise height maps
    // that OBJ files pass off as bump maps.
    bool isGreyscale() const;

    std::string name;  // for diagnostics

private:
    struct Level {
        int width = 0, height = 0;
        std::vector<Color8> pixels;
    };

    void buildMips();

    std::vector<Level> levels_;
    float lodOffset_ = 0.0f;  // 0.5 * log2(width * height)
    bool srgb_ = true;
    WrapMode wrapU_ = WrapMode::Repeat, wrapV_ = WrapMode::Repeat;
};

// Load an image file (PNG, JPEG, TGA, BMP, ...) through stb_image.
// Returns an empty texture on failure.
Texture loadTexture(const std::string& path, bool srgb);

// Decode an image held in memory.
Texture loadTextureFromMemory(const unsigned char* data, std::size_t size, bool srgb);

// Convert a greyscale height map into a tangent-space normal map.
Texture heightToNormalMap(const Texture& height, float strength = 2.0f);

}  // namespace termesh
