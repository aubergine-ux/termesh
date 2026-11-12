#include "app/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>

namespace termesh {

namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

bool containsIgnoreCase(const std::string& haystack, const char* needle) {
    const std::string n(needle);
    return std::search(haystack.begin(), haystack.end(), n.begin(), n.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != haystack.end();
}

}  // namespace

TerminalCaps capsFromEnvironment() {
    TerminalCaps caps;
    const std::string term = env("TERM");
    const std::string colorterm = env("COLORTERM");
    const std::string program = env("TERM_PROGRAM");

    // --- colour depth ---
    if (!env("NO_COLOR").empty() || term == "dumb") {
        caps.color = ColorDepth::None;
    } else if (containsIgnoreCase(colorterm, "truecolor") || containsIgnoreCase(colorterm, "24bit") ||
               !env("WT_SESSION").empty() || containsIgnoreCase(term, "direct") ||
               containsIgnoreCase(term, "kitty") || containsIgnoreCase(term, "ghostty") ||
               containsIgnoreCase(term, "alacritty") || containsIgnoreCase(term, "wezterm") ||
               containsIgnoreCase(term, "foot")) {
        caps.color = ColorDepth::TrueColor;
    } else if (containsIgnoreCase(term, "256color")) {
        caps.color = ColorDepth::Indexed256;
    } else if (term.empty()) {
#if defined(_WIN32)
        caps.color = ColorDepth::TrueColor;  // any console new enough for VT sequences
#else
        caps.color = ColorDepth::None;
#endif
    } else {
        // An 8/16-colour terminal: too few colours to be worth using.
        caps.color = ColorDepth::None;
    }

    // --- Unicode ---
    // The locale says what encoding programs should emit. Only distrust
    // Unicode when it names some other encoding outright.
    std::string locale = env("LC_ALL");
    if (locale.empty()) locale = env("LC_CTYPE");
    if (locale.empty()) locale = env("LANG");
    const bool utf8 = containsIgnoreCase(locale, "utf-8") || containsIgnoreCase(locale, "utf8");
    const bool names8Bit = locale.find('.') != std::string::npos && !utf8;
    caps.unicode = !names8Bit && term != "dumb";

    // --- graphics (hints only; probeTerminal() gets the real answer) ---
    caps.kittyGraphics = !env("KITTY_WINDOW_ID").empty() || containsIgnoreCase(term, "kitty") ||
                         containsIgnoreCase(term, "ghostty") || program == "WezTerm" || program == "ghostty";
    // Multiplexers do not pass kitty graphics through.
    if (!env("TMUX").empty() || containsIgnoreCase(term, "screen")) caps.kittyGraphics = false;
    return caps;
}

void probeTerminal(Terminal& terminal, InputParser& parser, TerminalCaps& caps, int timeoutMs) {
    // What we learn here replaces the environment-based guesses.
    caps.kittyGraphics = false;
    caps.sixel = false;

    std::string queries;
    // Kitty graphics: "query" (a=q) a 1x1 image; a supporting terminal
    // replies OK without displaying anything.
    queries += "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
    queries += "\x1b[16t";  // size of a character cell in pixels
    queries += "\x1b[14t";  // size of the text area in pixels
    queries += "\x1b[c";    // primary device attributes - always answered
    terminal.write(queries);

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    int areaWidth = 0, areaHeight = 0;
    std::vector<InputEvent> events;
    std::string bytes;
    bool done = false;

    while (!done) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) break;
        bytes.clear();
        if (!terminal.read(bytes, static_cast<int>(remaining))) break;
        events.clear();
        parser.feed(bytes, events);

        for (const InputEvent& event : events) {
            const auto* reply = std::get_if<ReplyEvent>(&event);
            if (!reply) continue;  // a key typed during start-up: ignore
            switch (reply->type) {
                case ReplyEvent::Type::KittyGraphics:
                    caps.kittyGraphics = reply->ok;
                    break;
                case ReplyEvent::Type::CellSize:
                    if (reply->params.size() >= 2 && reply->params[0] > 0 && reply->params[1] > 0) {
                        caps.cellHeight = reply->params[0];
                        caps.cellWidth = reply->params[1];
                    }
                    break;
                case ReplyEvent::Type::TextAreaSize:
                    if (reply->params.size() >= 2) {
                        areaHeight = reply->params[0];
                        areaWidth = reply->params[1];
                    }
                    break;
                case ReplyEvent::Type::DeviceAttributes:
                    // Attribute 4 means "sixel graphics".
                    caps.sixel = std::find(reply->params.begin(), reply->params.end(), 4) != reply->params.end();
                    done = true;
                    break;
            }
        }
    }

    if (!env("TMUX").empty()) caps.kittyGraphics = false;

    if (caps.cellWidth == 0 && areaWidth > 0 && areaHeight > 0) {
        const TerminalSize size = terminal.size();
        caps.cellWidth = areaWidth / std::max(size.cols, 1);
        caps.cellHeight = areaHeight / std::max(size.rows, 1);
    }
}

void resolveCellSize(const TerminalSize& size, TerminalCaps& caps) {
    // The kernel's idea of the window's pixel size, if the terminal set it,
    // is always current - it follows resizes and font changes.
    if (size.pixelWidth > 0 && size.pixelHeight > 0 && size.cols > 0 && size.rows > 0) {
        caps.cellWidth = size.pixelWidth / size.cols;
        caps.cellHeight = size.pixelHeight / size.rows;
    }
    if (caps.cellWidth <= 0 || caps.cellHeight <= 0) {
        // Unknown: assume the usual 1:2 cell.
        caps.cellWidth = 10;
        caps.cellHeight = 20;
    }
}

BackendKind chooseBackend(const TerminalCaps& caps) {
    if (caps.kittyGraphics) return BackendKind::Kitty;
    if (caps.sixel) return BackendKind::Sixel;
    if (caps.unicode && caps.color != ColorDepth::None) return BackendKind::HalfBlock;
    return BackendKind::Ascii;
}

}  // namespace termesh
