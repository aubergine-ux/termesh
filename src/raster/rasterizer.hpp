// Triangle rasterization with a depth buffer.
#pragma once

#include "core/framebuffer.hpp"
#include "math/vec.hpp"

namespace termesh {

// Fill one flat-coloured triangle. Vertices are in screen space: x and y in
// pixels (y down, pixel centres at half-integers), z the depth in [0, 1].
// Either winding is accepted.
void fillTriangle(Framebuffer& fb, Vec3 a, Vec3 b, Vec3 c, Color8 color);

}  // namespace termesh
