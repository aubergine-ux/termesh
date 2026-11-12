// The two real-pixel backends: the kitty graphics protocol and sixel.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "output/backends.hpp"

namespace termesh {

namespace {

// Sending a full-resolution image every frame is a lot of bytes for the
// terminal to chew through, so pixel backends render at most this many
// pixels and have the image scaled up to fill the window.
constexpr double kPixelBudget = 520'000.0;

void appendInt(std::string& out, int value) { out += std::to_string(value); }

// ------------------------------------------------------------------ kitty ---
//
// The kitty graphics protocol sends an image inside an APC escape sequence:
//
//     ESC _ G  key=value,...  ;  base64 pixel data  ESC-backslash
//
// Large images are split into chunks of at most 4096 base64 characters, each
// marked m=1 ("more follows") except the last. The keys used here:
//
//     a=T      transmit the image and display it
//     f=24     pixel format: 24-bit RGB
//     s, v     width and height in pixels
//     i, p     image and placement ids; re-sending the same ids replaces the
//              previous frame in place, without flicker
//     c, r     scale the image to cover this many columns and rows
//     C=1      leave the cursor where it is
//     q=2      do not send back OK / error responses
class KittyBackend final : public Backend {
public:
    BackendKind kind() const override { return BackendKind::Kitty; }

    void configure(const DrawArea& area, int& width, int& height) override {
        area_ = area;
        const double fullWidth = static_cast<double>(area.cols) * area.cellWidth;
        const double fullHeight = static_cast<double>(area.rows) * area.cellHeight;
        const double scale = std::min(1.0, std::sqrt(kPixelBudget / std::max(fullWidth * fullHeight, 1.0)));
        width_ = width = std::max(static_cast<int>(fullWidth * scale), 1);
        height_ = height = std::max(static_cast<int>(fullHeight * scale), 1);
    }

    // The terminal stretches the image over the cell area, so a pixel's
    // shape is the ratio of the two scale factors (very nearly 1).
    float pixelAspect() const override {
        const float sx = static_cast<float>(area_.cols * area_.cellWidth) / static_cast<float>(width_);
        const float sy = static_cast<float>(area_.rows * area_.cellHeight) / static_cast<float>(height_);
        return sx / sy;
    }

    void present(ImageView image, std::string& out) override {
        out += "\x1b[H";
        transmit(image, true, out);
    }

    void print(ImageView image, std::string& out) override {
        transmit(image, false, out);
        out += '\n';
    }

    void invalidate() override {}

    void cleanup(std::string& out) override {
        out += "\x1b_Ga=d,d=I,i=";  // delete the image and free its data
        appendInt(out, kImageId);
        out += ",q=2\x1b\\";
    }

private:
    static constexpr int kImageId = 7341;

    void transmit(ImageView image, bool inPlace, std::string& out) {
        // Strip the alpha channel: 3 bytes per pixel instead of 4.
        rgb_.resize(static_cast<std::size_t>(image.width) * image.height * 3);
        const std::size_t pixelCount = static_cast<std::size_t>(image.width) * image.height;
        for (std::size_t i = 0; i < pixelCount; ++i) {
            rgb_[i * 3] = image.data[i].r;
            rgb_[i * 3 + 1] = image.data[i].g;
            rgb_[i * 3 + 2] = image.data[i].b;
        }

        // 3072 raw bytes encode to exactly 4096 base64 characters, so every
        // chunk but the last is a whole number of base64 groups.
        constexpr std::size_t kChunkBytes = 3072;
        bool first = true;
        for (std::size_t offset = 0; offset < rgb_.size() || first; offset += kChunkBytes) {
            const std::size_t n = std::min(kChunkBytes, rgb_.size() - offset);
            const bool more = offset + n < rgb_.size();
            out += "\x1b_G";
            if (first) {
                out += "a=T,q=2,f=24,s=";
                appendInt(out, image.width);
                out += ",v=";
                appendInt(out, image.height);
                out += ",i=";
                appendInt(out, kImageId);
                if (inPlace) {
                    out += ",p=1,C=1,c=";
                    appendInt(out, area_.cols);
                    out += ",r=";
                    appendInt(out, area_.rows);
                }
                out += ',';
                first = false;
            }
            out += more ? "m=1;" : "m=0;";
            appendBase64(rgb_.data() + offset, n, out);
            out += "\x1b\\";
        }
    }

    static void appendBase64(const std::uint8_t* data, std::size_t size, std::string& out) {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::size_t i = 0;
        for (; i + 2 < size; i += 3) {
            const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            out += alphabet[v >> 18];
            out += alphabet[(v >> 12) & 63];
            out += alphabet[(v >> 6) & 63];
            out += alphabet[v & 63];
        }
        if (i + 1 == size) {
            const std::uint32_t v = data[i] << 16;
            out += alphabet[v >> 18];
            out += alphabet[(v >> 12) & 63];
            out += "==";
        } else if (i + 2 == size) {
            const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
            out += alphabet[v >> 18];
            out += alphabet[(v >> 12) & 63];
            out += alphabet[(v >> 6) & 63];
            out += '=';
        }
    }

    DrawArea area_;
    int width_ = 1, height_ = 1;
    std::vector<std::uint8_t> rgb_;
};

// ------------------------------------------------------------------ sixel ---
//
// Sixel is a 1980s DEC printer protocol that modern terminals have adopted
// for inline images. The picture is sent in horizontal bands six pixels
// tall. Within a band each character encodes one column of six pixels - a
// "sixel" - as 63 plus a bit per lit pixel, top pixel in the lowest bit.
// A sixel character only paints the currently selected colour, so a band is
// drawn in several passes, one per colour, rewinding with '$' in between:
//
//     ESC P q                 start of sixel data
//     " 1 ; 1 ; W ; H         raster attributes: 1:1 pixels, image size
//     # n ; 2 ; r ; g ; b     define palette entry n (components in percent)
//     # n                     select colour n
//     ! count char            run-length encoding: repeat char `count` times
//     $                       carriage return: back to the start of the band
//     -                       line feed: on to the next band
//     ESC-backslash           end
//
// The palette is limited (256 entries at most), so the image is reduced to a
// fixed 6x7x6 colour cube first, with dithering to hide the steps: ordered
// while animating, Floyd-Steinberg for a single printed frame.
class SixelBackend final : public Backend {
public:
    BackendKind kind() const override { return BackendKind::Sixel; }

    void configure(const DrawArea& area, int& width, int& height) override {
        const int fullWidth = area.cols * area.cellWidth;
        // A whole number of bands, so the image ends exactly where we expect.
        const int fullHeight = area.rows * area.cellHeight / 6 * 6;
        // Sixel has no scaling of its own. To stay within the pixel budget,
        // render smaller and send every pixel as a zoom x zoom block.
        zoom_ = 1;
        while (static_cast<double>(fullWidth / zoom_) * (fullHeight / zoom_) > kPixelBudget && zoom_ < 6) ++zoom_;
        width = std::max(fullWidth / zoom_, 1);
        height = std::max(fullHeight / zoom_, 1);
    }

    float pixelAspect() const override { return 1.0f; }
    bool avoidsLastRow() const override { return true; }

    void present(ImageView image, std::string& out) override {
        out += "\x1b[H";
        encode(image, out, false);
    }

    void print(ImageView image, std::string& out) override {
        encode(image, out, true);
        out += '\n';
    }

    void invalidate() override {}

private:
    static constexpr int kRedLevels = 6, kGreenLevels = 7, kBlueLevels = 6;
    static constexpr int kCubeSize = kRedLevels * kGreenLevels * kBlueLevels;  // 252
    // One extra entry holds the exact background colour, so the empty area
    // around the model is flat instead of a dither pattern.
    static constexpr int kBackgroundIndex = kCubeSize;
    static constexpr int kPaletteSize = kCubeSize + 1;

    // Ordered (Bayer) dithering: before rounding each channel to the nearest
    // palette level, nudge it by a position-dependent threshold. Neighbouring
    // pixels round in different directions, so their average approximates
    // the true colour. Unlike error diffusion the pattern is stable from
    // frame to frame, which matters for animation.
    static int quantize(int value, int levels, int threshold) {
        // value in 0..255, threshold in 0..15 (a 4x4 Bayer matrix entry).
        const int scaled = value * (levels - 1) * 16 + threshold * 255;
        return std::min(scaled / (255 * 16), levels - 1);
    }

    void encode(ImageView image, std::string& out, bool still) {
        static constexpr int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        const int width = image.width, height = image.height;

        const DitherMode mode = resolveDither(dither_, still);
        if (mode == DitherMode::FloydSteinberg) {
            // Error diffusion needs to know the colour each pixel actually
            // got, so it runs as a pass of its own; what it leaves behind
            // is already on the cube and only needs converting to indices.
            auto toCube = [](Color8 c) {
                auto snap = [](int value, int levels) {
                    return static_cast<std::uint8_t>(quantize(value, levels, 8) * 255 / (levels - 1));
                };
                return Color8{snap(c.r, kRedLevels), snap(c.g, kGreenLevels), snap(c.b, kBlueLevels), c.a};
            };
            ditherImage(image, mode, 0.0f, toCube, dithered_);
            image = dithered_.view();
        }

        // Pass 1: reduce every pixel to a palette index.
        indices_.resize(static_cast<std::size_t>(width) * height);
        Color8 background{0, 0, 0, 0};
        for (int y = 0; y < height; ++y) {
            const Color8* row = image.row(y);
            std::uint8_t* dst = indices_.data() + static_cast<std::size_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                if (row[x].a == 0) {  // nothing was drawn here
                    background = row[x];
                    dst[x] = static_cast<std::uint8_t>(kBackgroundIndex);
                    continue;
                }
                // A threshold of 8 is plain rounding to the nearest level.
                const int t = mode == DitherMode::Ordered ? bayer[y & 3][x & 3] : 8;
                const int r = quantize(row[x].r, kRedLevels, t);
                const int g = quantize(row[x].g, kGreenLevels, t);
                const int b = quantize(row[x].b, kBlueLevels, t);
                dst[x] = static_cast<std::uint8_t>((r * kGreenLevels + g) * kBlueLevels + b);
            }
        }

        out += "\x1bPq\"1;1;";
        appendInt(out, width * zoom_);
        out += ';';
        appendInt(out, height * zoom_);
        out += '#';
        appendInt(out, kBackgroundIndex);
        out += ";2;";
        appendInt(out, (background.r * 100 + 127) / 255);
        out += ';';
        appendInt(out, (background.g * 100 + 127) / 255);
        out += ';';
        appendInt(out, (background.b * 100 + 127) / 255);
        for (int i = 0; i < kCubeSize; ++i) {
            const int b = i % kBlueLevels, g = i / kBlueLevels % kGreenLevels, r = i / (kBlueLevels * kGreenLevels);
            out += '#';
            appendInt(out, i);
            out += ";2;";
            appendInt(out, r * 100 / (kRedLevels - 1));
            out += ';';
            appendInt(out, g * 100 / (kGreenLevels - 1));
            out += ';';
            appendInt(out, b * 100 / (kBlueLevels - 1));
        }

        // Pass 2: emit the bands. `columns_` holds, for each palette colour,
        // the sixel bit pattern of every column of the current band.
        columns_.assign(static_cast<std::size_t>(kPaletteSize) * width, 0);
        std::vector<int> usedColors;
        bool used[kPaletteSize] = {};
        const int outHeight = height * zoom_;

        for (int bandTop = 0; bandTop < outHeight; bandTop += 6) {
            usedColors.clear();
            for (int bit = 0; bit < 6 && bandTop + bit < outHeight; ++bit) {
                // With zoom, several output rows read the same source row.
                const std::uint8_t* src = indices_.data() + static_cast<std::size_t>((bandTop + bit) / zoom_) * width;
                for (int x = 0; x < width; ++x) {
                    const int color = src[x];
                    columns_[static_cast<std::size_t>(color) * width + x] |= static_cast<std::uint8_t>(1 << bit);
                    if (!used[color]) {
                        used[color] = true;
                        usedColors.push_back(color);
                    }
                }
            }

            for (std::size_t n = 0; n < usedColors.size(); ++n) {
                const int color = usedColors[n];
                std::uint8_t* column = columns_.data() + static_cast<std::size_t>(color) * width;
                if (n > 0) out += '$';
                out += '#';
                appendInt(out, color);

                // Run-length encode, skipping trailing empty columns.
                int last = width - 1;
                while (last >= 0 && column[last] == 0) --last;
                for (int x = 0; x <= last;) {
                    const std::uint8_t pattern = column[x];
                    int run = 1;
                    while (x + run <= last && column[x + run] == pattern) ++run;
                    const int repeat = run * zoom_;
                    const char ch = static_cast<char>(63 + pattern);
                    if (repeat > 3) {
                        out += '!';
                        appendInt(out, repeat);
                        out += ch;
                    } else {
                        out.append(static_cast<std::size_t>(repeat), ch);
                    }
                    x += run;
                }
                std::fill(column, column + width, std::uint8_t{0});  // ready for the next band
                used[color] = false;
            }
            out += '-';
        }
        out += "\x1b\\";
    }

    int zoom_ = 1;
    std::vector<std::uint8_t> indices_;
    Image dithered_;
    std::vector<std::uint8_t> columns_;
};

}  // namespace

std::unique_ptr<Backend> makeKittyBackend() { return std::make_unique<KittyBackend>(); }
std::unique_ptr<Backend> makeSixelBackend() { return std::make_unique<SixelBackend>(); }

const char* backendName(BackendKind kind) {
    switch (kind) {
        case BackendKind::Kitty: return "kitty";
        case BackendKind::Sixel: return "sixel";
        case BackendKind::HalfBlock: return "halfblock";
        case BackendKind::Braille: return "braille";
        default: return "ascii";
    }
}

std::optional<BackendKind> backendFromName(std::string_view name) {
    for (BackendKind kind :
         {BackendKind::Kitty, BackendKind::Sixel, BackendKind::HalfBlock, BackendKind::Braille, BackendKind::Ascii})
        if (name == backendName(kind)) return kind;
    if (name == "half-block" || name == "blocks") return BackendKind::HalfBlock;
    return std::nullopt;
}

std::unique_ptr<Backend> makeBackend(BackendKind kind, ColorDepth color) {
    switch (kind) {
        case BackendKind::Kitty: return makeKittyBackend();
        case BackendKind::Sixel: return makeSixelBackend();
        case BackendKind::HalfBlock: return makeHalfBlockBackend(color);
        case BackendKind::Braille: return makeBrailleBackend(color);
        default: return makeAsciiBackend(color);
    }
}

}  // namespace termesh
