#include "app/options.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "export/recorder.hpp"
#include "scene/primitives.hpp"

namespace termesh {

std::string usageText() {
    std::string demos;
    for (const std::string& name : demoModelNames()) {
        if (!demos.empty()) demos += ", ";
        demos += name;
    }
    return std::string("termesh " TERMESH_VERSION
                       " - 3D model viewer for the terminal\n"
                       "\n"
                       "Usage:\n"
                       "  termesh [options] <model.obj | .gltf | .glb | .stl | .ply> [more models...]\n"
                       "  termesh [options] <scene.json>\n"
                       "  termesh [options] --demo <name>\n"
                       "\n"
                       "Several models are shown side by side. A scene file places models, lights and a\n"
                       "camera exactly; see docs/scenes.md.\n"
                       "\n"
                       "Display:\n"
                       "  -b, --backend <name>   kitty, sixel, halfblock, braille or ascii (default: best available)\n"
                       "      --color <depth>    truecolor, 256 or none (default: detect)\n"
                       "      --dither <mode>    auto, none, ordered or fs (Floyd-Steinberg); with braille,\n"
                       "                         ordered / fs draw shaded surfaces as a halftone\n"
                       "  -s, --shading <mode>   flat, gouraud, phong, pbr or toon (default: phong)\n"
                       "  -w, --wireframe        start in wireframe mode\n"
                       "      --no-textures      ignore textures\n"
                       "      --no-normal-maps   ignore normal maps\n"
                       "      --no-cull          draw back faces too\n"
                       "      --points           draw every mesh as points, one splat per vertex\n"
                       "      --point-size <x>   scale the splats of point clouds (default: 1)\n"
                       "      --bg <rrggbb>      background colour\n"
                       "      --ssaa <n>         supersampling factor, 1-4 (default: depends on backend)\n"
                       "      --threads <n>      worker threads (default: all cores)\n"
                       "      --fps <n>          frame rate limit (default: 60)\n"
                       "      --target-fps <n>   lower detail and resolution to hold this rate when moving;\n"
                       "                         0 never does (default: 30)\n"
                       "      --lod <px>         level of detail: allow this much error, in pixels; 'off' or 0\n"
                       "                         always draws the full mesh (default: 0.5 in the viewer, off\n"
                       "                         for exports)\n"
                       "      --no-stats         hide the status bar\n"
                       "\n"
                       "Lighting and effects:\n"
                       "      --light-angle <d>  turn the key light about the vertical axis (default: 0)\n"
                       "      --shadows          cast shadows from the key light\n"
                       "      --shadow-size <n>  shadow map resolution, 64-2048 (default: 1024)\n"
                       "      --shadow-softness <n>  width of the shadow edge, 1 (hard) to 7 (default: 3)\n"
                       "      --env <name|file>  environment lighting: studio, sky, sunset, or an .hdr panorama\n"
                       "      --skybox, --no-skybox   draw the environment as the background, or do not\n"
                       "      --sky-blur <0-1>   blur the background\n"
                       "      --env-intensity <x>, --env-rotation <deg>   brightness and turn of the environment\n"
                       "      --ssao             ambient occlusion: darken creases and corners\n"
                       "      --bloom            glow around very bright areas\n"
                       "      --outline, --no-outline   ink outlines (on by default with toon shading)\n"
                       "      --fxaa             fast approximate anti-aliasing of the final image\n"
                       "      --tonemap <curve>  aces, reinhard, filmic or linear (default: aces)\n"
                       "      --exposure <stops> brighten (+) or darken (-) before tone mapping\n"
                       "\n"
                       "Camera:\n"
                       "      --camera-time <s>  where along a scene's camera path to start, or to take a still\n"
                       "      --no-path          ignore the scene's camera path\n"
                       "      --yaw <deg>        initial rotation around the vertical axis\n"
                       "      --pitch <deg>      initial elevation\n"
                       "      --zoom <factor>    initial zoom, > 1 is closer (default: 1)\n"
                       "      --fov <deg>        vertical field of view (default: 40)\n"
                       "      --spin             auto-rotate (the default when interactive)\n"
                       "      --no-spin          start with auto-rotate off\n"
                       "      --spin-speed <d>   degrees per second (default: 40)\n"
                       "\n"
                       "Animation:\n"
                       "      --animation <a>    clip to play, by number (from 1) or name (default: the first)\n"
                       "      --anim-time <s>    start this many seconds into the clip\n"
                       "      --anim-speed <x>   playback rate, negative plays backwards (default: 1)\n"
                       "      --paused           start with the animation paused\n"
                       "      --list-animations  print the animations in the file and exit\n"
                       "\n"
                       "Export:\n"
                       "      --preview          a quick still for a file manager's preview pane: no terminal\n"
                       "                         queries, and remembered between runs (see docs/previews.md)\n"
                       "      --no-cache         with --preview: render afresh, and do not remember the result\n"
                       "      --preview-max-mb <n>   with --preview: describe larger files instead of drawing\n"
                       "                         them (default: 256)\n"
                       "      --print            print a single frame and exit\n"
                       "      --cols <n>, --rows <n>   size of the printed frame in cells\n"
                       "      --png <file>       render the view to a PNG and exit\n"
                       "      --gif <file>       record a looping GIF and exit: a 360-degree turntable, or\n"
                       "                         one loop of the animation (add --spin to turn it as well)\n"
                       "      --mp4 <file>       the same as an MP4 video (needs ffmpeg installed)\n"
                       "      --size <WxH>       image size for --png / --gif / --mp4\n"
                       "      --frames <n>       frames in the GIF or MP4 (default: 72, or one animation loop)\n"
                       "      --gif-fps <n>      GIF / MP4 playback rate (default: 24)\n"
                       "      --record <file>    record the interactive session from the start: .cast (asciinema),\n"
                       "                         .gif or .mp4. In the viewer, R starts and stops a recording\n"
                       "      --blocks <n>       draw each pixel as an n x n block, like terminal cells\n"
                       "      --bench <n>        render n frames off-screen and print timings\n"
                       "\n"
                       "Benchmark suite (no model needed):\n"
                       "      --benchmark        run the built-in scenes and print reproducible counters and timings\n"
                       "      --benchmark-scene <name>   run only one scene\n"
                       "      --benchmark-list   print the scene names\n"
                       "      --benchmark-json <file>    also write the results as JSON\n"
                       "      --frames <n>       frames per scene (default: 24)\n"
                       "\n"
                       "      --demo <name>      built-in model: ") +
           demos +
           "\n"
           "  -h, --help             show this help\n"
           "  -V, --version          show the version\n"
           "\n"
           "Keys:\n"
           "  drag / arrows / hjkl   orbit            1-5 / s     flat, gouraud, phong, pbr, toon / cycle\n"
           "  right-drag / HJKL      pan              w           wireframe\n"
           "  wheel / + -            zoom             t  n        textures, normal maps\n"
           "  space                  auto-rotate      c           back-face culling\n"
           "  [ ]                    turn the light   b           next output backend\n"
           "  r                      reset view       o           status bar\n"
           "  p                      save a PNG       ?  q        help, quit\n"
           "\n"
           "  a                      play / pause the animation      m  M   next / previous animation\n"
           "  ,  .                   step back / forward             <  >   slower / faster\n"
           "\n"
           "  x                      shadows          e  y        next environment, sky background\n"
           "  u  g  i  f             ambient occlusion, bloom (glow), ink outlines, FXAA\n"
           "  z                      next tone curve  {  }        exposure down / up\n"
           "  d                      next dithering mode\n"
           "  P  (  )                draw as points; smaller / larger points\n";
}

RenderSettings makeRenderSettings(const Options& options) {
    RenderSettings settings;
    settings.shading = options.shading;
    settings.wireframe = options.wireframe;
    settings.textures = options.textures;
    settings.normalMaps = options.normalMaps;
    settings.cullBackfaces = options.cullBackfaces;
    settings.points = options.points;
    settings.pointScale = options.pointSize;
    settings.background = options.background;
    settings.shadows = options.shadows;
    settings.shadowMapSize = options.shadowMapSize;
    settings.shadowSoftness = options.shadowSoftness;
    settings.skyboxBlur = options.skyBlur;
    settings.post.ssao = options.ssao;
    settings.post.bloom = options.bloom;
    settings.post.fxaa = options.fxaa;
    settings.post.outline = options.outline.value_or(options.shading == ShadingMode::Toon);
    settings.post.toneMapper = options.toneMapper;
    settings.post.exposure = options.exposure;
    return settings;
}

bool loadEnvironmentOption(const Options& options, Environment& out, std::string& error) {
    const std::string& name = options.environment;
    if (name.empty()) out = Environment();
    else if (!makeBuiltinEnvironment(name, out) && !loadEnvironment(name, out, error)) return false;
    out.intensity = options.envIntensity;
    out.rotation = radians(options.envRotation);
    return true;
}

bool wantsSkybox(const Options& options, const Environment& environment) {
    return options.skybox.value_or(environment.hasMap());
}

OrbitCamera makeCamera(const Options& options, const Model& model) {
    return makeCamera(options, model.framingBounds());
}

OrbitCamera makeCamera(const Options& options, const Bounds& bounds) {
    OrbitCamera camera;
    camera.fovY = radians(options.fov);  // before frame(): the distance depends on it
    camera.frame(bounds);
    if (options.yaw) camera.yaw = radians(*options.yaw);
    if (options.pitch) camera.pitch = radians(std::clamp(*options.pitch, -89.0f, 89.0f));
    camera.zoom(1.0f / options.zoom);
    return camera;
}

int findAnimation(const Model& model, const std::string& spec) {
    const int count = static_cast<int>(model.rig.animations.size());
    if (count == 0) return -1;
    if (spec.empty()) return 0;

    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    // A name wins over a number, in case a clip is called "2".
    for (int i = 0; i < count; ++i)
        if (lower(model.rig.animations[static_cast<std::size_t>(i)].name) == lower(spec)) return i;

    char* end = nullptr;
    const long number = std::strtol(spec.c_str(), &end, 10);
    if (end != spec.c_str() && *end == '\0' && number >= 1 && number <= count) return static_cast<int>(number - 1);
    return -1;
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
    if (!std::isfinite(value)) return false;  // strtof happily parses "nan" and "inf"
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

bool isSceneFile(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "json";
}

bool applySceneRenderSettings(const Json& render, Options& o, std::string& error) {
    if (render.isNull()) return true;
    auto fail = [&](const std::string& key, const std::string& what) {
        error = "render." + key + ": " + what;
        return false;
    };
    for (const auto& [key, value] : render.members()) {
        // Switches.
        bool* flag = nullptr;
        if (key == "wireframe") flag = &o.wireframe;
        else if (key == "textures") flag = &o.textures;
        else if (key == "normalMaps") flag = &o.normalMaps;
        else if (key == "cull") flag = &o.cullBackfaces;
        else if (key == "shadows") flag = &o.shadows;
        else if (key == "ssao") flag = &o.ssao;
        else if (key == "bloom") flag = &o.bloom;
        else if (key == "fxaa") flag = &o.fxaa;
        if (flag) {
            if (!value.isBool()) return fail(key, "expected true or false");
            *flag = value.boolean();
            continue;
        }
        if (key == "outline") {
            if (!value.isBool()) return fail(key, "expected true or false");
            o.outline = value.boolean();
            continue;
        }

        // Numbers, with the same limits as the command line.
        struct Range {
            double lo, hi;
        };
        auto numberIn = [&](Range range, double& out) {
            if (!value.isNumber() || value.number() < range.lo || value.number() > range.hi) {
                fail(key, "expected a number from " + std::to_string(static_cast<int>(range.lo)) + " to " +
                              std::to_string(static_cast<int>(range.hi)));
                return false;
            }
            out = value.number();
            return true;
        };
        double v = 0.0;
        if (key == "shadowSize") {
            if (!numberIn({64, 2048}, v)) return false;
            o.shadowMapSize = static_cast<int>(v);
        } else if (key == "shadowSoftness") {
            if (!numberIn({1, 7}, v)) return false;
            o.shadowSoftness = static_cast<int>(v);
        } else if (key == "ssaa") {
            if (!numberIn({1, 4}, v)) return false;
            o.supersampling = static_cast<int>(v);
        } else if (key == "exposure") {
            if (!numberIn({-16, 16}, v)) return false;
            o.exposure = static_cast<float>(v);
        } else if (key == "lightAngle") {
            if (!numberIn({-100000, 100000}, v)) return false;
            o.lightAngle = static_cast<float>(v);
        } else if (key == "lod") {
            if (!numberIn({0, 64}, v)) return false;
            o.lodTolerance = static_cast<float>(v);
        } else if (key == "fov") {
            if (!numberIn({5, 150}, v)) return false;
            o.fov = static_cast<float>(v);
        } else if (key == "shading") {
            const std::string& mode = value.string();
            if (mode == "flat") o.shading = ShadingMode::Flat;
            else if (mode == "gouraud") o.shading = ShadingMode::Gouraud;
            else if (mode == "phong") o.shading = ShadingMode::Phong;
            else if (mode == "pbr") o.shading = ShadingMode::Pbr;
            else if (mode == "toon") o.shading = ShadingMode::Toon;
            else return fail(key, "expected \"flat\", \"gouraud\", \"phong\", \"pbr\" or \"toon\"");
        } else if (key == "tonemap") {
            const std::string& curve = value.string();
            if (curve == "aces") o.toneMapper = ToneMapper::Aces;
            else if (curve == "reinhard") o.toneMapper = ToneMapper::Reinhard;
            else if (curve == "filmic") o.toneMapper = ToneMapper::Filmic;
            else if (curve == "linear") o.toneMapper = ToneMapper::Linear;
            else return fail(key, "expected \"aces\", \"reinhard\", \"filmic\" or \"linear\"");
        } else if (key == "background") {
            if (!value.isString() || !parseColor(value.string(), o.background))
                return fail(key, "expected \"#rrggbb\"");
        } else if (key == "dither") {
            const auto mode = ditherModeFromName(value.string());
            if (!mode) return fail(key, "expected \"auto\", \"none\", \"ordered\" or \"fs\"");
            o.dither = *mode;
        } else {
            return fail(key, "is not a render setting");
        }
    }
    return true;
}

ParseResult parseOptions(int argc, const char* const* argv, const Options& defaults) {
    ParseResult result;
    Options& o = result.options;
    o = defaults;
    // What to show always comes from this command line.
    o.inputs.clear();
    o.modelPath.clear();

    auto fail = [&](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg.empty() || arg[0] != '-' || arg == "-") {
            o.inputs.emplace_back(arg);
            if (o.modelPath.empty()) o.modelPath = o.inputs.front();
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
        else if (name == "--points") o.points = true;
        else if (name == "--no-stats") o.stats = false;
        else if (name == "--spin") o.spin = o.spinExplicit = true;
        else if (name == "--paused") o.animationPaused = true;
        else if (name == "--no-path") o.followPath = false;
        else if (name == "--shadows") o.shadows = true;
        else if (name == "--skybox") o.skybox = true;
        else if (name == "--no-skybox") o.skybox = false;
        else if (name == "--ssao") o.ssao = true;
        else if (name == "--bloom") o.bloom = true;
        else if (name == "--fxaa") o.fxaa = true;
        else if (name == "--outline") o.outline = true;
        else if (name == "--no-outline") o.outline = false;
        else if (name == "--list-animations") o.listAnimations = true;
        else if (name == "--no-spin") o.spin = false;
        else if (name == "--print") o.print = true;
        else if (name == "--preview") o.preview = true;
        else if (name == "--no-cache") o.previewCache = false;
        else if (name == "--benchmark") o.benchmark = true;
        else if (name == "--benchmark-list") o.benchmarkList = true;
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
        } else if (name == "--dither") {
            if (!needValue()) return missing();
            const auto mode = ditherModeFromName(value);
            if (!mode) return invalid();
            o.dither = *mode;
        } else if (name == "-s" || name == "--shading") {
            if (!needValue()) return missing();
            if (value == "flat") o.shading = ShadingMode::Flat;
            else if (value == "gouraud") o.shading = ShadingMode::Gouraud;
            else if (value == "phong") o.shading = ShadingMode::Phong;
            else if (value == "pbr") o.shading = ShadingMode::Pbr;
            else if (value == "toon") o.shading = ShadingMode::Toon;
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
        } else if (name == "--lod") {
            if (!needValue()) return missing();
            float v = 0.0f;
            if (value == "off") o.lodTolerance = 0.0f;
            else if (parseFloat(value, v) && v >= 0.0f && v <= 64.0f) o.lodTolerance = v;
            else return invalid();
        } else if (name == "--target-fps") {
            if (!needValue()) return missing();
            if (!intOption(o.targetFps, 0, 1000)) return invalid();
        } else if (name == "--fps") {
            if (!needValue()) return missing();
            if (!intOption(o.fpsLimit, 1, 1000)) return invalid();
        } else if (name == "--preview-max-mb") {
            if (!needValue()) return missing();
            if (!intOption(o.previewMaxMb, 1, 1000000)) return invalid();
        } else if (name == "--point-size") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.pointSize) || o.pointSize <= 0.0f || o.pointSize > 100.0f) return invalid();
        } else if (name == "--camera-time") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.cameraTime) || o.cameraTime < 0.0f) return invalid();
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
        } else if (name == "--light-angle") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.lightAngle)) return invalid();
        } else if (name == "--shadow-size") {
            if (!needValue()) return missing();
            if (!intOption(o.shadowMapSize, 64, 2048)) return invalid();
        } else if (name == "--shadow-softness") {
            if (!needValue()) return missing();
            if (!intOption(o.shadowSoftness, 1, 7)) return invalid();
        } else if (name == "--env") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.environment = std::string(value);
        } else if (name == "--sky-blur") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.skyBlur) || o.skyBlur < 0.0f || o.skyBlur > 1.0f) return invalid();
        } else if (name == "--env-intensity") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.envIntensity) || o.envIntensity < 0.0f || o.envIntensity > 1000.0f)
                return invalid();
        } else if (name == "--env-rotation") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.envRotation)) return invalid();
        } else if (name == "--tonemap") {
            if (!needValue()) return missing();
            if (value == "aces") o.toneMapper = ToneMapper::Aces;
            else if (value == "reinhard") o.toneMapper = ToneMapper::Reinhard;
            else if (value == "filmic") o.toneMapper = ToneMapper::Filmic;
            else if (value == "linear" || value == "none") o.toneMapper = ToneMapper::Linear;
            else return invalid();
        } else if (name == "--exposure") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.exposure) || std::abs(o.exposure) > 16.0f) return invalid();
        } else if (name == "--animation") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.animation = std::string(value);
        } else if (name == "--anim-time") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.animationTime) || o.animationTime < 0.0f) return invalid();
            o.animationTimeSet = true;
        } else if (name == "--anim-speed") {
            if (!needValue()) return missing();
            if (!parseFloat(value, o.animationSpeed) || std::abs(o.animationSpeed) > 100.0f) return invalid();
            o.animationSpeedSet = true;
        } else if (name == "--png") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.pngPath = std::string(value);
        } else if (name == "--gif") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.gifPath = std::string(value);
        } else if (name == "--mp4") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.mp4Path = std::string(value);
        } else if (name == "--record") {
            if (!needValue()) return missing();
            if (value.empty() || !recordingFormatOf(std::string(value))) {
                return fail("--record needs a file name ending in .cast, .gif or .mp4");
            }
            o.recordPath = std::string(value);
        } else if (name == "--size") {
            if (!needValue()) return missing();
            const std::size_t x = value.find_first_of("xX");
            if (x == std::string_view::npos || !parseInt(value.substr(0, x), o.width) ||
                !parseInt(value.substr(x + 1), o.height) || o.width < 8 || o.height < 8 || o.width > 8192 ||
                o.height > 8192)
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
        } else if (name == "--benchmark-scene") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.benchmarkScene = std::string(value);
            o.benchmark = true;
        } else if (name == "--benchmark-json") {
            if (!needValue()) return missing();
            if (value.empty()) return invalid();
            o.benchmarkJsonPath = std::string(value);
            o.benchmark = true;
        } else if (name == "--bench") {
            if (!needValue()) return missing();
            if (!intOption(o.benchFrames, 1, 1000000)) return invalid();
        } else {
            return fail("unknown option " + name);
        }
    }

    if (o.showHelp || o.showVersion || o.benchmark || o.benchmarkList) return result;
    if (o.inputs.empty() && o.demo.empty()) return fail("no model given");
    if (!o.inputs.empty() && !o.demo.empty()) return fail("give either model files or --demo, not both");
    // A scene file is a whole scene; it cannot be combined with more models.
    const bool hasScene = std::any_of(o.inputs.begin(), o.inputs.end(), isSceneFile);
    if (hasScene && o.inputs.size() > 1) return fail("a scene file cannot be combined with other files");
    return result;
}

}  // namespace termesh
