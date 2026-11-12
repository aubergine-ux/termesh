// Terminal for WebAssembly: the "terminal" is whatever the page provides.
//
// In a browser that is an xterm.js widget. The page (web/playground.js)
// hands the module an object, Module.termeshIO, and this file is the other
// side of it:
//
//   write(bytes)                bytes for the terminal to display
//   input                       an array of Uint8Arrays: what was typed
//   cols, rows                  the size of the terminal in cells
//   pixelWidth, pixelHeight     ...and in pixels
//   quit                        set to stop the viewer
//
// One thing is different in kind from the other platforms: read() cannot
// wait. A browser gives a program no way to block - the page would freeze -
// so it returns at once with whatever has arrived, and the page calls the
// viewer once per animation frame instead of the viewer sleeping between
// frames (Viewer::step).
//
// Without a termeshIO (the command-line build run under Node) there is no
// terminal: output goes to stdout and there is never any input.
#include "input/terminal.hpp"

#include <cstdio>

#include <emscripten.h>

// clang-format off
EM_JS(int, termesh_js_has_terminal, (), {
    return Module["termeshIO"] ? 1 : 0;
});

EM_JS(void, termesh_js_write, (const char* data, int length), {
    Module["termeshIO"].write(HEAPU8.slice(data, data + length));
});

// Move queued input into `buffer`; returns the number of bytes.
EM_JS(int, termesh_js_read, (char* buffer, int capacity), {
    const io = Module["termeshIO"];
    if (!io) return 0;
    let count = 0;
    while (io.input.length > 0) {
        let chunk = io.input[0];
        if (chunk.length > capacity - count) {
            if (count > 0) break;                    // it will fit next time
            chunk = chunk.subarray(0, capacity);     // larger than the whole buffer: cut it
        }
        io.input.shift();
        HEAPU8.set(chunk, buffer + count);
        count += chunk.length;
    }
    return count;
});

// which: 0 cols, 1 rows, 2 width in pixels, 3 height in pixels.
EM_JS(int, termesh_js_size, (int which), {
    const io = Module["termeshIO"];
    if (!io) return 0;
    return [io.cols, io.rows, io.pixelWidth, io.pixelHeight][which] | 0;
});

EM_JS(int, termesh_js_quit, (), {
    const io = Module["termeshIO"];
    return io && io.quit ? 1 : 0;
});
// clang-format on

namespace termesh {

struct Terminal::State {};

Terminal::Terminal() : state_(nullptr) {}
Terminal::~Terminal() = default;

bool Terminal::inputIsTerminal() { return termesh_js_has_terminal() != 0; }
bool Terminal::outputIsTerminal() { return termesh_js_has_terminal() != 0; }
bool Terminal::errorIsTerminal() { return false; }
void Terminal::prepareOutput() {}

bool Terminal::enterRawMode() { return termesh_js_has_terminal() != 0; }
void Terminal::leaveRawMode() {}

TerminalSize Terminal::size() const {
    TerminalSize size;
    if (termesh_js_has_terminal()) {
        size.cols = termesh_js_size(0) > 0 ? termesh_js_size(0) : size.cols;
        size.rows = termesh_js_size(1) > 0 ? termesh_js_size(1) : size.rows;
        size.pixelWidth = termesh_js_size(2);
        size.pixelHeight = termesh_js_size(3);
    }
    return size;
}

bool Terminal::read(std::string& out, int /*timeoutMs*/) {
    char buffer[4096];
    for (;;) {
        const int count = termesh_js_read(buffer, static_cast<int>(sizeof(buffer)));
        if (count <= 0) break;
        out.append(buffer, static_cast<std::size_t>(count));
    }
    return true;
}

void Terminal::write(std::string_view data) {
    if (data.empty()) return;
    if (termesh_js_has_terminal()) {
        termesh_js_write(data.data(), static_cast<int>(data.size()));
    } else {
        std::fwrite(data.data(), 1, data.size(), stdout);
        std::fflush(stdout);
    }
}

bool Terminal::quitRequested() { return termesh_js_quit() != 0; }

}  // namespace termesh
