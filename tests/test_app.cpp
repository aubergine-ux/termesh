#include <doctest/doctest.h>

#include <algorithm>

#include <initializer_list>
#include <vector>

#include "app/capabilities.hpp"
#include "app/benchmark.hpp"
#include "app/options.hpp"
#include "app/preview.hpp"
#include "support.hpp"
#include "export/offscreen.hpp"
#include "scene/primitives.hpp"

using namespace termesh;

namespace {

ParseResult parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv = {"termesh"};
    argv.insert(argv.end(), args.begin(), args.end());
    return parseOptions(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("options: defaults") {
    const ParseResult r = parse({"model.glb"});
    REQUIRE(r.ok);
    CHECK(r.options.modelPath == "model.glb");
    CHECK(r.options.shading == ShadingMode::Phong);
    CHECK(r.options.spin);
    CHECK(r.options.textures);
    CHECK_FALSE(r.options.wireframe);
    CHECK_FALSE(r.options.backend.has_value());
}

TEST_CASE("options: both value syntaxes") {
    const ParseResult a = parse({"--shading", "flat", "--backend=braille", "-w", "m.obj"});
    REQUIRE(a.ok);
    CHECK(a.options.shading == ShadingMode::Flat);
    CHECK(a.options.backend == BackendKind::Braille);
    CHECK(a.options.wireframe);

    const ParseResult b = parse({"m.obj", "--size=320x200", "--yaw", "-45", "--bg", "#102030", "--color", "256"});
    REQUIRE(b.ok);
    CHECK(b.options.width == 320);
    CHECK(b.options.height == 200);
    CHECK(*b.options.yaw == -45.0f);  // a negative number is a value, not an option
    CHECK(b.options.background == Color8{0x10, 0x20, 0x30, 255});
    CHECK(b.options.color == ColorDepth::Indexed256);
}

TEST_CASE("options: plugins, recording and the physics demo") {
    const ParseResult a =
        parse({"--plugin", "a.so", "--plugin=b.so", "--no-plugins", "--backend", "quadrant", "m.obj"});
    REQUIRE(a.ok);
    CHECK(a.options.plugins == std::vector<std::string>{"a.so", "b.so"});
    CHECK_FALSE(a.options.autoPlugins);
    // A name that is not built in is taken on trust until the plugins are loaded.
    CHECK(a.options.backend == BackendKind::Custom);
    CHECK(a.options.customBackend == "quadrant");
    // A later built-in choice, or "auto", replaces it.
    CHECK(parse({"--backend", "quadrant", "--backend", "ascii", "m.obj"}).options.customBackend.empty());
    const ParseResult automatic = parse({"--backend", "quadrant", "--backend", "auto", "m.obj"});
    CHECK_FALSE(automatic.options.backend.has_value());
    CHECK(automatic.options.customBackend.empty());

    // --list-plugins needs no model.
    const ParseResult list = parse({"--list-plugins"});
    REQUIRE(list.ok);
    CHECK(list.options.listPlugins);
    CHECK(parse({"m.obj"}).options.autoPlugins);

    const ParseResult record = parse({"m.obj", "--record", "session.cast", "--mp4", "turn.mp4"});
    REQUIRE(record.ok);
    CHECK(record.options.recordPath == "session.cast");
    CHECK(record.options.mp4Path == "turn.mp4");
    CHECK_FALSE(parse({"m.obj", "--record", "session.txt"}).ok);
    CHECK_FALSE(parse({"m.obj", "--record"}).ok);

    const ParseResult physics = parse({"--demo", "physics", "--bodies", "40", "--no-shadows"});
    REQUIRE(physics.ok);
    CHECK(physics.options.demo == "physics");
    CHECK(physics.options.bodies == 40);
    CHECK_FALSE(parse({"--demo", "physics", "--bodies", "0"}).ok);
    CHECK_FALSE(parse({"--demo", "physics", "--bodies", "9999"}).ok);
}

TEST_CASE("options: turntable recording") {
    const ParseResult r = parse({"--spin", "--gif", "out.gif", "--frames", "36", "--demo", "knot"});
    REQUIRE(r.ok);
    CHECK(r.options.spin);
    CHECK(r.options.gifPath == "out.gif");
    CHECK(r.options.frames == 36);
    CHECK(r.options.demo == "knot");
}

TEST_CASE("options: errors are reported, not ignored") {
    CHECK_FALSE(parse({}).ok);                             // no model
    CHECK(parse({"a.obj", "b.obj"}).ok);                   // several models are shown side by side
    CHECK_FALSE(parse({"a.obj", "--demo", "knot"}).ok);    // model and demo
    CHECK_FALSE(parse({"a.obj", "--shading", "cel"}).ok);  // bad enum value
    CHECK_FALSE(parse({"a.obj", "--frames"}).ok);          // missing value
    CHECK_FALSE(parse({"a.obj", "--frames", "many"}).ok);  // not a number
    CHECK_FALSE(parse({"a.obj", "--ssaa", "9"}).ok);       // out of range
    CHECK_FALSE(parse({"a.obj", "--size", "640"}).ok);     // malformed size
    CHECK_FALSE(parse({"a.obj", "--frobnicate"}).ok);      // unknown option
    CHECK_FALSE(parse({"a.obj", "--zoom", "nan"}).ok);     // not a finite number
    CHECK_FALSE(parse({"a.obj", "--yaw", "inf"}).ok);
    CHECK_FALSE(parse({"a.obj", "--png", ""}).ok);  // empty file name
    CHECK(parse({"a.obj", "--frobnicate"}).error.find("--frobnicate") != std::string::npos);
    // --help and --version need no model.
    CHECK(parse({"--help"}).ok);
    CHECK(parse({"-V"}).ok);
}

TEST_CASE("options: camera options shape the initial view") {
    const Model model = makeDemoModel("cube");
    const ParseResult base = parse({"--demo", "cube"});
    const ParseResult custom = parse({"--demo", "cube", "--yaw", "90", "--pitch", "30", "--zoom", "2"});
    REQUIRE(base.ok);
    REQUIRE(custom.ok);
    const OrbitCamera a = makeCamera(base.options, model);
    const OrbitCamera b = makeCamera(custom.options, model);
    CHECK(b.yaw == doctest::Approx(radians(90.0f)));
    CHECK(b.pitch == doctest::Approx(radians(30.0f)));
    CHECK(b.distance == doctest::Approx(a.distance / 2.0f));
}

TEST_CASE("capabilities: backend preference order") {
    TerminalCaps caps;
    caps.unicode = true;
    caps.color = ColorDepth::TrueColor;
    CHECK(chooseBackend(caps) == BackendKind::HalfBlock);

    caps.sixel = true;
    CHECK(chooseBackend(caps) == BackendKind::Sixel);
    caps.kittyGraphics = true;
    CHECK(chooseBackend(caps) == BackendKind::Kitty);

    TerminalCaps plain;
    plain.unicode = false;
    CHECK(chooseBackend(plain) == BackendKind::Ascii);
    plain.unicode = true;
    plain.color = ColorDepth::None;
    CHECK(chooseBackend(plain) == BackendKind::Ascii);  // half-blocks need colour
    plain.color = ColorDepth::Indexed256;
    CHECK(chooseBackend(plain) == BackendKind::HalfBlock);
}

TEST_CASE("capabilities: cell size sources") {
    TerminalCaps caps;
    TerminalSize size;
    size.cols = 100;
    size.rows = 40;

    resolveCellSize(size, caps);  // nothing known: assume a 1:2 cell
    CHECK(caps.cellWidth * 2 == caps.cellHeight);

    size.pixelWidth = 900;
    size.pixelHeight = 720;
    resolveCellSize(size, caps);
    CHECK(caps.cellWidth == 9);
    CHECK(caps.cellHeight == 18);
}

TEST_CASE("backend names round-trip") {
    for (BackendKind kind :
         {BackendKind::Kitty, BackendKind::Sixel, BackendKind::HalfBlock, BackendKind::Braille, BackendKind::Ascii})
        CHECK(backendFromName(backendName(kind)) == kind);
    CHECK_FALSE(backendFromName("opengl").has_value());
}

TEST_CASE("offscreen: image size, opacity and block upscaling") {
    const Model model = makeDemoModel("cube");
    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera camera;
    camera.frame(model.bounds());

    OffscreenTarget target;
    target.width = 96;
    target.height = 64;
    const Image smooth = renderOffscreen(renderer, pool, model, camera, {}, target);
    CHECK(smooth.width == 96);
    CHECK(smooth.height == 64);
    bool opaque = true;
    for (const Color8& p : smooth.pixels) opaque = opaque && p.a == 255;
    CHECK(opaque);
    // Something other than background ended up in the middle.
    const RenderSettings defaults;
    CHECK_FALSE((smooth.row(32)[48].r == defaults.background.r && smooth.row(32)[48].g == defaults.background.g));

    target.blocks = 8;
    const Image blocky = renderOffscreen(renderer, pool, model, camera, {}, target);
    CHECK(blocky.width == 96);
    CHECK(blocky.height == 64);
    // Every 8x8 block is a single flat colour.
    bool flat = true;
    for (int y = 0; y < blocky.height; ++y)
        for (int x = 0; x < blocky.width; ++x) flat = flat && blocky.row(y)[x] == blocky.row(y / 8 * 8)[x / 8 * 8];
    CHECK(flat);
}

TEST_CASE("options: animation controls") {
    const ParseResult r = parse({"fox.glb", "--animation", "Run", "--anim-time=0.5", "--anim-speed", "-2", "--paused"});
    REQUIRE(r.ok);
    CHECK(r.options.animation == "Run");
    CHECK(r.options.animationTime == doctest::Approx(0.5f));
    CHECK(r.options.animationSpeed == doctest::Approx(-2.0f));
    CHECK(r.options.animationPaused);
    CHECK_FALSE(r.options.spinExplicit);

    CHECK(parse({"fox.glb", "--list-animations"}).options.listAnimations);
    CHECK(parse({"fox.glb", "--spin"}).options.spinExplicit);
    // No --frames: the GIF length is chosen later, from the clip.
    CHECK(parse({"fox.glb"}).options.frames == 0);

    CHECK_FALSE(parse({"fox.glb", "--animation"}).ok);
    CHECK_FALSE(parse({"fox.glb", "--animation", ""}).ok);
    CHECK_FALSE(parse({"fox.glb", "--anim-time", "-1"}).ok);
    CHECK_FALSE(parse({"fox.glb", "--anim-time", "soon"}).ok);
    CHECK_FALSE(parse({"fox.glb", "--anim-speed", "nan"}).ok);
    CHECK_FALSE(parse({"fox.glb", "--anim-speed", "1000"}).ok);
}

TEST_CASE("options: lighting and effects") {
    const ParseResult r = parse({"m.glb",
                                 "-s",
                                 "pbr",
                                 "--shadows",
                                 "--shadow-size",
                                 "512",
                                 "--shadow-softness=5",
                                 "--env",
                                 "sky",
                                 "--sky-blur",
                                 "0.5",
                                 "--env-intensity",
                                 "2",
                                 "--env-rotation",
                                 "90",
                                 "--ssao",
                                 "--bloom",
                                 "--fxaa",
                                 "--tonemap",
                                 "filmic",
                                 "--exposure",
                                 "-1.5",
                                 "--light-angle",
                                 "45",
                                 "--dither",
                                 "fs"});
    REQUIRE_MESSAGE(r.ok, r.error);
    const Options& o = r.options;
    CHECK(o.shading == ShadingMode::Pbr);
    CHECK(o.shadows);
    CHECK(o.shadowMapSize == 512);
    CHECK(o.shadowSoftness == 5);
    CHECK(o.environment == "sky");
    CHECK(o.dither == DitherMode::FloydSteinberg);
    CHECK(o.lightAngle == doctest::Approx(45.0f));

    const RenderSettings settings = makeRenderSettings(o);
    CHECK(settings.shading == ShadingMode::Pbr);
    CHECK(settings.shadows);
    CHECK(settings.shadowMapSize == 512);
    CHECK(settings.skyboxBlur == doctest::Approx(0.5f));
    CHECK(settings.post.ssao);
    CHECK(settings.post.bloom);
    CHECK(settings.post.fxaa);
    CHECK_FALSE(settings.post.outline);
    CHECK(settings.post.toneMapper == ToneMapper::Filmic);
    CHECK(settings.post.exposure == doctest::Approx(-1.5f));

    Environment environment;
    std::string error;
    REQUIRE(loadEnvironmentOption(o, environment, error));
    CHECK(environment.name == "sky");
    CHECK(environment.intensity == doctest::Approx(2.0f));
    CHECK(environment.rotation == doctest::Approx(radians(90.0f)));
    // A panorama is shown as the background unless told otherwise.
    CHECK(wantsSkybox(o, environment));
    CHECK_FALSE(wantsSkybox(parse({"m.glb", "--env", "sky", "--no-skybox"}).options, environment));
    CHECK_FALSE(wantsSkybox(parse({"m.glb"}).options, Environment()));
    CHECK(wantsSkybox(parse({"m.glb", "--skybox"}).options, Environment()));

    // Toon shading brings outlines with it, unless they are declined.
    CHECK(makeRenderSettings(parse({"m.glb", "-s", "toon"}).options).post.outline);
    CHECK_FALSE(makeRenderSettings(parse({"m.glb", "-s", "toon", "--no-outline"}).options).post.outline);
    CHECK(makeRenderSettings(parse({"m.glb", "--outline"}).options).post.outline);

    Options missing = o;
    missing.environment = "/no/such/file.hdr";
    CHECK_FALSE(loadEnvironmentOption(missing, environment, error));
    CHECK_FALSE(error.empty());

    CHECK_FALSE(parse({"m.glb", "--shadow-size", "8"}).ok);
    CHECK_FALSE(parse({"m.glb", "--shadow-softness", "0"}).ok);
    CHECK_FALSE(parse({"m.glb", "--sky-blur", "2"}).ok);
    CHECK_FALSE(parse({"m.glb", "--tonemap", "sepia"}).ok);
    CHECK_FALSE(parse({"m.glb", "--exposure", "lots"}).ok);
    CHECK_FALSE(parse({"m.glb", "--dither", "sparkly"}).ok);
    CHECK_FALSE(parse({"m.glb", "--env"}).ok);
    CHECK_FALSE(parse({"m.glb", "--env-intensity", "-1"}).ok);
}

TEST_CASE("options: level of detail and the target frame rate") {
    // Unset means "the default for the mode": on in the viewer, off for exports.
    CHECK_FALSE(parse({"m.glb"}).options.lodTolerance.has_value());
    CHECK(parse({"m.glb"}).options.targetFps == 30);

    const ParseResult r = parse({"m.glb", "--lod", "1.5", "--target-fps", "45"});
    REQUIRE(r.ok);
    CHECK(*r.options.lodTolerance == doctest::Approx(1.5f));
    CHECK(r.options.targetFps == 45);
    CHECK(*parse({"m.glb", "--lod", "off"}).options.lodTolerance == 0.0f);
    CHECK(*parse({"m.glb", "--lod=0"}).options.lodTolerance == 0.0f);
    CHECK(parse({"m.glb", "--target-fps", "0"}).options.targetFps == 0);

    CHECK_FALSE(parse({"m.glb", "--lod", "-1"}).ok);
    CHECK_FALSE(parse({"m.glb", "--lod", "lots"}).ok);
    CHECK_FALSE(parse({"m.glb", "--lod"}).ok);
    CHECK_FALSE(parse({"m.glb", "--target-fps", "-5"}).ok);
}

TEST_CASE("benchmark: the suite does the same work every time") {
    const std::vector<std::string> names = benchmarkSceneNames();
    REQUIRE(names.size() >= 10);
    ThreadPool one(1), many(5);
    BenchmarkResult result;
    CHECK_FALSE(runBenchmarkScene("no-such-scene", 2, one, result));

    std::vector<BenchmarkResult> results;
    for (const std::string& name : names) {
        BenchmarkResult a, b;
        REQUIRE(runBenchmarkScene(name, 2, one, a));
        REQUIRE(runBenchmarkScene(name, 2, many, b));
        CAPTURE(name);
        CHECK(a.name == name);
        CHECK_FALSE(a.description.empty());
        CHECK(a.frames == 2);
        // Something was drawn...
        CHECK(a.triangles > 0);
        CHECK(a.pixels > 0);
        CHECK(a.minMs <= a.medianMs);
        // ...and exactly the same thing whatever the thread count.
        CHECK(a.triangles == b.triangles);
        CHECK(a.pixels == b.pixels);
        CHECK(a.bytes == b.bytes);
        CHECK(a.checksum == b.checksum);
        // The scenes that encode for a terminal say how much they wrote.
        CHECK((a.bytes > 0) == (name.rfind("present-", 0) == 0));
        results.push_back(a);
    }
    // Different scenes are different work.
    CHECK(results[0].checksum != results[1].checksum);

    // A frame count below one still runs one frame.
    REQUIRE(runBenchmarkScene(names[0], 0, one, result));
    CHECK(result.frames == 1);

    // Every scene appears in both reports.
    const std::string table = benchmarkTable(results), json = benchmarkJson(results, 1);
    for (const std::string& name : names) {
        CHECK(table.find(name) != std::string::npos);
        CHECK(json.find("\"name\": \"" + name + "\"") != std::string::npos);
    }
    CHECK(json.find("\"threads\": 1") != std::string::npos);
    CHECK(std::count(json.begin(), json.end(), '{') == std::count(json.begin(), json.end(), '}'));
}

TEST_CASE("options: the benchmark suite needs no model") {
    CHECK(parse({"--benchmark"}).ok);
    CHECK(parse({"--benchmark"}).options.benchmark);
    CHECK(parse({"--benchmark-list"}).options.benchmarkList);
    const ParseResult r = parse({"--benchmark-scene", "knot-phong", "--benchmark-json", "out.json", "--frames", "8"});
    REQUIRE(r.ok);
    CHECK(r.options.benchmark);
    CHECK(r.options.benchmarkScene == "knot-phong");
    CHECK(r.options.benchmarkJsonPath == "out.json");
    CHECK(r.options.frames == 8);
    CHECK_FALSE(parse({"--benchmark-scene"}).ok);
    CHECK_FALSE(parse({"--benchmark-json", ""}).ok);
}

TEST_CASE("preview cache: keyed by the file, its contents and the request") {
    TempDir dir;
    const std::string model = dir.write("model.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const std::string key = previewCacheKey(model, "80x24 halfblock");
    REQUIRE(key.size() == 16);
    CHECK(key.find_first_not_of("0123456789abcdef") == std::string::npos);

    // The same request gives the same key; a different one does not.
    CHECK(previewCacheKey(model, "80x24 halfblock") == key);
    CHECK(previewCacheKey(model, "60x24 halfblock") != key);
    CHECK(previewCacheKey(dir.write("other.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"), "80x24 halfblock") != key);
    // Changing the file changes the key: its old previews are never shown again.
    dir.write("model.obj", "v 0 0 0\nv 2 0 0\nv 0 2 0\nv 0 0 2\nf 1 2 3\n");
    CHECK(previewCacheKey(model, "80x24 halfblock") != key);
    // A file that is not there has no key, and so is never cached.
    CHECK(previewCacheKey(dir.file("missing.obj"), "80x24").empty());

    CHECK(fileSize(model) == std::string("v 0 0 0\nv 2 0 0\nv 0 2 0\nv 0 0 2\nf 1 2 3\n").size());
    CHECK(fileSize(dir.file("missing.obj")) == 0);
}

TEST_CASE("preview cache: storing, fetching and pruning") {
    TempDir dir;
    const std::string cache = dir.file("cache/previews");  // does not exist yet
    std::string out;
    CHECK_FALSE(readPreviewCache(cache, "0123456789abcdef", out));

    const std::string picture =
        std::string("\x1b[38;2;1;2;3m") + "\xe2\x96\x80" + std::string(1, '\0') + "\n";  // arbitrary bytes
    writePreviewCache(cache, "0123456789abcdef", picture);
    REQUIRE(readPreviewCache(cache, "0123456789abcdef", out));
    CHECK(out == picture);
    CHECK_FALSE(readPreviewCache(cache, "fedcba9876543210", out));

    // Nothing to store, nowhere to store it, or no name for it: quietly nothing.
    writePreviewCache(cache, "aaaaaaaaaaaaaaaa", "");
    CHECK_FALSE(readPreviewCache(cache, "aaaaaaaaaaaaaaaa", out));
    writePreviewCache("", "aaaaaaaaaaaaaaaa", "x");
    writePreviewCache(cache, "", "x");
    CHECK_FALSE(readPreviewCache("", "aaaaaaaaaaaaaaaa", out));
    CHECK_FALSE(readPreviewCache(cache, "", out));

    // Past the limit, the cache is cut back rather than left to grow.
    for (int i = 0; i < 25; ++i) writePreviewCache(cache, "entry" + std::to_string(i), "data", 10);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache)) files += entry.is_regular_file();
    CHECK(files <= 12);
    CHECK(files >= 3);
    // The newest entry survives.
    CHECK(readPreviewCache(cache, "entry24", out));
}

TEST_CASE("options: preview mode") {
    const ParseResult r =
        parse({"model.glb", "--preview", "--cols", "40", "--rows", "12", "--no-cache", "--preview-max-mb", "64"});
    REQUIRE(r.ok);
    CHECK(r.options.preview);
    CHECK_FALSE(r.options.previewCache);
    CHECK(r.options.previewMaxMb == 64);
    CHECK(r.options.cols == 40);
    CHECK(parse({"model.glb", "--preview"}).options.previewCache);
    CHECK_FALSE(parse({"model.glb", "--preview-max-mb", "0"}).ok);
}
