// A grid of coloured character cells, and the code that writes one to a
// terminal while sending as few bytes as possible.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/framebuffer.hpp"
#include "output/backend.hpp"

namespace termesh {

// Cell colours are packed into 32 bits:
//   0x00RRGGBB   a 24-bit colour
//   0x010000NN   entry NN of the 256-colour palette
//   kDefaultColor  the terminal's own default foreground / background
inline constexpr std::uint32_t kDefaultColor = 0xFFFFFFFFu;
inline constexpr std::uint32_t kIndexedFlag = 0x01000000u;

// Convert a colour to the packed form for the given colour depth.
std::uint32_t packColor(Color8 c, ColorDepth depth);

// Nearest entry in the xterm 256-colour palette (the 6x6x6 cube and the grey
// ramp; the first 16 entries are user-themeable, so they are avoided).
int nearestXterm256(Color8 c);

struct Cell {
    char32_t glyph = U' ';
    std::uint32_t fg = kDefaultColor, bg = kDefaultColor;
    constexpr bool operator==(const Cell&) const = default;
};

struct CellGrid {
    int cols = 0, rows = 0;
    std::vector<Cell> cells;

    void resize(int c, int r) {
        cols = c;
        rows = r;
        cells.assign(static_cast<std::size_t>(c) * static_cast<std::size_t>(r), Cell{});
    }
    Cell& at(int col, int row) { return cells[static_cast<std::size_t>(row) * cols + col]; }
    const Cell& at(int col, int row) const { return cells[static_cast<std::size_t>(row) * cols + col]; }
};

void appendUtf8(std::string& out, char32_t codepoint);

// Write `next` to the terminal with its top-left cell at the top-left of the
// screen, skipping every cell that already shows the right thing according
// to `shown`. Afterwards `shown` equals `next`.
//
// Pass redrawAll = true when the screen's contents are unknown (first frame,
// after a resize or clear). Returns the number of cells written.
int writeCellDiff(const CellGrid& next, CellGrid& shown, bool redrawAll, std::string& out);

// Write the whole grid as lines of text at the current cursor position.
void writeCellLines(const CellGrid& grid, std::string& out);

}  // namespace termesh
