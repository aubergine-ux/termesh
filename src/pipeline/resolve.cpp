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

}  // namespace termesh
