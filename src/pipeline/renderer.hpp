// Renderer: drives models through the whole pipeline into a framebuffer.
//
//   shadow passes     depth-only renders from each shadow-casting light
//   vertex stage      model vertices -> clip space + varyings   (parallel)
//   primitive setup   clip, project, cull, snap to pixels       (parallel)
//   binning           sort triangles into screen tiles
//   rasterization     depth test + shade each tile, in HDR      (parallel)
//   post-processing   sky, SSAO, outlines, bloom, tone mapping  (parallel)
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "core/framebuffer.hpp"
#include "core/thread_pool.hpp"
#include "pipeline/clip.hpp"
#include "pipeline/shading.hpp"
#include "pipeline/shadow.hpp"
#include "post/post.hpp"
#include "raster/rasterizer.hpp"
#include "accel/model_accel.hpp"
#include "scene/camera.hpp"
#include "scene/draw_item.hpp"
#include "scene/environment.hpp"
#include "scene/light.hpp"
#include "scene/model.hpp"

namespace termesh {

enum class ShadingMode { Flat, Gouraud, Phong, Pbr, Toon };
inline constexpr int kShadingModeCount = 5;

const char* shadingModeName(ShadingMode mode);

struct RenderSettings {
    ShadingMode shading = ShadingMode::Phong;
    bool wireframe = false;
    bool textures = true;
    bool normalMaps = true;
    bool cullBackfaces = true;
    Color8 background{16, 17, 22, 255};
    float wireWidth = 1.0f;  // wireframe line width in framebuffer pixels

    // Lights. When the list is empty a default rig is used: a key light and
    // a dim fill light attached to the camera, so that orbiting looks like
    // turning the model in your hands. `lightDirection` aims that key
    // light: the direction towards it, in view space.
    std::vector<Light> lights;
    Vec3 lightDirection{-0.45f, 0.55f, 0.7f};

    // Ambient light and reflections; null means the default studio
    // hemisphere. With `skybox` it is also drawn as the background.
    const Environment* environment = nullptr;
    bool skybox = false;
    float skyboxBlur = 0.0f;

    // Shadows from directional and spot lights that have castsShadow set
    // (with the default rig: from the key light). Per-pixel shading modes
    // only: Phong, PBR and toon.
    bool shadows = false;
    int shadowMapSize = 1024;
    int shadowSoftness = 3;  // width of the filter in shadow-map texels; 1 = hard

    PostSettings post;
    // Supersampling factor the frame will be resolved with, so that effects
    // measured in output pixels (outline width, bloom reach) can scale.
    float pixelScale = 1.0f;

    // Draw every mesh as points: one splat per vertex. (Point clouds, which
    // have nothing but vertices, are always drawn this way.)
    bool points = false;
    float pointScale = 1.0f;  // multiplies every splat's radius

    // Level of detail. A mesh that has coarser versions (ModelAccel::lods)
    // is drawn with the coarsest one whose error, projected to the screen,
    // is at most this many framebuffer pixels. 0 always draws the original.
    float lodTolerance = 0.0f;

    // Draw a mesh tinted, to show it is selected: its position in the draw
    // list and in its model. An item with mesh -1 tints the whole model;
    // item -1 tints nothing.
    int highlightItem = -1, highlightMesh = -1;
};

struct RenderStats {
    std::size_t trianglesSubmitted = 0;
    std::size_t trianglesDrawn = 0;  // after culling and clipping
    // Skipped before the vertex stage because their box, or their BVH node,
    // was outside the view (only for items that come with a ModelAccel).
    std::size_t meshesCulled = 0, trianglesCulled = 0;
    // Triangles saved by drawing coarser levels of detail.
    std::size_t trianglesSimplified = 0;
    double vertexMs = 0.0, setupMs = 0.0, rasterMs = 0.0;
    double shadowMs = 0.0, postMs = 0.0;

    double totalMs() const { return vertexMs + setupMs + rasterMs + shadowMs + postMs; }
};

class Renderer {
public:
    explicit Renderer(ThreadPool& pool) : pool_(pool), post_(pool) {}

    // Draws the model into the framebuffer, replacing its contents. The
    // framebuffer must not exceed kMaxFramebufferPixels.
    RenderStats render(const Model& model, const Camera& camera, const RenderSettings& settings, Framebuffer& fb);

    // The same for any number of models, each with its own transform.
    RenderStats render(std::span<const DrawItem> items, const Camera& camera, const RenderSettings& settings,
                       Framebuffer& fb);

    // Tiles are small enough to stay in cache and to load-balance well across
    // threads even in an 80-column terminal.
    static constexpr int kTileSize = 32;

    // At most this many lights cast shadows in one frame; the rest light the
    // scene without them.
    static constexpr int kMaxShadowMaps = 4;

    // A material together with the textures its indices refer to.
    struct MaterialRef {
        const Material* material;
        const Texture* textures;
    };

    // How the vertex stage fills the varyings.
    enum class VaryingLayout {
        DepthOnly,  // texture coordinates only (for alpha cut-outs)
        Lit,        // lighting already computed: per vertex or per face
        PerPixel,   // the inputs of per-pixel lighting
    };

private:
    struct GeometryPass;

    // Vertex stage, primitive setup and binning for one view of the scene.
    // Leaves the triangles in chunks_ and bins_.
    void buildGeometry(std::span<const DrawItem> items, const GeometryPass& pass, RenderStats& stats);

    // Clear and rasterize every tile. `finishTile(tile)` runs on each tile
    // straight after it has been drawn, on the thread that drew it.
    template <typename Target, typename Shader, typename FinishTile>
    void rasterize(Target& target, const Shader& shader, typename Target::Pixel clearColor, bool edges, int lineWidth,
                   const FinishTile& finishTile);

    void renderShadowMap(std::span<const DrawItem> items, const Light& light, Vec3 center, float radius,
                         const RenderSettings& settings, const std::uint8_t* lodChoice, ShadowMap& map);

    ThreadPool& pool_;
    PostProcessor post_;

    // The frame before post-processing: linear HDR colour and depth.
    HdrFramebuffer hdr_;
    ShadowMap shadowMaps_[kMaxShadowMaps];

    // Scratch memory, kept between frames so steady-state rendering does not
    // allocate.
    std::vector<MaterialRef> materials_;
    std::vector<ShadingLight> shadingLights_;
    std::vector<ClipVertex> clipVertices_;
    std::vector<std::vector<RasterTriangle>> chunks_;
    std::vector<char> chunkBlends_;  // per chunk: holds translucent triangles
    std::vector<const RasterTriangle*> blendOrder_;
    std::vector<std::uint64_t> visible_;  // one bit per triangle of the mesh being set up
    // Whether the pass just built contains splats or per-vertex colours:
    // decides which version of the fragment shader the frame needs.
    bool passHasExtras_ = false;
    std::vector<std::uint8_t> lodChoice_;  // per mesh of the draw list: 0 = original, k = lods[k - 1]
    std::vector<std::vector<const RasterTriangle*>> bins_;
    int binsX_ = 0, binsY_ = 0;
};

}  // namespace termesh
