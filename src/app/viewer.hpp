// The interactive viewer: input handling, the frame loop and the status bar.
#pragma once

#include <chrono>
#include <future>
#include <string_view>
#include <memory>
#include <string>
#include <vector>

#include "export/recorder.hpp"
#include "anim/player.hpp"
#include "app/capabilities.hpp"
#include "app/inspector.hpp"
#include "app/options.hpp"
#include "app/quality.hpp"
#include "core/image.hpp"
#include "input/events.hpp"
#include "input/terminal.hpp"
#include "output/backend.hpp"
#include "pipeline/renderer.hpp"
#include "scene/camera.hpp"
#include "scene/model.hpp"
#include "stage/scene.hpp"

namespace termesh {

class Viewer {
public:
    // The scene must outlive the viewer. Its environment is the one to
    // start with; its lights and camera path are used as they are.
    Viewer(const Options& options, Scene& scene, ThreadPool& pool);

    // Runs until the user quits. Returns the process exit code.
    int run();

    // run() in three parts, for hosts that own the main loop themselves
    // (a browser calls step() once per animation frame):
    //
    //   begin()  take over the terminal; false if there is none to take
    //   step()   handle input that has arrived and draw a frame if one is
    //            due; false once the user has quit
    //   end()    give the terminal back
    bool begin();
    bool step();
    void end();

private:
    using Clock = std::chrono::steady_clock;

    // --- input ---
    void handleEvent(const InputEvent& event);
    void handleKey(const KeyEvent& key);
    void handleMouse(const MouseEvent& mouse);
    bool handleAnimationKey(char32_t ch);  // true if the key was an animation control
    bool handleEffectKey(char32_t ch);     // true if the key was a lighting / effect control
    void pickAt(int col, int row);         // select the mesh under a cell
    void select(int object, int mesh);     // -1, -1 clears the selection
    void leavePath();                      // stop following the scene's camera path
    Camera cameraFor(float aspect) const;
    SceneObject* activeObject() const;  // the model the animation keys act on
    AnimationPlayer* activePlayer() const;
    void applyQuality(int level);  // size the framebuffer for a quality level
    Camera currentCamera() const;
    RenderSettings frameSettings() const;  // settings_ plus everything decided per frame

    // --- output ---
    BackendKind effectiveBackend() const;
    void rebuildBackend();  // (re)create the backend for the current mode
    void layout();          // recompute sizes after a resize or mode change
    void drawFrame();
    void drawStatusBar(std::string& out);
    void drawHelp(std::string& out);
    void drawInspector(std::string& out);
    void toggleInspector();
    void followInspector();  // highlight what the inspector has selected
    void saveScreenshot();
    void resetView();
    void notify(std::string message);

    // --- recording ---
    void emit(std::string_view bytes);  // to the terminal, and to a recording of it
    void toggleRecording();
    bool startRecording(const std::string& path);
    void stopRecording();
    double recordingSeconds() const;

    const Options& options_;
    Scene& scene_;
    ThreadPool& pool_;
    Renderer renderer_;
    int selectedObject_ = -1, selectedMesh_ = -1;  // what has been clicked on

    // The scene inspector: a sidebar, toggled with Tab.
    Inspector inspector_;
    bool showInspector_ = false;
    int panelWidth_ = 0;                   // in columns, including its border; 0 when hidden
    std::vector<std::string> panelShown_;  // the lines currently on screen

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
    std::future<std::vector<std::vector<std::vector<MeshLod>>>> pendingLods_;  // per object, per mesh

    // State of the main loop between steps.
    Clock::duration frameInterval_{};
    std::string inputBytes_;
    std::vector<InputEvent> inputEvents_;
    bool begun_ = false;

    SessionRecorder recorder_;
    Clock::time_point recordingStart_{};
    std::string recordingResult_;  // what the last recording came to, for after the screen is restored

    OrbitCamera camera_;
    // While the scene's camera path is being followed, the view comes from
    // it and camera_ waits to take over.
    bool followPath_ = false;
    float pathTime_ = 0.0f;
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
