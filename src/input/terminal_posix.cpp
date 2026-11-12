// Terminal implementation for Linux and macOS.
#include "input/terminal.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace termesh {

namespace {

std::atomic<bool> g_quit{false};

void onQuitSignal(int) { g_quit.store(true); }

}  // namespace

struct Terminal::State {
    termios original{};
    bool raw = false;
};

Terminal::Terminal() : state_(new State) {}

Terminal::~Terminal() {
    leaveRawMode();
    delete state_;
}

bool Terminal::inputIsTerminal() { return isatty(STDIN_FILENO) != 0; }
bool Terminal::outputIsTerminal() { return isatty(STDOUT_FILENO) != 0; }
bool Terminal::errorIsTerminal() { return isatty(STDERR_FILENO) != 0; }
void Terminal::prepareOutput() {}
bool Terminal::quitRequested() { return g_quit.load(); }

bool Terminal::enterRawMode() {
    if (state_->raw) return true;
    if (!inputIsTerminal() || !outputIsTerminal()) return false;
    if (tcgetattr(STDIN_FILENO, &state_->original) != 0) return false;

    termios raw = state_->original;
    // Input: no CR/NL translation, no flow control, leave the 8th bit alone.
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Local: no echo, no line editing, no signal keys (Ctrl-C arrives as a
    // byte so we can restore the screen before exiting).
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= CS8;
    // read() returns immediately with whatever is available; poll() does
    // the waiting.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
    state_->raw = true;

    // Signals from outside still need to end the main loop cleanly.
    struct sigaction action {};
    action.sa_handler = onQuitSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    return true;
}

void Terminal::leaveRawMode() {
    if (!state_->raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &state_->original);
    state_->raw = false;
}

TerminalSize Terminal::size() const {
    TerminalSize size;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        size.cols = ws.ws_col;
        size.rows = ws.ws_row;
        size.pixelWidth = ws.ws_xpixel;
        size.pixelHeight = ws.ws_ypixel;
    }
    return size;
}

bool Terminal::read(std::string& out, int timeoutMs) {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    const int ready = poll(&fd, 1, timeoutMs);
    if (ready < 0) return errno == EINTR;  // interrupted by a signal: not an error
    if (ready == 0) return true;           // timed out
    if (fd.revents & (POLLERR | POLLNVAL)) return false;

    char buffer[4096];
    const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (n > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
        return true;
    }
    if (n == 0) return !(fd.revents & POLLHUP);
    return errno == EINTR || errno == EAGAIN;
}

void Terminal::write(std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(STDOUT_FILENO, data.data(), data.size());
        if (n > 0) {
            data.remove_prefix(static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EAGAIN) {
            // The terminal is not keeping up; wait until it can take more.
            pollfd fd{STDOUT_FILENO, POLLOUT, 0};
            poll(&fd, 1, 100);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return;  // the terminal has gone away
        }
    }
}

}  // namespace termesh
