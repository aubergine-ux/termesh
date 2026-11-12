// Command-line options.
#pragma once

#include <optional>
#include <string>

#include "output/backend.hpp"
#include "pipeline/renderer.hpp"
#include "scene/camera.hpp"
#include "scene/model.hpp"

namespace termesh {

struct Options {
    // What to show: a model file, or a built-in demo.
    std::string modelPath;
    std::string demo;

    // Output selection (unset = detect).
    std::optional<BackendKind> backend;
    std::optional<ColorDepth> color;
    DitherMode dither = DitherMode::Auto;

    // Rendering.
    ShadingMode shading = ShadingMode::Phong;
    bool wireframe = false;
    bool textures = true;
    bool normalMaps = true;
    bool cullBackfaces = true;
    Color8 background{16, 17, 22, 255};
    int supersampling = 0;  // 0 = backend default
    int threads = 0;        // 0 = all hardware threads

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
    bool listAnimations = false;  // print the clips in the file and exit

    // Non-interactive modes.
    bool print = false;         // print one frame to the terminal and exit
    std::string pngPath;        // render one frame to a PNG and exit
    std::string gifPath;        // record a turntable animation and exit
    int width = 0, height = 0;  // image size for --png / --gif (0 = default)
    int cols = 0, rows = 0;     // terminal size override for --print
    int frames = 0;             // GIF length (0 = a full turn, or one loop of the animation)
    int gifFps = 24;
    int blocks = 1;       // draw each pixel as an NxN block ("terminal look")
    int benchFrames = 0;  // render N frames off-screen and report timings

    bool showHelp = false, showVersion = false;
};

struct ParseResult {
    Options options;
    bool ok = true;
    std::string error;
};

ParseResult parseOptions(int argc, const char* const* argv);

// Which clip `--animation` refers to: a 1-based number or a (case-insensitive)
// name. Returns -1 if there is no such clip; an empty spec means the first.
int findAnimation(const Model& model, const std::string& spec);

// The initial render settings and camera described by the options. The
// environment is not part of them: load it with loadEnvironmentOption and
// point RenderSettings::environment at it.
RenderSettings makeRenderSettings(const Options& options);

// The environment `--env` asks for (a built-in name or a file), with
// --env-intensity and --env-rotation applied. Returns false and sets `error`
// if it cannot be loaded.
bool loadEnvironmentOption(const Options& options, Environment& out, std::string& error);

// Whether the environment should be drawn as the background.
bool wantsSkybox(const Options& options, const Environment& environment);
OrbitCamera makeCamera(const Options& options, const Model& model);

std::string usageText();

}  // namespace termesh
