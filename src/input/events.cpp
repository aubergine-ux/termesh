#include "input/events.hpp"

#include <cstdint>

namespace termesh {

namespace {

constexpr char kEsc = '\x1b';

// xterm encodes modifiers as 1 + a bit mask: 1 shift, 2 alt, 4 ctrl.
void applyModifiers(KeyEvent& key, int param) {
    if (param <= 1) return;
    const int mask = param - 1;
    key.shift = mask & 1;
    key.alt = mask & 2;
    key.ctrl = mask & 4;
}

bool keyForFinalByte(char final, Key& key) {
    switch (final) {
        case 'A': key = Key::Up; return true;
        case 'B': key = Key::Down; return true;
        case 'C': key = Key::Right; return true;
        case 'D': key = Key::Left; return true;
        case 'H': key = Key::Home; return true;
        case 'F': key = Key::End; return true;
        default: return false;
    }
}

}  // namespace

void InputParser::feed(std::string_view bytes, std::vector<InputEvent>& out) {
    buffer_.append(bytes);
    std::size_t consumed = 0;
    for (;;) {
        // parseOne looks at the front of buffer_, so drop what is done first.
        if (consumed) buffer_.erase(0, consumed);
        if (buffer_.empty()) break;
        consumed = parseOne(out);
        if (consumed == 0) break;  // incomplete: wait for more
    }
}

void InputParser::flush(std::vector<InputEvent>& out) {
    if (buffer_.empty()) return;
    if (buffer_ == "\x1b") {
        out.push_back(KeyEvent{Key::Escape});
    } else if (buffer_.size() >= 2 && buffer_[0] == kEsc && buffer_[1] == kEsc) {
        out.push_back(KeyEvent{Key::Escape});
    }
    // Anything else left over is a truncated sequence; discard it.
    buffer_.clear();
}

std::size_t InputParser::parseOne(std::vector<InputEvent>& out) {
    const auto first = static_cast<unsigned char>(buffer_[0]);

    if (first == 0x1b) return parseEscape(out);

    // Control characters.
    if (first == '\r' || first == '\n') {
        out.push_back(KeyEvent{Key::Enter});
        return 1;
    }
    if (first == '\t') {
        out.push_back(KeyEvent{Key::Tab});
        return 1;
    }
    if (first == 0x7f || first == 0x08) {
        out.push_back(KeyEvent{Key::Backspace});
        return 1;
    }
    if (first < 0x20) {
        // Ctrl+letter arrives as the letter's code with the top bits cleared.
        KeyEvent key;
        key.character = static_cast<char32_t>(first + 0x60);
        key.ctrl = true;
        out.push_back(key);
        return 1;
    }

    // A UTF-8 encoded character: the lead byte says how long it is.
    std::size_t length = 1;
    char32_t cp = first;
    if (first >= 0xF0) {
        length = 4;
        cp = first & 0x07;
    } else if (first >= 0xE0) {
        length = 3;
        cp = first & 0x0F;
    } else if (first >= 0xC0) {
        length = 2;
        cp = first & 0x1F;
    } else if (first >= 0x80) {
        return 1;  // stray continuation byte: skip
    }
    if (buffer_.size() < length) return 0;
    for (std::size_t i = 1; i < length; ++i)
        cp = (cp << 6) | (static_cast<unsigned char>(buffer_[i]) & 0x3F);

    KeyEvent key;
    key.character = cp;
    out.push_back(key);
    return length;
}

std::size_t InputParser::parseEscape(std::vector<InputEvent>& out) {
    if (buffer_.size() < 2) return 0;  // lone ESC so far; flush() decides
    const char second = buffer_[1];

    if (second == '[') return parseCsi(out);

    if (second == 'O') {
        // SS3: how arrow keys arrive in "application cursor" mode.
        if (buffer_.size() < 3) return 0;
        KeyEvent key;
        if (keyForFinalByte(buffer_[2], key.key)) out.push_back(key);
        return 3;
    }

    if (second == '_' || second == 'P') {
        // APC or DCS string, terminated by ESC \ (ST). The only one we
        // expect is the kitty graphics reply:  ESC _ G i=31;OK ESC \ .
        const std::size_t end = buffer_.find("\x1b\\", 2);
        if (end == std::string::npos) return buffer_.size() > 4096 ? buffer_.size() : 0;
        if (second == '_' && buffer_.size() > 2 && buffer_[2] == 'G') {
            ReplyEvent reply;
            reply.type = ReplyEvent::Type::KittyGraphics;
            const std::string_view body(buffer_.data() + 3, end - 3);
            const std::size_t semicolon = body.find(';');
            reply.ok = semicolon != std::string_view::npos && body.substr(semicolon + 1) == "OK";
            out.push_back(reply);
        }
        return end + 2;
    }

    if (second == kEsc) {
        // ESC ESC: the first one was the Escape key.
        out.push_back(KeyEvent{Key::Escape});
        return 1;
    }

    // ESC followed by an ordinary character is Alt+character.
    const std::size_t before = out.size();
    buffer_.erase(0, 1);
    const std::size_t used = parseOne(out);
    buffer_.insert(buffer_.begin(), kEsc);
    if (used == 0) return 0;
    if (out.size() > before)
        if (auto* key = std::get_if<KeyEvent>(&out.back())) key->alt = true;
    return used + 1;
}

// CSI sequences:  ESC [  <private marker>?  <params ; separated>  <final byte>
std::size_t InputParser::parseCsi(std::vector<InputEvent>& out) {
    std::size_t i = 2;
    char marker = 0;
    if (i < buffer_.size() && (buffer_[i] == '<' || buffer_[i] == '?' || buffer_[i] == '>')) marker = buffer_[i++];

    std::vector<int> params;
    int current = 0;
    bool haveDigits = false;
    for (;; ++i) {
        if (i >= buffer_.size()) return buffer_.size() > 256 ? buffer_.size() : 0;
        const char c = buffer_[i];
        if (c >= '0' && c <= '9') {
            // Saturate: a hostile or broken terminal can send any number of digits.
            if (current < 100'000'000) current = current * 10 + (c - '0');
            haveDigits = true;
        } else if (c == ';' || c == ':') {
            params.push_back(current);
            current = 0;
            haveDigits = false;
        } else if (c >= 0x40 && c <= 0x7e) {
            break;  // the final byte
        }
        // Intermediate bytes (0x20..0x2f) are skipped.
    }
    if (haveDigits || !params.empty()) params.push_back(current);
    const char final = buffer_[i];
    const std::size_t length = i + 1;
    auto param = [&](std::size_t n, int fallback) { return n < params.size() ? params[n] : fallback; };

    // SGR mouse report:  ESC [ < button ; column ; row  M (press) / m (release)
    if (marker == '<' && (final == 'M' || final == 'm') && params.size() >= 3) {
        const int code = params[0];
        MouseEvent mouse;
        mouse.col = params[1] - 1;
        mouse.row = params[2] - 1;
        mouse.shift = code & 4;
        mouse.alt = code & 8;
        mouse.ctrl = code & 16;
        const int button = code & 3;
        if (code & 64) {
            mouse.type = button == 0 ? MouseEvent::Type::WheelUp : MouseEvent::Type::WheelDown;
        } else if (code & 32) {
            // Motion: button 3 means "no button held".
            mouse.type = button == 3 ? MouseEvent::Type::Move : MouseEvent::Type::Drag;
            mouse.button = button;
        } else {
            mouse.type = final == 'M' ? MouseEvent::Type::Press : MouseEvent::Type::Release;
            mouse.button = button;
        }
        out.push_back(mouse);
        return length;
    }

    // Device attributes reply:  ESC [ ? 62 ; 4 ; ... c
    if (marker == '?' && final == 'c') {
        ReplyEvent reply;
        reply.type = ReplyEvent::Type::DeviceAttributes;
        reply.params = params;
        reply.ok = true;
        out.push_back(reply);
        return length;
    }

    // Window size replies:  ESC [ 6 ; height ; width t   (one cell, pixels)
    //                       ESC [ 4 ; height ; width t   (text area, pixels)
    if (marker == 0 && final == 't' && params.size() >= 3 && (params[0] == 6 || params[0] == 4)) {
        ReplyEvent reply;
        reply.type = params[0] == 6 ? ReplyEvent::Type::CellSize : ReplyEvent::Type::TextAreaSize;
        reply.params = {params[1], params[2]};
        reply.ok = true;
        out.push_back(reply);
        return length;
    }

    if (marker != 0) return length;  // some other private sequence: ignore

    KeyEvent key;
    if (keyForFinalByte(final, key.key)) {
        // With modifiers:  ESC [ 1 ; <mod> A
        applyModifiers(key, param(1, 1));
        out.push_back(key);
    } else if (final == 'Z') {
        key.key = Key::Tab;  // back-tab
        key.shift = true;
        out.push_back(key);
    } else if (final == '~') {
        // Keys identified by number:  ESC [ <n> ; <mod> ~
        bool known = true;
        switch (param(0, 0)) {
            case 1: case 7: key.key = Key::Home; break;
            case 4: case 8: key.key = Key::End; break;
            case 5: key.key = Key::PageUp; break;
            case 6: key.key = Key::PageDown; break;
            default: known = false;
        }
        if (known) {
            applyModifiers(key, param(1, 1));
            out.push_back(key);
        }
    }
    return length;
}

}  // namespace termesh
