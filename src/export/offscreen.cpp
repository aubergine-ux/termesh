#include "export/offscreen.hpp"

#include <algorithm>
#include <utility>

#include <stb/stb_image_write.h>

#include "pipeline/resolve.hpp"

namespace termesh {

Image renderOffscreen(Renderer& renderer, ThreadPool& pool, const Model& model, const OrbitCamera& camera,
                      RenderSettings settings, const OffscreenTarget& target, RenderStats* stats) {
    const DrawItem item{&model, Mat4::identity(), nullptr};
    return renderOffscreen(renderer, pool, std::span<const DrawItem>(&item, 1), camera, std::move(settings), target,
                           stats);
}

Image renderOffscreen(Renderer& renderer, ThreadPool& pool, std::span<const DrawItem> items, const OrbitCamera& camera,
                      RenderSettings settings, const OffscreenTarget& target, RenderStats* stats) {
    return renderOffscreen(
        renderer, pool, items, [&](float aspect) { return camera.camera(aspect); }, std::move(settings), target, stats);
}

Image renderOffscreen(Renderer& renderer, ThreadPool& pool, std::span<const DrawItem> items, const CameraSource& camera,
                      RenderSettings settings, const OffscreenTarget& target, RenderStats* stats) {
    const int blocks = std::max(target.blocks, 1);
    const int width = std::max(target.width / blocks, 1);
    const int height = std::max(target.height / blocks, 1);

    // Keep the supersampled framebuffer within what the rasterizer's
    // fixed-point maths can address.
    int ssaa = std::clamp(target.supersampling, 1, 4);
    while (ssaa > 1 && static_cast<std::int64_t>(width) * ssaa * height * ssaa > kMaxFramebufferPixels) --ssaa;

    Framebuffer fb(width * ssaa, height * ssaa);
    settings.wireWidth *= static_cast<float>(ssaa);
    settings.pixelScale = static_cast<float>(ssaa);
    settings.lodTolerance *= static_cast<float>(ssaa);
    const RenderStats s =
        renderer.render(items, camera(static_cast<float>(width) / static_cast<float>(height)), settings, fb);
    if (stats) *stats = s;

    Image resolved;
    resolve(fb, ssaa, resolved, pool);
    if (settings.post.fxaa) applyFxaa(resolved, pool);
    for (Color8& p : resolved.pixels) p.a = 255;
    if (blocks == 1) return resolved;

    // Nearest-neighbour upscale: every source pixel becomes a solid block.
    Image out(width * blocks, height * blocks);
    for (int y = 0; y < out.height; ++y) {
        const Color8* src = resolved.row(y / blocks);
        Color8* dst = out.row(y);
        for (int x = 0; x < out.width; ++x) dst[x] = src[x / blocks];
    }
    return out;
}

bool writePng(const std::string& path, const Image& image) {
    return stbi_write_png(path.c_str(), image.width, image.height, 4, image.pixels.data(), image.width * 4) != 0;
}

}  // namespace termesh
