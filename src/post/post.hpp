// Post-processing: everything that happens to the picture after the
// triangles have been drawn.
//
// The rasterizer leaves behind two images: linear HDR colour and depth.
// Each pass here reads those and rewrites the colour:
//
//   sky          fill the pixels nothing was drawn on with the environment
//   SSAO         darken creases and corners, estimated from depth alone
//   outline      ink lines along silhouettes and sharp edges
//   bloom        let very bright areas glow into their surroundings
//   tone map     compress to displayable range and encode as 8-bit sRGB
//
// and, after supersampling has been resolved, on the final image:
//
//   FXAA         smooth whatever jagged edges are left
#pragma once

#include <vector>

#include "core/framebuffer.hpp"
#include "core/image.hpp"
#include "core/thread_pool.hpp"
#include "post/tonemap.hpp"
#include "scene/camera.hpp"
#include "scene/environment.hpp"

namespace termesh {

struct PostSettings {
    ToneMapper toneMapper = ToneMapper::Aces;
    float exposure = 0.0f;  // in stops: +1 doubles the brightness

    bool ssao = false;
    float ssaoRadius = 0.08f;   // sampling radius, as a fraction of the scene's depth
    float ssaoStrength = 1.0f;  // 0 = no effect, 1 = full

    bool bloom = false;
    float bloomThreshold = 1.0f;  // only light brighter than this glows
    float bloomStrength = 0.5f;

    bool outline = false;
    float outlineWidth = 1.0f;  // in output pixels
    Vec3 outlineColor{0.01f, 0.01f, 0.015f};

    bool fxaa = false;
};

// What goes behind the model.
struct Backdrop {
    Color8 color{16, 17, 22, 255};
    const Environment* sky = nullptr;  // if set, drawn instead of the flat colour
    float skyBlur = 0.0f;
};

class PostProcessor {
public:
    explicit PostProcessor(ThreadPool& pool) : pool_(pool) {}

    // Run the HDR passes on `hdr` in place, then tone map it into `out`
    // (resized to match). `pixelScale` is the supersampling factor, so
    // that widths given in output pixels can be converted.
    void run(HdrFramebuffer& hdr, const Camera& camera, const PostSettings& settings, const Backdrop& backdrop,
             float pixelScale, Framebuffer& out);

    // True if run() has passes that need the whole frame at once. When it
    // does not, the only work left is tone mapping, which looks at one
    // pixel at a time - so the renderer can call presentRect() on each tile
    // as soon as it is drawn, while its pixels are still in the cache,
    // instead of making a second trip through memory afterwards.
    static bool needsWholeFrame(const PostSettings& settings, const Backdrop& backdrop) {
        return backdrop.sky || settings.ssao || settings.outline || settings.bloom;
    }

    // Tone map the pixels in [x0, x1) x [y0, y1) of `hdr` into `out`, which
    // must already be the same size. Safe to call from several threads on
    // rectangles that do not overlap.
    static void presentRect(const HdrFramebuffer& hdr, const PostSettings& settings, const Backdrop& backdrop,
                            Framebuffer& out, int x0, int y0, int x1, int y1);

private:
    void fillSky(HdrFramebuffer& hdr, const Camera& camera, const Backdrop& backdrop);
    void reconstructGeometry(const HdrFramebuffer& hdr, const Camera& camera);
    void ambientOcclusion(HdrFramebuffer& hdr, const Camera& camera, const PostSettings& settings);
    void outline(HdrFramebuffer& hdr, const PostSettings& settings, float pixelScale);
    void bloom(HdrFramebuffer& hdr, const PostSettings& settings, float pixelScale);

    ThreadPool& pool_;

    // Scratch images, kept between frames.
    std::vector<Vec3> positions_;  // view-space position of each pixel
    std::vector<Vec3> normals_;    // view-space normal, rebuilt from depth
    std::vector<float> scratchA_, scratchB_;
    struct BloomLevel {
        int width = 0, height = 0;
        std::vector<Vec3> pixels;
    };
    std::vector<BloomLevel> bloomLevels_;
};

// Fast approximate anti-aliasing, applied to a finished image in place.
void applyFxaa(Image& image, ThreadPool& pool);

}  // namespace termesh
