// termesh - a 3D model viewer, and scene renderer, for the terminal.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "anim/skinning.hpp"
#include "app/benchmark.hpp"
#include "app/capabilities.hpp"
#include "app/options.hpp"
#include "app/preview.hpp"
#include "app/setup.hpp"
#include "app/viewer.hpp"
#include "export/gif.hpp"
#include "export/offscreen.hpp"
#include "export/recorder.hpp"
#include "pipeline/resolve.hpp"
#include "plugin/plugin.hpp"
#include "raster/simd.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"
#include "stage/scene_file.hpp"

using namespace termesh;

namespace {

// The render settings the options describe, for a scene.
RenderSettings renderSettings(const Options& options, const Scene& scene) {
    RenderSettings settings = makeRenderSettings(options);
    settings.lightDirection =
        transformDirection(rotate({0.0f, 1.0f, 0.0f}, radians(options.lightAngle)), settings.lightDirection);
    settings.lights = scene.lights;
    settings.environment = &scene.environment;
    settings.skybox = options.skybox.value_or(scene.skybox.value_or(scene.environment.hasMap()));
    if (options.skyBlur == 0.0f) settings.skyboxBlur = scene.skyBlur;
    return settings;
}

// Where the non-interactive modes look from: along the scene's camera path
// if it has one, from its fixed camera if it has one of those, and
// otherwise from the usual framing of everything, turned by the options.
struct View {
    View(const Options& options, const Scene& scene) : scene_(scene), bounds_(scene.bounds()) {
        usesPath_ = options.followPath && !scene.cameraPath.empty();
        const bool aimed = options.yaw || options.pitch;
        if (scene.camera && !aimed) {
            orbit_ = orbitFromPose(*scene.camera, bounds_);
            orbit_.zoom(1.0f / options.zoom);
        } else {
            orbit_ = makeCamera(options, bounds_);
        }
    }

    bool usesPath() const { return usesPath_; }
    float pathDuration() const { return scene_.cameraPath.duration; }

    // The camera `pathTime` seconds along the path, or - without a path -
    // turned by `turn` radians from where it started.
    Camera camera(float aspect, float pathTime, float turn) const {
        if (usesPath_) return cameraFromPose(scene_.cameraPath.evaluate(pathTime), aspect, bounds_);
        OrbitCamera orbit = orbit_;
        orbit.yaw += turn;
        return orbit.camera(aspect);
    }

private:
    const Scene& scene_;
    Bounds bounds_;
    OrbitCamera orbit_;
    bool usesPath_ = false;
};

// Level of detail for the non-interactive modes: off unless asked for.
// Returns the tolerance in output pixels, having built the levels if needed.
float prepareLods(const Options& options, Scene& scene, ThreadPool& pool) {
    const float tolerance = options.lodTolerance.value_or(0.0f);
    if (tolerance > 0.0f) scene.buildLods(&pool);
    return tolerance;
}

// --png: one frame to an image file.
int runPng(const Options& options, Scene& scene, ThreadPool& pool) {
    Renderer renderer(pool);
    OffscreenTarget target;
    if (options.width > 0) {
        target.width = options.width;
        target.height = options.height;
    }
    target.supersampling = options.supersampling > 0 ? options.supersampling : 2;
    target.blocks = options.blocks;

    RenderSettings settings = renderSettings(options, scene);
    settings.lodTolerance = prepareLods(options, scene, pool);
    const View view(options, scene);
    const std::vector<DrawItem> items = scene.drawItems();
    const Image image = renderOffscreen(
        renderer, pool, items, [&](float aspect) { return view.camera(aspect, options.cameraTime, 0.0f); }, settings,
        target);
    if (!writePng(options.pngPath, image)) {
        std::fprintf(stderr, "termesh: could not write '%s'\n", options.pngPath.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s (%dx%d)\n", options.pngPath.c_str(), image.width, image.height);
    return 0;
}

// --gif and --mp4: a looping animation. What moves depends on the scene:
//
//   a camera path     the camera follows it, once round
//   animated models   one loop of the animation, from a fixed camera
//                     (add --spin to turn the camera as well)
//   neither           a full turn of the camera around the scene
int runGif(const Options& options, Scene& scene, ThreadPool& pool) {
    Renderer renderer(pool);
    OffscreenTarget target;
    target.width = options.width > 0 ? options.width : 480;
    target.height = options.height > 0 ? options.height : 360;
    target.supersampling = options.supersampling > 0 ? options.supersampling : 3;
    target.blocks = options.blocks;

    RenderSettings settings = renderSettings(options, scene);
    settings.lodTolerance = prepareLods(options, scene, pool);
    const View view(options, scene);
    // Spin the same way as the interactive viewer; a negative speed reverses.
    const float direction = options.spinSpeed < 0.0f ? -1.0f : 1.0f;
    // A scene that moves by itself (the physics demo) is watched from a
    // fixed camera for a few seconds.
    const bool simulated = scene.driver != nullptr;
    const bool spin = !view.usesPath() && ((!scene.animated() && !simulated) || options.spinExplicit);

    // Where each animation starts, so that every frame can be placed
    // exactly rather than accumulated.
    std::vector<float> startTimes;
    float loopSeconds = 0.0f;  // the longest animation loop, at its playback speed
    for (const auto& object : scene.objects) {
        startTimes.push_back(object->player ? object->player->time() : 0.0f);
        if (object->player && object->player->speed() != 0.0f)
            loopSeconds = std::max(loopSeconds, object->player->duration() / std::abs(object->player->speed()));
    }

    // By default the GIF lasts exactly as long as whatever is moving, so
    // that it repeats seamlessly.
    int frameCount = options.frames;
    if (frameCount <= 0) {
        const float seconds = view.usesPath() ? view.pathDuration() : (simulated ? 8.0f : loopSeconds);
        frameCount =
            seconds > 0.0f
                ? std::clamp(static_cast<int>(std::lround(seconds * static_cast<float>(options.gifFps))), 2, 2000)
                : 72;
    }

    const bool progress = Terminal::errorIsTerminal();
    std::vector<Image> frames;
    frames.reserve(static_cast<std::size_t>(frameCount));
    for (int i = 0; i < frameCount; ++i) {
        const float seconds = static_cast<float>(i) / static_cast<float>(options.gifFps);
        // The last frame stops one step short of a full turn, so the loop
        // joins up without a repeated frame.
        const float turn =
            spin ? direction * 2.0f * kPi * static_cast<float>(i) / static_cast<float>(frameCount) : 0.0f;
        for (std::size_t k = 0; k < scene.objects.size(); ++k)
            if (AnimationPlayer* player = scene.objects[k]->player.get())
                player->seek(startTimes[k] + player->speed() * seconds);
        scene.pose(&pool);
        if (simulated && i > 0) scene.driver->update(1.0f / static_cast<float>(options.gifFps));

        const std::vector<DrawItem> items = scene.drawItems();
        frames.push_back(renderOffscreen(
            renderer, pool, items,
            [&](float aspect) { return view.camera(aspect, options.cameraTime + seconds, turn); }, settings, target));
        if (progress) std::fprintf(stderr, "\rrendering frame %d/%d", i + 1, frameCount);
    }

    if (progress) std::fprintf(stderr, "\rencoding...                 \r");
    if (!options.gifPath.empty()) {
        const GifPalette palette = buildGifPalette(frames);
        GifWriter writer;
        // GIF delays are whole hundredths of a second.
        const int delayCs = std::max((100 + options.gifFps / 2) / options.gifFps, 2);
        bool ok = writer.open(options.gifPath, frames[0].width, frames[0].height, palette, delayCs);
        for (const Image& frame : frames) ok = ok && writer.addFrame(frame);
        ok = writer.close() && ok;
        if (!ok) {
            std::fprintf(stderr, "termesh: could not write '%s'\n", options.gifPath.c_str());
            return 1;
        }
        std::fprintf(stderr, "wrote %s (%dx%d, %d frames)\n", options.gifPath.c_str(), frames[0].width,
                     frames[0].height, frameCount);
    }
    if (!options.mp4Path.empty()) {
        std::string error;
        std::unique_ptr<VideoSink> video =
            makeMp4Sink(options.mp4Path, frames[0].width, frames[0].height, options.gifFps, error);
        bool ok = video != nullptr;
        for (const Image& frame : frames) ok = ok && video->addFrame(frame);
        ok = ok && video->close(error);
        if (!ok) {
            std::fprintf(stderr, "termesh: %s\n", error.c_str());
            return 1;
        }
        std::fprintf(stderr, "wrote %s (%dx%d, %d frames)\n", options.mp4Path.c_str(), frames[0].width & ~1,
                     frames[0].height & ~1, frameCount);
    }
    return 0;
}

// --print: a single frame written into the terminal's normal scrollback.
int runPrint(const Options& options, Scene& scene, ThreadPool& pool) {
    TerminalCaps caps = capsFromEnvironment();
    Terminal terminal;
    const TerminalSize size = terminal.size();

    // If we are attached to a real terminal, ask it what it supports.
    if (Terminal::inputIsTerminal() && Terminal::outputIsTerminal() && terminal.enterRawMode()) {
        InputParser parser;
        probeTerminal(terminal, parser, caps);
        terminal.leaveRawMode();
    } else {
        caps.kittyGraphics = false;  // cannot be confirmed without asking
    }
    if (options.color) caps.color = *options.color;
    resolveCellSize(size, caps);

    DrawArea area;
    area.cols = options.cols > 0 ? options.cols : size.cols;
    // By default, about as tall as it is wide, but never taller than the window.
    const int squareRows = area.cols * caps.cellWidth / caps.cellHeight;
    area.rows = options.rows > 0 ? options.rows : std::clamp(squareRows, 2, std::max(size.rows - 2, 2));
    area.cellWidth = caps.cellWidth;
    area.cellHeight = caps.cellHeight;

    const BackendKind kind = options.backend.value_or(chooseBackend(caps));
    const auto backend = makeBackend(kind, caps.color, options.customBackend);
    backend->setDither(options.dither);
    int width = 0, height = 0;
    backend->configure(area, width, height);

    int ssaa = options.supersampling > 0 ? options.supersampling : backend->defaultSupersampling();
    while (ssaa > 1 && static_cast<std::int64_t>(width) * ssaa * height * ssaa > kMaxFramebufferPixels) --ssaa;

    RenderSettings settings = renderSettings(options, scene);
    settings.wireframe = settings.wireframe || backend->wantsWireframe();
    settings.wireWidth = static_cast<float>(ssaa);
    settings.pixelScale = static_cast<float>(ssaa);
    settings.lodTolerance = prepareLods(options, scene, pool) * static_cast<float>(ssaa);

    Renderer renderer(pool);
    Framebuffer fb(width * ssaa, height * ssaa);
    const float aspect = static_cast<float>(width) * backend->pixelAspect() / static_cast<float>(height);
    const std::vector<DrawItem> items = scene.drawItems();
    renderer.render(items, View(options, scene).camera(aspect, options.cameraTime, 0.0f), settings, fb);
    Image image;
    resolve(fb, ssaa, image, pool);
    if (settings.post.fxaa) applyFxaa(image, pool);

    std::string out;
    backend->print(image.view(), out);
    Terminal::prepareOutput();
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    return 0;
}

// --preview: one still for a file manager's preview pane.
//
// Everything about it is chosen for an answer that arrives at once and
// cannot go wrong: the terminal is not asked what it supports (a preview
// pane's output is a pipe, and a query would wait for a reply that never
// comes), the default is half-block text that every file manager can show,
// and the finished output is cached.
struct PreviewTarget {
    BackendKind backend;
    ColorDepth color;
    int cols, rows;
    std::string cacheKey;  // empty: do not cache
};

PreviewTarget previewTarget(const Options& options) {
    PreviewTarget target;
    const TerminalCaps caps = capsFromEnvironment();
    target.color = options.color.value_or(caps.color == ColorDepth::None ? ColorDepth::Indexed256 : caps.color);
    target.backend = options.backend.value_or(BackendKind::HalfBlock);
    target.cols = options.cols > 0 ? options.cols : 80;
    target.rows = options.rows > 0 ? options.rows : 24;
    if (options.previewCache && options.inputs.size() == 1) {
        // Everything that changes the picture. (The model itself is covered
        // by its size and modification time.)
        char parameters[256];
        std::snprintf(
            parameters, sizeof(parameters), "%d %d %d %d|%d %d %d %d %d %d %d|%g %g %g %g %g|%s|%s|%d %g %d %d %d",
            static_cast<int>(target.backend), static_cast<int>(target.color), target.cols, target.rows,
            static_cast<int>(options.shading), options.wireframe, options.textures, options.normalMaps,
            options.cullBackfaces, options.points, options.supersampling,
            static_cast<double>(options.yaw.value_or(-1000.0f)), static_cast<double>(options.pitch.value_or(-1000.0f)),
            static_cast<double>(options.zoom), static_cast<double>(options.fov),
            static_cast<double>(options.cameraTime), options.environment.c_str(), options.animation.c_str(),
            options.shadows, static_cast<double>(options.exposure), static_cast<int>(options.toneMapper),
            static_cast<int>(options.dither),
            options.background.r << 16 | options.background.g << 8 | options.background.b);
        target.cacheKey = previewCacheKey(options.inputs.front(), parameters + options.customBackend);
    }
    return target;
}

std::string renderPreview(const Options& options, Scene& scene, const PreviewTarget& target, ThreadPool& pool) {
    // A caption underneath, if there is room for one.
    const bool caption = target.rows >= 8;
    DrawArea area;
    area.cols = target.cols;
    area.rows = std::max(target.rows - (caption ? 1 : 0), 1);

    const auto backend = makeBackend(target.backend, target.color, options.customBackend);
    backend->setDither(options.dither);
    int width = 0, height = 0;
    backend->configure(area, width, height);
    int ssaa = options.supersampling > 0 ? options.supersampling : backend->defaultSupersampling();
    while (ssaa > 1 && static_cast<std::int64_t>(width) * ssaa * height * ssaa > kMaxFramebufferPixels) --ssaa;

    RenderSettings settings = renderSettings(options, scene);
    settings.wireframe = settings.wireframe || backend->wantsWireframe();
    settings.wireWidth = static_cast<float>(ssaa);
    settings.pixelScale = static_cast<float>(ssaa);
    settings.lodTolerance = prepareLods(options, scene, pool) * static_cast<float>(ssaa);

    Renderer renderer(pool);
    Framebuffer fb(width * ssaa, height * ssaa);
    const float aspect = static_cast<float>(width) * backend->pixelAspect() / static_cast<float>(height);
    const std::vector<DrawItem> items = scene.drawItems();
    renderer.render(items, View(options, scene).camera(aspect, options.cameraTime, 0.0f), settings, fb);
    Image image;
    resolve(fb, ssaa, image, pool);
    if (settings.post.fxaa) applyFxaa(image, pool);

    std::string out;
    backend->print(image.view(), out);
    if (caption) {
        char counts[64];
        const std::size_t triangles = scene.triangleCount();
        if (triangles >= 1000)
            std::snprintf(counts, sizeof(counts), "%.1fk triangles", static_cast<double>(triangles) / 1e3);
        else if (triangles > 0) std::snprintf(counts, sizeof(counts), "%zu triangles", triangles);
        else std::snprintf(counts, sizeof(counts), "%zu points", scene.vertexCount());
        std::string text = " " + scene.name + "  " + counts;
        if (scene.animated()) text += "  animated";
        if (static_cast<int>(text.size()) > target.cols) text.resize(static_cast<std::size_t>(target.cols));
        out += (target.color == ColorDepth::None ? "" : "\x1b[2m") + text + "\x1b[0m\n";
    }
    return out;
}

// --bench: time the renderer alone, with no terminal in the way.
int runBench(const Options& options, Scene& scene, ThreadPool& pool) {
    Renderer renderer(pool);
    const int width = options.width > 0 ? options.width : 640;
    const int height = options.height > 0 ? options.height : 360;
    const int ssaa = options.supersampling > 0 ? options.supersampling : 1;
    Framebuffer fb(width * ssaa, height * ssaa);
    Image image;
    RenderSettings settings = renderSettings(options, scene);
    settings.wireWidth = static_cast<float>(ssaa);
    settings.pixelScale = static_cast<float>(ssaa);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const View view(options, scene);

    if (options.lodTolerance.value_or(0.0f) > 0.0f) {
        const auto lodStart = std::chrono::steady_clock::now();
        scene.buildLods(&pool);
        std::printf("lod          built in %.1f ms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lodStart).count());
        settings.lodTolerance = *options.lodTolerance * static_cast<float>(ssaa);
    }

    RenderStats total;
    double poseMs = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < options.benchFrames; ++i) {
        // Fixed steps, so every run renders the same frames: two degrees of
        // turn (or a sixtieth of a second of camera path) per frame.
        const float seconds = static_cast<float>(i) / 60.0f;
        if (scene.animated()) {
            const auto poseStart = std::chrono::steady_clock::now();
            scene.update(1.0f / 60.0f, &pool);
            poseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - poseStart).count();
        }
        const std::vector<DrawItem> items = scene.drawItems();
        const RenderStats s = renderer.render(
            items, view.camera(aspect, options.cameraTime + seconds, radians(2.0f) * static_cast<float>(i + 1)),
            settings, fb);
        resolve(fb, ssaa, image, pool);
        if (settings.post.fxaa) applyFxaa(image, pool);
        total.vertexMs += s.vertexMs;
        total.setupMs += s.setupMs;
        total.rasterMs += s.rasterMs;
        total.shadowMs += s.shadowMs;
        total.postMs += s.postMs;
        total.trianglesDrawn += s.trianglesDrawn;
        total.trianglesCulled += s.trianglesCulled;
        total.trianglesSimplified += s.trianglesSimplified;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double n = options.benchFrames;

    std::printf("model        %s: %zu triangles, %zu vertices\n", scene.name.c_str(), scene.triangleCount(),
                scene.vertexCount());
    std::printf("target       %dx%d, ssaa %d, %s shading%s, %u threads, %s\n", width, height, ssaa,
                shadingModeName(settings.shading), settings.wireframe ? " (wireframe)" : "", pool.threadCount(),
                simd::kName);
    std::printf("frames       %d in %.2f s  ->  %.1f fps (%.2f ms/frame)\n", options.benchFrames, seconds, n / seconds,
                seconds * 1000.0 / n);
    std::printf("per frame    vertex %.2f ms, setup %.2f ms, raster %.2f ms, %.0f triangles drawn\n",
                total.vertexMs / n, total.setupMs / n, total.rasterMs / n,
                static_cast<double>(total.trianglesDrawn) / n);
    if (total.trianglesSimplified > 0)
        std::printf("lod          %.0f triangles per frame saved by coarser levels of detail\n",
                    static_cast<double>(total.trianglesSimplified) / n);
    if (total.trianglesCulled > 0)
        std::printf("culling      %.0f triangles per frame skipped before the vertex stage\n",
                    static_cast<double>(total.trianglesCulled) / n);
    if (total.shadowMs > 0.0 || total.postMs > 0.0)
        std::printf("effects      shadow maps %.2f ms, post-processing %.2f ms\n", total.shadowMs / n,
                    total.postMs / n);
    if (scene.animated()) std::printf("animation    pose + skinning %.2f ms per frame\n", poseMs / n);
    return 0;
}

// --benchmark: the built-in suite.
int runBenchmark(const Options& options) {
    ThreadPool pool(static_cast<unsigned>(options.threads));
    const int frames = options.frames > 0 ? options.frames : 24;
    std::vector<std::string> names = benchmarkSceneNames();
    if (!options.benchmarkScene.empty()) names = {options.benchmarkScene};

    std::vector<BenchmarkResult> results;
    for (const std::string& name : names) {
        BenchmarkResult result;
        if (!runBenchmarkScene(name, frames, pool, result)) {
            std::fprintf(stderr, "termesh: no benchmark scene '%s' (try --benchmark-list)\n", name.c_str());
            return 2;
        }
        results.push_back(std::move(result));
    }
    std::printf("termesh %s benchmark: %u threads, %s\n\n%s", TERMESH_VERSION, pool.threadCount(), simd::kName,
                benchmarkTable(results).c_str());
    if (!options.benchmarkJsonPath.empty()) {
        std::FILE* file = std::fopen(options.benchmarkJsonPath.c_str(), "wb");
        const std::string json = benchmarkJson(results, pool.threadCount());
        const bool ok = file && std::fwrite(json.data(), 1, json.size(), file) == json.size();
        if (file) std::fclose(file);
        if (!ok) {
            std::fprintf(stderr, "termesh: could not write '%s'\n", options.benchmarkJsonPath.c_str());
            return 1;
        }
    }
    return 0;
}

// Load the plugins named on the command line and the ones in the plugin
// directories. Returns an exit code if the program should stop here (an
// error, or --list-plugins), and -1 to carry on.
int loadPlugins(const Options& options) {
    for (const std::string& path : options.plugins) {
        if (std::string error; !loadPlugin(path, error)) {
            std::fprintf(stderr, "termesh: %s\n", error.c_str());
            return 1;
        }
    }
    // A plugin that was not asked for by name and does not load is worth a
    // warning, but not worth refusing to run.
    if (options.autoPlugins && pluginsSupported()) {
        for (const std::string& directory : pluginDirectories()) {
            std::vector<std::string> errors;
            loadPluginDirectory(directory, errors);
            for (const std::string& error : errors) std::fprintf(stderr, "termesh: warning: %s\n", error.c_str());
        }
    }

    if (options.listPlugins) {
        if (!pluginsSupported()) std::printf("this build of termesh cannot load plugins\n");
        else if (loadedPlugins().empty()) std::printf("no plugins loaded\n");
        for (const PluginInfo& plugin : loadedPlugins()) {
            std::printf("%s  (%s)\n", plugin.name.c_str(), plugin.path.c_str());
            if (!plugin.extensions.empty()) {
                std::printf("  loads   ");
                for (const std::string& extension : plugin.extensions) std::printf(" .%s", extension.c_str());
                std::printf("\n");
            }
            for (std::size_t i = 0; i < plugin.backends.size(); ++i)
                std::printf("  backend  %s  %s\n", plugin.backends[i].c_str(), plugin.backendDescriptions[i].c_str());
        }
        if (pluginsSupported() && options.autoPlugins) {
            std::printf("plugin directories:\n");
            for (const std::string& directory : pluginDirectories()) std::printf("  %s\n", directory.c_str());
        }
        return 0;
    }

    if (options.backend == BackendKind::Custom && !isRegisteredBackend(options.customBackend)) {
        std::string known = "kitty, sixel, halfblock, braille, ascii";
        for (const std::string& name : registeredBackendNames()) known += ", " + name;
        std::fprintf(stderr, "termesh: unknown backend '%s' (there is: %s)\nTry 'termesh --help'.\n",
                     options.customBackend.c_str(), known.c_str());
        return 2;
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    ParseResult parsed = parseOptions(argc, argv);
    if (!parsed.ok) {
        std::fprintf(stderr, "termesh: %s\nTry 'termesh --help'.\n", parsed.error.c_str());
        return 2;
    }
    Options& options = parsed.options;
    if (options.showHelp) {
        std::fputs(usageText().c_str(), stdout);
        return 0;
    }
    if (options.showVersion) {
        std::printf("termesh %s\n", TERMESH_VERSION);
#ifdef TERMESH_HAS_ASSIMP
        std::printf("with the Assimp backend\n");
#endif
        return 0;
    }
    if (options.benchmarkList) {
        for (const std::string& name : benchmarkSceneNames()) std::printf("%s\n", name.c_str());
        return 0;
    }

    // Plugins come first: they decide which files can be opened and which
    // backends exist.
    if (const int status = loadPlugins(options); status >= 0) return status;
    if (options.benchmark) return runBenchmark(options);

    // Previews: answer from the cache if possible, before loading anything.
    PreviewTarget preview{};
    if (options.preview) {
        preview = previewTarget(options);
        if (std::string cached; readPreviewCache(previewCacheDirectory(), preview.cacheKey, cached)) {
            Terminal::prepareOutput();
            std::fwrite(cached.data(), 1, cached.size(), stdout);
            return 0;
        }
        // A file too big to load in the time a preview has gets a line of
        // text instead of a long wait.
        if (!options.inputs.empty()) {
            const std::uint64_t megabytes = fileSize(options.inputs.front()) >> 20;
            if (megabytes > static_cast<std::uint64_t>(options.previewMaxMb)) {
                std::printf("%s\n%llu MB: too large to preview (limit %d MB; see --preview-max-mb)\n",
                            options.inputs.front().c_str(), static_cast<unsigned long long>(megabytes),
                            options.previewMaxMb);
                return 0;
            }
        }
    }

    ThreadPool pool(static_cast<unsigned>(options.threads));
    Scene scene;
    if (!buildScene(argc, argv, options, pool, scene)) return options.demo.empty() ? 1 : 2;

    if (options.listAnimations) {
        for (const auto& object : scene.objects) {
            const Model& model = object->model;
            if (scene.objects.size() > 1) std::printf("%s:\n", object->name.c_str());
            if (!model.animated()) std::printf("%s has no animations\n", object->name.c_str());
            for (std::size_t i = 0; i < model.rig.animations.size(); ++i) {
                const Animation& animation = model.rig.animations[i];
                std::printf("%2zu  %-24s %6.2f s  %zu channels\n", i + 1,
                            animation.name.empty() ? "(unnamed)" : animation.name.c_str(), animation.duration,
                            animation.channels.size());
            }
        }
        return 0;
    }

    // An environment named on the command line replaces the scene's.
    if (!options.environment.empty() || !scene.hasEnvironment) {
        if (std::string error; !loadEnvironmentOption(options, scene.environment, error)) {
            std::fprintf(stderr, "termesh: %s\n", error.c_str());
            return 1;
        }
    }
    if (!setUpAnimations(options, scene, pool)) return 2;
    // --anim-time also runs a simulation forward to that moment.
    if (scene.driver && options.animationTimeSet)
        for (float t = 0.0f; t < options.animationTime; t += 1.0f / 60.0f) scene.driver->update(1.0f / 60.0f);

    if (options.preview) {
        const std::string out = renderPreview(options, scene, preview, pool);
        writePreviewCache(previewCacheDirectory(), preview.cacheKey, out);
        Terminal::prepareOutput();
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    if (options.benchFrames > 0) return runBench(options, scene, pool);
    // --png, --gif and --mp4 can be combined: one run writes them all.
    const bool video = !options.gifPath.empty() || !options.mp4Path.empty();
    if (!options.pngPath.empty() || video) {
        int status = 0;
        if (!options.pngPath.empty()) status = runPng(options, scene, pool);
        if (status == 0 && video) status = runGif(options, scene, pool);
        return status;
    }
    if (options.print) return runPrint(options, scene, pool);

    Viewer viewer(options, scene, pool);
    return viewer.run();
}
