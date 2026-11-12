// Supersampling resolve: average blocks of rendered pixels down to the
// output resolution.
#pragma once

#include "core/framebuffer.hpp"
#include "core/image.hpp"
#include "core/thread_pool.hpp"

namespace termesh {

// Shrink `src` by an integer `factor` in each direction, writing into `dst`
// (which is resized to fit). Rendering at factor x the resolution and
// averaging is the simplest form of anti-aliasing, and at terminal
// resolutions - where every pixel is huge - it makes a dramatic difference.
//
// Colours are averaged in linear light, not as sRGB codes; otherwise
// anti-aliased edges come out darker than either side. Alpha (the coverage
// flag) is averaged too, giving the fraction of the pixel covered by geometry.
void resolve(const Framebuffer& src, int factor, Image& dst, ThreadPool& pool);

// Stretch `src` to width x height with bilinear filtering, writing into
// `dst`. Used when a frame was rendered below the display resolution to
// save time; smooth enlargement hides that better than blocky pixels.
void upscale(const Image& src, int width, int height, Image& dst, ThreadPool& pool);

}  // namespace termesh
