// termesh - temporary driver: render a demo model to a PNG.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <stb/stb_image_write.h>

#include "core/image.hpp"
#include "pipeline/renderer.hpp"
#include "pipeline/resolve.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"

using namespace termesh;

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "knot";
    const std::string out = argc > 2 ? argv[2] : "out.png";
    RenderSettings settings;
    if (argc > 3) {
        const std::string mode = argv[3];
        if (mode == "flat") settings.shading = ShadingMode::Flat;
        if (mode == "gouraud") settings.shading = ShadingMode::Gouraud;
        if (mode == "wire") settings.wireframe = true;
    }

    Model model;
    if (name.find('.') != std::string::npos) {
        LoadResult loaded = loadModel(name);
        for (const auto& w : loaded.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        if (!loaded.ok) {
            std::fprintf(stderr, "error: %s\n", loaded.error.c_str());
            return 1;
        }
        model = std::move(loaded.model);
    } else {
        model = makeDemoModel(name);
    }
    if (model.meshes.empty()) {
        std::fprintf(stderr, "unknown demo model '%s'\n", name.c_str());
        return 1;
    }

    const int width = 960, height = 720, ssaa = 2;
    ThreadPool pool;
    Renderer renderer(pool);
    Framebuffer fb(width * ssaa, height * ssaa);
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    settings.wireWidth = static_cast<float>(ssaa);

    const RenderStats stats =
        renderer.render(model, orbit.camera(static_cast<float>(width) / height), settings, fb);
    Image image;
    resolve(fb, ssaa, image, pool);
    for (Color8& p : image.pixels) p.a = 255;
    stbi_write_png(out.c_str(), width, height, 4, image.pixels.data(), width * 4);
    std::printf("%zu/%zu tris, vertex %.2f ms, setup %.2f ms, raster %.2f ms\n", stats.trianglesDrawn,
                stats.trianglesSubmitted, stats.vertexMs, stats.setupMs, stats.rasterMs);
    return 0;
}
