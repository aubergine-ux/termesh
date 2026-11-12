// Input events, and the parser that decodes them from the byte stream a
// terminal sends.
#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace termesh {

enum class Key {
    Character,  // a printable character; see KeyEvent::character
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    Enter,
    Tab,
    Backspace,
    Escape,
};

struct KeyEvent {
    Key key = Key::Character;
    char32_t character = 0;  // for Key::Character
    bool shift = false, alt = false, ctrl = false;
};

struct MouseEvent {
    enum class Type { Press, Release, Drag, Move, WheelUp, WheelDown };
    Type type = Type::Move;
    int button = 0;        // 0 left, 1 middle, 2 right (press / release / drag)
    int col = 0, row = 0;  // zero-based cell under the pointer
    bool shift = false, alt = false, ctrl = false;
};

// An answer from the terminal to one of our capability queries.
struct ReplyEvent {
    enum class Type {
        DeviceAttributes,  // params: the attribute list (4 = sixel)
        CellSize,          // params: height, width of a cell in pixels
        TextAreaSize,      // params: height, width of the text area in pixels
        KittyGraphics,     // ok: whether the graphics query succeeded
    };
    Type type = Type::DeviceAttributes;
    std::vector<int> params;
    bool ok = false;
};

using InputEvent = std::variant<KeyEvent, MouseEvent, ReplyEvent>;

// Incremental decoder for terminal input: UTF-8 text, CSI / SS3 key
// sequences, SGR mouse reports and query replies. Bytes can be fed in
// arbitrary pieces; incomplete sequences are kept until the rest arrives.
class InputParser {
public:
    void feed(std::string_view bytes, std::vector<InputEvent>& out);

    // Call when input has gone quiet. A lone ESC byte is ambiguous - it is
    // both the Escape key and the first byte of every escape sequence - and
    // the only way to tell is that nothing follows it.
    void flush(std::vector<InputEvent>& out);

    bool hasPending() const { return !buffer_.empty(); }

private:
    // Tries to decode one event from the front of the buffer. Returns the
    // number of bytes consumed, or 0 if more input is needed.
    std::size_t parseOne(std::vector<InputEvent>& out);
    std::size_t parseEscape(std::vector<InputEvent>& out);
    std::size_t parseCsi(std::vector<InputEvent>& out);

    std::string buffer_;
};

}  // namespace termesh
