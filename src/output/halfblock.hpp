// Half-block output: two pixels per terminal cell.
#pragma once

#include <string>

#include "core/image.hpp"

namespace termesh {

// Encode a framebuffer as rows of U+2580 UPPER HALF BLOCK characters with
// 24-bit colour. The glyph's foreground paints the top half of the cell and
// its background the bottom half, so every cell shows two vertically stacked
// pixels - and since cells are about twice as tall as they are wide, those
// pixels come out roughly square.
std::string encodeHalfBlocks(ImageView fb);

}  // namespace termesh
