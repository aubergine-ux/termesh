// The three character-cell backends: half-blocks, Braille and ASCII.
//
// Each one turns an image into a CellGrid; the shared base class then writes
// only the cells that changed since the previous frame.
#include <algorithm>

#include "core/color.hpp"
#include "output/backends.hpp"
#include "output/cell_grid.hpp"

namespace termesh {

namespace {

class CellBackend : public Backend {
public:
    explicit CellBackend(ColorDepth color) : color_(color) {}

    void present(ImageView image, std::string& out) override {
        encode(image, next_);
        writeCellDiff(next_, shown_, redrawAll_, out);
        redrawAll_ = false;
    }

    void print(ImageView image, std::string& out) override {
        encode(image, next_);
        writeCellLines(next_, out);
    }

    void invalidate() override { redrawAll_ = true; }

protected:
    virtual void encode(ImageView image, CellGrid& grid) = 0;

    ColorDepth color_;
    DrawArea area_;

private:
    CellGrid next_, shown_;
    bool redrawAll_ = true;
};

// ------------------------------------------------------------- half-block ---
//
// U+2580 UPPER HALF BLOCK fills the top half of a cell with the foreground
// colour and leaves the bottom half showing the background colour: two
// independently coloured pixels per cell.
class HalfBlockBackend final : public CellBackend {
public:
    using CellBackend::CellBackend;
    BackendKind kind() const override { return BackendKind::HalfBlock; }

    void configure(const DrawArea& area, int& width, int& height) override {
        area_ = area;
        width = area.cols;
        height = area.rows * 2;
        invalidate();
    }
    // A pixel is a whole cell wide and half a cell tall.
    float pixelAspect() const override {
        return static_cast<float>(area_.cellWidth) / (static_cast<float>(area_.cellHeight) * 0.5f);
    }
    int defaultSupersampling() const override { return 3; }

protected:
    void encode(ImageView image, CellGrid& grid) override {
        const int cols = image.width, rows = image.height / 2;
        if (grid.cols != cols || grid.rows != rows) grid.resize(cols, rows);
        for (int row = 0; row < rows; ++row) {
            const Color8* top = image.row(row * 2);
            const Color8* bottom = image.row(row * 2 + 1);
            for (int col = 0; col < cols; ++col) {
                Cell& cell = grid.at(col, row);
                const std::uint32_t upper = packColor(top[col], color_);
                const std::uint32_t lower = packColor(bottom[col], color_);
                if (upper == lower) {
                    // Both halves the same: a plain space is cheaper to send
                    // and never shows seams between rows.
                    cell = {U' ', lower, lower};
                } else {
                    cell = {U'▀', upper, lower};
                }
            }
        }
    }
};

// ---------------------------------------------------------------- braille ---
//
// The Braille block has one character for every combination of 2x4 dots,
// which makes each cell a tiny 2x4 monochrome bitmap. The code point is
// U+2800 plus a bit per raised dot:
//
//     0x01  0x08
//     0x02  0x10
//     0x04  0x20
//     0x40  0x80
//
// A dot is raised wherever the renderer drew something (alpha > 0); the
// cell's single foreground colour is the average of those pixels.
class BrailleBackend final : public CellBackend {
public:
    using CellBackend::CellBackend;
    BackendKind kind() const override { return BackendKind::Braille; }

    void configure(const DrawArea& area, int& width, int& height) override {
        area_ = area;
        width = area.cols * 2;
        height = area.rows * 4;
        invalidate();
    }
    float pixelAspect() const override {
        return (static_cast<float>(area_.cellWidth) / 2.0f) / (static_cast<float>(area_.cellHeight) / 4.0f);
    }
    bool wantsWireframe() const override { return true; }

protected:
    void encode(ImageView image, CellGrid& grid) override {
        static constexpr int dotBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
        const int cols = image.width / 2, rows = image.height / 4;
        if (grid.cols != cols || grid.rows != rows) grid.resize(cols, rows);

        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < cols; ++col) {
                int bits = 0, r = 0, g = 0, b = 0, n = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    const Color8* px = image.row(row * 4 + dy) + col * 2;
                    for (int dx = 0; dx < 2; ++dx) {
                        if (px[dx].a < 64) continue;
                        bits |= dotBit[dy][dx];
                        r += px[dx].r;
                        g += px[dx].g;
                        b += px[dx].b;
                        ++n;
                    }
                }
                Cell& cell = grid.at(col, row);
                if (n == 0) {
                    cell = Cell{};
                } else {
                    const Color8 average{static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n),
                                         static_cast<std::uint8_t>(b / n), 255};
                    cell = {static_cast<char32_t>(0x2800 + bits), packColor(average, color_), kDefaultColor};
                }
            }
    }
};

// ------------------------------------------------------------------ ascii ---
//
// One character per pixel, chosen by brightness from a ramp of glyphs that
// put progressively more ink in the cell. With colour available, the glyph
// carries the brightness and the foreground colour carries the hue.
class AsciiBackend final : public CellBackend {
public:
    using CellBackend::CellBackend;
    BackendKind kind() const override { return BackendKind::Ascii; }

    void configure(const DrawArea& area, int& width, int& height) override {
        area_ = area;
        width = area.cols;
        height = area.rows;
        invalidate();
    }
    float pixelAspect() const override {
        return static_cast<float>(area_.cellWidth) / static_cast<float>(area_.cellHeight);
    }
    int defaultSupersampling() const override { return 3; }

protected:
    void encode(ImageView image, CellGrid& grid) override {
        static constexpr char ramp[] = " .,:;=+ox%#@";
        constexpr int rampSize = static_cast<int>(sizeof(ramp)) - 1;

        if (grid.cols != image.width || grid.rows != image.height) grid.resize(image.width, image.height);
        for (int row = 0; row < image.height; ++row) {
            const Color8* px = image.row(row);
            for (int col = 0; col < image.width; ++col) {
                Cell& cell = grid.at(col, row);
                const Color8 c = px[col];
                if (c.a < 32) {  // background
                    cell = Cell{};
                    continue;
                }
                // Weight by coverage so silhouette edges fade out. Geometry
                // never drops below the first visible glyph.
                const int level = luma(c) * c.a / 255;
                const int index = std::clamp(1 + level * (rampSize - 1) / 256, 1, rampSize - 1);

                // Scale the colour up to full brightness: the glyph already
                // accounts for how dark the pixel is.
                const int peak = std::max({static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b), 1});
                const Color8 hue{static_cast<std::uint8_t>(c.r * 255 / peak),
                                 static_cast<std::uint8_t>(c.g * 255 / peak),
                                 static_cast<std::uint8_t>(c.b * 255 / peak), 255};
                cell = {static_cast<char32_t>(ramp[index]), packColor(hue, color_), kDefaultColor};
            }
        }
    }
};

}  // namespace

std::unique_ptr<Backend> makeHalfBlockBackend(ColorDepth color) {
    return std::make_unique<HalfBlockBackend>(color);
}
std::unique_ptr<Backend> makeBrailleBackend(ColorDepth color) {
    return std::make_unique<BrailleBackend>(color);
}
std::unique_ptr<Backend> makeAsciiBackend(ColorDepth color) { return std::make_unique<AsciiBackend>(color); }

}  // namespace termesh
