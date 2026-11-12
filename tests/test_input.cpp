#include <doctest/doctest.h>

#include "input/events.hpp"

using namespace termesh;

namespace {

std::vector<InputEvent> parse(std::string_view bytes) {
    InputParser parser;
    std::vector<InputEvent> events;
    parser.feed(bytes, events);
    parser.flush(events);
    return events;
}

const KeyEvent& asKey(const InputEvent& e) {
    REQUIRE(std::holds_alternative<KeyEvent>(e));
    return std::get<KeyEvent>(e);
}
const MouseEvent& asMouse(const InputEvent& e) {
    REQUIRE(std::holds_alternative<MouseEvent>(e));
    return std::get<MouseEvent>(e);
}
const ReplyEvent& asReply(const InputEvent& e) {
    REQUIRE(std::holds_alternative<ReplyEvent>(e));
    return std::get<ReplyEvent>(e);
}

}  // namespace

TEST_CASE("input: plain and UTF-8 characters") {
    const auto events = parse("aZ \xc3\xa9\xe2\x82\xac\xf0\x9f\x99\x82");
    REQUIRE(events.size() == 6);
    CHECK(asKey(events[0]).character == U'a');
    CHECK(asKey(events[1]).character == U'Z');
    CHECK(asKey(events[2]).character == U' ');
    CHECK(asKey(events[3]).character == U'é');
    CHECK(asKey(events[4]).character == U'€');
    CHECK(asKey(events[5]).character == U'\U0001F642');
}

TEST_CASE("input: control keys") {
    const auto events = parse("\r\t\x7f\x03");
    REQUIRE(events.size() == 4);
    CHECK(asKey(events[0]).key == Key::Enter);
    CHECK(asKey(events[1]).key == Key::Tab);
    CHECK(asKey(events[2]).key == Key::Backspace);
    CHECK(asKey(events[3]).ctrl);
    CHECK(asKey(events[3]).character == U'c');
}

TEST_CASE("input: arrow keys in both encodings, with modifiers") {
    const auto events = parse("\x1b[A\x1b[B\x1bOC\x1bOD\x1b[1;2A\x1b[1;5D\x1b[5~\x1b[6~\x1b[H\x1b[F");
    REQUIRE(events.size() == 10);
    CHECK(asKey(events[0]).key == Key::Up);
    CHECK(asKey(events[1]).key == Key::Down);
    CHECK(asKey(events[2]).key == Key::Right);
    CHECK(asKey(events[3]).key == Key::Left);
    CHECK(asKey(events[4]).key == Key::Up);
    CHECK(asKey(events[4]).shift);
    CHECK_FALSE(asKey(events[4]).ctrl);
    CHECK(asKey(events[5]).key == Key::Left);
    CHECK(asKey(events[5]).ctrl);
    CHECK(asKey(events[6]).key == Key::PageUp);
    CHECK(asKey(events[7]).key == Key::PageDown);
    CHECK(asKey(events[8]).key == Key::Home);
    CHECK(asKey(events[9]).key == Key::End);
}

TEST_CASE("input: a lone ESC is the Escape key only once input goes quiet") {
    InputParser parser;
    std::vector<InputEvent> events;
    parser.feed("\x1b", events);
    CHECK(events.empty());  // could still be the start of a sequence
    CHECK(parser.hasPending());
    parser.flush(events);
    REQUIRE(events.size() == 1);
    CHECK(asKey(events[0]).key == Key::Escape);
    CHECK_FALSE(parser.hasPending());
}

TEST_CASE("input: sequences split across reads are reassembled") {
    InputParser parser;
    std::vector<InputEvent> events;
    parser.feed("\x1b", events);
    parser.feed("[", events);
    CHECK(events.empty());
    parser.feed("1;2", events);
    parser.feed("C", events);
    REQUIRE(events.size() == 1);
    CHECK(asKey(events[0]).key == Key::Right);
    CHECK(asKey(events[0]).shift);

    events.clear();
    parser.feed("\xe2\x82", events);  // first two bytes of the euro sign
    CHECK(events.empty());
    parser.feed("\xac", events);
    REQUIRE(events.size() == 1);
    CHECK(asKey(events[0]).character == U'€');
}

TEST_CASE("input: alt+key") {
    const auto events = parse("\x1bx");
    REQUIRE(events.size() == 1);
    CHECK(asKey(events[0]).character == U'x');
    CHECK(asKey(events[0]).alt);
}

TEST_CASE("input: SGR mouse reports") {
    const auto events = parse(
        "\x1b[<0;10;5M"    // left press at column 10, row 5
        "\x1b[<32;12;6M"   // drag with the left button
        "\x1b[<0;12;6m"    // release
        "\x1b[<2;1;1M"     // right press
        "\x1b[<64;40;20M"  // wheel up
        "\x1b[<65;40;20M"  // wheel down
        "\x1b[<4;3;3M"     // shift + left press
        "\x1b[<35;7;7M");  // motion with no button
    REQUIRE(events.size() == 8);

    CHECK(asMouse(events[0]).type == MouseEvent::Type::Press);
    CHECK(asMouse(events[0]).button == 0);
    CHECK(asMouse(events[0]).col == 9);  // zero-based
    CHECK(asMouse(events[0]).row == 4);

    CHECK(asMouse(events[1]).type == MouseEvent::Type::Drag);
    CHECK(asMouse(events[1]).col == 11);
    CHECK(asMouse(events[2]).type == MouseEvent::Type::Release);
    CHECK(asMouse(events[3]).type == MouseEvent::Type::Press);
    CHECK(asMouse(events[3]).button == 2);
    CHECK(asMouse(events[4]).type == MouseEvent::Type::WheelUp);
    CHECK(asMouse(events[5]).type == MouseEvent::Type::WheelDown);
    CHECK(asMouse(events[6]).shift);
    CHECK(asMouse(events[6]).type == MouseEvent::Type::Press);
    CHECK(asMouse(events[7]).type == MouseEvent::Type::Move);
}

TEST_CASE("input: terminal query replies") {
    const auto events = parse(
        "\x1b_Gi=31;OK\x1b\\"  // kitty graphics supported
        "\x1b[6;18;9t"         // cell is 18 px tall, 9 wide
        "\x1b[4;540;900t"      // text area 540 x 900 px
        "\x1b[?62;4;22c");     // device attributes incl. sixel
    REQUIRE(events.size() == 4);

    CHECK(asReply(events[0]).type == ReplyEvent::Type::KittyGraphics);
    CHECK(asReply(events[0]).ok);

    CHECK(asReply(events[1]).type == ReplyEvent::Type::CellSize);
    CHECK(asReply(events[1]).params == std::vector<int>{18, 9});

    CHECK(asReply(events[2]).type == ReplyEvent::Type::TextAreaSize);
    CHECK(asReply(events[2]).params == std::vector<int>{540, 900});

    CHECK(asReply(events[3]).type == ReplyEvent::Type::DeviceAttributes);
    CHECK(asReply(events[3]).params == std::vector<int>{62, 4, 22});
}

TEST_CASE("input: a kitty error reply is not OK") {
    const auto events = parse("\x1b_Gi=31;ENOTSUPPORTED:nope\x1b\\");
    REQUIRE(events.size() == 1);
    CHECK_FALSE(asReply(events[0]).ok);
}

TEST_CASE("input: unknown sequences are skipped without derailing the stream") {
    const auto events = parse(
        "\x1b[?1;2$y"
        "a"
        "\x1b[99~"
        "b"
        "\x1b[200;1;1;1x"
        "c");
    REQUIRE(events.size() == 3);
    CHECK(asKey(events[0]).character == U'a');
    CHECK(asKey(events[1]).character == U'b');
    CHECK(asKey(events[2]).character == U'c');
}

TEST_CASE("input: absurdly long numbers saturate instead of overflowing") {
    const auto events = parse(
        "\x1b[<0;99999999999999999999;99999999999999999999M"
        "\x1b[99999999999999999999;1R"
        "z");
    REQUIRE(events.size() >= 2);
    CHECK(asMouse(events[0]).col > 0);
    CHECK(asMouse(events[0]).row > 0);
    CHECK(asKey(events.back()).character == U'z');
}
