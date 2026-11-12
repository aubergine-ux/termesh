// termesh - a 3D model viewer for the terminal.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "app/capabilities.hpp"
#include "app/options.hpp"
#include "anim/player.hpp"
#include "anim/skinning.hpp"
#include "app/viewer.hpp"
#include "export/gif.hpp"
#include "export/offscreen.hpp"
#include "pipeline/resolve.hpp"
#include "raster/simd.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"

using namespace termesh;

namespace {

// --png: one frame to an image file.
int runPng(const Options& options, const Model& model, ThreadPool& pool) {
    Renderer renderer(pool);
    OffscreenTarget target;
    if (options.width > 0) {
        target.width = options.width;
        target.height = options.height;
    }
    target.supersampling = options.supersampling > 0 ? options.supersampling : 2;
    target.blocks = options.blocks;

    const RenderSettings settings = makeRenderSettings(options);
    const Image image = renderOffscreen(renderer, pool, model, makeCamera(options, model), settings, target);
    if (!writePng(options.pngPath, image)) {
        std::fprintf(stderr, "termesh: could not write '%s'\n", options.pngPath.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s (%dx%d)\n", options.pngPath.c_str(), image.width, image.height);
    return 0;
}

// --gif: a looping animation. For a static model that is a full turn of the
// camera; for an animated one it is one loop of the clip, seen from a fixed
// camera unless --spin asks for the turn as well.
int runGif(const Options& options, Model& model, AnimationPlayer* player, ThreadPool& pool) {
    Renderer renderer(pool);
    OffscreenTarget target;
    target.width = options.width > 0 ? options.width : 480;
    target.height = options.height > 0 ? options.height : 360;
    target.supersampling = options.supersampling > 0 ? options.supersampling : 3;
    target.blocks = options.blocks;

    const RenderSettings settings = makeRenderSettings(options);
    OrbitCamera camera = makeCamera(options, model);
    const float startYaw = camera.yaw;
    // Spin the same way as the interactive viewer; a negative speed reverses.
    const float direction = options.spinSpeed < 0.0f ? -1.0f : 1.0f;
    const bool spin = !player || options.spinExplicit;

    // By default an animated GIF lasts exactly one loop of the clip, so that
    // it repeats seamlessly.
    const float rate = player ? std::abs(player->speed()) : 0.0f;
    const float startTime = player ? player->time() : 0.0f;
    int frameCount = options.frames;
    if (frameCount <= 0) {
        frameCount = 72;
        if (player && player->duration() > 0.0f && rate > 0.0f)
            frameCount = std::clamp(
                static_cast<int>(std::lround(player->duration() / rate * static_cast<float>(options.gifFps))), 2, 2000);
    }

    const bool progress = Terminal::errorIsTerminal();
    std::vector<Image> frames;
    frames.reserve(static_cast<std::size_t>(frameCount));
    for (int i = 0; i < frameCount; ++i) {
        // The last frame stops one step short of a full turn, so the loop
        // joins up without a repeated frame.
        if (spin)
            camera.yaw = startYaw + direction * 2.0f * kPi * static_cast<float>(i) / static_cast<float>(frameCount);
        if (player) {
            player->seek(startTime + player->speed() * static_cast<float>(i) / static_cast<float>(options.gifFps));
            applyPose(model, player->pose(), &pool);
        }
        frames.push_back(renderOffscreen(renderer, pool, model, camera, settings, target));
        if (progress) std::fprintf(stderr, "\rrendering frame %d/%d", i + 1, frameCount);
    }

    if (progress) std::fprintf(stderr, "\rencoding...                 \r");
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
    std::fprintf(stderr, "wrote %s (%dx%d, %d frames)\n", options.gifPath.c_str(), frames[0].width, frames[0].height,
                 frameCount);
    return 0;
}

// --print: a single frame written into the terminal's normal scrollback.
int runPrint(const Options& options, const Model& model, ThreadPool& pool) {
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
    const auto backend = makeBackend(kind, caps.color);
    int width = 0, height = 0;
    backend->configure(area, width, height);

    int ssaa = options.supersampling > 0 ? options.supersampling : backend->defaultSupersampling();
    while (ssaa > 1 && static_cast<std::int64_t>(width) * ssaa * height * ssaa > kMaxFramebufferPixels) --ssaa;

    RenderSettings settings = makeRenderSettings(options);
    settings.wireframe = settings.wireframe || backend->wantsWireframe();
    settings.wireWidth = static_cast<float>(ssaa);

    Renderer renderer(pool);
    Framebuffer fb(width * ssaa, height * ssaa);
    const float aspect = static_cast<float>(width) * backend->pixelAspect() / static_cast<float>(height);
    renderer.render(model, makeCamera(options, model).camera(aspect), settings, fb);
    Image image;
    resolve(fb, ssaa, image, pool);

    std::string out;
    backend->print(image.view(), out);
    Terminal::prepareOutput();
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    return 0;
}

// --bench: time the renderer alone, with no terminal in the way.
int runBench(const Options& options, Model& model, AnimationPlayer* player, ThreadPool& pool) {
    Renderer renderer(pool);
    const int width = options.width > 0 ? options.width : 640;
    const int height = options.height > 0 ? options.height : 360;
    const int ssaa = options.supersampling > 0 ? options.supersampling : 1;
    Framebuffer fb(width * ssaa, height * ssaa);
    Image image;
    RenderSettings settings = makeRenderSettings(options);
    settings.wireWidth = static_cast<float>(ssaa);
    OrbitCamera camera = makeCamera(options, model);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    RenderStats total;
    double poseMs = 0.0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < options.benchFrames; ++i) {
        camera.yaw += radians(2.0f);
        if (player) {
            // A fixed time step, so every run poses the same frames.
            const auto poseStart = std::chrono::steady_clock::now();
            player->advance(1.0f / 60.0f);
            applyPose(model, player->pose(), &pool);
            poseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - poseStart).count();
        }
        const RenderStats s = renderer.render(model, camera.camera(aspect), settings, fb);
        resolve(fb, ssaa, image, pool);
        total.vertexMs += s.vertexMs;
        total.setupMs += s.setupMs;
        total.rasterMs += s.rasterMs;
        total.trianglesDrawn += s.trianglesDrawn;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double n = options.benchFrames;

    std::printf("model        %s: %zu triangles, %zu vertices\n", model.name.c_str(), model.triangleCount(),
                model.vertexCount());
    std::printf("target       %dx%d, ssaa %d, %s shading%s, %u threads, %s\n", width, height, ssaa,
                shadingModeName(settings.shading), settings.wireframe ? " (wireframe)" : "", pool.threadCount(),
                simd::kName);
    std::printf("frames       %d in %.2f s  ->  %.1f fps (%.2f ms/frame)\n", options.benchFrames, seconds, n / seconds,
                seconds * 1000.0 / n);
    std::printf("per frame    vertex %.2f ms, setup %.2f ms, raster %.2f ms, %.0f triangles drawn\n",
                total.vertexMs / n, total.setupMs / n, total.rasterMs / n,
                static_cast<double>(total.trianglesDrawn) / n);
    if (player)
        std::printf("animation    %s: pose + skinning %.2f ms per frame\n", player->clipName(player->clip()).c_str(),
                    poseMs / n);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const ParseResult parsed = parseOptions(argc, argv);
    if (!parsed.ok) {
        std::fprintf(stderr, "termesh: %s\nTry 'termesh --help'.\n", parsed.error.c_str());
        return 2;
    }
    const Options& options = parsed.options;
    if (options.showHelp) {
        std::fputs(usageText().c_str(), stdout);
        return 0;
    }
    if (options.showVersion) {
        std::printf("termesh %s\n", TERMESH_VERSION);
        return 0;
    }

    Model model;
    if (!options.demo.empty()) {
        model = makeDemoModel(options.demo);
        if (model.meshes.empty()) {
            std::fprintf(stderr, "termesh: unknown demo '%s'\nTry 'termesh --help'.\n", options.demo.c_str());
            return 2;
        }
    } else {
        LoadResult loaded = loadModel(options.modelPath);
        for (const std::string& warning : loaded.warnings)
            std::fprintf(stderr, "termesh: warning: %s\n", warning.c_str());
        if (!loaded.ok) {
            std::fprintf(stderr, "termesh: %s\n", loaded.error.c_str());
            return 1;
        }
        model = std::move(loaded.model);
    }

    if (options.listAnimations) {
        if (!model.animated()) std::printf("%s has no animations\n", model.name.c_str());
        for (std::size_t i = 0; i < model.rig.animations.size(); ++i) {
            const Animation& animation = model.rig.animations[i];
            std::printf("%2zu  %-24s %6.2f s  %zu channels\n", i + 1,
                        animation.name.empty() ? "(unnamed)" : animation.name.c_str(), animation.duration,
                        animation.channels.size());
        }
        return 0;
    }

    ThreadPool pool(static_cast<unsigned>(options.threads));

    // Animated models get a player, positioned where the options ask.
    std::optional<AnimationPlayer> playerStorage;
    AnimationPlayer* player = nullptr;
    if (model.animated()) {
        const int clip = findAnimation(model, options.animation);
        if (clip < 0) {
            std::fprintf(stderr, "termesh: no animation '%s' in %s (try --list-animations)\n",
                         options.animation.c_str(), model.name.c_str());
            return 2;
        }
        player = &playerStorage.emplace(model.rig);
        player->setClip(clip);
        player->setSpeed(options.animationSpeed);
        player->seek(options.animationTime);
        if (options.animationPaused) player->pause();
        applyPose(model, player->pose(), &pool);
    } else if (!options.animation.empty()) {
        std::fprintf(stderr, "termesh: warning: %s has no animations; --animation ignored\n", model.name.c_str());
    }

    if (options.benchFrames > 0) return runBench(options, model, player, pool);
    // --png and --gif can be combined: one run writes both.
    if (!options.pngPath.empty() || !options.gifPath.empty()) {
        int status = 0;
        if (!options.pngPath.empty()) status = runPng(options, model, pool);
        if (status == 0 && !options.gifPath.empty()) status = runGif(options, model, player, pool);
        return status;
    }
    if (options.print) return runPrint(options, model, pool);

    Viewer viewer(options, model, player, pool);
    return viewer.run();
}
