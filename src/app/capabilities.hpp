// Working out what the terminal can do, and picking an output backend.
#pragma once

#include "input/events.hpp"
#include "input/terminal.hpp"
#include "output/backend.hpp"

namespace termesh {

struct TerminalCaps {
    bool kittyGraphics = false;
    bool sixel = false;
    bool unicode = true;
    ColorDepth color = ColorDepth::TrueColor;
    // Size of a character cell in pixels (0 = unknown).
    int cellWidth = 0, cellHeight = 0;
};

// First guess from environment variables alone (TERM, COLORTERM, LANG, ...).
// This is all we have when output is not an interactive terminal.
TerminalCaps capsFromEnvironment();

// Ask the terminal directly: send capability queries and collect the
// replies, refining `caps`. The terminal must be in raw mode. Terminals that
// do not understand a query simply stay silent, so the last query is one
// every terminal answers (primary device attributes) and acts as a marker
// for "no more replies are coming".
void probeTerminal(Terminal& terminal, InputParser& parser, TerminalCaps& caps, int timeoutMs = 400);

// Fill in the cell size from whatever source is available.
void resolveCellSize(const TerminalSize& size, TerminalCaps& caps);

// The best backend the terminal supports.
BackendKind chooseBackend(const TerminalCaps& caps);

}  // namespace termesh
