// Renderer: drives a model through the whole pipeline into a framebuffer.
//
//   vertex stage      model vertices -> clip space + varyings   (parallel)
//   primitive setup   clip, project, cull, snap to pixels       (parallel)
//   binning           sort triangles into screen tiles
//   rasterization     depth test + shade each tile              (parallel)
#pragma once

#include <cstddef>
#include <vector>

#include "core/framebuffer.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/clip.hpp"
#include "raster/rasterizer.hpp"
#include "scene/camera.hpp"
#include "scene/model.hpp"

namespace termesh {

enum class ShadingMode { Flat, Gouraud, Phong };

const char* shadingModeName(ShadingMode mode);

struct RenderSettings {
    ShadingMode shading = ShadingMode::Phong;
    bool wireframe = false;
    bool textures = true;
    bool normalMaps = true;
    bool cullBackfaces = true;
    Color8 background{16, 17, 22, 255};
    // Direction towards the key light, in *view* space: the light is attached
    // to the camera, so orbiting looks like turning the model in your hands.
    Vec3 lightDirection{-0.45f, 0.55f, 0.7f};
    float wireWidth = 1.0f;  // wireframe line width in framebuffer pixels
};

struct RenderStats {
    std::size_t trianglesSubmitted = 0;
    std::size_t trianglesDrawn = 0;  // after culling and clipping
    double vertexMs = 0.0, setupMs = 0.0, rasterMs = 0.0;
};

class Renderer {
public:
    explicit Renderer(ThreadPool& pool) : pool_(pool) {}

    // Clears the framebuffer and draws the model. The framebuffer must not
    // exceed kMaxFramebufferPixels.
    RenderStats render(const Model& model, const Camera& camera, const RenderSettings& settings,
                       Framebuffer& fb);

    // Tiles are small enough to stay in cache and to load-balance well across
    // threads even in an 80-column terminal.
    static constexpr int kTileSize = 32;

private:
    ThreadPool& pool_;

    // Scratch memory, kept between frames so steady-state rendering does not
    // allocate.
    std::vector<ClipVertex> clipVertices_;
    std::vector<std::vector<RasterTriangle>> chunks_;
    std::vector<std::vector<const RasterTriangle*>> bins_;
};

}  // namespace termesh
