#include "export/recorder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "export/gif.hpp"

#ifdef _WIN32
#define TERMESH_POPEN _popen
#define TERMESH_PCLOSE _pclose
#else
#define TERMESH_POPEN popen
#define TERMESH_PCLOSE pclose
#endif

namespace termesh {

namespace {

std::string lowercaseExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string extension = path.substr(dot + 1);
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

// Length of the UTF-8 sequence starting at text[i], or 0 if the bytes
// there are not a valid one.
std::size_t utf8Length(std::string_view text, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    std::uint32_t code = 0, minimum = 0;
    if (lead < 0x80) return 1;
    if ((lead & 0xe0) == 0xc0) length = 2, code = lead & 0x1f, minimum = 0x80;
    else if ((lead & 0xf0) == 0xe0) length = 3, code = lead & 0x0f, minimum = 0x800;
    else if ((lead & 0xf8) == 0xf0) length = 4, code = lead & 0x07, minimum = 0x10000;
    else return 0;
    if (i + length > text.size()) return 0;
    for (std::size_t k = 1; k < length; ++k) {
        if ((byte(i + k) & 0xc0) != 0x80) return 0;
        code = (code << 6) | (byte(i + k) & 0x3f);
    }
    // Overlong forms, surrogates and values past the end of Unicode.
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    return length;
}

}  // namespace

std::optional<RecordingFormat> recordingFormatOf(const std::string& path) {
    const std::string extension = lowercaseExtension(path);
    if (extension == "cast") return RecordingFormat::Cast;
    if (extension == "gif") return RecordingFormat::Gif;
    if (extension == "mp4") return RecordingFormat::Mp4;
    return std::nullopt;
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        } else if (c < 0x20 || c == 0x7f) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
            ++i;
        } else if (const std::size_t length = utf8Length(text, i)) {
            out.append(text.substr(i, length));
            i += length;
        } else {
            out += "\xef\xbf\xbd";  // U+FFFD, the replacement character
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------- cast ---

bool CastWriter::open(const std::string& path, int cols, int rows, const std::string& title) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    failed_ = false;
    const char* term = std::getenv("TERM");
    const std::string header =
        "{\"version\": 2, \"width\": " + std::to_string(cols) + ", \"height\": " + std::to_string(rows) +
        ", \"timestamp\": " + std::to_string(static_cast<long long>(std::time(nullptr))) + ", \"title\": \"" +
        jsonEscape(title) + "\", \"env\": {\"TERM\": \"" + jsonEscape(term ? term : "xterm-256color") + "\"}}\n";
    failed_ = std::fwrite(header.data(), 1, header.size(), file_) != header.size();
    return !failed_;
}

void CastWriter::output(double seconds, std::string_view bytes) {
    if (!file_ || bytes.empty()) return;
    char time[32];
    std::snprintf(time, sizeof(time), "[%.6f, \"o\", \"", std::max(seconds, 0.0));
    const std::string line = time + jsonEscape(bytes) + "\"]\n";
    if (std::fwrite(line.data(), 1, line.size(), file_) != line.size()) failed_ = true;
}

void CastWriter::resize(double seconds, int cols, int rows) {
    if (!file_) return;
    if (std::fprintf(file_, "[%.6f, \"r\", \"%dx%d\"]\n", std::max(seconds, 0.0), cols, rows) < 0) failed_ = true;
}

bool CastWriter::close() {
    if (!file_) return !failed_;
    if (std::fclose(file_) != 0) failed_ = true;
    file_ = nullptr;
    return !failed_;
}

// ----------------------------------------------------------------- GIF ---

namespace {

class GifSink : public VideoSink {
public:
    GifSink(std::string path, int fps) : path_(std::move(path)), fps_(std::max(fps, 1)) {}

    bool addFrame(const Image& frame) override {
        if (failed_) return false;
        if (!opened_) {
            held_.push_back(frame);
            // Enough to choose colours from: a second, or a few megabytes.
            if (static_cast<int>(held_.size()) >= fps_ ||
                held_.size() * frame.pixels.size() * sizeof(Color8) > (48u << 20))
                begin();
            return !failed_;
        }
        if (!writer_.addFrame(frame)) failed_ = true;
        return !failed_;
    }

    bool close(std::string& error) override {
        if (!opened_ && !held_.empty()) begin();
        const bool wrote = opened_ && writer_.close() && !failed_;
        if (!wrote) error = "could not write '" + path_ + "'";
        return wrote;
    }

private:
    // Choose the palette from the frames held so far and write them out.
    void begin() {
        // The session will go on to show colours these frames do not have
        // (another shading mode, another environment). A small extra image
        // holding an even spread of every colour gives the palette a
        // foothold everywhere without taking much from what is on screen.
        std::vector<Image> sample = held_;
        Image spread(64, 64);
        for (int i = 0; i < 64 * 64; ++i) {
            const auto level = [](int v) { return static_cast<std::uint8_t>(v * 255 / 15); };
            spread.pixels[static_cast<std::size_t>(i)] = {level(i & 15), level((i >> 4) & 15), level((i >> 8) & 15),
                                                          255};
        }
        sample.push_back(std::move(spread));
        palette_ = buildGifPalette(sample);

        // GIF delays are whole hundredths of a second.
        const int delayCs = std::max((100 + fps_ / 2) / fps_, 2);
        opened_ = writer_.open(path_, held_[0].width, held_[0].height, palette_, delayCs);
        failed_ = !opened_;
        for (const Image& frame : held_)
            if (!failed_ && !writer_.addFrame(frame)) failed_ = true;
        held_.clear();
        held_.shrink_to_fit();
    }

    std::string path_;
    int fps_;
    std::vector<Image> held_;
    GifPalette palette_;
    GifWriter writer_;
    bool opened_ = false, failed_ = false;
};

}  // namespace

std::unique_ptr<VideoSink> makeGifSink(const std::string& path, int fps) {
    return std::make_unique<GifSink>(path, fps);
}

// ----------------------------------------------------------------- MP4 ---

namespace {

#ifdef _WIN32
const char* const kNullDevice = "NUL";
#else
const char* const kNullDevice = "/dev/null";
#endif

// `path` as one argument of a shell command line, or empty if it cannot be
// made safe.
std::string shellQuote(const std::string& path) {
#ifdef _WIN32
    if (path.find_first_of("\"%") != std::string::npos) return "";
    return "\"" + path + "\"";
#else
    std::string out = "'";
    for (char c : path) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
#endif
}

class Mp4Sink : public VideoSink {
public:
    Mp4Sink(std::FILE* pipe, std::string path, int width, int height)
        : pipe_(pipe), path_(std::move(path)), width_(width), height_(height) {}
    ~Mp4Sink() override {
        if (pipe_) TERMESH_PCLOSE(pipe_);
    }

    bool addFrame(const Image& frame) override {
        if (!pipe_ || failed_) return false;
        // The encoder was told the frame size once, and wants even numbers:
        // send exactly that much of each row.
        for (int y = 0; y < height_; ++y) {
            const Color8* row = frame.row(std::min(y, frame.height - 1));
            if (std::fwrite(row, sizeof(Color8), static_cast<std::size_t>(width_), pipe_) !=
                static_cast<std::size_t>(width_)) {
                failed_ = true;
                return false;
            }
        }
        return true;
    }

    bool close(std::string& error) override {
        if (!pipe_) return false;
        const int status = TERMESH_PCLOSE(pipe_);
        pipe_ = nullptr;
        if (failed_ || status != 0) {
            error = "ffmpeg could not write '" + path_ + "'";
            return false;
        }
        return true;
    }

private:
    std::FILE* pipe_;
    std::string path_;
    int width_, height_;
    bool failed_ = false;
};

}  // namespace

bool ffmpegAvailable() {
#ifdef __EMSCRIPTEN__
    return false;
#else
    const std::string command = std::string("ffmpeg -version > ") + kNullDevice + " 2>&1";
    return std::system(command.c_str()) == 0;
#endif
}

std::unique_ptr<VideoSink> makeMp4Sink(const std::string& path, int width, int height, int fps, std::string& error) {
#ifdef __EMSCRIPTEN__
    (void)path, (void)width, (void)height, (void)fps;
    error = "MP4 recording is not available here";
    return nullptr;
#else
    if (!ffmpegAvailable()) {
        error = "MP4 recording needs ffmpeg, which was not found (GIF and .cast do not)";
        return nullptr;
    }
    const std::string quoted = shellQuote(path);
    if (quoted.empty()) {
        error = "cannot write an MP4 to a path with quotes in it";
        return nullptr;
    }
    // H.264 wants even dimensions.
    width &= ~1;
    height &= ~1;
    if (width < 2 || height < 2) {
        error = "the picture is too small to record";
        return nullptr;
    }
    const std::string command = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(width) +
                                "x" + std::to_string(height) + " -r " + std::to_string(std::max(fps, 1)) +
                                " -i - -an -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p -movflags +faststart " +
                                quoted;
#ifdef _WIN32
    std::FILE* pipe = TERMESH_POPEN(command.c_str(), "wb");
#else
    std::FILE* pipe = TERMESH_POPEN(command.c_str(), "w");
#endif
    if (!pipe) {
        error = "could not start ffmpeg";
        return nullptr;
    }
    return std::make_unique<Mp4Sink>(pipe, path, width, height);
#endif
}

// --------------------------------------------------------------- pacing ---

Image scaleForVideo(ImageView image, float pixelAspect, int targetWidth, int width, int height) {
    // Whole-number scale factors keep every source pixel a crisp block.
    const float aspect = pixelAspect > 0.0f ? pixelAspect : 1.0f;
    const float widest = static_cast<float>(image.width) * std::max(aspect, 1.0f);
    const int scale = std::clamp(static_cast<int>(static_cast<float>(targetWidth) / std::max(widest, 1.0f)), 1, 16);
    const int sx =
        aspect >= 1.0f ? std::max(static_cast<int>(std::lround(static_cast<float>(scale) * aspect)), 1) : scale;
    const int sy =
        aspect >= 1.0f ? scale : std::max(static_cast<int>(std::lround(static_cast<float>(scale) / aspect)), 1);

    const int scaledWidth = image.width * sx, scaledHeight = image.height * sy;
    if (width <= 0 || height <= 0) width = scaledWidth, height = scaledHeight;

    Image out(width, height);
    // Centre it; whatever does not fit is cropped, whatever is left over is
    // filled with the corner colour (the background).
    const Color8 fill = image.width > 0 && image.height > 0
                            ? Color8{image.at(0, 0).r, image.at(0, 0).g, image.at(0, 0).b, 255}
                            : Color8{0, 0, 0, 255};
    const int offsetX = (width - scaledWidth) / 2, offsetY = (height - scaledHeight) / 2;
    for (int y = 0; y < height; ++y) {
        Color8* row = out.row(y);
        const int sourceY = (y - offsetY) / sy;
        const bool insideY = y >= offsetY && sourceY < image.height;
        for (int x = 0; x < width; ++x) {
            const int sourceX = (x - offsetX) / sx;
            if (insideY && x >= offsetX && sourceX < image.width) {
                const Color8& p = image.at(sourceX, sourceY);
                row[x] = {p.r, p.g, p.b, 255};
            } else {
                row[x] = fill;
            }
        }
    }
    return out;
}

void FramePacer::writeUntil(double seconds) {
    if (!hasCurrent_) return;
    const int due = static_cast<int>(std::floor(seconds * fps_ + 1e-6));
    while (written_ < due && !failed_) {
        if (!sink_->addFrame(current_)) failed_ = true;
        ++written_;
    }
}

void FramePacer::frame(ImageView image, float pixelAspect, double seconds) {
    if (image.width <= 0 || image.height <= 0) return;
    // The picture on show until now fills the time up to this moment.
    writeUntil(seconds);
    current_ = scaleForVideo(image, pixelAspect, 640, width_, height_);
    if (!hasCurrent_) {
        width_ = current_.width;
        height_ = current_.height;
        hasCurrent_ = true;
    }
}

bool FramePacer::finish(double seconds, std::string& error) {
    writeUntil(seconds);
    // However short the recording, it has at least its one picture.
    if (hasCurrent_ && written_ == 0 && !failed_) {
        if (!sink_->addFrame(current_)) failed_ = true;
        ++written_;
    }
    if (!hasCurrent_) {
        std::string ignored;
        sink_->close(ignored);
        error = "nothing was drawn while recording";
        return false;
    }
    return sink_->close(error) && !failed_;
}

// -------------------------------------------------------------- session ---

SessionRecorder::SessionRecorder() = default;
SessionRecorder::~SessionRecorder() {
    if (active_) {
        std::string ignored;
        stop(0.0, ignored);
    }
}

bool SessionRecorder::start(const std::string& path, int cols, int rows, std::string& error) {
    if (active_) {
        error = "already recording";
        return false;
    }
    const std::optional<RecordingFormat> format = recordingFormatOf(path);
    if (!format) {
        error = "cannot tell what to record from '" + path + "': use a name ending in .cast, .gif or .mp4";
        return false;
    }
    format_ = *format;
    path_ = path;
    pacer_.reset();
    pacerStarted_ = false;
    switch (format_) {
        case RecordingFormat::Cast:
            if (!cast_.open(path, cols, rows, "termesh")) {
                error = "could not write '" + path + "'";
                return false;
            }
            break;
        case RecordingFormat::Gif:
            // Make sure the file can be created before promising to record.
            if (std::FILE* probe = std::fopen(path.c_str(), "wb")) {
                std::fclose(probe);
            } else {
                error = "could not write '" + path + "'";
                return false;
            }
            break;
        case RecordingFormat::Mp4:
            if (!ffmpegAvailable()) {
                error = "MP4 recording needs ffmpeg, which was not found (GIF and .cast do not)";
                return false;
            }
            break;
    }
    active_ = true;
    return true;
}

void SessionRecorder::terminalOutput(double seconds, std::string_view bytes) {
    if (active_ && format_ == RecordingFormat::Cast) cast_.output(seconds, bytes);
}

void SessionRecorder::terminalResize(double seconds, int cols, int rows) {
    if (active_ && format_ == RecordingFormat::Cast) cast_.resize(seconds, cols, rows);
}

void SessionRecorder::frame(double seconds, ImageView image, float pixelAspect) {
    if (!active_ || format_ == RecordingFormat::Cast) return;
    if (!pacerStarted_) {
        // The video's size comes from the first picture.
        pacerStarted_ = true;
        std::unique_ptr<VideoSink> sink;
        if (format_ == RecordingFormat::Gif) {
            sink = makeGifSink(path_, kVideoFps);
        } else {
            const Image first = scaleForVideo(image, pixelAspect, 640, 0, 0);
            std::string error;
            sink = makeMp4Sink(path_, first.width, first.height, kVideoFps, error);
        }
        if (sink) pacer_ = std::make_unique<FramePacer>(std::move(sink), kVideoFps);
    }
    if (pacer_) pacer_->frame(image, pixelAspect, seconds);
}

bool SessionRecorder::stop(double seconds, std::string& summary) {
    if (!active_) {
        summary = "not recording";
        return false;
    }
    active_ = false;
    bool ok = false;
    if (format_ == RecordingFormat::Cast) {
        ok = cast_.close();
        summary = ok ? "saved " + path_ : "could not write '" + path_ + "'";
    } else if (pacer_) {
        std::string error;
        ok = pacer_->finish(seconds, error);
        summary = ok ? "saved " + path_ + " (" + std::to_string(pacer_->framesWritten()) + " frames)" : error;
        pacer_.reset();
    } else {
        summary = "nothing was drawn while recording";
    }
    return ok;
}

std::string defaultRecordingName(RecordingFormat format) {
    const std::time_t now = std::time(nullptr);
    char stamp[32] = "recording";
    if (const std::tm* local = std::localtime(&now)) std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local);
    const char* extension = format == RecordingFormat::Cast  ? ".cast"
                            : format == RecordingFormat::Gif ? ".gif"
                                                             : ".mp4";
    return std::string("termesh-") + stamp + extension;
}

}  // namespace termesh
