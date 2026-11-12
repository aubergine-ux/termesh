/*
 * A backend plugin: quadrant blocks.
 *
 * Unicode has a character for every way of filling the four quarters of a
 * cell (U+2596 - U+259F, with the half blocks and the full block). Each
 * cell can show two colours - foreground and background - so this backend
 * gives every cell 2 x 2 pixels, splits them into a brighter and a darker
 * group, and picks the character whose shape is the brighter group. That
 * is twice the horizontal resolution of half blocks, at the price of only
 * two colours for four pixels.
 *
 *     cc -shared -fPIC -I<termesh>/include quadrant.c -o quadrant.so
 *     termesh --plugin ./quadrant.so --backend quadrant model.glb
 */
#include <stdio.h>
#include <string.h>

#include <termesh_plugin.h>

/* The character for each set of bright quarters: bit 0 top-left, bit 1
 * top-right, bit 2 bottom-left, bit 3 bottom-right. */
static const char* const kGlyphs[16] = {
    " ",
    "\xe2\x96\x98",
    "\xe2\x96\x9d",
    "\xe2\x96\x80", /* (none)  ▘  ▝  ▀ */
    "\xe2\x96\x96",
    "\xe2\x96\x8c",
    "\xe2\x96\x9e",
    "\xe2\x96\x9b", /* ▖  ▌  ▞  ▛ */
    "\xe2\x96\x97",
    "\xe2\x96\x9a",
    "\xe2\x96\x90",
    "\xe2\x96\x9c", /* ▗  ▚  ▐  ▜ */
    "\xe2\x96\x84",
    "\xe2\x96\x99",
    "\xe2\x96\x9f",
    "\xe2\x96\x88", /* ▄  ▙  ▟  █ */
};

static int luminance(const uint8_t* p) { return p[0] * 3 + p[1] * 6 + p[2]; }

/* The nearest colour of the xterm 256-colour palette's 6 x 6 x 6 cube. */
static int cube256(int r, int g, int b) {
    return 16 + 36 * ((r * 5 + 127) / 255) + 6 * ((g * 5 + 127) / 255) + (b * 5 + 127) / 255;
}

/* Append the escape sequence that selects a colour. `layer` is 38 for the
 * foreground and 48 for the background. */
static size_t set_color(char* out, int layer, int color_mode, const int rgb[3]) {
    if (color_mode == TERMESH_PLUGIN_COLOR_TRUECOLOR)
        return (size_t)sprintf(out, "\x1b[%d;2;%d;%d;%dm", layer, rgb[0], rgb[1], rgb[2]);
    return (size_t)sprintf(out, "\x1b[%d;5;%dm", layer, cube256(rgb[0], rgb[1], rgb[2]));
}

static void draw(void* user, const termesh_plugin_frame* frame, termesh_plugin_output* out, const termesh_host* host) {
    /* At most 2 colour changes of 20 bytes and a 3-byte character per cell. */
    static char line[4096 * 44 + 1];
    int row, col;
    (void)user;

    for (row = 0; row < frame->rows && row * 2 + 1 < frame->height; ++row) {
        size_t length = 0;
        int last_fg[3] = {-1, -1, -1}, last_bg[3] = {-1, -1, -1};
        for (col = 0; col < frame->cols && col < 4096 && col * 2 + 1 < frame->width; ++col) {
            const uint8_t* pixels[4];
            int i, k, lo = 1 << 30, hi = -1, threshold, mask = 0, counts[2] = {0, 0};
            int sums[2][3] = {{0, 0, 0}, {0, 0, 0}};
            int fg[3], bg[3];

            for (i = 0; i < 4; ++i) {
                const int x = col * 2 + (i & 1), y = row * 2 + (i >> 1);
                const int l = luminance(pixels[i] = frame->rgba + ((size_t)y * (size_t)frame->width + (size_t)x) * 4);
                if (l < lo) lo = l;
                if (l > hi) hi = l;
            }
            /* Brighter than the middle: foreground. */
            threshold = (lo + hi + 1) / 2;
            for (i = 0; i < 4; ++i) {
                const int bright = hi > lo && luminance(pixels[i]) >= threshold;
                if (bright) mask |= 1 << i;
                for (k = 0; k < 3; ++k) sums[bright][k] += pixels[i][k];
                ++counts[bright];
            }
            for (k = 0; k < 3; ++k) {
                bg[k] = counts[0] ? sums[0][k] / counts[0] : 0;
                fg[k] = counts[1] ? sums[1][k] / counts[1] : bg[k];
            }

            if (frame->color == TERMESH_PLUGIN_COLOR_NONE) {
                /* No colours: the shape alone, lit where the picture is
                 * bright at all. */
                mask = 0;
                for (i = 0; i < 4; ++i)
                    if (luminance(pixels[i]) > 10 * 60) mask |= 1 << i;
            } else {
                if (mask && memcmp(fg, last_fg, sizeof(fg)) != 0) {
                    length += set_color(line + length, 38, frame->color, fg);
                    memcpy(last_fg, fg, sizeof(fg));
                }
                if (memcmp(bg, last_bg, sizeof(bg)) != 0) {
                    length += set_color(line + length, 48, frame->color, bg);
                    memcpy(last_bg, bg, sizeof(bg));
                }
            }
            memcpy(line + length, kGlyphs[mask], strlen(kGlyphs[mask]));
            length += strlen(kGlyphs[mask]);
        }
        host->write_row(out, row, line, length);
    }
}

TERMESH_PLUGIN_EXPORT int termesh_plugin_init(const termesh_host* host) {
    termesh_plugin_backend backend;
    if (host->api_version < 1) return 1;
    host->backend_init(&backend);
    backend.name = "quadrant";
    backend.description = "2 x 2 pixels per cell, drawn with Unicode quadrant blocks";
    backend.cell_pixels_x = 2;
    backend.cell_pixels_y = 2;
    backend.supersampling = 2;
    backend.draw = draw;
    return host->register_backend(host, &backend);
}
