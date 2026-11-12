// Off-screen rendering to images, for PNG screenshots and GIF recording.
#pragma once

#include <string>

#include "core/image.hpp"
#include "pipeline/renderer.hpp"
#include "scene/camera.hpp"

namespace termesh {

struct OffscreenTarget {
    int width = 1280, height = 960;
    int supersampling = 2;
    // Draw each rendered pixel as a blocks x blocks square. With blocks > 1
    // the image looks like half-block terminal output: big, crisp pixels.
    int blocks = 1;
};

// Render one frame into an image of exactly target.width x target.height
// (rounded down to a multiple of target.blocks). Background pixels come out
// opaque.
Image renderOffscreen(Renderer& renderer, ThreadPool& pool, const Model& model, const OrbitCamera& camera,
                      RenderSettings settings, const OffscreenTarget& target, RenderStats* stats = nullptr);

bool writePng(const std::string& path, const Image& image);

}  // namespace termesh
