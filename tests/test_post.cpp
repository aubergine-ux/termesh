// Post-processing passes and environment lighting.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <vector>

#include "core/color.hpp"
#include "post/post.hpp"
#include "scene/environment.hpp"
#include "support.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

Camera frontCamera() {
    Camera camera;
    camera.projection = perspective(radians(60.0f), 1.0f, 1.0f, 10.0f);
    return camera;  // at the origin, looking down -z
}

// The depth-buffer value of a point `distance` in front of the camera.
float depthAt(const Camera& camera, float distance) {
    const Vec4 clip = camera.projection * Vec4(0.0f, 0.0f, -distance, 1.0f);
    return clip.z / clip.w * 0.5f + 0.5f;
}

// A frame entirely covered by a wall facing the camera.
HdrFramebuffer wall(int size, const Camera& camera, float distance, Vec3 color) {
    HdrFramebuffer hdr(size, size);
    hdr.clear(Vec4(color, 1.0f));
    const float depth = depthAt(camera, distance);
    for (int y = 0; y < size; ++y) std::fill_n(hdr.depthRow(y), size, depth);
    return hdr;
}

Framebuffer run(HdrFramebuffer& hdr, const PostSettings& settings, const Backdrop& backdrop = {}) {
    ThreadPool pool(2);
    PostProcessor post(pool);
    Framebuffer out;
    post.run(hdr, frontCamera(), settings, backdrop, 1.0f, out);
    return out;
}

}  // namespace

// ------------------------------------------------------------ tone mapping ---

TEST_CASE("tone mapping: every curve is monotonic and stays in range") {
    for (ToneMapper mapper : {ToneMapper::Aces, ToneMapper::Reinhard, ToneMapper::Filmic, ToneMapper::Linear}) {
        CAPTURE(toneMapperName(mapper));
        float previous = -1.0f;
        for (int i = 0; i <= 2000; ++i) {
            const float x = static_cast<float>(i) * 0.01f;  // 0 .. 20
            const float y = toneMap(x, mapper);
            CHECK(y >= previous - 1e-6f);
            CHECK(y >= 0.0f);
            CHECK(y <= 1.0f);
            previous = y;
        }
        CHECK(toneMap(0.0f, mapper) == Approx(0.0f).scale(1.0).epsilon(0.01));
        CHECK(toneMap(-3.0f, mapper) == Approx(toneMap(0.0f, mapper)));  // negatives are black, not NaN
        CHECK(toneMap(20.0f, mapper) > 0.9f);                            // very bright is (nearly) white
    }
    CHECK(toneMap(0.25f, ToneMapper::Linear) == 0.25f);
    CHECK(toneMap(1.0f, ToneMapper::Reinhard) == Approx(0.5f));
}

TEST_CASE("post: presenting a frame applies exposure, the curve and the background") {
    HdrFramebuffer hdr(4, 1);
    hdr.at(0, 0) = Vec4(0.25f, 0.25f, 0.25f, 1.0f);  // geometry
    hdr.at(1, 0) = Vec4(0.0f);                       // nothing
    hdr.at(2, 0) = Vec4(0.1f, 0.0f, 0.0f, 0.5f);     // half-covered by something translucent
    hdr.at(3, 0) = Vec4(5.0f, 5.0f, 5.0f, 1.0f);     // far brighter than white

    PostSettings settings;
    settings.toneMapper = ToneMapper::Linear;
    Backdrop backdrop;
    backdrop.color = {10, 200, 30, 255};

    Framebuffer out = run(hdr, settings, backdrop);
    CHECK(out.at(0, 0) == linearToSrgb(Vec3{0.25f, 0.25f, 0.25f}));
    // Untouched pixels are the background colour exactly, flagged as empty.
    CHECK(out.at(1, 0) == Color8{10, 200, 30, 0});
    // Partial coverage: what was drawn, plus the uncovered share of the background.
    const Vec3 mixed = Vec3{0.1f, 0, 0} + srgbToLinear(backdrop.color) * 0.5f;
    CHECK(out.at(2, 0) == linearToSrgb(mixed));
    CHECK(out.at(3, 0) == Color8{255, 255, 255, 255});

    // One stop of exposure doubles the light.
    settings.exposure = 1.0f;
    out = run(hdr, settings, backdrop);
    CHECK(out.at(0, 0) == linearToSrgb(Vec3{0.5f, 0.5f, 0.5f}));
    CHECK(out.at(1, 0) == Color8{10, 200, 30, 0});  // the background is not light: unaffected

    // ACES leaves headroom: 5.0 is bright but the curve has not clipped mid-tones.
    settings.exposure = 0.0f;
    settings.toneMapper = ToneMapper::Aces;
    out = run(hdr, settings, backdrop);
    CHECK(out.at(0, 0).r < out.at(3, 0).r);
    CHECK(out.at(0, 0).r > 100);
}

TEST_CASE("post: the tile-at-a-time path matches the whole-frame path") {
    HdrFramebuffer hdr(40, 30);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x)
            hdr.at(x, y) = (x + y) % 3 ? Vec4(static_cast<float>(x) * 0.05f, 0.3f, static_cast<float>(y) * 0.1f, 1.0f)
                                       : Vec4(0.0f);
    PostSettings settings;
    CHECK_FALSE(PostProcessor::needsWholeFrame(settings, {}));
    Framebuffer whole = run(hdr, settings);

    Framebuffer tiled(40, 30);
    for (int y = 0; y < 30; y += 7)
        for (int x = 0; x < 40; x += 9)
            PostProcessor::presentRect(hdr, settings, {}, tiled, x, y, std::min(x + 9, 40), std::min(y + 7, 30));
    CHECK(whole.pixels() == tiled.pixels());

    settings.bloom = true;
    CHECK(PostProcessor::needsWholeFrame(settings, {}));
}

// ------------------------------------------------------------------- bloom ---

TEST_CASE("bloom: only light above the threshold glows, and it spreads") {
    const Camera camera = frontCamera();
    PostSettings settings;
    settings.bloom = true;
    settings.toneMapper = ToneMapper::Linear;

    SUBCASE("a frame with nothing bright is left exactly alone") {
        HdrFramebuffer hdr = wall(64, camera, 5.0f, {0.5f, 0.4f, 0.3f});
        const Framebuffer out = run(hdr, settings);
        for (const Color8& p : out.pixels()) REQUIRE(p == linearToSrgb(Vec3{0.5f, 0.4f, 0.3f}));
    }
    SUBCASE("a bright spot lights up its surroundings, fading with distance") {
        HdrFramebuffer hdr = wall(64, camera, 5.0f, {0.0f, 0.0f, 0.0f});
        for (int y = 30; y < 34; ++y)
            for (int x = 30; x < 34; ++x) hdr.at(x, y) = Vec4(400.0f, 200.0f, 0.0f, 1.0f);
        const Framebuffer out = run(hdr, settings);
        const int near = out.at(38, 32).r, mid = out.at(46, 32).r, far = out.at(62, 2).r;
        CHECK(near > 0);
        CHECK(near >= mid);
        CHECK(mid >= far);
        CHECK(near > far);
        // The glow keeps the colour of its source: more red than green, no
        // blue. (Checked where it has faded enough not to be clipped.)
        int compared = 0;
        for (int x = 34; x < 64; ++x) {
            const Color8 p = out.at(x, 32);
            if (p.r == 0 || p.r == 255) continue;
            ++compared;
            CHECK(p.r > p.g);
            CHECK(p.b == 0);
        }
        CHECK(compared > 0);
    }
    SUBCASE("glow spills over the background too") {
        HdrFramebuffer hdr(64, 64);
        hdr.clear(Vec4(0.0f));
        for (int y = 30; y < 34; ++y)
            for (int x = 30; x < 34; ++x) hdr.at(x, y) = Vec4(400.0f, 400.0f, 400.0f, 1.0f);
        Backdrop backdrop;
        backdrop.color = {0, 0, 0, 255};
        const Framebuffer out = run(hdr, settings, backdrop);
        CHECK(out.at(37, 32).r > 0);
        CHECK(out.at(37, 32).a == 0);  // still "nothing drawn here"
    }
}

// -------------------------------------------------------------------- SSAO ---

TEST_CASE("ssao: open surfaces are untouched, the inside of a pit is darkened") {
    const Camera camera = frontCamera();
    PostSettings settings;
    settings.ssao = true;
    settings.toneMapper = ToneMapper::Linear;
    // The radius is a fraction of the depth range (1 .. 10): about half a
    // unit, or five pixels at this distance.
    settings.ssaoRadius = 0.5f / 4.5f;
    const Vec3 grey{0.5f, 0.5f, 0.5f};
    const int flat = linearToSrgb(grey).r;

    SUBCASE("a flat wall") {
        HdrFramebuffer hdr = wall(64, camera, 5.0f, grey);
        const Framebuffer out = run(hdr, settings);
        for (int y = 8; y < 56; ++y)
            for (int x = 8; x < 56; ++x) REQUIRE(std::abs(out.at(x, y).r - flat) <= 2);
    }
    SUBCASE("a square pit in the wall") {
        HdrFramebuffer hdr = wall(64, camera, 5.0f, grey);
        const float pit = depthAt(camera, 5.5f);
        for (int y = 22; y < 42; ++y)
            for (int x = 22; x < 42; ++x) hdr.depthRow(y)[x] = pit;
        const Framebuffer out = run(hdr, settings);

        const int besideWall = out.at(23, 32).r;  // on the pit floor, right next to its wall
        const int pitCentre = out.at(32, 32).r;   // on the floor, far from any wall
        const int openWall = out.at(4, 4).r;      // the surrounding surface, far from the pit
        CHECK(besideWall < flat - 10);
        CHECK(besideWall < pitCentre);
        CHECK(std::abs(pitCentre - flat) <= 4);
        CHECK(std::abs(openWall - flat) <= 2);

        // Strength scales the effect; zero switches it off.
        settings.ssaoStrength = 0.0f;
        HdrFramebuffer again = wall(64, camera, 5.0f, grey);
        for (int y = 22; y < 42; ++y)
            for (int x = 22; x < 42; ++x) again.depthRow(y)[x] = pit;
        CHECK(std::abs(run(again, settings).at(23, 32).r - flat) <= 1);
    }
    SUBCASE("background pixels are neither darkened nor treated as occluders") {
        HdrFramebuffer hdr(64, 64);
        hdr.clear(Vec4(0.0f));  // depth 1: the far plane
        const float depth = depthAt(camera, 5.0f);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x) {
                hdr.at(x, y) = Vec4(grey, 1.0f);
                hdr.depthRow(y)[x] = depth;
            }
        const Framebuffer out = run(hdr, settings);
        CHECK(out.at(40, 32).a == 0);
        CHECK(std::abs(out.at(10, 32).r - flat) <= 2);
    }
}

// ---------------------------------------------------------------- outlines ---

TEST_CASE("outline: lines follow silhouettes, depth steps and creases, and nothing else") {
    const Camera camera = frontCamera();
    PostSettings settings;
    settings.outline = true;
    settings.toneMapper = ToneMapper::Linear;
    settings.outlineColor = {0, 0, 0};
    const Vec3 paint{0.6f, 0.6f, 0.6f};
    const Color8 painted = linearToSrgb(paint);
    auto isInk = [](Color8 c) { return c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255; };

    SUBCASE("silhouette against the background") {
        HdrFramebuffer hdr(64, 64);
        hdr.clear(Vec4(0.0f));
        const float depth = depthAt(camera, 5.0f);
        for (int y = 16; y < 48; ++y)
            for (int x = 16; x < 48; ++x) {
                hdr.at(x, y) = Vec4(paint, 1.0f);
                hdr.depthRow(y)[x] = depth;
            }
        const Framebuffer out = run(hdr, settings);
        CHECK(isInk(out.at(16, 30)));  // the object's outermost pixels
        CHECK(isInk(out.at(47, 30)));
        CHECK(isInk(out.at(30, 16)));
        CHECK(out.at(30, 30) == painted);  // the interior is untouched
        CHECK(out.at(15, 30).a == 0);      // the line is on the object, not the background
        CHECK(out.at(5, 5).a == 0);
    }
    SUBCASE("a step in depth between two surfaces") {
        HdrFramebuffer hdr = wall(64, camera, 5.0f, paint);
        const float nearer = depthAt(camera, 3.0f);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 32; ++x) hdr.depthRow(y)[x] = nearer;
        const Framebuffer out = run(hdr, settings);
        CHECK((isInk(out.at(31, 32)) || isInk(out.at(32, 32))));
        CHECK(out.at(10, 32) == painted);
        CHECK(out.at(50, 32) == painted);
    }
    SUBCASE("a surface receding at a steep angle is not a line") {
        // Depth changes quickly from pixel to pixel but evenly: one plane.
        HdrFramebuffer hdr = wall(64, camera, 5.0f, paint);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                // The plane z = -5 + 1.2 x: tilted about fifty degrees away
                // from facing the camera. Along the ray through this pixel,
                // x = -z * ndcX * tan(fov / 2); solve for the distance.
                const float ndcX = (static_cast<float>(x) + 0.5f) / 32.0f - 1.0f;
                const float tanHalf = std::tan(radians(30.0f));
                const float distance = 5.0f / (1.0f + 1.2f * ndcX * tanHalf);
                hdr.depthRow(y)[x] = depthAt(camera, distance);
            }
        const Framebuffer out = run(hdr, settings);
        int inked = 0;
        for (int y = 4; y < 60; ++y)
            for (int x = 4; x < 60; ++x) inked += isInk(out.at(x, y));
        CHECK(inked == 0);
    }
    SUBCASE("width") {
        HdrFramebuffer hdr(64, 64);
        hdr.clear(Vec4(0.0f));
        const float depth = depthAt(camera, 5.0f);
        for (int y = 16; y < 48; ++y)
            for (int x = 16; x < 48; ++x) {
                hdr.at(x, y) = Vec4(paint, 1.0f);
                hdr.depthRow(y)[x] = depth;
            }
        settings.outlineWidth = 5.0f;
        const Framebuffer out = run(hdr, settings);
        int inked = 0;
        for (int x = 0; x < 64; ++x) inked += isInk(out.at(x, 30));
        CHECK(inked >= 8);  // two lines, each several pixels wide
        CHECK(inked <= 12);
    }
}

// -------------------------------------------------------------------- FXAA ---

TEST_CASE("fxaa: jagged edges are softened, everything else is left alone") {
    ThreadPool pool(2);

    SUBCASE("a flat image and a perfectly straight edge do not change") {
        Image flat(32, 32);
        for (Color8& p : flat.pixels) p = {90, 120, 200, 255};
        const std::vector<Color8> before = flat.pixels;
        applyFxaa(flat, pool);
        CHECK(flat.pixels == before);

        Image straight(32, 32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                straight.row(y)[x] = x < 16 ? Color8{0, 0, 0, 255} : Color8{255, 255, 255, 255};
        const std::vector<Color8> edge = straight.pixels;
        applyFxaa(straight, pool);
        CHECK(straight.pixels == edge);
    }
    SUBCASE("a staircase gets in-between values along it") {
        Image stairs(48, 48);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 48; ++x)
                // A shallow diagonal: the edge steps one pixel every three.
                stairs.row(y)[x] = x < 12 + y / 3 ? Color8{0, 0, 0, 200} : Color8{255, 255, 255, 200};
        applyFxaa(stairs, pool);
        int blended = 0, wrongAlpha = 0;
        for (const Color8& p : stairs.pixels) {
            blended += p.r > 10 && p.r < 245;
            wrongAlpha += p.a != 200;
        }
        CHECK(blended > 20);
        CHECK(wrongAlpha == 0);  // coverage is not a colour: it passes through
        // Far from the edge nothing moved.
        CHECK(stairs.row(24)[2] == Color8{0, 0, 0, 200});
        CHECK(stairs.row(24)[45] == Color8{255, 255, 255, 200});
    }
    SUBCASE("tiny images are ignored rather than read out of bounds") {
        Image tiny(2, 2);
        applyFxaa(tiny, pool);
        Image empty;
        applyFxaa(empty, pool);
    }
}

// ------------------------------------------------------------- environment ---

namespace {

// A panorama that is one colour above the horizon and another below.
Environment twoTone(Vec3 above, Vec3 below, int width = 64, int height = 32) {
    std::vector<Vec3> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[static_cast<std::size_t>(y) * width + x] = y < height / 2 ? above : below;
    return Environment::fromEquirect(width, height, std::move(pixels));
}

}  // namespace

TEST_CASE("environment: the default is a two-colour hemisphere") {
    const Environment env = Environment::hemisphere({1, 1, 1}, {0, 0, 0});
    CHECK_FALSE(env.hasMap());
    CHECK(env.irradiance({0, 1, 0}).x == Approx(1.0f));
    CHECK(env.irradiance({0, -1, 0}).x == Approx(0.0f));
    CHECK(env.irradiance({1, 0, 0}).x == Approx(0.5f));
    CHECK(env.specular({0, 1, 0}, 0.3f).x == Approx(1.0f));
    CHECK(env.background({0, -1, 0}, 0.0f).x == Approx(0.0f));

    Environment bright = env;
    bright.intensity = 3.0f;
    CHECK(bright.irradiance({0, 1, 0}).x == Approx(3.0f));
}

TEST_CASE("environment: a uniform panorama lights every surface equally") {
    // The integral of the cosine over a hemisphere is pi; irradiance() has
    // that divided out, so a uniformly white sky gives exactly white.
    const Environment env = twoTone({0.8f, 0.6f, 0.4f}, {0.8f, 0.6f, 0.4f});
    REQUIRE(env.hasMap());
    for (const Vec3 n : {Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}, normalize(Vec3{1, 2, -3})}) {
        const Vec3 e = env.irradiance(n);
        CHECK(e.x == Approx(0.8f).epsilon(0.02));
        CHECK(e.y == Approx(0.6f).epsilon(0.02));
        CHECK(e.z == Approx(0.4f).epsilon(0.02));
    }
    CHECK(env.specular({0, 0, 1}, 0.0f).x == Approx(0.8f));
    CHECK(env.specular({0, 0, 1}, 1.0f).x == Approx(0.8f));
}

TEST_CASE("environment: light comes from where the panorama is bright") {
    const Environment env = twoTone({1, 1, 1}, {0, 0, 0});

    // Diffuse: facing the sky gets all of it, facing away none, sideways half.
    CHECK(env.irradiance({0, 1, 0}).x == Approx(1.0f).epsilon(0.05));
    CHECK(env.irradiance({0, -1, 0}).x == Approx(0.0f).scale(1.0).epsilon(0.05));
    CHECK(env.irradiance({1, 0, 0}).x == Approx(0.5f).epsilon(0.1));

    // Mirror reflections see the panorama itself...
    CHECK(env.specular({0, 1, 0}, 0.0f).x == Approx(1.0f));
    CHECK(env.specular({0, -1, 0}, 0.0f).x == Approx(0.0f));
    CHECK(env.background({0.3f, 0.9f, 0.1f}, 0.0f).x == Approx(1.0f));
    // ...and rough ones a blurred version of it: looking at the horizon,
    // a mirror sees a hard edge, a rough surface an even mix.
    const float rough = env.specular(normalize(Vec3{1, 0.15f, 0}), 1.0f).x;
    CHECK(rough > 0.2f);
    CHECK(rough < 0.8f);
    CHECK(env.specular(normalize(Vec3{1, 0.15f, 0}), 0.0f).x == Approx(1.0f));
}

TEST_CASE("environment: rotation turns the panorama about the vertical axis") {
    // Bright only in the +x direction.
    const int width = 64, height = 32;
    std::vector<Vec3> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / width;
            pixels[static_cast<std::size_t>(y) * width + x] =
                std::abs(u - 0.5f) < 0.1f ? Vec3{10, 10, 10} : Vec3{0, 0, 0};
        }
    Environment env = Environment::fromEquirect(width, height, std::move(pixels));
    CHECK(env.background({1, 0, 0}, 0.0f).x > 5.0f);
    CHECK(env.background({0, 0, 1}, 0.0f).x == Approx(0.0f));
    CHECK(env.irradiance({1, 0, 0}).x > env.irradiance({-1, 0, 0}).x);

    env.rotation = radians(90.0f);  // +x now shows what was... a quarter turn away
    CHECK(env.background({1, 0, 0}, 0.0f).x == Approx(0.0f));
    const float alongZ = std::max(env.background({0, 0, 1}, 0.0f).x, env.background({0, 0, -1}, 0.0f).x);
    CHECK(alongZ > 5.0f);
    const float irradianceZ = std::max(env.irradiance({0, 0, 1}).x, env.irradiance({0, 0, -1}).x);
    CHECK(irradianceZ > env.irradiance({1, 0, 0}).x);
}

TEST_CASE("environment: built-ins, files and bad input") {
    for (const std::string& name : builtinEnvironmentNames()) {
        Environment env;
        REQUIRE(makeBuiltinEnvironment(name, env));
        CHECK(env.name == name);
        // Every built-in is brighter above than below.
        CHECK(env.irradiance({0, 1, 0}).y > env.irradiance({0, -1, 0}).y);
    }
    Environment sky;
    REQUIRE(makeBuiltinEnvironment("sky", sky));
    CHECK(sky.hasMap());
    // The sun is far brighter than white: that is what makes it HDR.
    CHECK(sky.background(normalize(Vec3{-0.45f, 0.75f, 0.5f}), 0.0f).x > 5.0f);
    CHECK_FALSE(makeBuiltinEnvironment("nowhere", sky));

    // A Radiance .hdr file: 64 x 32 pixels of uncompressed RGBE, every one
    // (2, 1, 0): mantissas 128, 64, 0 with exponent 130 (x 4).
    TempDir dir;
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 32 +X 64\n";
    const std::string path = dir.file("tiny.hdr");
    {
        std::ofstream file(path, std::ios::binary);
        file << header;
        const unsigned char rgbe[4] = {128, 64, 0, 130};
        for (int i = 0; i < 64 * 32; ++i) file.write(reinterpret_cast<const char*>(rgbe), sizeof(rgbe));
    }
    Environment loaded;
    std::string error;
    REQUIRE_MESSAGE(loadEnvironment(path, loaded, error), error);
    CHECK(loaded.hasMap());
    CHECK(loaded.name == "tiny.hdr");
    // Values above 1 survive the trip.
    CHECK(loaded.irradiance({0, 1, 0}).x == Approx(2.0f).epsilon(0.03));
    CHECK(loaded.irradiance({0, 1, 0}).y == Approx(1.0f).epsilon(0.03));
    CHECK(loaded.background({1, 0, 0}, 0.0f).z < 0.02f);

    CHECK_FALSE(loadEnvironment(dir.file("missing.hdr"), loaded, error));
    CHECK(error.find("missing.hdr") != std::string::npos);
    CHECK_FALSE(loadEnvironment(dir.write("garbage.hdr", "this is not an image"), loaded, error));

    // Degenerate input gives the default rather than a broken map.
    CHECK_FALSE(Environment::fromEquirect(0, 0, {}).hasMap());
    CHECK_FALSE(Environment::fromEquirect(4, 4, std::vector<Vec3>(3)).hasMap());
    // Non-finite and negative texels are cleaned up on the way in.
    std::vector<Vec3> bad(64 * 32, Vec3{1, 1, 1});
    bad[5] = {-4.0f, std::nanf(""), 1e30f};
    const Environment cleaned = Environment::fromEquirect(64, 32, std::move(bad));
    const Vec3 e = cleaned.irradiance({0, 1, 0});
    CHECK(std::isfinite(e.x));
    CHECK(std::isfinite(e.y));
    CHECK(std::isfinite(e.z));
    CHECK(e.y >= 0.0f);
}
