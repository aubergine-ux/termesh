// Session recording: asciinema casts, and frames paced into GIF / MP4.
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "export/recorder.hpp"
#include "support.hpp"

using namespace termesh;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

// Parse one line of a cast file; a null value if it is not valid JSON.
Json parseLine(const std::string& line) {
    Json json;
    std::string error;
    const bool ok = Json::parse(line, json, error);
    CHECK_MESSAGE(ok, error);
    return json;
}

Image solid(int width, int height, Color8 color) {
    Image image(width, height);
    for (Color8& p : image.pixels) p = color;
    return image;
}

// Remembers what it is given.
struct FakeSink : VideoSink {
    std::vector<Image>* frames;
    bool* closed;
    FakeSink(std::vector<Image>* f, bool* c) : frames(f), closed(c) {}
    bool addFrame(const Image& frame) override {
        frames->push_back(frame);
        return true;
    }
    bool close(std::string&) override {
        *closed = true;
        return true;
    }
};

// How many images a GIF file holds.
int gifFrameCount(const std::string& bytes) {
    int count = 0;
    // Each image is introduced by a graphic control extension: 21 F9 04.
    for (std::size_t i = 0; i + 2 < bytes.size(); ++i)
        count += bytes[i] == '\x21' && bytes[i + 1] == '\xf9' && bytes[i + 2] == '\x04';
    return count;
}

}  // namespace

TEST_CASE("recorder: the format comes from the file name") {
    CHECK(recordingFormatOf("a.cast") == RecordingFormat::Cast);
    CHECK(recordingFormatOf("dir.d/session.GIF") == RecordingFormat::Gif);
    CHECK(recordingFormatOf("/tmp/x.mp4") == RecordingFormat::Mp4);
    CHECK(!recordingFormatOf("a.txt"));
    CHECK(!recordingFormatOf("cast"));
    CHECK(!recordingFormatOf("dir.gif/file"));
    const std::string name = defaultRecordingName(RecordingFormat::Gif);
    CHECK(name.rfind("termesh-", 0) == 0);
    CHECK(recordingFormatOf(name) == RecordingFormat::Gif);
}

TEST_CASE("recorder: JSON escaping") {
    CHECK(jsonEscape("plain") == "plain");
    CHECK(jsonEscape("a\"b\\c") == "a\\\"b\\\\c");
    CHECK(jsonEscape("\x1b[0m\r\n") == "\\u001b[0m\\u000d\\u000a");
    CHECK(jsonEscape("\xe2\x96\x80") == "\xe2\x96\x80");  // valid UTF-8 passes through
    CHECK(jsonEscape("a\xff"
                     "b") ==
          "a\xef\xbf\xbd"
          "b");                                                   // a stray byte becomes U+FFFD
    CHECK(jsonEscape("\xe2\x96") == "\xef\xbf\xbd\xef\xbf\xbd");  // a sequence cut short
    CHECK(jsonEscape("\xc0\x80") == "\xef\xbf\xbd\xef\xbf\xbd");  // an overlong encoding
    CHECK(jsonEscape(std::string("a\0b", 3)) == "a\\u0000b");
}

TEST_CASE("recorder: an asciinema cast is valid JSON, line by line") {
    TempDir dir;
    const std::string path = dir.file("session.cast");
    {
        CastWriter cast;
        REQUIRE(cast.open(path, 100, 30, "a \"title\""));
        cast.output(0.0, "\x1b[?1049h\x1b[2J");
        cast.output(0.25, "half \xe2\x96\x80 block, a quote \" and a backslash \\");
        cast.output(0.5, "");  // nothing written: no event
        cast.resize(1.0, 120, 40);
        cast.output(1.5, std::string("bad \xff byte"));
        CHECK(cast.close());
    }
    const std::vector<std::string> rows = lines(slurp(path));
    REQUIRE(rows.size() == 5);

    const Json header = parseLine(rows[0]);
    CHECK(header["version"].number() == 2);
    CHECK(header["width"].number() == 100);
    CHECK(header["height"].number() == 30);
    CHECK(header["title"].string() == "a \"title\"");
    CHECK(header["timestamp"].number() > 1e9);

    const Json first = parseLine(rows[1]);
    CHECK(first[0].number() == doctest::Approx(0.0));
    CHECK(first[1].string() == "o");
    CHECK(first[2].string() == "\x1b[?1049h\x1b[2J");

    const Json second = parseLine(rows[2]);
    CHECK(second[0].number() == doctest::Approx(0.25));
    CHECK(second[2].string() == "half \xe2\x96\x80 block, a quote \" and a backslash \\");

    const Json resize = parseLine(rows[3]);
    CHECK(resize[1].string() == "r");
    CHECK(resize[2].string() == "120x40");

    CHECK(parseLine(rows[4])[2].string() == "bad \xef\xbf\xbd byte");

    CastWriter unwritable;
    CHECK(!unwritable.open(dir.file("no/such/dir/x.cast"), 80, 24, "t"));
}

TEST_CASE("recorder: frames are paced to a steady rate") {
    std::vector<Image> frames;
    bool closed = false;
    FramePacer pacer(std::make_unique<FakeSink>(&frames, &closed), 10);

    const Image red = solid(40, 20, {255, 0, 0, 255});
    const Image blue = solid(40, 20, {0, 0, 255, 255});
    pacer.frame(red.view(), 1.0f, 0.0);    // shown from 0.0
    pacer.frame(blue.view(), 1.0f, 0.35);  // red filled 0.0 - 0.35: three tenths
    CHECK(frames.size() == 3);
    pacer.frame(red.view(), 1.0f, 0.36);  // blue was up for too short a time to get a frame
    CHECK(frames.size() == 3);
    std::string error;
    REQUIRE(pacer.finish(1.0, error));  // red again until the end: ten frames in all
    CHECK(closed);
    REQUIRE(frames.size() == 10);
    CHECK(pacer.framesWritten() == 10);
    CHECK(frames[0].pixels[0].r == 255);
    CHECK(frames[2].pixels[0].r == 255);
    CHECK(frames[3].pixels[0].r == 255);
    for (const Image& frame : frames) {
        CHECK(frame.width == pacer.width());
        CHECK(frame.height == pacer.height());
    }
    // 40 pixels wide, scaled by a whole number towards 640.
    CHECK(pacer.width() == 640);
    CHECK(pacer.height() == 320);
}

TEST_CASE("recorder: a recording with one picture has one frame, and one with none fails") {
    std::vector<Image> frames;
    bool closed = false;
    {
        FramePacer pacer(std::make_unique<FakeSink>(&frames, &closed), 20);
        pacer.frame(solid(8, 8, {1, 2, 3, 255}).view(), 1.0f, 0.0);
        std::string error;
        CHECK(pacer.finish(0.01, error));
        CHECK(frames.size() == 1);
    }
    frames.clear();
    closed = false;
    FramePacer empty(std::make_unique<FakeSink>(&frames, &closed), 20);
    std::string error;
    CHECK(!empty.finish(2.0, error));
    CHECK(!error.empty());
    CHECK(closed);
    CHECK(frames.empty());
}

TEST_CASE("recorder: frames are scaled to how they look on screen") {
    // ASCII cells are twice as tall as wide: each pixel becomes k x 2k.
    Image image(4, 2);
    for (int i = 0; i < 8; ++i)
        image.pixels[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(i * 30), 0, 0, 0};
    const Image tall = scaleForVideo(image.view(), 0.5f, 64, 0, 0);
    CHECK(tall.width == 4 * 16);
    CHECK(tall.height == 2 * 32);
    CHECK(tall.row(0)[0].r == 0);
    CHECK(tall.row(0)[16].r == 30);
    CHECK(tall.row(32)[0].r == 120);
    CHECK(tall.row(0)[0].a == 255);  // video frames are opaque

    // A later frame of another size is centred in the first one's frame.
    const Image small = solid(2, 2, {9, 9, 9, 255});
    const Image fitted = scaleForVideo(small.view(), 1.0f, 64, 100, 80);
    CHECK(fitted.width == 100);
    CHECK(fitted.height == 80);
    CHECK(fitted.row(40)[50].r == 9);
    // ...and one that is too big is cropped rather than overrunning.
    const Image big = solid(300, 300, {7, 7, 7, 255});
    const Image cropped = scaleForVideo(big.view(), 1.0f, 64, 100, 80);
    CHECK(cropped.width == 100);
    CHECK(cropped.row(79)[99].r == 7);
}

TEST_CASE("recorder: a session to GIF") {
    TempDir dir;
    const std::string path = dir.file("session.gif");
    SessionRecorder recorder;
    std::string message;
    REQUIRE(recorder.start(path, 80, 24, message));
    CHECK(recorder.active());
    CHECK(recorder.format() == RecordingFormat::Gif);
    CHECK(!recorder.start(path, 80, 24, message));  // one at a time

    // Two seconds: longer than the palette's hold-back, so frames arrive
    // both before and after the file is opened.
    for (int i = 0; i < 40; ++i) {
        Image frame =
            solid(32, 16, {static_cast<std::uint8_t>(i * 6), 80, static_cast<std::uint8_t>(255 - i * 6), 255});
        recorder.frame(i * 0.05, frame.view(), 1.0f);
        recorder.terminalOutput(i * 0.05, "ignored by a GIF");
    }
    REQUIRE(recorder.stop(2.0, message));
    CHECK(!recorder.active());
    CHECK(message.find("40 frames") != std::string::npos);

    const std::string bytes = slurp(path);
    REQUIRE(bytes.size() > 100);
    CHECK(bytes.substr(0, 6) == "GIF89a");
    CHECK(bytes.back() == '\x3b');  // the trailer: the file was finished
    CHECK(gifFrameCount(bytes) == 40);
    // Logical screen size, little-endian, straight after the signature.
    CHECK(static_cast<unsigned char>(bytes[6]) + 256 * static_cast<unsigned char>(bytes[7]) == 512);
}

TEST_CASE("recorder: a session to a cast") {
    TempDir dir;
    const std::string path = dir.file("session.cast");
    SessionRecorder recorder;
    std::string message;
    REQUIRE(recorder.start(path, 90, 28, message));
    recorder.terminalOutput(0.1, "hello");
    recorder.frame(0.1, solid(4, 4, {1, 1, 1, 255}).view(), 1.0f);  // ignored by a cast
    recorder.terminalResize(0.2, 100, 30);
    REQUIRE(recorder.stop(0.3, message));
    CHECK(message.find(path) != std::string::npos);
    const std::vector<std::string> rows = lines(slurp(path));
    REQUIRE(rows.size() == 3);
    CHECK(parseLine(rows[0])["width"].number() == 90);
    CHECK(parseLine(rows[1])[2].string() == "hello");
}

TEST_CASE("recorder: problems are reported") {
    TempDir dir;
    SessionRecorder recorder;
    std::string message;
    CHECK(!recorder.start(dir.file("session.txt"), 80, 24, message));
    CHECK(message.find(".cast") != std::string::npos);
    CHECK(!recorder.start(dir.file("missing/dir/a.gif"), 80, 24, message));
    CHECK(!recorder.start(dir.file("missing/dir/a.cast"), 80, 24, message));
    CHECK(!recorder.active());
    CHECK(!recorder.stop(0.0, message));

    // A recording during which nothing was drawn.
    REQUIRE(recorder.start(dir.file("empty.gif"), 80, 24, message));
    CHECK(!recorder.stop(1.0, message));
    CHECK(!message.empty());
}

TEST_CASE("recorder: a session to MP4, where ffmpeg is installed") {
    if (!ffmpegAvailable()) {
        MESSAGE("ffmpeg not found: skipping");
        return;
    }
    TempDir dir;
    const std::string path = dir.file("it's a session.mp4");  // quoting matters
    SessionRecorder recorder;
    std::string message;
    REQUIRE(recorder.start(path, 80, 24, message));
    for (int i = 0; i < 20; ++i) {
        Image frame = solid(33, 17, {static_cast<std::uint8_t>(i * 12), 60, 200, 255});  // odd sizes are evened out
        recorder.frame(i * 0.05, frame.view(), 1.0f);
    }
    REQUIRE_MESSAGE(recorder.stop(1.0, message), message);
    const std::string bytes = slurp(path);
    REQUIRE(bytes.size() > 500);
    CHECK(bytes.substr(4, 4) == "ftyp");  // an MP4 begins with its file-type box
}
