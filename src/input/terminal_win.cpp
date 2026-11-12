// Terminal implementation for Windows (Windows Terminal / modern conhost).
//
// Windows 10 and later consoles understand the same escape sequences as
// Unix terminals once "virtual terminal" processing is switched on, for both
// output and input. After that the rest of termesh needs no special cases.
#include "input/terminal.hpp"

#include <atomic>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>

namespace termesh {

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI onConsoleEvent(DWORD) {
    g_quit.store(true);
    return TRUE;
}

// Append a UTF-16 code unit sequence to a UTF-8 string.
void appendUtf8(std::string& out, const wchar_t* text, int length) {
    if (length <= 0) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    const std::size_t offset = out.size();
    out.resize(offset + static_cast<std::size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data() + offset, bytes, nullptr, nullptr);
}

}  // namespace

struct Terminal::State {
    HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
    DWORD originalInputMode = 0, originalOutputMode = 0;
    UINT originalOutputCodePage = 0;
    wchar_t pendingSurrogate = 0;
    bool raw = false;
};

Terminal::Terminal() : state_(new State) {
    state_->input = GetStdHandle(STD_INPUT_HANDLE);
    state_->output = GetStdHandle(STD_OUTPUT_HANDLE);
}

Terminal::~Terminal() {
    leaveRawMode();
    delete state_;
}

bool Terminal::inputIsTerminal() { return _isatty(_fileno(stdin)) != 0; }
bool Terminal::outputIsTerminal() { return _isatty(_fileno(stdout)) != 0; }
bool Terminal::quitRequested() { return g_quit.load(); }

bool Terminal::enterRawMode() {
    if (state_->raw) return true;
    if (!GetConsoleMode(state_->input, &state_->originalInputMode)) return false;
    if (!GetConsoleMode(state_->output, &state_->originalOutputMode)) return false;

    // Output: interpret escape sequences, and do not move to the next line
    // on our behalf when a row is filled.
    const DWORD outputMode = state_->originalOutputMode | ENABLE_PROCESSED_OUTPUT |
                             ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(state_->output, outputMode)) return false;

    // Input: deliver keys (and mouse reports) as escape sequences, unbuffered
    // and unechoed.
    DWORD inputMode = state_->originalInputMode;
    inputMode &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                                     ENABLE_QUICK_EDIT_MODE);
    inputMode |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(state_->input, inputMode)) {
        SetConsoleMode(state_->output, state_->originalOutputMode);
        return false;
    }

    state_->originalOutputCodePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    state_->raw = true;
    return true;
}

void Terminal::leaveRawMode() {
    if (!state_->raw) return;
    SetConsoleMode(state_->input, state_->originalInputMode);
    SetConsoleMode(state_->output, state_->originalOutputMode);
    SetConsoleOutputCP(state_->originalOutputCodePage);
    state_->raw = false;
}

TerminalSize Terminal::size() const {
    TerminalSize size;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(state_->output, &info)) {
        size.cols = info.srWindow.Right - info.srWindow.Left + 1;
        size.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    return size;
}

bool Terminal::read(std::string& out, int timeoutMs) {
    const DWORD wait = WaitForSingleObject(state_->input, static_cast<DWORD>(timeoutMs < 0 ? 0 : timeoutMs));
    if (wait == WAIT_TIMEOUT) return true;
    if (wait != WAIT_OBJECT_0) return false;

    // Drain the event queue. Only key-down events carry text; with virtual
    // terminal input enabled, special keys and mouse reports arrive as the
    // characters of their escape sequences.
    for (;;) {
        DWORD available = 0;
        if (!GetNumberOfConsoleInputEvents(state_->input, &available)) return false;
        if (available == 0) break;

        INPUT_RECORD records[128];
        DWORD count = 0;
        if (!ReadConsoleInputW(state_->input, records, 128, &count)) return false;
        for (DWORD i = 0; i < count; ++i) {
            if (records[i].EventType != KEY_EVENT) continue;
            const KEY_EVENT_RECORD& key = records[i].Event.KeyEvent;
            const wchar_t ch = key.uChar.UnicodeChar;
            if (!key.bKeyDown || ch == 0) continue;

            // Characters outside the BMP arrive as two surrogate halves.
            if (ch >= 0xD800 && ch <= 0xDBFF) {
                state_->pendingSurrogate = ch;
                continue;
            }
            const int repeat = key.wRepeatCount > 0 ? key.wRepeatCount : 1;
            for (int r = 0; r < repeat; ++r) {
                if (state_->pendingSurrogate) {
                    const wchar_t pair[2] = {state_->pendingSurrogate, ch};
                    appendUtf8(out, pair, 2);
                } else {
                    appendUtf8(out, &ch, 1);
                }
            }
            state_->pendingSurrogate = 0;
        }
    }
    return true;
}

void Terminal::write(std::string_view data) {
    while (!data.empty()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(data.size() < 65536 ? data.size() : 65536);
        if (!WriteFile(state_->output, data.data(), chunk, &written, nullptr) || written == 0) return;
        data.remove_prefix(written);
    }
}

}  // namespace termesh
