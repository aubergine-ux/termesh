#include "output/cell_grid.hpp"

#include <algorithm>
#include <cstdlib>

namespace termesh {

int nearestXterm256(Color8 c) {
    // The colour cube uses these six levels per channel (not evenly spaced).
    static constexpr int levels[6] = {0, 95, 135, 175, 215, 255};
    auto nearestLevel = [](int v) {
        int best = 0;
        for (int i = 1; i < 6; ++i)
            if (std::abs(levels[i] - v) < std::abs(levels[best] - v)) best = i;
        return best;
    };
    const int r = nearestLevel(c.r), g = nearestLevel(c.g), b = nearestLevel(c.b);
    const int cubeDist = (levels[r] - c.r) * (levels[r] - c.r) + (levels[g] - c.g) * (levels[g] - c.g) +
                         (levels[b] - c.b) * (levels[b] - c.b);

    // The grey ramp: 24 steps from 8 to 238.
    const int average = (c.r + c.g + c.b) / 3;
    const int grey = std::clamp((average - 8 + 5) / 10, 0, 23);
    const int greyValue = 8 + grey * 10;
    const int greyDist = (greyValue - c.r) * (greyValue - c.r) + (greyValue - c.g) * (greyValue - c.g) +
                         (greyValue - c.b) * (greyValue - c.b);

    return greyDist < cubeDist ? 232 + grey : 16 + 36 * r + 6 * g + b;
}

std::uint32_t packColor(Color8 c, ColorDepth depth) {
    switch (depth) {
        case ColorDepth::TrueColor:
            return (static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) | c.b;
        case ColorDepth::Indexed256:
            return kIndexedFlag | static_cast<std::uint32_t>(nearestXterm256(c));
        default:
            return kDefaultColor;
    }
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

namespace {

// std::to_string allocates; this runs for nearly every changed cell.
void appendInt(std::string& out, unsigned value) {
    char buf[10];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) out += buf[--n];
}

// Append the SGR parameters selecting a colour. `base` is 38 for foreground
// or 48 for background; the "default colour" codes are base + 1.
void appendColorParams(std::string& out, std::uint32_t color, unsigned base) {
    if (color == kDefaultColor) {
        appendInt(out, base + 1);
    } else if (color & kIndexedFlag) {
        appendInt(out, base);
        out += ";5;";
        appendInt(out, color & 0xFF);
    } else {
        appendInt(out, base);
        out += ";2;";
        appendInt(out, (color >> 16) & 0xFF);
        out += ';';
        appendInt(out, (color >> 8) & 0xFF);
        out += ';';
        appendInt(out, color & 0xFF);
    }
}

// Tracks the colours the terminal currently has selected, so a run of cells
// sharing a colour only pays for it once.
struct PenState {
    std::uint32_t fg = kDefaultColor, bg = kDefaultColor;

    void apply(const Cell& cell, std::string& out) {
        // A space shows no foreground, so its foreground colour is irrelevant.
        const bool needFg = cell.glyph != U' ' && cell.fg != fg;
        const bool needBg = cell.bg != bg;
        if (!needFg && !needBg) return;
        out += "\x1b[";
        if (needFg) {
            appendColorParams(out, cell.fg, 38);
            fg = cell.fg;
        }
        if (needBg) {
            if (needFg) out += ';';
            appendColorParams(out, cell.bg, 48);
            bg = cell.bg;
        }
        out += 'm';
    }
};

}  // namespace

int writeCellDiff(const CellGrid& next, CellGrid& shown, bool redrawAll, std::string& out) {
    if (shown.cols != next.cols || shown.rows != next.rows) {
        shown.resize(next.cols, next.rows);
        redrawAll = true;
    }

    out += "\x1b[0m";
    PenState pen;
    int written = 0;
    // Where the terminal's cursor is, if we know (-1 = unknown).
    int cursorRow = -1, cursorCol = -1;

    for (int row = 0; row < next.rows; ++row) {
        for (int col = 0; col < next.cols; ++col) {
            const Cell& cell = next.at(col, row);
            Cell& old = shown.at(col, row);
            if (!redrawAll && cell == old) continue;

            // Move the cursor only if it is not already here - after writing
            // a cell it sits on the next one, so runs of changes are free.
            if (cursorRow != row || cursorCol != col) {
                if (cursorRow == row && col > cursorCol) {
                    out += "\x1b[";  // CUF: cursor forward
                    appendInt(out, static_cast<unsigned>(col - cursorCol));
                    out += 'C';
                } else {
                    out += "\x1b[";  // CUP: absolute position (1-based)
                    appendInt(out, static_cast<unsigned>(row + 1));
                    out += ';';
                    appendInt(out, static_cast<unsigned>(col + 1));
                    out += 'H';
                }
            }
            pen.apply(cell, out);
            appendUtf8(out, cell.glyph);
            old = cell;
            ++written;
            cursorRow = row;
            cursorCol = col + 1;
        }
    }
    out += "\x1b[0m";
    return written;
}

void writeCellLines(const CellGrid& grid, std::string& out) {
    for (int row = 0; row < grid.rows; ++row) {
        PenState pen;
        for (int col = 0; col < grid.cols; ++col) {
            const Cell& cell = grid.at(col, row);
            pen.apply(cell, out);
            appendUtf8(out, cell.glyph);
        }
        out += "\x1b[0m\n";
    }
}

}  // namespace termesh
