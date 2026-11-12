#include "output/halfblock.hpp"

namespace termesh {

namespace {

void appendColor(std::string& out, const char* prefix, Color8 c) {
    out += prefix;
    out += std::to_string(c.r);
    out += ';';
    out += std::to_string(c.g);
    out += ';';
    out += std::to_string(c.b);
    out += 'm';
}

}  // namespace

std::string encodeHalfBlocks(const Framebuffer& fb) {
    std::string out;
    out.reserve(static_cast<std::size_t>(fb.width()) * fb.height() * 12);

    for (int y = 0; y + 1 < fb.height(); y += 2) {
        const Color8* top = fb.colorRow(y);
        const Color8* bottom = fb.colorRow(y + 1);
        Color8 fg{}, bg{};
        bool haveColors = false;
        for (int x = 0; x < fb.width(); ++x) {
            // Only emit colour codes when they change from the previous cell.
            if (!haveColors || !(top[x] == fg)) appendColor(out, "\x1b[38;2;", top[x]);
            if (!haveColors || !(bottom[x] == bg)) appendColor(out, "\x1b[48;2;", bottom[x]);
            fg = top[x];
            bg = bottom[x];
            haveColors = true;
            out += "\xe2\x96\x80";  // U+2580 in UTF-8
        }
        out += "\x1b[0m\n";
    }
    return out;
}

}  // namespace termesh
