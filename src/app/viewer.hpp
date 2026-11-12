// The interactive viewer: input handling, the frame loop and the status bar.
#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "anim/player.hpp"
#include "app/capabilities.hpp"
#include "app/options.hpp"
#include "app/quality.hpp"
#include "core/image.hpp"
#include "input/events.hpp"
#include "input/terminal.hpp"
#include "output/backend.hpp"
#include "pipeline/renderer.hpp"
#include "scene/camera.hpp"
#include "scene/model.hpp"

namespace termesh {

class Viewer {
public:
    // `player` drives the model's animation; null for a static model.
    // `environment` is the one chosen on the command line.
    Viewer(const Options& options, Model& model, AnimationPlayer* player, const Environment& environment,
           ThreadPool& pool);

    // Runs until the user quits. Returns the process exit code.
    int run();

private:
    using Clock = std::chrono::steady_clock;

    // --- input ---
    void handleEvent(const InputEvent& event);
    void handleKey(const KeyEvent& key);
    void handleMouse(const MouseEvent& mouse);
    bool handleAnimationKey(char32_t ch);  // true if the key was an animation control
    bool handleEffectKey(char32_t ch);     // true if the key was a lighting / effect control
    void pickAt(int col, int row);         // select the mesh under a cell
    void applyQuality(int level);          // size the framebuffer for a quality level
    Camera currentCamera() const;
    RenderSettings frameSettings() const;  // settings_ plus everything decided per frame

    // --- output ---
    BackendKind effectiveBackend() const;
    void rebuildBackend();  // (re)create the backend for the current mode
    void layout();          // recompute sizes after a resize or mode change
    void drawFrame();
    void drawStatusBar(std::string& out);
    void drawHelp(std::string& out);
    void saveScreenshot();
    void resetView();
    void notify(std::string message);

    const Options& options_;
    Model& model_;
    AnimationPlayer* player_;
    ThreadPool& pool_;
    Renderer renderer_;
    // Boxes and BVHs for the model: frustum culling and click-to-select.
    ModelAccel accel_;
    int selectedMesh_ = -1;

    Terminal terminal_;
    InputParser parser_;
    TerminalCaps caps_;
    TerminalSize termSize_;

    std::unique_ptr<Backend> backend_;
    BackendKind baseBackend_ = BackendKind::HalfBlock;  // what the user selected
    std::vector<BackendKind> available_;                // for cycling with 'b'
    DrawArea area_;
    int imageWidth_ = 0, imageHeight_ = 0;    // what the backend wants
    int baseSupersampling_ = 1;               // at full quality
    int supersampling_ = 1;                   // for the frame in hand
    int renderWidth_ = 0, renderHeight_ = 0;  // what is rendered: smaller than the image when degraded
    Framebuffer framebuffer_;
    Image resolved_, scaled_;

    // Adaptive quality: while something is moving, detail and resolution
    // are traded for frame rate (see quality.hpp).
    QualityGovernor governor_;
    int appliedQuality_ = 0;  // the level the framebuffer is sized for
    bool moving_ = false;
    float lodTolerance_ = 0.0f;  // output pixels; 0 = no level of detail
    // Levels of detail for a large static model are built off to the side
    // so the first frame does not wait for them.
    std::future<std::vector<std::vector<MeshLod>>> pendingLods_;

    OrbitCamera camera_;
    RenderSettings settings_;
    float lightYaw_ = 0.0f;
    // Environments to cycle through with 'e': the built-in ones, plus the
    // file given on the command line if there was one.
    std::vector<Environment> environments_;
    std::size_t environment_ = 0;
    // Outlines follow the shading mode (on for toon) until toggled by hand.
    bool outlineAuto_ = true;
    DitherMode dither_ = DitherMode::Auto;

    bool running_ = true;
    bool dirty_ = true;  // the picture needs redrawing
    bool autoRotate_ = true;
    bool showStats_ = true;
    bool showHelp_ = false;
    bool helpDrawn_ = false;

    // Mouse drag state.
    bool dragging_ = false;
    bool dragMoved_ = false;  // the pointer has moved since the button went down
    int dragButton_ = 0;
    int lastMouseCol_ = 0, lastMouseRow_ = 0;

    // Timing and statistics.
    Clock::time_point lastFrameTime_;
    RenderStats stats_;
    double presentMs_ = 0.0;
    double fps_ = 0.0;
    int framesSinceFpsUpdate_ = 0;
    Clock::time_point fpsWindowStart_;
    std::size_t bytesLastFrame_ = 0;

    std::string message_;
    Clock::time_point messageExpiry_;
    std::string lastStatus_;
};

}  // namespace termesh
