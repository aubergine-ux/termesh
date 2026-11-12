// Recording a viewer session, as it happens.
//
// Three formats, chosen by the file's extension, and two quite different
// things being recorded:
//
//   .cast   an asciinema recording: the bytes sent to the terminal, with
//           the time each batch was sent. Small, exact, and replayable in
//           a terminal (asciinema play) or on a web page.
//   .gif    the pictures the renderer produced, at a fixed frame rate.
//   .mp4    the same pictures, encoded by ffmpeg (which must be installed;
//           everything else here is self-contained).
#pragma once

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/image.hpp"

namespace termesh {

enum class RecordingFormat { Cast, Gif, Mp4 };

// From the extension (.cast, .gif, .mp4); nothing for anything else.
std::optional<RecordingFormat> recordingFormatOf(const std::string& path);

// `text` as the inside of a JSON string. Bytes that are not valid UTF-8
// become U+FFFD, so the result is always valid JSON.
std::string jsonEscape(std::string_view text);

// An asciinema "asciicast v2" file: a JSON header line, then one JSON array
// per event - [seconds, "o", "bytes written"].
class CastWriter {
public:
    CastWriter() = default;
    ~CastWriter() { close(); }
    CastWriter(const CastWriter&) = delete;
    CastWriter& operator=(const CastWriter&) = delete;

    bool open(const std::string& path, int cols, int rows, const std::string& title);
    bool isOpen() const { return file_ != nullptr; }
    void output(double seconds, std::string_view bytes);
    void resize(double seconds, int cols, int rows);
    bool close();  // false if anything failed to be written

private:
    std::FILE* file_ = nullptr;
    bool failed_ = false;
};

// Somewhere to put a sequence of same-sized frames.
class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual bool addFrame(const Image& frame) = 0;
    // Finish the file. False (with `error` set) if it could not be written.
    virtual bool close(std::string& error) = 0;
};

// A GIF. The palette is chosen from the first second or so of frames, which
// are held back until then; after that frames are written as they arrive,
// so a long recording does not have to fit in memory.
std::unique_ptr<VideoSink> makeGifSink(const std::string& path, int fps);

// An MP4 (H.264), by piping raw frames to ffmpeg. Null, with `error` set,
// if ffmpeg cannot be started.
std::unique_ptr<VideoSink> makeMp4Sink(const std::string& path, int width, int height, int fps, std::string& error);

// True if an `ffmpeg` program can be run.
bool ffmpegAvailable();

// Turns frames that arrive whenever the viewer happens to draw one into a
// video with a steady frame rate: each frame is shown until the next one
// arrives, however long that is.
class FramePacer {
public:
    FramePacer(std::unique_ptr<VideoSink> sink, int fps) : sink_(std::move(sink)), fps_(fps) {}

    // A new picture, `seconds` after the recording began. `pixelAspect` is
    // the width / height of one of its pixels on screen: frames are scaled
    // up to square pixels, to a size fixed by the first one.
    void frame(ImageView image, float pixelAspect, double seconds);

    // The recording ends `seconds` after it began.
    bool finish(double seconds, std::string& error);

    int framesWritten() const { return written_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void writeUntil(double seconds);

    std::unique_ptr<VideoSink> sink_;
    int fps_;
    int width_ = 0, height_ = 0;  // of the video
    Image current_;               // the picture being shown now
    bool hasCurrent_ = false;
    int written_ = 0;
    bool failed_ = false;
};

// Scale `image` by whole numbers of pixels so that it appears as it does on
// screen (pixelAspect) and is about `targetWidth` wide, centred in a
// width x height frame. With width or height 0 the frame is made to fit.
Image scaleForVideo(ImageView image, float pixelAspect, int targetWidth, int width, int height);

// One recording, of whichever kind the file name asks for.
class SessionRecorder {
public:
    SessionRecorder();
    ~SessionRecorder();

    // Begin. `cols` x `rows` is the terminal's size (for .cast files).
    bool start(const std::string& path, int cols, int rows, std::string& error);
    bool active() const { return active_; }
    RecordingFormat format() const { return format_; }
    const std::string& path() const { return path_; }

    // What the viewer feeds it. Each is ignored by the formats it does not
    // concern. `seconds` is the time since start().
    void terminalOutput(double seconds, std::string_view bytes);
    void terminalResize(double seconds, int cols, int rows);
    void frame(double seconds, ImageView image, float pixelAspect);

    // End. `summary` describes what was written, or what went wrong.
    bool stop(double seconds, std::string& summary);

    static constexpr int kVideoFps = 20;

private:
    bool active_ = false;
    RecordingFormat format_ = RecordingFormat::Cast;
    std::string path_;
    CastWriter cast_;
    std::unique_ptr<FramePacer> pacer_;
    bool pacerStarted_ = false;
};

// A name for a recording nobody named: termesh-20260131-142501.cast
std::string defaultRecordingName(RecordingFormat format);

}  // namespace termesh
