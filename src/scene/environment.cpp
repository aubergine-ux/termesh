#include "scene/environment.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <stb/stb_image.h>

namespace termesh {

namespace {

// Panoramas are reduced to at most this width. Lighting is low-frequency;
// more texels only cost memory and cache misses.
constexpr int kMaxWidth = 1024;
// A single absurdly bright texel (the sun's disc can be 50,000) would
// otherwise dominate everything it is averaged with.
constexpr float kMaxRadiance = 2048.0f;

Vec3 rotateY(Vec3 d, float angle) {
    if (angle == 0.0f) return d;
    const float c = std::cos(angle), s = std::sin(angle);
    return {c * d.x + s * d.z, d.y, -s * d.x + c * d.z};
}

// Direction -> position in an equirectangular image, both in [0, 1].
// u is the longitude, v the angle down from straight up.
void directionToUv(Vec3 d, float& u, float& v) {
    u = std::atan2(d.z, d.x) * (0.5f / kPi) + 0.5f;
    v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
}

Vec3 uvToDirection(float u, float v) {
    const float phi = (u - 0.5f) * 2.0f * kPi, theta = v * kPi;
    const float sinTheta = std::sin(theta);
    return {std::cos(phi) * sinTheta, std::cos(theta), std::sin(phi) * sinTheta};
}

// Halve an image, averaging 2x2 blocks. Longitude wraps around.
void downsample(int width, int height, const std::vector<Vec3>& src, int& outWidth, int& outHeight,
                std::vector<Vec3>& dst) {
    outWidth = std::max(width / 2, 1);
    outHeight = std::max(height / 2, 1);
    dst.resize(static_cast<std::size_t>(outWidth) * outHeight);
    for (int y = 0; y < outHeight; ++y) {
        const int y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
        for (int x = 0; x < outWidth; ++x) {
            const int x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
            const Vec3 sum =
                src[static_cast<std::size_t>(y0) * width + x0] + src[static_cast<std::size_t>(y0) * width + x1] +
                src[static_cast<std::size_t>(y1) * width + x0] + src[static_cast<std::size_t>(y1) * width + x1];
            dst[static_cast<std::size_t>(y) * outWidth + x] = sum * 0.25f;
        }
    }
}

}  // namespace

Environment Environment::hemisphere(Vec3 sky, Vec3 ground) {
    Environment env;
    env.sky_ = sky;
    env.ground_ = ground;
    return env;
}

Environment Environment::fromEquirect(int width, int height, std::vector<Vec3> pixels) {
    Environment env;
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<std::size_t>(width) * height) return env;

    for (Vec3& p : pixels) {
        // Negative or non-finite values in a damaged file must not spread.
        for (int c = 0; c < 3; ++c) p[c] = p[c] >= 0.0f ? std::min(p[c], kMaxRadiance) : 0.0f;
    }
    while (width > kMaxWidth) {
        std::vector<Vec3> smaller;
        int w = 0, h = 0;
        downsample(width, height, pixels, w, h, smaller);
        pixels = std::move(smaller);
        width = w;
        height = h;
    }

    // The mip chain doubles as the "prefiltered" environment: each level is
    // the one above blurred to half the resolution, so rougher surfaces
    // simply read from further down the chain.
    env.levels_.push_back({width, height, std::move(pixels)});
    while (env.levels_.back().width > 8 && env.levels_.back().height > 4) {
        const Level& top = env.levels_.back();
        Level next;
        downsample(top.width, top.height, top.pixels, next.width, next.height, next.pixels);
        env.levels_.push_back(std::move(next));
    }
    env.projectIrradiance();
    env.name = "panorama";
    return env;
}

// Irradiance - all the light arriving over a hemisphere, weighted by the
// cosine of its angle to the normal - is a very smooth function of the
// normal. Smooth functions on a sphere are captured well by a few spherical
// harmonics (the sphere's equivalent of the first terms of a Fourier
// series). Nine coefficients per colour channel reproduce irradiance to
// within about 1% (Ramamoorthi and Hanrahan, 2001), turning "integrate the
// whole panorama" into a nine-term polynomial in the normal.
void Environment::projectIrradiance() {
    // Any level around 64 texels wide has more than enough detail.
    const Level* level = &levels_.back();
    for (const Level& candidate : levels_)
        if (candidate.width <= 64) {
            level = &candidate;
            break;
        }

    std::array<Vec3, 9> coefficients{};
    for (int y = 0; y < level->height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(level->height);
        // Texels near the poles cover less of the sphere: weight by solid angle.
        const float solidAngle = (2.0f * kPi / static_cast<float>(level->width)) *
                                 (kPi / static_cast<float>(level->height)) * std::sin(v * kPi);
        for (int x = 0; x < level->width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(level->width);
            const Vec3 d = uvToDirection(u, v);
            const Vec3 radiance = level->pixels[static_cast<std::size_t>(y) * level->width + x] * solidAngle;
            const float basis[9] = {
                0.282095f,
                0.488603f * d.y,
                0.488603f * d.z,
                0.488603f * d.x,
                1.092548f * d.x * d.y,
                1.092548f * d.y * d.z,
                0.315392f * (3.0f * d.z * d.z - 1.0f),
                1.092548f * d.x * d.z,
                0.546274f * (d.x * d.x - d.y * d.y),
            };
            for (int i = 0; i < 9; ++i) coefficients[i] += radiance * basis[i];
        }
    }
    // Convolving with the cosine lobe scales each band by a constant
    // (pi, 2pi/3, pi/4); dividing by pi, as irradiance() promises, leaves
    // 1, 2/3 and 1/4.
    const float band[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < 9; ++i) sh_[i] = coefficients[i] * band[i];
}

Vec3 Environment::mapIrradiance(Vec3 n) const {
    const Vec3 d = rotateY(n, -rotation);
    const Vec3 e = sh_[0] * 0.282095f + sh_[1] * (0.488603f * d.y) + sh_[2] * (0.488603f * d.z) +
                   sh_[3] * (0.488603f * d.x) + sh_[4] * (1.092548f * d.x * d.y) + sh_[5] * (1.092548f * d.y * d.z) +
                   sh_[6] * (0.315392f * (3.0f * d.z * d.z - 1.0f)) + sh_[7] * (1.092548f * d.x * d.z) +
                   sh_[8] * (0.546274f * (d.x * d.x - d.y * d.y));
    // The truncated series can dip slightly below zero opposite a bright sun.
    return max(e, Vec3(0.0f)) * intensity;
}

Vec3 Environment::mapSpecular(Vec3 r, float roughness) const {
    // A reflection's blur grows with roughness squared, and each mip level
    // doubles the blur, so the level goes with the logarithm of roughness:
    // every halving of roughness moves two levels towards the sharp end.
    const float last = static_cast<float>(levels_.size() - 1);
    const float level = std::clamp(last + 2.0f * std::log2(std::max(roughness, 1e-3f)), 0.0f, last);
    return sampleLevel(r, level) * intensity;
}

Vec3 Environment::background(Vec3 dir, float blur) const {
    if (!hasMap()) return lerp(ground_, sky_, dir.y * 0.5f + 0.5f) * intensity;
    return sampleLevel(dir, std::clamp(blur, 0.0f, 1.0f) * static_cast<float>(levels_.size() - 1)) * intensity;
}

Vec3 Environment::sampleLevel(Vec3 dir, float level) const {
    float u, v;
    directionToUv(rotateY(dir, -rotation), u, v);
    const int lo = std::clamp(static_cast<int>(level), 0, static_cast<int>(levels_.size()) - 1);
    const int hi = std::min(lo + 1, static_cast<int>(levels_.size()) - 1);
    const float t = level - static_cast<float>(lo);
    const Vec3 a = sampleBilinear(levels_[static_cast<std::size_t>(lo)], u, v);
    if (hi == lo || t <= 0.0f) return a;
    return lerp(a, sampleBilinear(levels_[static_cast<std::size_t>(hi)], u, v), t);
}

Vec3 Environment::sampleBilinear(const Level& level, float u, float v) const {
    const float x = u * static_cast<float>(level.width) - 0.5f;
    const float y = v * static_cast<float>(level.height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    // Longitude wraps; latitude stops at the poles.
    auto wrap = [&](int i) { return ((i % level.width) + level.width) % level.width; };
    const int x0 = wrap(static_cast<int>(fx)), x1 = wrap(static_cast<int>(fx) + 1);
    const int y0 = std::clamp(static_cast<int>(fy), 0, level.height - 1);
    const int y1 = std::clamp(static_cast<int>(fy) + 1, 0, level.height - 1);
    const Vec3* p = level.pixels.data();
    const Vec3 top = lerp(p[static_cast<std::size_t>(y0) * level.width + x0],
                          p[static_cast<std::size_t>(y0) * level.width + x1], tx);
    const Vec3 bottom = lerp(p[static_cast<std::size_t>(y1) * level.width + x0],
                             p[static_cast<std::size_t>(y1) * level.width + x1], tx);
    return lerp(top, bottom, ty);
}

bool loadEnvironment(const std::string& path, Environment& out, std::string& error) {
    int width = 0, height = 0, channels = 0;
    // stb_image decodes Radiance .hdr to linear floats, and converts ordinary
    // 8-bit images to (approximately) linear ones, so a JPEG panorama works
    // too, just without real highlights.
    float* data = stbi_loadf(path.c_str(), &width, &height, &channels, 3);
    if (!data) {
        error = "cannot load environment '" + path + "': " + stbi_failure_reason();
        return false;
    }
    std::vector<Vec3> pixels(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = {data[i * 3], data[i * 3 + 1], data[i * 3 + 2]};
    stbi_image_free(data);

    out = Environment::fromEquirect(width, height, std::move(pixels));
    if (!out.hasMap()) {
        error = "environment '" + path + "' is empty";
        return false;
    }
    const std::size_t slash = path.find_last_of("/\\");
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    return true;
}

namespace {

// A simple analytic sky: a gradient from the horizon to the zenith, a bright
// sun with a glow around it, and a dull ground. Not physically derived, but
// it has what image-based lighting needs: a strong directional source, soft
// coloured fill from the rest of the sky, and bounce light from below.
Environment makeSky(Vec3 sunDirection, Vec3 sunColor, Vec3 zenith, Vec3 horizon, Vec3 ground) {
    const int width = 512, height = 256;
    const Vec3 sun = normalize(sunDirection);
    std::vector<Vec3> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const Vec3 d =
                uvToDirection((static_cast<float>(x) + 0.5f) / width, (static_cast<float>(y) + 0.5f) / height);
            Vec3 color;
            if (d.y >= 0.0f) {
                // Most of the change in sky colour happens near the horizon.
                color = lerp(horizon, zenith, std::pow(d.y, 0.45f));
                const float cosAngle = std::max(dot(d, sun), 0.0f);
                color += sunColor * (0.35f * std::pow(cosAngle, 24.0f));  // glow
                if (cosAngle > 0.9985f) color += sunColor * 60.0f;        // the disc itself
            } else {
                // Blend into the ground just below the horizon to avoid a hard line.
                color = lerp(horizon * 0.6f, ground, std::min(-d.y * 6.0f, 1.0f));
            }
            pixels[static_cast<std::size_t>(y) * width + x] = color;
        }
    return Environment::fromEquirect(width, height, std::move(pixels));
}

}  // namespace

std::vector<std::string> builtinEnvironmentNames() { return {"studio", "sky", "sunset"}; }

bool makeBuiltinEnvironment(const std::string& name, Environment& out) {
    if (name == "studio") {
        out = Environment();
    } else if (name == "sky") {
        out = makeSky({-0.45f, 0.75f, 0.5f}, {1.0f, 0.95f, 0.85f}, {0.18f, 0.36f, 0.75f}, {0.75f, 0.85f, 0.95f},
                      {0.2f, 0.17f, 0.14f});
    } else if (name == "sunset") {
        out = makeSky({-0.8f, 0.12f, 0.45f}, {1.0f, 0.55f, 0.25f}, {0.12f, 0.16f, 0.4f}, {0.95f, 0.55f, 0.35f},
                      {0.1f, 0.08f, 0.08f});
    } else {
        return false;
    }
    out.name = name;
    return true;
}

}  // namespace termesh
