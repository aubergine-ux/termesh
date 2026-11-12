// Terminal: raw access to the terminal the program is running in.
//
// The implementation is platform-specific (terminal_posix.cpp uses termios,
// terminal_win.cpp the Windows console API); everything above this interface
// speaks plain escape sequences and is the same everywhere.
#pragma once

#include <string>
#include <string_view>

namespace termesh {

struct TerminalSize {
    int cols = 80, rows = 24;
    // Size of the text area in pixels, or 0 if the system does not say.
    int pixelWidth = 0, pixelHeight = 0;
};

class Terminal {
public:
    Terminal();
    ~Terminal();  // restores the terminal if still in raw mode

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    static bool inputIsTerminal();
    static bool outputIsTerminal();

    // Switch input to raw mode: no echo, no line buffering, keys delivered
    // as they are pressed. Returns false if stdin / stdout is not a terminal.
    bool enterRawMode();
    void leaveRawMode();

    TerminalSize size() const;

    // Wait up to `timeoutMs` for input and append whatever arrived to `out`.
    // Returns false once input is closed.
    bool read(std::string& out, int timeoutMs);

    // Write all of `data` to the terminal.
    void write(std::string_view data);

    // True once the process has been asked to exit (SIGINT, SIGTERM, SIGHUP
    // or a console close event).
    static bool quitRequested();

private:
    struct State;
    State* state_;
};

}  // namespace termesh
