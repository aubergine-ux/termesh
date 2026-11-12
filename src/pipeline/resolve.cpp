#include "pipeline/resolve.hpp"

#include <algorithm>

#include "core/color.hpp"

namespace termesh {

void resolve(const Framebuffer& src, int factor, Image& dst, ThreadPool& pool) {
    factor = std::max(factor, 1);
    const int width = src.width() / factor, height = src.height() / factor;
    dst.resize(width, height);

    if (factor == 1) {
        std::copy(src.pixels().begin(), src.pixels().end(), dst.pixels.begin());
        return;
    }

    const float scale = 1.0f / static_cast<float>(factor * factor);
    constexpr int kRowsPerJob = 16;
    pool.parallelFor((height + kRowsPerJob - 1) / kRowsPerJob, [&](int job) {
        const int yEnd = std::min((job + 1) * kRowsPerJob, height);
        for (int y = job * kRowsPerJob; y < yEnd; ++y) {
            Color8* out = dst.row(y);
            for (int x = 0; x < width; ++x) {
                Vec3 sum;
                int alpha = 0;
                for (int sy = 0; sy < factor; ++sy) {
                    const Color8* in = src.colorRow(y * factor + sy) + x * factor;
                    for (int sx = 0; sx < factor; ++sx) {
                        sum += srgbToLinear(in[sx]);
                        alpha += in[sx].a;
                    }
                }
                out[x] = linearToSrgb(sum * scale, static_cast<std::uint8_t>(static_cast<float>(alpha) * scale + 0.5f));
            }
        }
    });
}

void upscale(const Image& src, int width, int height, Image& dst, ThreadPool& pool) {
    dst.resize(width, height);
    if (src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0) return;
    const float scaleX = static_cast<float>(src.width) / static_cast<float>(width);
    const float scaleY = static_cast<float>(src.height) / static_cast<float>(height);
    constexpr int kRowsPerJob = 16;
    pool.parallelFor((height + kRowsPerJob - 1) / kRowsPerJob, [&](int job) {
        const int yEnd = std::min((job + 1) * kRowsPerJob, height);
        for (int y = job * kRowsPerJob; y < yEnd; ++y) {
            // Where this output pixel's centre falls among the source's.
            const float fy =
                std::clamp((static_cast<float>(y) + 0.5f) * scaleY - 0.5f, 0.0f, static_cast<float>(src.height - 1));
            const int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, src.height - 1);
            const int wy = static_cast<int>((fy - static_cast<float>(y0)) * 256.0f);
            const Color8* top = src.row(y0);
            const Color8* bottom = src.row(y1);
            Color8* out = dst.row(y);
            for (int x = 0; x < width; ++x) {
                const float fx =
                    std::clamp((static_cast<float>(x) + 0.5f) * scaleX - 0.5f, 0.0f, static_cast<float>(src.width - 1));
                const int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, src.width - 1);
                const int wx = static_cast<int>((fx - static_cast<float>(x0)) * 256.0f);
                // Fixed-point weights out of 256 in each direction.
                auto mix = [&](int a, int b, int c, int d) {
                    const int t = a * (256 - wx) + b * wx, u = c * (256 - wx) + d * wx;
                    return static_cast<std::uint8_t>((t * (256 - wy) + u * wy + (1 << 15)) >> 16);
                };
                const Color8 &a = top[x0], &b = top[x1], &c = bottom[x0], &d = bottom[x1];
                out[x] = {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b),
                          mix(a.a, b.a, c.a, d.a)};
            }
        }
    });
}

}  // namespace termesh
