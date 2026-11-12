// Command-line options.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "output/backend.hpp"
#include "pipeline/renderer.hpp"
#include "scene/camera.hpp"
#include "scene/model.hpp"

namespace termesh {

struct Options {
    // What to show: model files, a scene file (.json), or a built-in demo.
    // Several models are stood side by side.
    std::vector<std::string> inputs;
    std::string modelPath;  // the first of `inputs`
    std::string demo;
    int bodies = 28;  // how many things the physics demo starts with

    // Output selection (unset = detect).
    std::optional<BackendKind> backend;
    // Which one BackendKind::Custom means: a backend from a plugin. Whether
    // there is such a backend is only known once the plugins are loaded.
    std::string customBackend;
    std::optional<ColorDepth> color;
    DitherMode dither = DitherMode::Auto;

    // Rendering.
    ShadingMode shading = ShadingMode::Phong;
    bool wireframe = false;
    bool textures = true;
    bool normalMaps = true;
    bool cullBackfaces = true;
    bool points = false;     // draw every mesh as points, one splat per vertex
    float pointSize = 1.0f;  // multiplies the size of point splats
    Color8 background{16, 17, 22, 255};
    int supersampling = 0;  // 0 = backend default
    int threads = 0;        // 0 = all hardware threads
    // Level of detail: the error allowed, in output pixels. 0 = off. Unset =
    // the default for the mode: on in the viewer, off for exports.
    std::optional<float> lodTolerance;
    // The viewer lowers detail and resolution to hold this frame rate while
    // something is moving. 0 = never.
    int targetFps = 30;

    // Lighting.
    float lightAngle = 0.0f;  // turn of the key light about the vertical axis, degrees
    bool shadows = false;
    int shadowMapSize = 1024;
    int shadowSoftness = 3;
    std::string environment;     // built-in name or path to an .hdr; empty = studio
    std::optional<bool> skybox;  // unset = show the environment if it is a panorama
    float skyBlur = 0.0f;        // 0-1
    float envIntensity = 1.0f;
    float envRotation = 0.0f;  // degrees

    // Post-processing.
    bool ssao = false, bloom = false, fxaa = false;
    std::optional<bool> outline;  // unset = on with toon shading
    ToneMapper toneMapper = ToneMapper::Aces;
    float exposure = 0.0f;  // stops

    // Camera.
    float cameraTime = 0.0f;          // where along a scene's camera path to start (or take a still)
    bool followPath = true;           // play the scene's camera path, if it has one
    std::optional<float> yaw, pitch;  // degrees
    float zoom = 1.0f;                // > 1 moves closer
    float fov = 40.0f;                // vertical field of view, degrees

    // Interaction.
    bool spin = true;           // auto-rotate
    bool spinExplicit = false;  // --spin was given, rather than being the default
    float spinSpeed = 40.0f;    // degrees per second
    int fpsLimit = 60;
    bool stats = true;  // status bar

    // Animation.
    std::string animation;        // clip to play: a number (from 1) or a name; empty = the first
    float animationTime = 0.0f;   // where to start, in seconds
    float animationSpeed = 1.0f;  // playback rate
    bool animationPaused = false;
    bool animationTimeSet = false, animationSpeedSet = false;  // given, rather than defaults
    bool listAnimations = false;                               // print the clips in the file and exit

    // Non-interactive modes.
    bool preview = false;      // a quick still for a file manager's preview pane
    bool previewCache = true;  // remember previews between runs
    int previewMaxMb = 256;    // files larger than this are described, not drawn
    bool print = false;        // print one frame to the terminal and exit
    std::string pngPath;       // render one frame to a PNG and exit
    std::string gifPath;       // record a turntable animation and exit
    std::string mp4Path;       // the same as an MP4 (needs ffmpeg)
    // Record the interactive session from the start: a .cast, .gif or .mp4.
    std::string recordPath;
    int width = 0, height = 0;  // image size for --png / --gif (0 = default)
    int cols = 0, rows = 0;     // terminal size override for --print
    int frames = 0;             // GIF length (0 = a full turn, or one loop of the animation)
    int gifFps = 24;
    int blocks = 1;       // draw each pixel as an NxN block ("terminal look")
    int benchFrames = 0;  // render N frames off-screen and report timings

    // The built-in benchmark suite (app/benchmark.hpp).
    bool benchmark = false;
    bool benchmarkList = false;     // print the scene names and exit
    std::string benchmarkScene;     // run only this scene
    std::string benchmarkJsonPath;  // also write the results here as JSON

    // Plugins (plugin/plugin.hpp).
    std::vector<std::string> plugins;  // files to load, from --plugin
    bool autoPlugins = true;           // also load the ones in the plugin directories
    bool listPlugins = false;          // say what was loaded and exit

    bool showHelp = false, showVersion = false;
};

struct ParseResult {
    Options options;
    bool ok = true;
    std::string error;
};

// Parse a command line. `defaults` is what every option starts as before
// the arguments are read - normally the built-in defaults, but a scene
// file's "render" settings can be slipped in underneath the command line
// this way (see applySceneRenderSettings).
ParseResult parseOptions(int argc, const char* const* argv, const Options& defaults = {});

// Apply the "render" block of a scene file to a set of options. Returns
// false and describes the problem if a setting is not understood.
bool applySceneRenderSettings(const Json& render, Options& options, std::string& error);

// True if a path names a scene file rather than a model.
bool isSceneFile(const std::string& path);

// Which clip `--animation` refers to: a 1-based number or a (case-insensitive)
// name. Returns -1 if there is no such clip; an empty spec means the first.
int findAnimation(const Model& model, const std::string& spec);

// The initial render settings and camera described by the options. The
// environment is not part of them: load it with loadEnvironmentOption and
// point RenderSettings::environment at it.
RenderSettings makeRenderSettings(const Options& options);
OrbitCamera makeCamera(const Options& options, const Bounds& bounds);

// The environment `--env` asks for (a built-in name or a file), with
// --env-intensity and --env-rotation applied. Returns false and sets `error`
// if it cannot be loaded.
bool loadEnvironmentOption(const Options& options, Environment& out, std::string& error);

// Whether the environment should be drawn as the background.
bool wantsSkybox(const Options& options, const Environment& environment);
OrbitCamera makeCamera(const Options& options, const Model& model);

std::string usageText();

}  // namespace termesh
