#include "app/viewer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

#include <string_view>

#include "anim/skinning.hpp"
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
    "click                      select the mesh under the pointer",
    "Tab                        scene inspector (arrows move, left / right fold)",
    "right-drag, H J K L        pan",
    "wheel, + -                 zoom",
    "space                      auto-rotate on / off",
    "r                          reset the view",
    "",
    "1  2  3  4  5              flat / gouraud / phong / pbr / toon",
    "s                          cycle shading modes",
    "w                          wireframe",
    "t                          textures on / off",
    "n                          normal maps on / off",
    "c                          back-face culling on / off",
    "[  ]                       turn the light",
    "x                          shadows",
    "e  y                       next environment, sky background",
    "u  g  i  f                 occlusion, bloom, ink outlines, FXAA",
    "z  {  }                    tone curve, exposure down / up",
    "v                          follow the scene's camera path / take over",
    "d                          next dithering mode",
    "",
    "a                          play / pause the animation",
    ",  .                       step one frame back / forward",
    "<  >                       slower / faster",
    "m  M                       next / previous animation",
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

Viewer::Viewer(const Options& options, Scene& scene, ThreadPool& pool)
    : options_(options), scene_(scene), pool_(pool), renderer_(pool) {
    const Environment& environment = scene.environment;
    settings_ = makeRenderSettings(options);
    settings_.lights = scene.lights;
    settings_.skybox = options.skybox.value_or(scene.skybox.value_or(environment.hasMap()));
    if (options.skyBlur == 0.0f) settings_.skyboxBlur = scene.skyBlur;

    // Level of detail is on by default here: at terminal resolutions a
    // detailed model has many triangles per pixel.
    lodTolerance_ = options.lodTolerance.value_or(0.5f);
    if (lodTolerance_ > 0.0f) {
        // Animated models are about to start moving, so theirs are built
        // from the rest pose now. The rest are built off to the side.
        for (const auto& object : scene_.objects)
            if (object->player) object->accel.buildLods(object->model, &pool_);
        pendingLods_ = std::async(std::launch::async, [this] {
            std::vector<std::vector<std::vector<MeshLod>>> lods(scene_.objects.size());
            for (std::size_t i = 0; i < scene_.objects.size(); ++i) {
                if (scene_.objects[i]->player) continue;
                for (const Mesh& mesh : scene_.objects[i]->model.meshes) lods[i].push_back(buildLods(mesh));
            }
            return lods;
        });
    }
    lightYaw_ = radians(options.lightAngle);
    dither_ = options.dither;
    outlineAuto_ = !options.outline.has_value();

    // The cycle starts on whatever was asked for. A built-in name selects
    // that entry; a file is added to the end of the list.
    for (const std::string& name : builtinEnvironmentNames()) {
        Environment builtin;
        makeBuiltinEnvironment(name, builtin);
        builtin.intensity = environment.intensity;
        builtin.rotation = environment.rotation;
        if (name == environment.name) environment_ = environments_.size();
        environments_.push_back(std::move(builtin));
    }
    if (environments_[environment_].name != environment.name) {
        environment_ = environments_.size();
        environments_.push_back(environment);
    }

    autoRotate_ = options.spin;
    showStats_ = options.stats;
    resetView();
}

void Viewer::resetView() {
    const Bounds bounds = scene_.bounds();
    // The scene's own camera, unless the command line aims it elsewhere.
    if (scene_.camera && !options_.yaw && !options_.pitch) {
        camera_ = orbitFromPose(*scene_.camera, bounds);
        camera_.zoom(1.0f / options_.zoom);
    } else {
        camera_ = makeCamera(options_, bounds);
    }
    followPath_ = options_.followPath && !scene_.cameraPath.empty();
    pathTime_ = options_.cameraTime;
    lightYaw_ = radians(options_.lightAngle);
    dirty_ = true;
}

// Hand the camera from the scene's path to the user, without a jump: the
// orbit camera takes over from exactly where the path had got to.
void Viewer::leavePath() {
    if (!followPath_) return;
    camera_ = orbitFromPose(scene_.cameraPath.evaluate(pathTime_), scene_.bounds());
    followPath_ = false;
}

// The model the animation keys act on: the selected one if it is animated,
// otherwise the first that is.
SceneObject* Viewer::activeObject() const {
    if (selectedObject_ >= 0 && scene_.objects[static_cast<std::size_t>(selectedObject_)]->player)
        return scene_.objects[static_cast<std::size_t>(selectedObject_)].get();
    for (const auto& object : scene_.objects)
        if (object->player) return object.get();
    return nullptr;
}

AnimationPlayer* Viewer::activePlayer() const {
    SceneObject* object = activeObject();
    return object ? object->player.get() : nullptr;
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
    backend_->setDither(dither_);
    layout();
}

void Viewer::layout() {
    int rows = termSize_.rows - (showStats_ ? 1 : 0);
    if (!showStats_ && backend_->avoidsLastRow()) --rows;

    // The inspector takes a column down the right-hand side; the picture
    // gets what is left. In a window too narrow for both, the picture wins.
    panelWidth_ = 0;
    if (showInspector_) {
        panelWidth_ = std::clamp(termSize_.cols * 34 / 100, 30, 46);
        if (termSize_.cols - panelWidth_ < 24) {
            showInspector_ = false;
            panelWidth_ = 0;
        }
    }
    panelShown_.clear();

    area_.cols = std::max(termSize_.cols - panelWidth_, 1);
    area_.rows = std::max(rows, 1);
    area_.cellWidth = caps_.cellWidth;
    area_.cellHeight = caps_.cellHeight;
    backend_->configure(area_, imageWidth_, imageHeight_);

    baseSupersampling_ = options_.supersampling > 0 ? options_.supersampling : backend_->defaultSupersampling();
    while (baseSupersampling_ > 1 &&
           static_cast<std::int64_t>(imageWidth_) * baseSupersampling_ * imageHeight_ * baseSupersampling_ >
               kMaxFramebufferPixels)
        --baseSupersampling_;
    applyQuality(moving_ ? governor_.current() : 0);

    terminal_.write("\x1b[0m\x1b[2J");
    backend_->invalidate();
    lastStatus_.clear();
    dirty_ = true;
}

void Viewer::applyQuality(int level) {
    const QualityGovernor::Level& q = QualityGovernor::level(level);
    appliedQuality_ = level;
    supersampling_ = std::max(baseSupersampling_ - q.ssaaDrop, 1);
    renderWidth_ = std::max(static_cast<int>(std::lround(static_cast<float>(imageWidth_) * q.renderScale)), 1);
    renderHeight_ = std::max(static_cast<int>(std::lround(static_cast<float>(imageHeight_) * q.renderScale)), 1);
    framebuffer_.resize(renderWidth_ * supersampling_, renderHeight_ * supersampling_);
}

void Viewer::drawFrame() {
    const auto now = Clock::now();
    const double dt = std::min(std::chrono::duration<double>(now - lastFrameTime_).count(), 0.1);
    lastFrameTime_ = now;
    if (followPath_) pathTime_ += static_cast<float>(dt);
    else if (autoRotate_ && !dragging_) camera_.yaw += radians(options_.spinSpeed) * static_cast<float>(dt);
    // Move the animation clocks on, re-skinning whatever changed pose.
    scene_.update(static_cast<float>(dt), &pool_);

    // Full quality for a picture that will be looked at; whatever the
    // governor has settled on for one that will be replaced in a moment.
    const int quality = moving_ ? governor_.current() : 0;
    if (quality != appliedQuality_) applyQuality(quality);
    const QualityGovernor::Level& level = QualityGovernor::level(quality);

    RenderSettings settings = frameSettings();
    settings.lodTolerance = lodTolerance_ * level.lodScale * static_cast<float>(supersampling_);
    settings.wireframe = settings_.wireframe || backend_->wantsWireframe();
    // Lines need to survive the supersampling resolve, so scale them with it.
    settings.wireWidth = static_cast<float>(supersampling_);
    settings.pixelScale = static_cast<float>(supersampling_);

    const std::vector<DrawItem> items = scene_.drawItems();
    stats_ = renderer_.render(items, currentCamera(), settings, framebuffer_);

    const auto presentStart = Clock::now();
    ImageView view = viewOf(framebuffer_);
    const bool enlarged = renderWidth_ != imageWidth_ || renderHeight_ != imageHeight_;
    if (supersampling_ > 1 || settings.post.fxaa || enlarged) {
        resolve(framebuffer_, supersampling_, resolved_, pool_);
        if (settings.post.fxaa) applyFxaa(resolved_, pool_);
        view = resolved_.view();
        if (enlarged) {
            upscale(resolved_, imageWidth_, imageHeight_, scaled_, pool_);
            view = scaled_.view();
        }
    }

    std::string out;
    out.reserve(bytesLastFrame_ + 1024);
    out += kBeginFrame;
    backend_->present(view, out);
    drawInspector(out);
    drawStatusBar(out);
    out += kEndFrame;
    terminal_.write(out);
    bytesLastFrame_ = out.size();
    presentMs_ = std::chrono::duration<double, std::milli>(Clock::now() - presentStart).count();
    // Tell the governor how this frame did against the target frame rate.
    if (moving_ && options_.targetFps > 0)
        governor_.frame(std::chrono::duration<double, std::milli>(Clock::now() - now).count(),
                        1000.0 / options_.targetFps, baseSupersampling_);

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
    const AnimationPlayer* player = activePlayer();

    // Three levels of detail; use the longest one that fits the window.
    std::string text;
    if (!message_.empty()) {
        text = " " + message_;
    } else {
        const std::string shading = std::string(shadingModeName(settings_.shading)) +
                                    ((settings_.wireframe || backend_->wantsWireframe()) ? " wire" : "") +
                                    (settings_.textures ? "" : " notex") +
                                    // For an animated model "paused" belongs to the timeline below.
                                    (autoRotate_ || followPath_ || player ? "" : " paused");
        const std::string target = std::string(backendName(backend_->kind())) + " " + std::to_string(imageWidth_) +
                                   "x" + std::to_string(imageHeight_);
        char fps[32], timing[96];
        // Nothing is drawn while nothing moves, so a rate would be meaningless.
        if (autoRotate_ || dragging_ || followPath_ || scene_.playing())
            std::snprintf(fps, sizeof(fps), "%.0f fps", fps_);
        else std::snprintf(fps, sizeof(fps), "idle");
        std::snprintf(timing, sizeof(timing), "render %.1f ms, present %.1f ms", stats_.totalMs(), presentMs_);

        // The timeline: clip name, position and playback state.
        std::string clip, clipShort;
        if (player) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.1f/%.1fs", player->time(), player->duration());
            clipShort = buf;
            if (!player->playing()) clipShort += " paused";
            else if (player->speed() != 1.0f) {
                std::snprintf(buf, sizeof(buf), " x%g", player->speed());
                clipShort += buf;
            }
            clip = " | " + player->clipName(player->clip()) + " " + clipShort;
            if (player->clipCount() > 1)
                clip += " (" + std::to_string(player->clip() + 1) + "/" + std::to_string(player->clipCount()) + ")";
            clipShort = " | " + clipShort;
        }
        // ...and the camera path, while it is being followed.
        if (followPath_) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " | path %.1f/%.1fs",
                          std::fmod(pathTime_, std::max(scene_.cameraPath.duration, 1e-3f)),
                          scene_.cameraPath.duration);
            clip += buf;
        }

        const std::string name = " " + scene_.name;
        const std::string tris = formatCount(scene_.triangleCount()) + " tris";
        const std::string candidates[] = {
            name + " | " + tris + " | " + shading + clip + " | " + target +
                (supersampling_ > 1 ? " ssaa" + std::to_string(supersampling_) : "") +
                // Shown while detail is being traded for frame rate.
                (appliedQuality_ > 0 ? " q-" + std::to_string(appliedQuality_) : "") + " | " + fps + " | " + timing,
            name + " | " + tris + " | " + shading + clip + " | " + target + " | " + fps,
            name + " | " + shading + clip + " | " + fps,
            name + clipShort + " | " + fps,
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

// The inspector sidebar. The Inspector produces lines of plain text; this
// places them, colours them, and - like the cell backends - only sends the
// lines that differ from what is already on screen.
void Viewer::drawInspector(std::string& out) {
    if (!showInspector_ || panelWidth_ < 2) return;
    const Inspector::Frame frame = inspector_.render(scene_, stats_, panelWidth_ - 1, area_.rows, caps_.unicode);
    const bool color = caps_.color != ColorDepth::None;
    const char* border = caps_.unicode ? "\xe2\x94\x82" : "|";  // a vertical line
    if (panelShown_.size() != frame.lines.size()) panelShown_.assign(frame.lines.size(), {});

    for (std::size_t i = 0; i < frame.lines.size(); ++i) {
        const int line = static_cast<int>(i);
        const bool heading = std::find(frame.headings.begin(), frame.headings.end(), line) != frame.headings.end();
        std::string styled = color ? "\x1b[0;48;5;235;38;5;240m" : "\x1b[0m";
        styled += border;
        if (line == frame.selectedLine) styled += color ? "\x1b[48;5;24;38;5;231m" : "\x1b[7m";
        else if (heading) styled += color ? "\x1b[1;38;5;223m" : "\x1b[1m";
        else if (color) styled += "\x1b[38;5;250m";
        styled += frame.lines[i];
        if (styled == panelShown_[i]) continue;
        panelShown_[i] = styled;
        out += "\x1b[" + std::to_string(line + 1) + ";" + std::to_string(area_.cols + 1) + "H" + styled + "\x1b[0m";
    }
}

// Make the 3D view's highlight follow the inspector's selection.
void Viewer::followInspector() {
    int object = -1, mesh = -1;
    inspector_.selection(object, mesh);
    select(object, mesh);
}

void Viewer::toggleInspector() {
    showInspector_ = !showInspector_;
    if (showInspector_) {
        inspector_.rebuild(scene_);
        if (selectedObject_ >= 0) inspector_.selectMesh(scene_, selectedObject_, selectedMesh_);
    }
    layout();
    if (!showInspector_ && panelWidth_ == 0 && termSize_.cols < 60)
        notify("the window is too narrow for the inspector");
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

    const std::vector<DrawItem> items = scene_.drawItems();
    const Image image = renderOffscreen(
        renderer_, pool_, items, [this](float imageAspect) { return cameraFor(imageAspect); }, frameSettings(), target);

    // termesh-<model>-01.png, -02.png, ... - never overwrite.
    std::string stem = scene_.name;
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

    if (key.key == Key::Tab) {
        const bool wanted = !showInspector_;
        toggleInspector();
        if (wanted && !showInspector_) notify("the window is too narrow for the inspector");
        return;
    }
    // While the inspector is open the arrow keys walk its outline (h j k l
    // and the mouse still turn the model).
    if (showInspector_) {
        bool handled = true;
        switch (key.key) {
            case Key::Up: inspector_.move(-1); break;
            case Key::Down: inspector_.move(1); break;
            case Key::PageUp: inspector_.move(-10); break;
            case Key::PageDown: inspector_.move(10); break;
            case Key::Left: inspector_.setExpanded(scene_, 0); break;
            case Key::Right: inspector_.setExpanded(scene_, 1); break;
            case Key::Enter: inspector_.setExpanded(scene_, -1); break;
            default: handled = false; break;
        }
        if (handled) {
            followInspector();
            return;
        }
    }

    const float orbitStep = radians(10.0f), panStep = 0.06f;
    const BackendKind before = effectiveBackend();

    // Steering the camera takes it off the scene's path.
    const bool steering = key.key == Key::Left || key.key == Key::Right || key.key == Key::Up || key.key == Key::Down ||
                          key.key == Key::PageUp || key.key == Key::PageDown ||
                          (ch && std::u32string_view(U"hjklHJKL+=-_").find(ch) != std::u32string_view::npos);
    if (steering) leavePath();

    switch (key.key) {
        case Key::Escape: running_ = false; return;
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
        case Key::PageUp: camera_.zoom(0.8f); break;
        case Key::PageDown: camera_.zoom(1.25f); break;
        case Key::Home: resetView(); break;
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
                case U'+':
                case U'=': camera_.zoom(0.9f); break;
                case U'-':
                case U'_': camera_.zoom(1.0f / 0.9f); break;
                case U' ':
                    autoRotate_ = !autoRotate_;
                    lastFrameTime_ = Clock::now();
                    break;
                case U'r': resetView(); break;
                case U'1': settings_.shading = ShadingMode::Flat; break;
                case U'2': settings_.shading = ShadingMode::Gouraud; break;
                case U'3': settings_.shading = ShadingMode::Phong; break;
                case U'4': settings_.shading = ShadingMode::Pbr; break;
                case U'5': settings_.shading = ShadingMode::Toon; break;
                case U's':
                    settings_.shading =
                        static_cast<ShadingMode>((static_cast<int>(settings_.shading) + 1) % kShadingModeCount);
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
                    const std::size_t index =
                        it == available_.end() ? 0 : static_cast<std::size_t>(it - available_.begin());
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
                default:
                    if (!handleAnimationKey(ch) && !handleEffectKey(ch)) return;  // unbound key: nothing changed
                    break;
            }
            break;
        default: return;
    }

    if (effectiveBackend() != before) rebuildBackend();
    dirty_ = true;
}

// The settings for the frame about to be drawn: what the user has chosen,
// plus the things that follow from it.
RenderSettings Viewer::frameSettings() const {
    RenderSettings settings = settings_;
    settings.lightDirection = transformDirection(rotate({0.0f, 1.0f, 0.0f}, lightYaw_), settings_.lightDirection);
    settings.environment = &environments_[environment_];
    if (outlineAuto_) settings.post.outline = settings_.shading == ShadingMode::Toon;
    if (selectedObject_ >= 0) {
        settings.highlightItem = scene_.itemOfObject(selectedObject_);
        settings.highlightMesh = selectedMesh_;  // -1: the whole object
    }
    return settings;
}

// The camera for an image of the given aspect ratio: on the scene's path
// while that is being followed, otherwise wherever the user has put it.
Camera Viewer::cameraFor(float aspect) const {
    if (followPath_) return cameraFromPose(scene_.cameraPath.evaluate(pathTime_), aspect, scene_.bounds());
    return camera_.camera(aspect);
}

Camera Viewer::currentCamera() const {
    return cameraFor(static_cast<float>(imageWidth_) * backend_->pixelAspect() / static_cast<float>(imageHeight_));
}

void Viewer::select(int object, int mesh) {
    selectedObject_ = object;
    selectedMesh_ = mesh;
    dirty_ = true;
}

// Click to select: cast a ray through the middle of the clicked cell and
// find the first triangle it meets.
void Viewer::pickAt(int col, int row) {
    if (col < 0 || row < 0 || col >= area_.cols || row >= area_.rows) return;
    const float ndcX = (static_cast<float>(col) + 0.5f) / static_cast<float>(area_.cols) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (static_cast<float>(row) + 0.5f) / static_cast<float>(area_.rows) * 2.0f;

    // Trees go out of date when an animation moves the vertices.
    for (const auto& object : scene_.objects) object->accel.updateTrees(object->model, &pool_);
    const std::vector<DrawItem> items = scene_.drawItems();
    PickResult hit;
    if (!pick(items, currentCamera(), ndcX, ndcY, settings_.cullBackfaces, hit)) {
        if (selectedMesh_ >= 0) notify("selection cleared");
        select(-1, -1);
        return;
    }
    select(scene_.objectOfItem(hit.item), hit.mesh);
    if (showInspector_) inspector_.selectMesh(scene_, selectedObject_, selectedMesh_);
    const SceneObject& object = *scene_.objects[static_cast<std::size_t>(selectedObject_)];
    const Mesh& mesh = object.model.meshes[static_cast<std::size_t>(hit.mesh)];
    const Material& material = object.model.materials[static_cast<std::size_t>(mesh.material)];
    std::string text;
    if (scene_.objects.size() > 1) text = object.name + ": ";
    text += "mesh " + std::to_string(hit.mesh + 1) + "/" + std::to_string(object.model.meshes.size());
    if (!mesh.name.empty()) text += " '" + mesh.name + "'";
    text += ": " + formatCount(mesh.indices.size() / 3) + " tris, " + formatCount(mesh.vertices.size()) + " verts";
    if (!material.name.empty() && material.name != mesh.name) text += ", material '" + material.name + "'";
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", metallic %.2f rough %.2f", material.metallic, material.roughness);
    text += buf;
    notify(text);
}

// Lighting and post-processing toggles.
bool Viewer::handleEffectKey(char32_t ch) {
    auto onOff = [&](const char* what, bool on) { notify(std::string(what) + (on ? " on" : " off")); };
    PostSettings& post = settings_.post;
    switch (ch) {
        case U'x':
            settings_.shadows = !settings_.shadows;
            if (settings_.shadows &&
                (settings_.shading == ShadingMode::Flat || settings_.shading == ShadingMode::Gouraud))
                notify("shadows need phong, pbr or toon shading");
            else onOff("shadows", settings_.shadows);
            break;
        case U'e':
            environment_ = (environment_ + 1) % environments_.size();
            // A panorama is worth looking at; the studio is just two colours.
            settings_.skybox = options_.skybox.value_or(environments_[environment_].hasMap());
            notify("environment: " + environments_[environment_].name);
            break;
        case U'y':
            settings_.skybox = !settings_.skybox;
            onOff("sky background", settings_.skybox);
            break;
        case U'u':
            post.ssao = !post.ssao;
            onOff("ambient occlusion", post.ssao);
            break;
        case U'g':
            post.bloom = !post.bloom;
            onOff("bloom", post.bloom);
            break;
        case U'i':
            // From here on the choice is the user's, whatever the shading mode.
            post.outline = outlineAuto_ ? settings_.shading != ShadingMode::Toon : !post.outline;
            outlineAuto_ = false;
            onOff("outlines", post.outline);
            break;
        case U'f':
            post.fxaa = !post.fxaa;
            onOff("FXAA", post.fxaa);
            break;
        case U'v':
            if (scene_.cameraPath.empty()) {
                notify("this scene has no camera path");
            } else if (followPath_) {
                leavePath();
                notify("camera path off: the view is yours");
            } else {
                followPath_ = true;
                notify("following the camera path");
            }
            break;
        case U'd':
            dither_ = static_cast<DitherMode>((static_cast<int>(dither_) + 1) % 4);
            backend_->setDither(dither_);
            notify(std::string("dithering: ") + ditherModeName(dither_));
            break;
        case U'z':
            post.toneMapper = static_cast<ToneMapper>((static_cast<int>(post.toneMapper) + 1) % 4);
            notify(std::string("tone curve: ") + toneMapperName(post.toneMapper));
            break;
        case U'{':
        case U'}': {
            post.exposure = std::clamp(post.exposure + (ch == U'}' ? 0.5f : -0.5f), -8.0f, 8.0f);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "exposure %+.1f stops", post.exposure);
            notify(buf);
            break;
        }
        default: return false;
    }
    return true;
}

// Timeline controls. Anything that moves the clock by hand re-poses the model
// straight away, so it works while paused too.
bool Viewer::handleAnimationKey(char32_t ch) {
    if (ch != U'a' && ch != U',' && ch != U'.' && ch != U'<' && ch != U'>' && ch != U'm' && ch != U'M') return false;
    AnimationPlayer* player = activePlayer();
    if (!player) {
        notify("this model has no animations");
        return true;
    }
    constexpr float kStep = 1.0f / 30.0f;  // one "frame" when scrubbing
    constexpr float kSpeeds[] = {-2.0f, -1.0f, -0.5f, 0.1f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f};

    switch (ch) {
        case U'a':
            if (player->playing()) player->pause();
            else player->play();
            lastFrameTime_ = Clock::now();  // do not jump by the time spent paused
            break;
        case U',':
        case U'.':
            player->pause();
            player->seek(player->time() + (ch == U'.' ? kStep : -kStep));
            break;
        case U'<':
        case U'>': {
            // Step to the neighbouring entry of the speed table.
            int index = 0;
            for (int i = 0; i < static_cast<int>(std::size(kSpeeds)); ++i)
                if (std::abs(kSpeeds[i] - player->speed()) < std::abs(kSpeeds[index] - player->speed())) index = i;
            index = std::clamp(index + (ch == U'>' ? 1 : -1), 0, static_cast<int>(std::size(kSpeeds)) - 1);
            player->setSpeed(kSpeeds[index]);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "animation speed x%g", kSpeeds[index]);
            notify(buf);
            break;
        }
        default: {  // m, M
            if (player->clipCount() < 2) {
                notify("this model has only one animation");
                return true;
            }
            const int count = player->clipCount();
            const int next = (player->clip() + (ch == U'm' ? 1 : count - 1)) % count;
            player->setClip(next, 0.3f);  // cross-fade rather than snap
            notify("animation: " + player->clipName(next));
            break;
        }
    }
    scene_.pose(&pool_);
    return true;
}

void Viewer::handleMouse(const MouseEvent& mouse) {
    if (showHelp_) return;
    // The pointer is over the inspector: clicks select rows (a second click
    // on a heading folds it), the wheel moves the selection.
    if (showInspector_ && mouse.col >= area_.cols && !dragging_) {
        if (mouse.type == MouseEvent::Type::Press) {
            const int row = inspector_.rowAtLine(mouse.row);
            if (row >= 0) {
                if (row == inspector_.selected()) inspector_.setExpanded(scene_, -1);
                else inspector_.select(row);
                followInspector();
            }
        } else if (mouse.type == MouseEvent::Type::WheelUp || mouse.type == MouseEvent::Type::WheelDown) {
            inspector_.move(mouse.type == MouseEvent::Type::WheelUp ? -3 : 3);
            followInspector();
        }
        return;
    }
    switch (mouse.type) {
        case MouseEvent::Type::WheelUp:
            leavePath();
            camera_.zoom(0.9f);
            dirty_ = true;
            break;
        case MouseEvent::Type::WheelDown:
            leavePath();
            camera_.zoom(1.0f / 0.9f);
            dirty_ = true;
            break;
        case MouseEvent::Type::Press:
            dragging_ = true;
            dragMoved_ = false;
            // Shift+drag pans, for mice and trackpads without a right button.
            dragButton_ = mouse.shift ? 2 : mouse.button;
            lastMouseCol_ = mouse.col;
            lastMouseRow_ = mouse.row;
            break;
        case MouseEvent::Type::Release:
            // A press and release without movement in between is a click.
            if (dragging_ && !dragMoved_ && dragButton_ == 0) pickAt(mouse.col, mouse.row);
            dragging_ = false;
            lastFrameTime_ = Clock::now();  // resume spinning without a jump
            dirty_ = true;
            break;
        case MouseEvent::Type::Drag: {
            if (!dragging_) break;
            if (mouse.col != lastMouseCol_ || mouse.row != lastMouseRow_) dragMoved_ = true;
            if (dragMoved_) leavePath();
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
        case MouseEvent::Type::Move: break;
    }
}

// --------------------------------------------------------------- main loop ---

int Viewer::run() {
    if (!terminal_.enterRawMode()) {
        std::fprintf(stderr,
                     "termesh: not running in an interactive terminal.\n"
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
        const bool animating = ((autoRotate_ && !dragging_) || followPath_ || scene_.playing()) && !showHelp_;

        // "Moving" decides between a quick frame and a full-quality one.
        // When the motion stops, draw once more at full quality.
        const bool moving = animating || dragging_;
        if (moving_ && !moving && appliedQuality_ > 0) dirty_ = true;
        moving_ = moving;

        // Levels of detail built in the background arrive here.
        if (pendingLods_.valid() && pendingLods_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::vector<std::vector<std::vector<MeshLod>>> lods = pendingLods_.get();
            for (std::size_t i = 0; i < scene_.objects.size(); ++i)
                if (!scene_.objects[i]->player) scene_.objects[i]->accel.setLods(std::move(lods[i]));
            dirty_ = true;
        }
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
