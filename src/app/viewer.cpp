#include "app/viewer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>

#include "export/offscreen.hpp"
#include "pipeline/resolve.hpp"

namespace termesh {

namespace {

// Terminal modes used while the viewer is running, and how to undo them.
//   ?1049  alternate screen (the shell's scrollback is left untouched)
//   ?25    cursor visibility
//   ?7     auto-wrap; off, so writing the bottom-right cell cannot scroll
//   ?1000  report mouse button presses
//   ?1002  ...and motion while a button is held
//   ?1006  ...using the SGR encoding, which has no coordinate limit
constexpr const char* kEnterScreen = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[2J";
constexpr const char* kLeaveScreen = "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?7h\x1b[0m\x1b[?25h\x1b[?1049l";

// Synchronized output: the terminal holds back repainting until the whole
// frame has arrived, which avoids tearing. Unsupporting terminals ignore it.
constexpr const char* kBeginFrame = "\x1b[?2026h";
constexpr const char* kEndFrame = "\x1b[?2026l";

const char* const kHelpLines[] = {
    ("termesh " TERMESH_VERSION),
    "",
    "drag, arrows, h j k l      orbit",
    "right-drag, H J K L        pan",
    "wheel, + -                 zoom",
    "space                      auto-rotate on / off",
    "r                          reset the view",
    "",
    "1  2  3                    flat / gouraud / phong shading",
    "s                          cycle shading modes",
    "w                          wireframe",
    "t                          textures on / off",
    "n                          normal maps on / off",
    "c                          back-face culling on / off",
    "[  ]                       turn the light",
    "",
    "b                          next output backend",
    "o                          status bar on / off",
    "p                          save a PNG of the view",
    "?                          this help",
    "q, Esc                     quit",
    "",
    "press any key to return",
};

std::string formatCount(std::size_t n) {
    char buf[32];
    if (n >= 1'000'000) std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1e6);
    else if (n >= 1'000) std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1e3);
    else std::snprintf(buf, sizeof(buf), "%zu", n);
    return buf;
}

bool fileExists(const std::string& path) {
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

}  // namespace

Viewer::Viewer(const Options& options, const Model& model, ThreadPool& pool)
    : options_(options), model_(model), pool_(pool), renderer_(pool) {
    settings_ = makeRenderSettings(options);
    camera_ = makeCamera(options, model);
    autoRotate_ = options.spin;
    showStats_ = options.stats;
}

void Viewer::resetView() {
    camera_ = makeCamera(options_, model_);
    lightYaw_ = 0.0f;
    dirty_ = true;
}

void Viewer::notify(std::string message) {
    message_ = std::move(message);
    messageExpiry_ = Clock::now() + std::chrono::seconds(3);
    dirty_ = true;
}

// ------------------------------------------------------------------ output ---

BackendKind Viewer::effectiveBackend() const {
    // Wireframe on a half-block display automatically moves to Braille,
    // which has four times the resolution for line art - unless half-blocks
    // were asked for by name.
    const bool forced = options_.backend && *options_.backend == BackendKind::HalfBlock;
    if (settings_.wireframe && baseBackend_ == BackendKind::HalfBlock && caps_.unicode && !forced)
        return BackendKind::Braille;
    return baseBackend_;
}

void Viewer::rebuildBackend() {
    if (backend_) {
        std::string out;
        backend_->cleanup(out);
        terminal_.write(out);
    }
    backend_ = makeBackend(effectiveBackend(), caps_.color);
    layout();
}

void Viewer::layout() {
    int rows = termSize_.rows - (showStats_ ? 1 : 0);
    if (!showStats_ && backend_->avoidsLastRow()) --rows;

    area_.cols = std::max(termSize_.cols, 1);
    area_.rows = std::max(rows, 1);
    area_.cellWidth = caps_.cellWidth;
    area_.cellHeight = caps_.cellHeight;
    backend_->configure(area_, imageWidth_, imageHeight_);

    supersampling_ = options_.supersampling > 0 ? options_.supersampling : backend_->defaultSupersampling();
    while (supersampling_ > 1 && static_cast<std::int64_t>(imageWidth_) * supersampling_ * imageHeight_ *
                                         supersampling_ > kMaxFramebufferPixels)
        --supersampling_;
    framebuffer_.resize(imageWidth_ * supersampling_, imageHeight_ * supersampling_);

    terminal_.write("\x1b[0m\x1b[2J");
    backend_->invalidate();
    lastStatus_.clear();
    dirty_ = true;
}

void Viewer::drawFrame() {
    const auto now = Clock::now();
    const double dt = std::min(std::chrono::duration<double>(now - lastFrameTime_).count(), 0.1);
    lastFrameTime_ = now;
    if (autoRotate_ && !dragging_)
        camera_.yaw += radians(options_.spinSpeed) * static_cast<float>(dt);

    RenderSettings settings = settings_;
    settings.wireframe = settings_.wireframe || backend_->wantsWireframe();
    // Lines need to survive the supersampling resolve, so scale them with it.
    settings.wireWidth = static_cast<float>(supersampling_);
    settings.lightDirection = transformDirection(rotate({0.0f, 1.0f, 0.0f}, lightYaw_), settings_.lightDirection);

    const float aspect = static_cast<float>(imageWidth_) * backend_->pixelAspect() / static_cast<float>(imageHeight_);
    stats_ = renderer_.render(model_, camera_.camera(aspect), settings, framebuffer_);

    const auto presentStart = Clock::now();
    ImageView view = viewOf(framebuffer_);
    if (supersampling_ > 1) {
        resolve(framebuffer_, supersampling_, resolved_, pool_);
        view = resolved_.view();
    }

    std::string out;
    out.reserve(bytesLastFrame_ + 1024);
    out += kBeginFrame;
    backend_->present(view, out);
    drawStatusBar(out);
    out += kEndFrame;
    terminal_.write(out);
    bytesLastFrame_ = out.size();
    presentMs_ = std::chrono::duration<double, std::milli>(Clock::now() - presentStart).count();

    // Frame rate, averaged over half-second windows so it is readable.
    ++framesSinceFpsUpdate_;
    const double window = std::chrono::duration<double>(now - fpsWindowStart_).count();
    if (window >= 0.5) {
        fps_ = framesSinceFpsUpdate_ / window;
        framesSinceFpsUpdate_ = 0;
        fpsWindowStart_ = now;
    }
    dirty_ = false;
}

void Viewer::drawStatusBar(std::string& out) {
    if (!showStats_) return;
    const int cols = termSize_.cols;

    // Three levels of detail; use the longest one that fits the window.
    std::string text;
    if (!message_.empty()) {
        text = " " + message_;
    } else {
        const std::string shading = std::string(shadingModeName(settings_.shading)) +
                                    ((settings_.wireframe || backend_->wantsWireframe()) ? " wire" : "") +
                                    (settings_.textures ? "" : " notex") + (autoRotate_ ? "" : " paused");
        const std::string target = std::string(backendName(backend_->kind())) + " " + std::to_string(imageWidth_) +
                                   "x" + std::to_string(imageHeight_);
        char fps[32], timing[96];
        // Nothing is drawn while nothing moves, so a rate would be meaningless.
        if (autoRotate_ || dragging_) std::snprintf(fps, sizeof(fps), "%.0f fps", fps_);
        else std::snprintf(fps, sizeof(fps), "idle");
        std::snprintf(timing, sizeof(timing), "render %.1f ms, present %.1f ms",
                      stats_.vertexMs + stats_.setupMs + stats_.rasterMs, presentMs_);

        const std::string name = " " + model_.name;
        const std::string tris = formatCount(model_.triangleCount()) + " tris";
        const std::string candidates[] = {
            name + " | " + tris + " | " + shading + " | " + target +
                (supersampling_ > 1 ? " ssaa" + std::to_string(supersampling_) : "") + " | " + fps + " | " + timing,
            name + " | " + tris + " | " + shading + " | " + target + " | " + fps,
            name + " | " + shading + " | " + fps,
            std::string(" ") + fps,
        };
        for (const std::string& candidate : candidates) {
            text = candidate;
            if (static_cast<int>(text.size()) + 1 <= cols) break;
        }
    }

    // Right-align a hint if there is room, then pad or cut to the width.
    const std::string hint = "? help ";
    if (static_cast<int>(text.size() + hint.size()) + 2 <= cols)
        text += std::string(static_cast<std::size_t>(cols) - text.size() - hint.size(), ' ') + hint;
    text.resize(static_cast<std::size_t>(cols), ' ');

    if (text == lastStatus_) return;
    lastStatus_ = text;

    out += "\x1b[" + std::to_string(termSize_.rows) + ";1H";
    out += caps_.color == ColorDepth::None ? "\x1b[0;7m" : "\x1b[0;48;5;236;38;5;250m";
    out += text;
    out += "\x1b[0m";
}

void Viewer::drawHelp(std::string& out) {
    backend_->cleanup(out);
    out += "\x1b[0m\x1b[2J";
    const int lineCount = static_cast<int>(std::size(kHelpLines));
    int width = 0;
    for (const char* line : kHelpLines) width = std::max(width, static_cast<int>(std::char_traits<char>::length(line)));
    const int top = std::max((termSize_.rows - lineCount) / 2, 0);
    const int left = std::max((termSize_.cols - width) / 2, 0);
    for (int i = 0; i < lineCount && top + i < termSize_.rows; ++i) {
        out += "\x1b[" + std::to_string(top + i + 1) + ";" + std::to_string(left + 1) + "H";
        if (i == 0) out += "\x1b[1m";
        out += kHelpLines[i];
        if (i == 0) out += "\x1b[0m";
    }
}

void Viewer::saveScreenshot() {
    // Same view as on screen, at a resolution worth keeping.
    const float aspect = static_cast<float>(area_.cols * area_.cellWidth) /
                         static_cast<float>(std::max(area_.rows * area_.cellHeight, 1));
    OffscreenTarget target;
    target.height = 1200;
    target.width = std::clamp(static_cast<int>(1200.0f * aspect), 320, 4096);
    target.supersampling = 2;

    RenderSettings settings = settings_;
    settings.lightDirection = transformDirection(rotate({0.0f, 1.0f, 0.0f}, lightYaw_), settings_.lightDirection);
    const Image image = renderOffscreen(renderer_, pool_, model_, camera_, settings, target);

    // termesh-<model>-01.png, -02.png, ... - never overwrite.
    std::string stem = model_.name;
    if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    for (char& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    std::string path;
    for (int n = 1; n < 1000; ++n) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "-%02d.png", n);
        path = "termesh-" + stem + suffix;
        if (!fileExists(path)) break;
    }
    notify(writePng(path, image) ? "saved " + path : "could not write " + path);
}

// ------------------------------------------------------------------- input ---

void Viewer::handleEvent(const InputEvent& event) {
    if (const auto* key = std::get_if<KeyEvent>(&event)) handleKey(*key);
    else if (const auto* mouse = std::get_if<MouseEvent>(&event)) handleMouse(*mouse);
    // Late replies to capability queries are ignored.
}

void Viewer::handleKey(const KeyEvent& key) {
    const char32_t ch = key.key == Key::Character ? key.character : 0;

    if (key.ctrl && (ch == U'c' || ch == U'd')) {
        running_ = false;
        return;
    }
    if (showHelp_) {
        // Any key leaves the help screen; q still quits.
        if (ch == U'q') running_ = false;
        showHelp_ = false;
        helpDrawn_ = false;
        layout();
        return;
    }

    const float orbitStep = radians(10.0f), panStep = 0.06f;
    const BackendKind before = effectiveBackend();

    switch (key.key) {
        case Key::Escape:
            running_ = false;
            return;
        case Key::Left:
            if (key.shift) camera_.pan(panStep, 0.0f);
            else camera_.orbit(orbitStep, 0.0f);
            break;
        case Key::Right:
            if (key.shift) camera_.pan(-panStep, 0.0f);
            else camera_.orbit(-orbitStep, 0.0f);
            break;
        case Key::Up:
            if (key.shift) camera_.pan(0.0f, -panStep);
            else camera_.orbit(0.0f, orbitStep);
            break;
        case Key::Down:
            if (key.shift) camera_.pan(0.0f, panStep);
            else camera_.orbit(0.0f, -orbitStep);
            break;
        case Key::PageUp:
            camera_.zoom(0.8f);
            break;
        case Key::PageDown:
            camera_.zoom(1.25f);
            break;
        case Key::Home:
            resetView();
            break;
        case Key::Character:
            switch (ch) {
                case U'q': running_ = false; return;
                case U'h': camera_.orbit(orbitStep, 0.0f); break;
                case U'l': camera_.orbit(-orbitStep, 0.0f); break;
                case U'k': camera_.orbit(0.0f, orbitStep); break;
                case U'j': camera_.orbit(0.0f, -orbitStep); break;
                case U'H': camera_.pan(panStep, 0.0f); break;
                case U'L': camera_.pan(-panStep, 0.0f); break;
                case U'K': camera_.pan(0.0f, -panStep); break;
                case U'J': camera_.pan(0.0f, panStep); break;
                case U'+': case U'=': camera_.zoom(0.9f); break;
                case U'-': case U'_': camera_.zoom(1.0f / 0.9f); break;
                case U' ':
                    autoRotate_ = !autoRotate_;
                    lastFrameTime_ = Clock::now();
                    break;
                case U'r': resetView(); break;
                case U'1': settings_.shading = ShadingMode::Flat; break;
                case U'2': settings_.shading = ShadingMode::Gouraud; break;
                case U'3': settings_.shading = ShadingMode::Phong; break;
                case U's':
                    settings_.shading = static_cast<ShadingMode>((static_cast<int>(settings_.shading) + 1) % 3);
                    break;
                case U'w': settings_.wireframe = !settings_.wireframe; break;
                case U't': settings_.textures = !settings_.textures; break;
                case U'n': settings_.normalMaps = !settings_.normalMaps; break;
                case U'c': settings_.cullBackfaces = !settings_.cullBackfaces; break;
                case U'[': lightYaw_ -= radians(15.0f); break;
                case U']': lightYaw_ += radians(15.0f); break;
                case U'b': {
                    // Step to the next backend this terminal supports.
                    const auto it = std::find(available_.begin(), available_.end(), baseBackend_);
                    const std::size_t index = it == available_.end() ? 0 : static_cast<std::size_t>(it - available_.begin());
                    baseBackend_ = available_[(index + 1) % available_.size()];
                    rebuildBackend();
                    notify(std::string("backend: ") + backendName(backend_->kind()));
                    return;
                }
                case U'o':
                    showStats_ = !showStats_;
                    layout();
                    return;
                case U'p': saveScreenshot(); return;
                case U'?':
                    showHelp_ = true;
                    helpDrawn_ = false;
                    return;
                default: return;  // unbound key: nothing changed
            }
            break;
        default:
            return;
    }

    if (effectiveBackend() != before) rebuildBackend();
    dirty_ = true;
}

void Viewer::handleMouse(const MouseEvent& mouse) {
    if (showHelp_) return;
    switch (mouse.type) {
        case MouseEvent::Type::WheelUp:
            camera_.zoom(0.9f);
            dirty_ = true;
            break;
        case MouseEvent::Type::WheelDown:
            camera_.zoom(1.0f / 0.9f);
            dirty_ = true;
            break;
        case MouseEvent::Type::Press:
            dragging_ = true;
            // Shift+drag pans, for mice and trackpads without a right button.
            dragButton_ = mouse.shift ? 2 : mouse.button;
            lastMouseCol_ = mouse.col;
            lastMouseRow_ = mouse.row;
            break;
        case MouseEvent::Type::Release:
            dragging_ = false;
            lastFrameTime_ = Clock::now();  // resume spinning without a jump
            dirty_ = true;
            break;
        case MouseEvent::Type::Drag: {
            if (!dragging_) break;
            // Convert the movement from cells to pixels so that horizontal
            // and vertical drags feel the same despite cells being tall.
            const float dx = static_cast<float>((mouse.col - lastMouseCol_) * area_.cellWidth);
            const float dy = static_cast<float>((mouse.row - lastMouseRow_) * area_.cellHeight);
            lastMouseCol_ = mouse.col;
            lastMouseRow_ = mouse.row;
            const float viewWidth = static_cast<float>(std::max(area_.cols * area_.cellWidth, 1));
            const float viewHeight = static_cast<float>(std::max(area_.rows * area_.cellHeight, 1));

            if (dragButton_ == 0) {
                // Dragging across the whole window turns the model about 3/4
                // of the way round. The model follows the pointer.
                const float radiansPerPixel = 1.5f * kPi / viewWidth;
                camera_.orbit(-dx * radiansPerPixel, dy * radiansPerPixel);
            } else {
                camera_.pan(-dx / viewHeight, dy / viewHeight);
            }
            dirty_ = true;
            break;
        }
        case MouseEvent::Type::Move:
            break;
    }
}

// --------------------------------------------------------------- main loop ---

int Viewer::run() {
    if (!terminal_.enterRawMode()) {
        std::fprintf(stderr, "termesh: not running in an interactive terminal.\n"
                             "Use --print, --png or --gif to render without one.\n");
        return 1;
    }

    caps_ = capsFromEnvironment();
    probeTerminal(terminal_, parser_, caps_);
    if (options_.color) caps_.color = *options_.color;
    termSize_ = terminal_.size();
    resolveCellSize(termSize_, caps_);

    if (caps_.kittyGraphics) available_.push_back(BackendKind::Kitty);
    if (caps_.sixel) available_.push_back(BackendKind::Sixel);
    if (caps_.unicode && caps_.color != ColorDepth::None) available_.push_back(BackendKind::HalfBlock);
    if (caps_.unicode) available_.push_back(BackendKind::Braille);
    available_.push_back(BackendKind::Ascii);
    baseBackend_ = options_.backend.value_or(chooseBackend(caps_));
    // An explicitly requested backend is always reachable when cycling.
    if (std::find(available_.begin(), available_.end(), baseBackend_) == available_.end())
        available_.insert(available_.begin(), baseBackend_);

    terminal_.write(kEnterScreen);
    rebuildBackend();

    lastFrameTime_ = fpsWindowStart_ = Clock::now();
    const auto frameInterval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(options_.fpsLimit, 1)));
    std::string bytes;
    std::vector<InputEvent> events;

    while (running_ && !Terminal::quitRequested()) {
        const bool animating = autoRotate_ && !dragging_ && !showHelp_;
        const auto nextFrame = lastFrameTime_ + frameInterval;

        // Sleep in read(): until the next frame is due if something is
        // moving, otherwise until the user does something.
        int timeoutMs = 250;
        if (dirty_ || animating) {
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrame - Clock::now()).count();
            timeoutMs = static_cast<int>(std::clamp<long long>(wait, 0, 250));
        }
        if (parser_.hasPending()) timeoutMs = std::min(timeoutMs, 25);

        bytes.clear();
        if (!terminal_.read(bytes, timeoutMs)) break;
        events.clear();
        if (!bytes.empty()) parser_.feed(bytes, events);
        else parser_.flush(events);
        for (const InputEvent& event : events) {
            handleEvent(event);
            if (!running_) break;
        }
        if (!running_) break;

        // Window resized?
        const TerminalSize size = terminal_.size();
        if (size.cols != termSize_.cols || size.rows != termSize_.rows || size.pixelWidth != termSize_.pixelWidth ||
            size.pixelHeight != termSize_.pixelHeight) {
            termSize_ = size;
            resolveCellSize(termSize_, caps_);
            helpDrawn_ = false;
            layout();
        }

        if (!message_.empty() && Clock::now() >= messageExpiry_) {
            message_.clear();
            dirty_ = true;
        }

        if (showHelp_) {
            if (!helpDrawn_) {
                std::string out;
                drawHelp(out);
                terminal_.write(out);
                helpDrawn_ = true;
            }
            continue;
        }

        if ((dirty_ || animating) && Clock::now() >= nextFrame) drawFrame();
    }

    std::string out;
    backend_->cleanup(out);
    out += kLeaveScreen;
    terminal_.write(out);
    terminal_.leaveRawMode();
    return 0;
}

}  // namespace termesh
