#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <stb/stb_image.h>

#include "core/image.hpp"
#include "export/gif.hpp"
#include "output/backends.hpp"
#include "output/cell_grid.hpp"

using namespace termesh;

namespace {

Image solid(int w, int h, Color8 c) {
    Image image(w, h);
    for (Color8& p : image.pixels) p = c;
    return image;
}

std::size_t count(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
    return n;
}

const std::string kHalfBlock = "\xe2\x96\x80";

}  // namespace

TEST_CASE("cells: xterm 256-colour quantisation") {
    CHECK(nearestXterm256({0, 0, 0, 255}) == 16);
    CHECK(nearestXterm256({255, 255, 255, 255}) == 231);
    CHECK(nearestXterm256({255, 0, 0, 255}) == 196);
    CHECK(nearestXterm256({0, 0, 255, 255}) == 21);
    // Mid greys are closer to the grey ramp than to any cube entry.
    const int grey = nearestXterm256({128, 128, 128, 255});
    CHECK(grey >= 232);
    CHECK(grey <= 255);
}

TEST_CASE("cells: packColor respects the colour depth") {
    const Color8 c{10, 20, 30, 255};
    CHECK(packColor(c, ColorDepth::TrueColor) == 0x0A141Eu);
    CHECK((packColor(c, ColorDepth::Indexed256) & kIndexedFlag) != 0u);
    CHECK(packColor(c, ColorDepth::None) == kDefaultColor);
}

TEST_CASE("cells: UTF-8 encoding") {
    std::string s;
    appendUtf8(s, U'A');
    appendUtf8(s, U'é');
    appendUtf8(s, U'▀');
    appendUtf8(s, U'\U0001F642');
    CHECK(s == "A\xc3\xa9\xe2\x96\x80\xf0\x9f\x99\x82");
}

TEST_CASE("cells: the first frame draws everything, an identical frame draws nothing") {
    CellGrid next, shown;
    next.resize(4, 2);
    for (Cell& cell : next.cells) cell = {U'x', 0xFF0000, 0x000000};

    std::string out;
    CHECK(writeCellDiff(next, shown, true, out) == 8);
    CHECK(count(out, "x") == 8);

    out.clear();
    CHECK(writeCellDiff(next, shown, false, out) == 0);
    CHECK(count(out, "x") == 0);
}

TEST_CASE("cells: only changed cells are rewritten, with a cursor jump to each run") {
    CellGrid next, shown;
    next.resize(10, 3);
    std::string out;
    writeCellDiff(next, shown, true, out);

    next.at(2, 1) = {U'a', 0x00FF00, kDefaultColor};
    next.at(3, 1) = {U'b', 0x00FF00, kDefaultColor};  // adjacent: no cursor movement needed
    next.at(7, 1) = {U'c', 0x00FF00, kDefaultColor};  // same row: a relative move
    next.at(0, 2) = {U'd', 0x0000FF, kDefaultColor};  // new row: an absolute move

    out.clear();
    CHECK(writeCellDiff(next, shown, false, out) == 4);
    CHECK(out.find("\x1b[2;3H") != std::string::npos);  // row 2, column 3 (1-based)
    CHECK(out.find("ab") != std::string::npos);
    CHECK(out.find("\x1b[3C") != std::string::npos);    // from column 4 forward to 7
    CHECK(out.find("\x1b[3;1H") != std::string::npos);
    // The colour shared by a, b and c is selected once.
    CHECK(count(out, "38;2;0;255;0") == 1);
    CHECK(shown.cells == next.cells);
}

TEST_CASE("cells: a size change forces a full redraw") {
    CellGrid next, shown;
    next.resize(3, 3);
    std::string out;
    writeCellDiff(next, shown, true, out);
    next.resize(5, 2);
    out.clear();
    CHECK(writeCellDiff(next, shown, false, out) == 10);
}

TEST_CASE("halfblock: two pixels per cell, top in the foreground") {
    auto backend = makeHalfBlockBackend(ColorDepth::TrueColor);
    DrawArea area{4, 2, 10, 20};
    int w = 0, h = 0;
    backend->configure(area, w, h);
    CHECK(w == 4);
    CHECK(h == 4);
    CHECK(backend->pixelAspect() == doctest::Approx(1.0f));  // 10 wide, 20 / 2 tall

    Image image = solid(4, 4, {0, 0, 0, 255});
    image.row(0)[1] = {255, 0, 0, 255};  // top half of cell (1, 0)
    image.row(1)[1] = {0, 0, 255, 255};  // bottom half

    std::string out;
    backend->print(image.view(), out);
    CHECK(count(out, kHalfBlock) == 1);  // every other cell is uniform: a space
    CHECK(out.find("38;2;255;0;0") != std::string::npos);
    CHECK(out.find("48;2;0;0;255") != std::string::npos);
    CHECK(count(out, "\n") == 2);
}

TEST_CASE("halfblock: present() sends only what changed") {
    auto backend = makeHalfBlockBackend(ColorDepth::TrueColor);
    DrawArea area{20, 10, 10, 20};
    int w = 0, h = 0;
    backend->configure(area, w, h);
    Image image = solid(w, h, {5, 5, 5, 255});

    std::string first, second, third;
    backend->present(image.view(), first);
    backend->present(image.view(), second);
    image.row(7)[3] = {200, 100, 50, 255};
    backend->present(image.view(), third);

    CHECK(second.size() < 20);            // just the resets
    CHECK(third.size() < first.size() / 4);
    CHECK(count(third, kHalfBlock) == 1);

    // After invalidate() the whole screen is redrawn.
    backend->invalidate();
    std::string fourth;
    backend->present(image.view(), fourth);
    CHECK(fourth.size() >= first.size());
}

TEST_CASE("braille: dot bits follow the Unicode layout") {
    auto backend = makeBrailleBackend(ColorDepth::None);
    DrawArea area{1, 1, 10, 20};
    int w = 0, h = 0;
    backend->configure(area, w, h);
    CHECK(w == 2);
    CHECK(h == 4);
    CHECK(backend->wantsWireframe());

    auto glyphFor = [&](std::initializer_list<std::pair<int, int>> dots) {
        Image image = solid(2, 4, {0, 0, 0, 0});  // alpha 0: nothing drawn
        for (auto [x, y] : dots) image.row(y)[x] = {255, 255, 255, 255};
        std::string out;
        backend->print(image.view(), out);
        return out.substr(0, out.find("\x1b[0m"));
    };
    CHECK(glyphFor({}) == " ");
    CHECK(glyphFor({{0, 0}}) == "\xe2\xa0\x81");                              // U+2801
    CHECK(glyphFor({{1, 0}}) == "\xe2\xa0\x88");                              // U+2808
    CHECK(glyphFor({{0, 3}}) == "\xe2\xa1\x80");                              // U+2840
    CHECK(glyphFor({{1, 3}}) == "\xe2\xa2\x80");                              // U+2880
    CHECK(glyphFor({{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}, {1, 2}, {0, 3}, {1, 3}}) == "\xe2\xa3\xbf");  // U+28FF
}

TEST_CASE("ascii: brighter pixels get denser glyphs, background stays blank") {
    auto backend = makeAsciiBackend(ColorDepth::None);
    DrawArea area{3, 1, 10, 20};
    int w = 0, h = 0;
    backend->configure(area, w, h);
    CHECK(w == 3);
    CHECK(h == 1);
    CHECK(backend->pixelAspect() == doctest::Approx(0.5f));

    Image image(3, 1);
    image.row(0)[0] = {0, 0, 0, 0};          // background
    image.row(0)[1] = {40, 40, 40, 255};     // dim
    image.row(0)[2] = {255, 255, 255, 255};  // bright
    std::string out;
    backend->print(image.view(), out);
    REQUIRE(out.size() >= 3);
    CHECK(out[0] == ' ');
    CHECK(out[1] != ' ');
    CHECK(out[2] == '@');
    CHECK(out[1] != out[2]);
}

TEST_CASE("kitty: chunked base64 round-trips the pixels") {
    auto backend = makeKittyBackend();
    DrawArea area{40, 20, 10, 20};
    int w = 0, h = 0;
    backend->configure(area, w, h);
    CHECK(w == 400);
    CHECK(h == 400);

    Image image(w, h);
    for (int i = 0; i < w * h; ++i)
        image.pixels[i] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8),
                           static_cast<std::uint8_t>(i * 7), 255};
    std::string out;
    backend->present(image.view(), out);

    // Pull the payload out of each APC chunk and check the framing.
    std::string payload;
    std::size_t pos = 0, chunks = 0;
    while ((pos = out.find("\x1b_G", pos)) != std::string::npos) {
        const std::size_t semicolon = out.find(';', pos);
        const std::size_t end = out.find("\x1b\\", pos);
        REQUIRE(semicolon < end);
        const std::string control = out.substr(pos + 3, semicolon - pos - 3);
        const std::string data = out.substr(semicolon + 1, end - semicolon - 1);
        CHECK(data.size() <= 4096);
        if (chunks == 0) {
            CHECK(control.find("a=T") != std::string::npos);
            CHECK(control.find("s=400") != std::string::npos);
            CHECK(control.find("v=400") != std::string::npos);
            CHECK(control.find("c=40") != std::string::npos);
            CHECK(control.find("r=20") != std::string::npos);
        }
        const bool last = out.find("\x1b_G", end) == std::string::npos;
        CHECK((control.find("m=0") != std::string::npos) == last);
        payload += data;
        ++chunks;
        pos = end;
    }
    CHECK(chunks == (400 * 400 * 3 + 3071) / 3072);

    // Decode the base64.
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : (c == '/' ? 63 : -1);
    };
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 3 < payload.size(); i += 4) {
        const int a = value(payload[i]), b = value(payload[i + 1]), c = value(payload[i + 2]), d = value(payload[i + 3]);
        bytes.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (c >= 0) bytes.push_back(static_cast<std::uint8_t>((b << 4) | (c >> 2)));
        if (d >= 0) bytes.push_back(static_cast<std::uint8_t>((c << 6) | d));
    }
    REQUIRE(bytes.size() == static_cast<std::size_t>(w) * h * 3);
    bool same = true;
    for (int i = 0; i < w * h && same; ++i)
        same = bytes[i * 3] == image.pixels[i].r && bytes[i * 3 + 1] == image.pixels[i].g &&
               bytes[i * 3 + 2] == image.pixels[i].b;
    CHECK(same);
}

TEST_CASE("sixel: structure of the encoded stream") {
    auto backend = makeSixelBackend();
    DrawArea area{4, 2, 10, 21};  // 40 x 42 pixels
    int w = 0, h = 0;
    backend->configure(area, w, h);
    CHECK(w == 40);
    CHECK(h == 42);  // a whole number of 6-pixel bands
    CHECK(backend->avoidsLastRow());

    Image image = solid(w, h, {255, 0, 0, 255});
    std::string out;
    backend->print(image.view(), out);

    CHECK(out.rfind("\x1bPq\"1;1;40;42", 0) == 0);
    CHECK(out.find("\x1b\\") != std::string::npos);
    CHECK(count(out, "-") == 7);  // seven bands
    // Pure red is palette entry (5, 0, 0) = 5 * 7 * 6 = 210, defined as 100% red.
    CHECK(out.find("#210;2;100;0;0") != std::string::npos);
    // A solid band is one run of 40 full columns ('~' = all six pixels set).
    CHECK(count(out, "#210!40~") == 7);
}

TEST_CASE("gif: frames survive an encode / decode round trip") {
    const int w = 48, h = 32, frameCount = 5;
    std::vector<Image> frames;
    for (int f = 0; f < frameCount; ++f) {
        Image image = solid(w, h, {16, 17, 22, 255});
        // A coloured square that moves each frame over a static background.
        for (int y = 8; y < 24; ++y)
            for (int x = 4 + f * 6; x < 20 + f * 6; ++x)
                image.row(y)[x] = {static_cast<std::uint8_t>(200 - f * 30), static_cast<std::uint8_t>(40 + x * 3),
                                   static_cast<std::uint8_t>(y * 8), 255};
        frames.push_back(std::move(image));
    }

    const GifPalette palette = buildGifPalette(frames);
    CHECK(palette.count > 2);
    CHECK(palette.count <= kGifColors);

    const std::string path = "termesh_test_roundtrip.gif";
    {
        GifWriter writer;
        REQUIRE(writer.open(path, w, h, palette, 4));
        for (const Image& frame : frames) REQUIRE(writer.addFrame(frame));
        REQUIRE(writer.close());
    }

    // Read it back with stb_image's GIF decoder.
    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    std::vector<unsigned char> bytes;
    unsigned char buffer[4096];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(file);
    std::remove(path.c_str());

    int dw = 0, dh = 0, dframes = 0, comp = 0;
    int* delays = nullptr;
    unsigned char* decoded = stbi_load_gif_from_memory(bytes.data(), static_cast<int>(bytes.size()), &delays, &dw,
                                                       &dh, &dframes, &comp, 4);
    REQUIRE(decoded != nullptr);
    CHECK(dw == w);
    CHECK(dh == h);
    CHECK(dframes == frameCount);
    CHECK(delays[0] == 40);  // stb reports milliseconds

    // Every pixel of every frame must come back as its nearest palette
    // colour - including the ones the encoder skipped as "unchanged".
    int mismatches = 0;
    for (int f = 0; f < frameCount; ++f)
        for (int i = 0; i < w * h; ++i) {
            const Color8 expected = palette.colors[palette.nearest(frames[f].pixels[i])];
            const unsigned char* px = decoded + (static_cast<std::size_t>(f) * w * h + i) * 4;
            mismatches += px[0] != expected.r || px[1] != expected.g || px[2] != expected.b;
        }
    CHECK(mismatches == 0);
    stbi_image_free(decoded);
    stbi_image_free(delays);
}

TEST_CASE("gif: the palette reproduces a small set of colours closely") {
    Image image(4, 1);
    image.pixels = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {250, 250, 250, 255}};
    const GifPalette palette = buildGifPalette({image});
    CHECK(palette.count == 4);
    for (const Color8& c : image.pixels) {
        const Color8 q = palette.colors[palette.nearest(c)];
        CHECK(std::abs(q.r - c.r) <= 8);
        CHECK(std::abs(q.g - c.g) <= 8);
        CHECK(std::abs(q.b - c.b) <= 8);
    }
}
