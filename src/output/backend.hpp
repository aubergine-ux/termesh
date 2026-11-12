// Output backends: ways of getting a rendered image onto a terminal.
//
//   kitty      real pixels via the kitty graphics protocol
//   sixel      real pixels via DEC sixel graphics
//   halfblock  two pixels per cell using U+2580 with 24-bit colour
//   braille    2x4 dots per cell using U+2800..U+28FF (wireframe)
//   ascii      one shaded character per cell; works anywhere
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/image.hpp"
#include "output/dither.hpp"

namespace termesh {

enum class BackendKind {
    Kitty,
    Sixel,
    HalfBlock,
    Braille,
    Ascii,
    Custom,  // one that was registered at run time (by a plugin): see registerBackend
};

const char* backendName(BackendKind kind);
std::optional<BackendKind> backendFromName(std::string_view name);

enum class ColorDepth {
    TrueColor,   // 24-bit SGR colours
    Indexed256,  // the xterm 256-colour palette
    None,        // no colour at all
};

// The part of the terminal a backend may draw on. It always starts at the
// top-left corner of the screen.
struct DrawArea {
    int cols = 80, rows = 24;
    // Size of one character cell in pixels. Only the ratio matters to the
    // cell-based backends; sixel needs the real numbers.
    int cellWidth = 10, cellHeight = 20;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual BackendKind kind() const = 0;
    // What it is called: on the command line and in the status bar.
    virtual const char* name() const { return backendName(kind()); }

    // Tell the backend where it is drawing; returns the size of the image it
    // wants to be given, in pixels. Forgets anything it knew about the
    // screen's current contents.
    virtual void configure(const DrawArea& area, int& width, int& height) = 0;

    // Width / height of one image pixel as it appears on screen, so the
    // projection can compensate for non-square pixels.
    virtual float pixelAspect() const = 0;

    // How many times finer than the output the scene should be rendered
    // before being averaged down (1 = no supersampling).
    virtual int defaultSupersampling() const { return 1; }

    // Braille can only show "dot" or "no dot", so it is fed wireframes.
    virtual bool wantsWireframe() const { return false; }

    // Pixel backends that must not touch the bottom row of the screen (a
    // sixel image ending there makes the terminal scroll).
    virtual bool avoidsLastRow() const { return false; }

    // Append the escape sequences that put `image` on screen. Cell-based
    // backends only emit the cells that differ from the previous call.
    virtual void present(ImageView image, std::string& out) = 0;

    // Like present(), but as a plain block of output at the cursor position,
    // for printing a still frame into the scrollback.
    virtual void print(ImageView image, std::string& out) = 0;

    // Assume the screen has been cleared: redraw everything next time.
    virtual void invalidate() = 0;

    // Append whatever is needed to remove the backend's output from the
    // screen (kitty images are separate objects that outlive a text clear).
    virtual void cleanup(std::string& out) { (void)out; }

    // How to hide the backend's limited range of colours (see dither.hpp).
    // Auto picks what suits the backend:
    //
    //   kitty      nothing to hide: 24-bit pixels
    //   sixel      ordered while animating, Floyd-Steinberg for a still
    //   halfblock  the same, but only on 256-colour terminals
    //   ascii      none: twelve glyphs are too few for noise to read as shade
    //   braille    none: dots mark where the wireframe is. Asking for ordered
    //              or Floyd-Steinberg switches Braille to a different job:
    //              shaded surfaces as a halftone, one dot per pixel
    void setDither(DitherMode mode) {
        dither_ = mode;
        invalidate();
    }
    DitherMode dither() const { return dither_; }

protected:
    DitherMode dither_ = DitherMode::Auto;
};

// `customName` says which registered backend BackendKind::Custom means.
std::unique_ptr<Backend> makeBackend(BackendKind kind, ColorDepth color, const std::string& customName = {});

// More backends can be added at run time, by name. The five built-in ones
// are not in this table and cannot be replaced.
using BackendFactory = std::function<std::unique_ptr<Backend>(ColorDepth)>;
void registerBackend(const std::string& name, BackendFactory factory);
bool isRegisteredBackend(std::string_view name);
std::vector<std::string> registeredBackendNames();

}  // namespace termesh
