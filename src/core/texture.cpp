#include "core/texture.hpp"

#include <algorithm>
#include <cmath>

#include <stb/stb_image.h>

#include "core/color.hpp"

namespace termesh {

namespace {

// Map a texel index that may lie outside [0, size) back into range.
int wrapIndex(int i, int size, WrapMode mode) {
    switch (mode) {
        case WrapMode::Clamp: return std::clamp(i, 0, size - 1);
        case WrapMode::Mirror: {
            const int period = 2 * size;
            int m = i % period;
            if (m < 0) m += period;
            return m < size ? m : period - 1 - m;
        }
        case WrapMode::Repeat:
        default: {
            int m = i % size;
            return m < 0 ? m + size : m;
        }
    }
}

}  // namespace

Texture::Texture(int width, int height, std::vector<Color8> pixels, bool srgb) : srgb_(srgb) {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return;
    levels_.push_back({width, height, std::move(pixels)});
    lodOffset_ = 0.5f * std::log2(static_cast<float>(width) * static_cast<float>(height));
    buildMips();
}

void Texture::setSrgb(bool srgb) {
    if (srgb == srgb_) return;
    srgb_ = srgb;
    if (!levels_.empty()) {
        levels_.resize(1);
        buildMips();
    }
}

// Each mip level halves the previous one with a 2x2 box filter. Colour is
// averaged in linear space: averaging sRGB codes directly makes minified
// textures come out too dark.
void Texture::buildMips() {
    while (levels_.back().width > 1 || levels_.back().height > 1) {
        const Level& src = levels_.back();
        Level dst;
        dst.width = std::max(src.width / 2, 1);
        dst.height = std::max(src.height / 2, 1);
        dst.pixels.resize(static_cast<std::size_t>(dst.width) * dst.height);

        for (int y = 0; y < dst.height; ++y) {
            const int y0 = std::min(2 * y, src.height - 1);
            const int y1 = std::min(2 * y + 1, src.height - 1);
            for (int x = 0; x < dst.width; ++x) {
                const int x0 = std::min(2 * x, src.width - 1);
                const int x1 = std::min(2 * x + 1, src.width - 1);
                const Color8 texels[4] = {
                    src.pixels[static_cast<std::size_t>(y0) * src.width + x0],
                    src.pixels[static_cast<std::size_t>(y0) * src.width + x1],
                    src.pixels[static_cast<std::size_t>(y1) * src.width + x0],
                    src.pixels[static_cast<std::size_t>(y1) * src.width + x1],
                };
                Color8& out = dst.pixels[static_cast<std::size_t>(y) * dst.width + x];
                int alpha = 0;
                if (srgb_) {
                    Vec3 sum;
                    for (const Color8& t : texels) {
                        sum += srgbToLinear(t);
                        alpha += t.a;
                    }
                    out = linearToSrgb(sum * 0.25f);
                } else {
                    int r = 0, g = 0, b = 0;
                    for (const Color8& t : texels) {
                        r += t.r;
                        g += t.g;
                        b += t.b;
                        alpha += t.a;
                    }
                    out = {static_cast<std::uint8_t>((r + 2) / 4), static_cast<std::uint8_t>((g + 2) / 4),
                           static_cast<std::uint8_t>((b + 2) / 4), 255};
                }
                out.a = static_cast<std::uint8_t>((alpha + 2) / 4);
            }
        }
        levels_.push_back(std::move(dst));
    }
}

Vec4 Texture::sampleLevel(Vec2 uv, int level) const {
    const Level& l = levels_[level];

    // Texel centres sit at half-integers, so shift by half a texel before
    // splitting into an integer texel index and a blend fraction.
    const float fx = uv.x * static_cast<float>(l.width) - 0.5f;
    const float fy = uv.y * static_cast<float>(l.height) - 0.5f;
    const float floorX = std::floor(fx), floorY = std::floor(fy);
    const float tx = fx - floorX, ty = fy - floorY;
    const int ix = static_cast<int>(floorX), iy = static_cast<int>(floorY);

    const int x0 = wrapIndex(ix, l.width, wrapU_), x1 = wrapIndex(ix + 1, l.width, wrapU_);
    const int y0 = wrapIndex(iy, l.height, wrapV_), y1 = wrapIndex(iy + 1, l.height, wrapV_);

    const Color8* row0 = l.pixels.data() + static_cast<std::size_t>(y0) * l.width;
    const Color8* row1 = l.pixels.data() + static_cast<std::size_t>(y1) * l.width;
    const Color8 c00 = row0[x0], c10 = row0[x1], c01 = row1[x0], c11 = row1[x1];

    const float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty);
    const float w01 = (1.0f - tx) * ty, w11 = tx * ty;

    const float alpha = (c00.a * w00 + c10.a * w10 + c01.a * w01 + c11.a * w11) * (1.0f / 255.0f);
    if (srgb_) {
        const auto& lut = detail::colorTables().toLinear;
        return {lut[c00.r] * w00 + lut[c10.r] * w10 + lut[c01.r] * w01 + lut[c11.r] * w11,
                lut[c00.g] * w00 + lut[c10.g] * w10 + lut[c01.g] * w01 + lut[c11.g] * w11,
                lut[c00.b] * w00 + lut[c10.b] * w10 + lut[c01.b] * w01 + lut[c11.b] * w11, alpha};
    }
    constexpr float k = 1.0f / 255.0f;
    return {(c00.r * w00 + c10.r * w10 + c01.r * w01 + c11.r * w11) * k,
            (c00.g * w00 + c10.g * w10 + c01.g * w01 + c11.g * w11) * k,
            (c00.b * w00 + c10.b * w10 + c01.b * w01 + c11.b * w11) * k, alpha};
}

Vec4 Texture::sample(Vec2 uv, float lodBias) const {
    if (levels_.empty()) return Vec4(1.0f);
    const float lod = lodBias + lodOffset_;
    const int maxLevel = static_cast<int>(levels_.size()) - 1;
    if (!(lod > 0.0f)) return sampleLevel(uv, 0);  // magnified (or NaN)
    if (lod >= static_cast<float>(maxLevel)) return sampleLevel(uv, maxLevel);

    // Trilinear: blend the two mip levels either side of the ideal one so the
    // texture does not visibly pop as the model moves towards the camera.
    const int level = static_cast<int>(lod);
    const float t = lod - static_cast<float>(level);
    return lerp(sampleLevel(uv, level), sampleLevel(uv, level + 1), t);
}

bool Texture::isGreyscale() const {
    if (levels_.empty()) return false;
    for (const Color8& p : levels_[0].pixels)
        if (std::abs(p.r - p.g) > 2 || std::abs(p.g - p.b) > 2) return false;
    return true;
}

static Texture fromStb(stbi_uc* data, int width, int height, bool srgb) {
    if (!data) return {};
    std::vector<Color8> pixels(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = {data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]};
    stbi_image_free(data);
    return Texture(width, height, std::move(pixels), srgb);
}

Texture loadTexture(const std::string& path, bool srgb) {
    int width = 0, height = 0, channels = 0;
    stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    Texture texture = fromStb(data, width, height, srgb);
    texture.name = path;
    return texture;
}

Texture loadTextureFromMemory(const unsigned char* data, std::size_t size, bool srgb) {
    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 4);
    return fromStb(decoded, width, height, srgb);
}

// The slope of the height field in u and v gives the two tangent directions;
// the normal is perpendicular to both. Central differences estimate the slope.
Texture heightToNormalMap(const Texture& height, float strength) {
    if (height.empty()) return {};
    const int w = height.width(), h = height.height();
    const auto& src = height.pixels();
    auto heightAt = [&](int x, int y) {
        x = (x % w + w) % w;
        y = (y % h + h) % h;
        return static_cast<float>(src[static_cast<std::size_t>(y) * w + x].r) / 255.0f;
    };
    std::vector<Color8> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = (heightAt(x + 1, y) - heightAt(x - 1, y)) * strength;
            // Image rows run downwards but tangent-space +Y points up the texture.
            const float dy = (heightAt(x, y - 1) - heightAt(x, y + 1)) * strength;
            const Vec3 n = normalize(Vec3{-dx, -dy, 1.0f});
            out[static_cast<std::size_t>(y) * w + x] = {static_cast<std::uint8_t>((n.x * 0.5f + 0.5f) * 255.0f + 0.5f),
                                                        static_cast<std::uint8_t>((n.y * 0.5f + 0.5f) * 255.0f + 0.5f),
                                                        static_cast<std::uint8_t>((n.z * 0.5f + 0.5f) * 255.0f + 0.5f),
                                                        255};
        }
    Texture result(w, h, std::move(out), false);
    result.name = height.name;
    return result;
}

}  // namespace termesh
