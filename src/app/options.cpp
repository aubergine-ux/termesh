#include "app/options.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "scene/primitives.hpp"

namespace termesh {

std::string usageText() {
    std::string demos;
    for (const std::string& name : demoModelNames()) {
        if (!demos.empty()) demos += ", ";
        demos += name;
    }
    return std::string(
               "termesh " TERMESH_VERSION " - 3D model viewer for the terminal\n"
               "\n"
               "Usage:\n"
               "  termesh [options] <model.obj | model.gltf | model.glb>\n"
               "  termesh [options] --demo <name>\n"
               "\n"
               "Display:\n"
               "  -b, --backend <name>   kitty, sixel, halfblock, braille or ascii (default: best available)\n"
               "      --color <depth>    truecolor, 256 or none (default: detect)\n"
               "  -s, --shading <mode>   flat, gouraud or phong (default: phong)\n"
               "  -w, --wireframe        start in wireframe mode\n"
               "      --no-textures      ignore textures\n"
               "      --no-normal-maps   ignore normal maps\n"
               "      --no-cull          draw back faces too\n"
               "      --bg <rrggbb>      background colour\n"
               "      --ssaa <n>         supersampling factor, 1-4 (default: depends on backend)\n"
               "      --threads <n>      worker threads (default: all cores)\n"
               "      --fps <n>          frame rate limit (default: 60)\n"
               "      --no-stats         hide the status bar\n"
               "\n"
               "Camera:\n"
               "      --yaw <deg>        initial rotation around the vertical axis\n"
               "      --pitch <deg>      initial elevation\n"
               "      --zoom <factor>    initial zoom, > 1 is closer (default: 1)\n"
               "      --fov <deg>        vertical field of view (default: 40)\n"
               "      --spin             auto-rotate (the default when interactive)\n"
               "      --no-spin          start with auto-rotate off\n"
               "      --spin-speed <d>   degrees per second (default: 40)\n"
               "\n"
               "Export:\n"
               "      --print            print a single frame and exit\n"
               "      --cols <n>, --rows <n>   size of the printed frame in cells\n"
               "      --png <file>       render the view to a PNG and exit\n"
               "      --gif <file>       record a 360-degree turntable GIF and exit\n"
               "      --size <WxH>       image size for --png / --gif\n"
               "      --frames <n>       frames in the GIF (default: 72)\n"
               "      --gif-fps <n>      GIF playback rate (default: 24)\n"
               "      --blocks <n>       draw each pixel as an n x n block, like terminal cells\n"
               "      --bench <n>        render n frames off-screen and print timings\n"
               "\n"
               "      --demo <name>      built-in model: ") +
           demos +
           "\n"
           "  -h, --help             show this help\n"
           "  -V, --version          show the version\n"
           "\n"
           "Keys:\n"
           "  drag / arrows / hjkl   orbit            1 2 3 / s   flat, gouraud, phong / cycle\n"
           "  right-drag / HJKL      pan              w           wireframe\n"
           "  wheel / + -            zoom             t  n        textures, normal maps\n"
           "  space                  auto-rotate      c           back-face culling\n"
           "  [ ]                    turn the light   b           next output backend\n"
           "  r                      reset view       o           status bar\n"
           "  p                      save a PNG       ?  q        help, quit\n";
}

RenderSettings makeRenderSettings(const Options& options) {
    RenderSettings settings;
    settings.shading = options.shading;
    settings.wireframe = options.wireframe;
    settings.textures = options.textures;
    settings.normalMaps = options.normalMaps;
    settings.cullBackfaces = options.cullBackfaces;
    settings.background = options.background;
    return settings;
}

OrbitCamera makeCamera(const Options& options, const Model& model) {
    OrbitCamera camera;
    camera.fovY = radians(options.fov);  // before frame(): the distance depends on it
    camera.frame(model.bounds());
    if (options.yaw) camera.yaw = radians(*options.yaw);
    if (options.pitch) camera.pitch = radians(std::clamp(*options.pitch, -89.0f, 89.0f));
    camera.zoom(1.0f / options.zoom);
    return camera;
}

namespace {

bool parseInt(std::string_view text, int& out) {
    const std::string copy(text);
    char* end = nullptr;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0') return false;
    out = static_cast<int>(value);
    return true;
}

bool parseFloat(std::string_view text, float& out) {
    const std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

bool parseColor(std::string_view text, Color8& out) {
    if (!text.empty() && text[0] == '#') text.remove_prefix(1);
    if (text.size() != 6) return false;
    const std::string copy(text);
    char* end = nullptr;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 16);
    if (*end != '\0') return false;
    out = {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
           static_cast<std::uint8_t>(value), 255};
    return true;
}

}  // namespace

ParseResult parseOptions(int argc, const char* const* argv) {
    ParseResult result;
    Options& o = result.options;

    auto fail = [&](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg.empty() || arg[0] != '-' || arg == "-") {
            if (!o.modelPath.empty()) return fail("more than one model given");
            o.modelPath = std::string(arg);
            continue;
        }

        // Accept both "--name value" and "--name=value".
        std::string_view inlineValue;
        bool hasInlineValue = false;
        if (arg.size() > 2 && arg[1] == '-') {
            if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
                inlineValue = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                hasInlineValue = true;
            }
        }
        const std::string name(arg);

        std::string_view value;
        auto needValue = [&]() -> bool {
            if (hasInlineValue) {
                value = inlineValue;
                return true;
            }
            if (i + 1 >= argc) return false;
            value = argv[++i];
            return true;
        };
        auto missing = [&] { return fail("option " + name + " needs a value"); };
        auto invalid = [&] { return fail("invalid value '" + std::string(value) + "' for " + name); };

        // Options taking an integer within a range.
        auto intOption = [&](int& target, int lo, int hi) -> bool {
            int v = 0;
            if (!parseInt(value, v) || v < lo || v > hi) return false;
            target = v;
            return true;
        };

        if (name == "-h" || name == "--help") o.showHelp = true;
        else if (name == "-V" || name == "--version") o.showVersion = true;
        else if (name == "-w" || name == "--wireframe") o.wireframe = true;
        else if (name == "--no-textures") o.textures = false;
        else if (name == "--no-normal-maps") o.normalMaps = false;
        else if (name == "--no-cull") o.cullBackfaces = false;
        else if (name == "--no-stats") o.stats = false;
        else if (name == "--spin") o.spin = true;
        else if (name == "--no-spin") o.spin = false;
        else if (name == "--print") o.print = true;
        else if (name == "--demo") {
            if (!needValue()) return missing();
            o.demo = std::string(value);
        } else if (name == "-b" || name == "--backend") {
            if (!needValue()) return missing();
            if (value != "auto") {
                o.backend = backendFromName(value);
                if (!o.backend) return invalid();
            }
        } else if (name == "--color" || name == "--colour") {
            if (!needValue()) return missing();
            if (value == "truecolor" || value == "24bit" || value == "rgb") o.color = ColorDepth::TrueColor;
            else if (value == "256") o.color = ColorDepth::Indexed256;
            else if (value == "none" || value == "mono") o.color = ColorDepth::None;
            else if (value != "auto") return invalid();
        } else if (name == "-s" || name == "--shading") {
            if (!needValue()) return missing();
            if (value == "flat") o.shading = ShadingMode::Flat;
            else if (value == "gouraud") o.shading = ShadingMode::Gouraud;
            else if (value == "phong") o.shading = ShadingMode::Phong;
            else return invalid();
        } else if (name == "--bg") {
            if (!needValue()) return missing();
            if (!parseColor(value, o.background)) return invalid();
        } else if (name == "--ssaa") {
            if (!needValue()) return missing();
            if (!intOption(o.supersampling, 1, 4)) return invalid();
        } else if (name == "--threads") {
            if (!needValue()) return missing();
            if (!intOption(o.threads, 1, 256)) return invalid();
        } else if (name == "--fps") {
            if (!needValue()) return missing();
            if (!intOption(o.fpsLimit, 1, 1000)) return invalid();
        } else if (name == "--yaw") {
            if (!needValue()) return missing();
            float v = 0.0f;
            if (!parseFloat(value, v)) return invalid();
            o.yaw = v;
        } else if (name == "--pitch") {
            if (!needValue()) return missing();
            float v = 0.0f;
            if (!parseFloat(value, v)) return invalid();
            o.pitch = v;
        } else if (name == "--zoom") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.zoom) || o.zoom <= 0.0f) return invalid();
        } else if (name == "--fov") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.fov) || o.fov < 5.0f || o.fov > 150.0f) return invalid();
        } else if (name == "--spin-speed") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.spinSpeed)) return invalid();
        } else if (name == "--png") {
            if (!needValue()) return missing();
            o.pngPath = std::string(value);
        } else if (name == "--gif") {
            if (!needValue()) return missing();
            o.gifPath = std::string(value);
        } else if (name == "--size") {
            if (!needValue()) return missing();
            const std::size_t x = value.find_first_of("xX");
            if (x == std::string_view::npos || !parseInt(value.substr(0, x), o.width) ||
                !parseInt(value.substr(x + 1), o.height) || o.width < 8 || o.height < 8 ||
                o.width > 8192 || o.height > 8192)
                return invalid();
        } else if (name == "--cols") {
            if (!needValue()) return missing();
            if (!intOption(o.cols, 2, 2000)) return invalid();
        } else if (name == "--rows") {
            if (!needValue()) return missing();
            if (!intOption(o.rows, 2, 2000)) return invalid();
        } else if (name == "--frames") {
            if (!needValue()) return missing();
            if (!intOption(o.frames, 2, 2000)) return invalid();
        } else if (name == "--gif-fps") {
            if (!needValue()) return missing();
            if (!intOption(o.gifFps, 1, 100)) return invalid();
        } else if (name == "--blocks") {
            if (!needValue()) return missing();
            if (!intOption(o.blocks, 1, 64)) return invalid();
        } else if (name == "--bench") {
            if (!needValue()) return missing();
            if (!intOption(o.benchFrames, 1, 1000000)) return invalid();
        } else {
            return fail("unknown option " + name);
        }
    }

    if (o.showHelp || o.showVersion) return result;
    if (o.modelPath.empty() && o.demo.empty()) return fail("no model given");
    if (!o.modelPath.empty() && !o.demo.empty()) return fail("give either a model file or --demo, not both");
    return result;
}

}  // namespace termesh
