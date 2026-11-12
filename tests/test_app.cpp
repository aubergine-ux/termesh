#include <doctest/doctest.h>

#include <initializer_list>
#include <vector>

#include "app/capabilities.hpp"
#include "app/options.hpp"
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

TEST_CASE("options: turntable recording") {
    const ParseResult r = parse({"--spin", "--gif", "out.gif", "--frames", "36", "--demo", "knot"});
    REQUIRE(r.ok);
    CHECK(r.options.spin);
    CHECK(r.options.gifPath == "out.gif");
    CHECK(r.options.frames == 36);
    CHECK(r.options.demo == "knot");
}

TEST_CASE("options: errors are reported, not ignored") {
    CHECK_FALSE(parse({}).ok);                                   // no model
    CHECK_FALSE(parse({"a.obj", "b.obj"}).ok);                   // two models
    CHECK_FALSE(parse({"a.obj", "--demo", "knot"}).ok);          // model and demo
    CHECK_FALSE(parse({"a.obj", "--shading", "cel"}).ok);        // bad enum value
    CHECK_FALSE(parse({"a.obj", "--frames"}).ok);                // missing value
    CHECK_FALSE(parse({"a.obj", "--frames", "many"}).ok);        // not a number
    CHECK_FALSE(parse({"a.obj", "--ssaa", "9"}).ok);             // out of range
    CHECK_FALSE(parse({"a.obj", "--size", "640"}).ok);           // malformed size
    CHECK_FALSE(parse({"a.obj", "--frobnicate"}).ok);            // unknown option
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
    for (BackendKind kind : {BackendKind::Kitty, BackendKind::Sixel, BackendKind::HalfBlock, BackendKind::Braille,
                             BackendKind::Ascii})
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
        for (int x = 0; x < blocky.width; ++x)
            flat = flat && blocky.row(y)[x] == blocky.row(y / 8 * 8)[x / 8 * 8];
    CHECK(flat);
}
