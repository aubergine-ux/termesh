// Lighting, shadows, blending and the renderer's draw list.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/color.hpp"
#include "pipeline/renderer.hpp"
#include "pipeline/shading.hpp"
#include "scene/primitives.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

// A rectangle spanned by du and dv from `origin`, facing cross(du, dv).
Model makeQuad(Vec3 origin, Vec3 du, Vec3 dv, Material material = {}) {
    Model model;
    Mesh mesh;
    const Vec3 n = normalize(cross(du, dv));
    for (const Vec3 p : {origin, origin + du, origin + du + dv, origin + dv}) {
        Vertex v;
        v.position = p;
        v.normal = n;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    model.meshes.push_back(mesh);
    model.materials.push_back(material);
    return model;
}

// A material whose colour does not depend on the lighting at all.
Material glowing(Vec3 color, float alpha = 1.0f) {
    Material m;
    m.baseColor = {0.0f, 0.0f, 0.0f, alpha};
    m.emissive = color;
    m.doubleSided = true;
    if (alpha < 1.0f) m.alphaMode = AlphaMode::Blend;
    return m;
}

Camera lookFrom(Vec3 eye, Vec3 target, Vec3 up = {0.0f, 1.0f, 0.0f}) {
    Camera camera;
    camera.position = eye;
    camera.view = lookAt(eye, target, up);
    camera.projection = perspective(radians(40.0f), 1.0f, 1.0f, 30.0f);
    return camera;
}

// The framebuffer pixel a world-space point projects to.
Color8 pixelAt(const Framebuffer& fb, const Camera& camera, Vec3 world) {
    const Vec4 clip = camera.projection * camera.view * Vec4(world, 1.0f);
    const int x = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(fb.width()));
    const int y = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * static_cast<float>(fb.height()));
    return fb.at(x, y);
}

int maxDifference(const Framebuffer& a, const Framebuffer& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        const Color8 p = a.pixels()[i], q = b.pixels()[i];
        worst = std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b), std::abs(p.a - q.a)});
    }
    return worst;
}

const Environment kDark = Environment::hemisphere({0, 0, 0}, {0, 0, 0});

Light sun(Vec3 direction) {
    Light light;
    light.direction = direction;
    light.castsShadow = true;
    return light;
}

}  // namespace

// ------------------------------------------------------------------ lights ---

TEST_CASE("lights: a directional light is the same everywhere") {
    Light light;
    light.direction = {0, -1, 0};
    light.color = {1, 0.5f, 0.25f};
    light.intensity = 2.0f;
    const ShadingLight prepared = prepareLight(light, nullptr);

    for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{100, -50, 3}}) {
        Incoming in;
        float nDotL = 0.0f;
        REQUIRE(incomingLight(prepared, p, {0, 1, 0}, true, in, nDotL));
        CHECK(in.direction == Vec3{0, 1, 0});
        CHECK(in.radiance.x == Approx(2.0f));
        CHECK(in.radiance.z == Approx(0.5f));
        CHECK(nDotL == Approx(1.0f));
    }
    // A surface facing away receives nothing.
    Incoming in;
    float nDotL = 0.0f;
    CHECK_FALSE(incomingLight(prepared, {0, 0, 0}, {0, -1, 0}, true, in, nDotL));
}

TEST_CASE("lights: point lights fall off with the square of the distance") {
    Light light;
    light.type = LightType::Point;
    light.position = {0, 4, 0};
    const ShadingLight prepared = prepareLight(light, nullptr);

    Incoming near, far;
    float nDotL = 0.0f;
    REQUIRE(incomingLight(prepared, {0, 3, 0}, {0, 1, 0}, true, near, nDotL));  // 1 away
    REQUIRE(incomingLight(prepared, {0, 2, 0}, {0, 1, 0}, true, far, nDotL));   // 2 away
    CHECK(near.radiance.x == Approx(1.0f));
    CHECK(far.radiance.x == Approx(0.25f));
    CHECK(far.direction.y == Approx(1.0f));

    // With a range, the light is gone at that distance and dimmer before it.
    light.range = 3.0f;
    const ShadingLight ranged = prepareLight(light, nullptr);
    Incoming in;
    REQUIRE(incomingLight(ranged, {0, 2, 0}, {0, 1, 0}, true, in, nDotL));
    CHECK(in.radiance.x < 0.25f);
    CHECK(in.radiance.x > 0.0f);
    CHECK_FALSE(incomingLight(ranged, {0, 0.5f, 0}, {0, 1, 0}, true, in, nDotL));  // 3.5 away
}

TEST_CASE("lights: a spot light is confined to its cone, with a soft edge") {
    Light light;
    light.type = LightType::Spot;
    light.position = {0, 10, 0};
    light.direction = {0, -1, 0};
    light.innerCone = radians(20.0f);
    light.outerCone = radians(30.0f);
    const ShadingLight prepared = prepareLight(light, nullptr);

    auto brightness = [&](float angleDegrees) {
        // A point on the floor at that angle from the spot's axis. Undo the
        // distance falloff so only the cone's shape is left.
        const float x = 10.0f * std::tan(radians(angleDegrees));
        Incoming in;
        float nDotL = 0.0f;
        if (!incomingLight(prepared, {x, 0, 0}, {0, 1, 0}, true, in, nDotL)) return 0.0f;
        return in.radiance.x * (100.0f + x * x);
    };
    CHECK(brightness(0.0f) == Approx(1.0f));
    CHECK(brightness(19.0f) == Approx(1.0f));  // inside the inner cone: full
    CHECK(brightness(25.0f) > 0.1f);           // on the edge: partial
    CHECK(brightness(25.0f) < 0.9f);
    CHECK(brightness(31.0f) == 0.0f);  // outside
    CHECK(brightness(60.0f) == 0.0f);
}

// --------------------------------------------------------------------- PBR ---

TEST_CASE("pbr: the GGX distribution integrates to one") {
    // Projected onto the surface, the facets must add up to exactly the
    // surface's area: the integral of D(h) (n.h) over the hemisphere is 1.
    // If it were not, rougher or smoother surfaces would create or destroy
    // light.
    for (float roughness : {0.2f, 0.5f, 0.9f}) {
        const float a = roughness * roughness;
        double sum = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i) {
            const double theta = (i + 0.5) / steps * (kPi / 2.0);
            const float cosTheta = static_cast<float>(std::cos(theta));
            sum += distributionGgx(cosTheta, a) * cosTheta * std::sin(theta) * 2.0 * kPi * (kPi / 2.0 / steps);
        }
        CAPTURE(roughness);
        CHECK(sum == Approx(1.0).epsilon(0.01));
    }
}

TEST_CASE("pbr: Fresnel reflectance rises from f0 to one at grazing angles") {
    const Vec3 f0{0.04f, 0.5f, 1.0f};
    const Vec3 headOn = fresnelSchlick(1.0f, f0), grazing = fresnelSchlick(0.0f, f0);
    CHECK(headOn.x == Approx(0.04f));
    CHECK(headOn.y == Approx(0.5f));
    CHECK(grazing.x == Approx(1.0f));
    CHECK(grazing.y == Approx(1.0f));
    CHECK(fresnelSchlick(0.5f, f0).x > 0.04f);
    CHECK(fresnelSchlick(0.5f, f0).x < 0.1f);
}

TEST_CASE("pbr: materials behave like materials") {
    Light light;
    light.direction = {0, 0, -1};  // shining straight at a surface facing +z
    const ShadingLight prepared = prepareLight(light, nullptr);
    LightingContext ctx;
    ctx.lights = &prepared;
    ctx.lightCount = 1;
    ctx.environment = &kDark;

    Surface surface;
    surface.normal = {0, 0, 1};
    surface.view = {0, 0, 1};
    surface.albedo = {0.8f, 0.2f, 0.2f};
    surface.roughness = 1.0f;

    SUBCASE("a rough non-metal lit head-on shows (nearly) its base colour") {
        const Vec3 c = shadePbr(ctx, surface);
        CHECK(c.x == Approx(0.8f).epsilon(0.1));
        CHECK(c.y == Approx(0.2f).epsilon(0.25));
    }
    SUBCASE("a metal has no diffuse colour: only its tinted reflection") {
        surface.metallic = 1.0f;
        surface.albedo = {1.0f, 0.0f, 0.0f};
        const Vec3 c = shadePbr(ctx, surface);
        CHECK(c.x > 0.0f);
        CHECK(c.y == Approx(0.0f));  // a red metal reflects no green or blue head-on
        CHECK(c.z == Approx(0.0f));
    }
    SUBCASE("smoother surfaces concentrate the highlight") {
        surface.albedo = {0, 0, 0};
        surface.roughness = 0.8f;
        const float rough = shadePbr(ctx, surface).x;
        surface.roughness = 0.2f;
        const float smooth = shadePbr(ctx, surface).x;
        CHECK(smooth > rough * 10.0f);  // at its centre...

        // ...and dimmer away from it.
        surface.view = normalize(Vec3{0.7f, 0, 0.7f});
        const float smoothOff = shadePbr(ctx, surface).x;
        surface.roughness = 0.8f;
        const float roughOff = shadePbr(ctx, surface).x;
        CHECK(smoothOff < roughOff);
    }
    SUBCASE("no light, no colour") {
        ctx.lightCount = 0;
        CHECK(shadePbr(ctx, surface) == Vec3{0, 0, 0});
    }
    SUBCASE("a uniform white environment cannot make a surface brighter than white") {
        const Environment white = Environment::hemisphere({1, 1, 1}, {1, 1, 1});
        ctx.lightCount = 0;
        ctx.environment = &white;
        for (float metallic : {0.0f, 1.0f})
            for (float roughness : {0.1f, 0.5f, 1.0f}) {
                surface.albedo = {1, 1, 1};
                surface.metallic = metallic;
                surface.roughness = roughness;
                const Vec3 c = shadePbr(ctx, surface);
                CAPTURE(metallic);
                CAPTURE(roughness);
                CHECK(c.x <= 1.1f);
                // ...and a white surface reflects most of it. Rough metals
                // are the dimmest: light that bounces between facets more
                // than once is not accounted for by this model, and on a
                // rough surface that is a sizeable share.
                CHECK(c.x >= (metallic > 0.5f ? 0.4f : 0.8f));
            }
    }
}

TEST_CASE("toon: shading comes in a few flat tones") {
    Model model = makeDemoModel("sphere");
    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    const Camera camera = orbit.camera(1.0f);

    // The share of the sphere's pixels taken up by its three commonest tones.
    auto topThreeShare = [&](ShadingMode mode) {
        RenderSettings settings;
        settings.shading = mode;
        settings.textures = false;
        settings.normalMaps = false;
        Framebuffer fb(128, 128);
        renderer.render(model, camera, settings, fb);
        std::vector<int> histogram(256, 0);
        int covered = 0;
        for (const Color8& p : fb.pixels())
            if (p.a) {
                ++histogram[static_cast<std::size_t>(luma(p))];
                ++covered;
            }
        std::sort(histogram.rbegin(), histogram.rend());
        return static_cast<double>(histogram[0] + histogram[1] + histogram[2]) / covered;
    };
    // Smooth shading spreads over the whole range; cel shading sits on a
    // few flat tones, with only thin anti-aliased bands between them.
    CHECK(topThreeShare(ShadingMode::Toon) > 0.75);
    CHECK(topThreeShare(ShadingMode::Phong) < 0.5);
}

// ----------------------------------------------------------------- shadows ---

TEST_CASE("shadow map: comparison, bias and soft edges") {
    ShadowMap map;
    map.viewProjection = Mat4::identity();  // world x, y, z in [-1, 1] are the map's own coordinates
    map.biasWorld = 0.0015f;                // ...and one world unit is one unit of depth
    map.target.resize(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) map.target.depthRow(y)[x] = x < 32 ? 0.25f : 1.0f;  // an occluder on the left

    // depth = z * 0.5 + 0.5, so z = 0 is depth 0.5: behind the occluder.
    map.kernel = 1;
    CHECK(map.sample({-0.5f, 0, 0}, 1.0f) == Approx(0.0f));      // under the occluder
    CHECK(map.sample({0.5f, 0, 0}, 1.0f) == Approx(1.0f));       // in the open
    CHECK(map.sample({-0.5f, 0, -0.9f}, 1.0f) == Approx(1.0f));  // in front of the occluder: lit

    // On the occluder itself (depth exactly 0.25) the bias prevents self-shadowing.
    CHECK(map.sample({-0.5f, 0, -0.5f}, 1.0f) == Approx(1.0f));

    // Outside the map, behind the light or beyond its far plane: lit.
    CHECK(map.sample({5, 0, 0}, 1.0f) == 1.0f);
    CHECK(map.sample({0, 0, 2}, 1.0f) == 1.0f);

    // Across the edge the result ramps from 0 to 1, over a distance that
    // grows with the kernel.
    auto rampWidth = [&](int kernel) {
        map.kernel = kernel;
        int partial = 0;
        float previous = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float v = map.sample({-0.3f + 0.6f * static_cast<float>(i) / 400.0f, 0, 0}, 1.0f);
            CHECK(v >= previous - 1e-5f);  // monotonic
            previous = v;
            partial += v > 0.01f && v < 0.99f;
        }
        return partial;
    };
    const int hard = rampWidth(1), soft = rampWidth(5);
    CHECK(hard > 0);
    CHECK(soft > hard * 3);
}

TEST_CASE("shadows: an object darkens the ground behind it, for directional and spot lights") {
    // A floor, and a small square hovering one unit above its centre.
    Material white;
    white.roughness = 1.0f;
    white.doubleSided = true;
    const Model floor = makeQuad({-4, 0, 4}, {8, 0, 0}, {0, 0, -8}, white);
    const Model square = makeQuad({-0.5f, 1, 0.5f}, {1, 0, 0}, {0, 0, -1}, white);
    const DrawItem items[] = {{&floor, Mat4::identity()}, {&square, Mat4::identity()}};

    ThreadPool pool(2);
    Renderer renderer(pool);
    Framebuffer fb(160, 160);
    RenderSettings settings;
    settings.environment = &kDark;  // no ambient: a shadow is black
    settings.shadows = true;

    SUBCASE("directional") {
        // Shining down and towards -x: the shadow lands one unit to the left.
        settings.lights = {sun({-1, -1, 0})};
        const Camera camera = lookFrom({0, 12, 0}, {0, 0, 0}, {0, 0, -1});
        for (ShadingMode mode : {ShadingMode::Phong, ShadingMode::Pbr, ShadingMode::Toon}) {
            settings.shading = mode;
            const RenderStats stats = renderer.render(items, camera, settings, fb);
            CAPTURE(shadingModeName(mode));
            CHECK(stats.shadowMs > 0.0);
            const int shadowed = luma(pixelAt(fb, camera, {-1.0f, 0, 0}));
            const int lit = luma(pixelAt(fb, camera, {2.0f, 0, 0}));
            CHECK(lit > 60);
            CHECK(shadowed < lit / 4);
            // The shadow has the square's shape: no wider than it in z.
            CHECK(luma(pixelAt(fb, camera, {-1.0f, 0, 1.2f})) > lit / 2);
        }

        // Without shadows the same spot is as bright as the rest.
        settings.shading = ShadingMode::Phong;
        settings.shadows = false;
        const RenderStats stats = renderer.render(items, camera, settings, fb);
        CHECK(stats.shadowMs == 0.0);
        CHECK(luma(pixelAt(fb, camera, {-1.0f, 0, 0})) ==
              Approx(luma(pixelAt(fb, camera, {2.0f, 0, 0}))).epsilon(0.05));
    }
    SUBCASE("spot") {
        Light spot;
        spot.type = LightType::Spot;
        spot.position = {0, 5, 0};
        spot.direction = {0, -1, 0};
        spot.innerCone = radians(25.0f);
        spot.outerCone = radians(35.0f);
        spot.intensity = 25.0f;
        spot.castsShadow = true;
        settings.lights = {spot};
        // Seen from the side, so the shadow under the square is in view.
        const Camera camera = lookFrom({9, 9, 0}, {0, 0, 0});
        renderer.render(items, camera, settings, fb);
        const int shadowed = luma(pixelAt(fb, camera, {-0.2f, 0, 0}));
        const int lit = luma(pixelAt(fb, camera, {1.6f, 0, 0}));
        const int outside = luma(pixelAt(fb, camera, {-2.9f, 0, 2.9f}));  // beyond the cone
        CHECK(lit > 60);
        CHECK(shadowed < lit / 4);
        CHECK(outside < lit / 4);
    }
    SUBCASE("point lights and vertex-lit modes simply go without") {
        Light bulb;
        bulb.type = LightType::Point;
        bulb.position = {0, 5, 0};
        bulb.intensity = 25.0f;
        bulb.castsShadow = true;
        settings.lights = {bulb};
        const Camera camera = lookFrom({0, 12, 0}, {0, 0, 0}, {0, 0, -1});
        CHECK(renderer.render(items, camera, settings, fb).shadowMs == 0.0);

        settings.lights = {sun({-1, -1, 0})};
        settings.shading = ShadingMode::Gouraud;
        CHECK(renderer.render(items, camera, settings, fb).shadowMs == 0.0);
    }
}

// ---------------------------------------------------------------- blending ---

TEST_CASE("blending: translucent surfaces mix with what is behind them, in linear light") {
    // Blue behind, half-transparent red in front and overhanging it.
    const Model back = makeQuad({-1, -1, 0}, {2, 0, 0}, {0, 2, 0}, glowing({0, 0, 1}));
    const Model front = makeQuad({-2, -2, 1}, {4, 0, 0}, {0, 4, 0}, glowing({1, 0, 0}, 0.5f));
    const Camera camera = lookFrom({0, 0, 12}, {0, 0, 0});

    ThreadPool pool(2);
    Renderer renderer(pool);
    RenderSettings settings;
    settings.environment = &kDark;
    Light off;
    off.intensity = 0.0f;
    settings.lights = {off};
    settings.background = {0, 80, 0, 255};

    Framebuffer fb(96, 96);
    const DrawItem items[] = {{&back, Mat4::identity()}, {&front, Mat4::identity()}};
    renderer.render(items, camera, settings, fb);

    // Over the blue square: half red, half blue, mixed before tone mapping.
    const Color8 centre = pixelAt(fb, camera, {0, 0, 0});
    const Color8 expected = linearToSrgb(toneMap(Vec3{0.5f, 0.0f, 0.5f}));
    CHECK(centre.r == Approx(expected.r).epsilon(0.02));
    CHECK(centre.b == Approx(expected.b).epsilon(0.02));
    CHECK(centre.g == 0);
    CHECK(centre.a == 255);

    // Over the background: half red, and half of the background colour as given.
    const Color8 edge = pixelAt(fb, camera, {1.5f, 1.5f, 1});
    const Vec3 mixed = toneMap(Vec3{0.5f, 0, 0}) + srgbToLinear(settings.background) * 0.5f;
    CHECK(edge.r == Approx(linearToSrgb(mixed).r).epsilon(0.02));
    CHECK(edge.g == Approx(linearToSrgb(mixed).g).epsilon(0.02));
    CHECK(edge.a == 255);

    // Nothing drawn: exactly the background, marked as empty.
    CHECK(fb.at(1, 1) == Color8{0, 80, 0, 0});

    // The order the models are submitted in does not matter: translucent
    // triangles are sorted behind-to-front after the opaque ones.
    Framebuffer reversed(96, 96);
    const DrawItem swapped[] = {{&front, Mat4::identity()}, {&back, Mat4::identity()}};
    renderer.render(swapped, camera, settings, reversed);
    CHECK(fb.pixels() == reversed.pixels());
}

TEST_CASE("blending: two translucent layers are drawn furthest first") {
    const Model farLayer = makeQuad({-1, -1, 0}, {2, 0, 0}, {0, 2, 0}, glowing({0, 1, 0}, 0.5f));
    const Model nearLayer = makeQuad({-1, -1, 1}, {2, 0, 0}, {0, 2, 0}, glowing({1, 0, 0}, 0.5f));
    const Camera camera = lookFrom({0, 0, 12}, {0, 0, 0});
    ThreadPool pool(1);
    Renderer renderer(pool);
    RenderSettings settings;
    settings.environment = &kDark;
    Light off;
    off.intensity = 0.0f;
    settings.lights = {off};
    settings.background = {0, 0, 0, 255};
    settings.post.toneMapper = ToneMapper::Linear;

    // Near red over far green over black: 0.5 red + 0.25 green.
    for (const bool nearFirst : {false, true}) {
        const DrawItem a{&farLayer, Mat4::identity()}, b{&nearLayer, Mat4::identity()};
        const DrawItem items[] = {nearFirst ? b : a, nearFirst ? a : b};
        Framebuffer fb(64, 64);
        renderer.render(items, camera, settings, fb);
        const Color8 c = fb.at(32, 32);
        CHECK(c.r == Approx(linearToSrgb(0.5f)).epsilon(0.02));
        CHECK(c.g == Approx(linearToSrgb(0.25f)).epsilon(0.02));
    }
}

TEST_CASE("renderer: the direct and HDR paths produce the same picture") {
    const Model model = makeDemoModel("knot");
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    const Camera camera = orbit.camera(1.0f);
    ThreadPool pool(4);
    Renderer renderer(pool);

    RenderSettings settings;
    Framebuffer direct(128, 128), viaHdr(128, 128), wholeFrame(128, 128);
    renderer.render(model, camera, settings, direct);

    // An invisible translucent sheet in front of everything forces the
    // frame through the HDR buffer without changing a single pixel.
    const Bounds bounds = model.bounds();
    const float r = bounds.radius();
    const Model sheet =
        makeQuad(bounds.center() + Vec3{-r, -r, 0}, {2 * r, 0, 0}, {0, 2 * r, 0}, glowing({1, 1, 1}, 0.001f));
    const DrawItem items[] = {{&model, Mat4::identity()}, {&sheet, Mat4::identity()}};
    renderer.render(items, camera, settings, viaHdr);
    CHECK(maxDifference(direct, viaHdr) <= 1);

    // Bloom with an unreachable threshold adds nothing, but makes the frame
    // take the whole-frame post-processing route.
    settings.post.bloom = true;
    settings.post.bloomThreshold = 1e9f;
    renderer.render(model, camera, settings, wholeFrame);
    CHECK(maxDifference(direct, wholeFrame) <= 1);
}

// --------------------------------------------------------------- draw list ---

TEST_CASE("draw list: a transform places a model like baked-in vertices would") {
    const Model cube = makeDemoModel("cube");
    const Mat4 transform = translate({1.5f, 0.5f, 0}) * rotate({0, 1, 0}, 0.6f) * scale({1, 2, 1});
    Model baked = cube;
    for (Mesh& mesh : baked.meshes) {
        for (Vertex& v : mesh.vertices) {
            v.position = transformPoint(transform, v.position);
            v.normal = normalize(transformDirection(normalMatrix(transform), v.normal));
            v.tangent = Vec4(transformDirection(transform, v.tangent.xyz()), v.tangent.w);
        }
    }
    const Camera camera = lookFrom({3, 4, 10}, {1.5f, 0.5f, 0});
    ThreadPool pool(2);
    Renderer renderer(pool);

    for (ShadingMode mode : {ShadingMode::Flat, ShadingMode::Gouraud, ShadingMode::Phong, ShadingMode::Pbr}) {
        RenderSettings settings;
        settings.shading = mode;
        Framebuffer placed(128, 128), reference(128, 128);
        const DrawItem item{&cube, transform};
        const RenderStats stats = renderer.render(std::span<const DrawItem>(&item, 1), camera, settings, placed);
        renderer.render(baked, camera, settings, reference);
        CAPTURE(shadingModeName(mode));
        CHECK(stats.trianglesDrawn == 6);

        // Identical up to rounding: the two reach the same positions by
        // different arithmetic, so allow a little along the edges.
        int different = 0;
        for (std::size_t i = 0; i < placed.pixels().size(); ++i) {
            const Color8 a = placed.pixels()[i], b = reference.pixels()[i];
            different += std::abs(a.r - b.r) > 3 || std::abs(a.g - b.g) > 3 || std::abs(a.b - b.b) > 3 || a.a != b.a;
        }
        CHECK(different < 128 * 128 / 100);
    }
}

TEST_CASE("draw list: mirrored transforms keep their front faces") {
    const Model cube = makeDemoModel("cube");
    const Camera camera = lookFrom({3, 4, 10}, {0, 0, 0});
    ThreadPool pool(2);
    Renderer renderer(pool);
    RenderSettings settings;
    Framebuffer normal(96, 96), mirrored(96, 96);

    const DrawItem plain{&cube, Mat4::identity()}, flipped{&cube, scale({-1, 1, 1})};
    renderer.render(std::span<const DrawItem>(&plain, 1), camera, settings, normal);
    const RenderStats stats = renderer.render(std::span<const DrawItem>(&flipped, 1), camera, settings, mirrored);
    // Mirroring reverses the winding; if that were not accounted for, the
    // three faces towards the camera would be culled and the far ones drawn.
    CHECK(stats.trianglesDrawn == 6);
    int covered = 0, coveredMirrored = 0;
    for (std::size_t i = 0; i < normal.pixels().size(); ++i) {
        covered += normal.pixels()[i].a != 0;
        coveredMirrored += mirrored.pixels()[i].a != 0;
    }
    CHECK(coveredMirrored == Approx(covered).epsilon(0.02));  // a cube is symmetric
    // Lit from the same side: the mirrored cube is not dark inside-out.
    CHECK(luma(mirrored.at(48, 48)) == Approx(luma(normal.at(48, 48))).epsilon(0.25));
}

TEST_CASE("draw list: several models share one frame, each with its own materials") {
    const Model red = makeQuad({-2, -1, 0}, {2, 0, 0}, {0, 2, 0}, glowing({1, 0, 0}));
    const Model blue = makeQuad({-2, -1, 0}, {2, 0, 0}, {0, 2, 0}, glowing({0, 0, 1}));
    const Camera camera = lookFrom({0, 0, 12}, {0, 0, 0});
    ThreadPool pool(2);
    Renderer renderer(pool);
    RenderSettings settings;
    settings.environment = &kDark;
    Light off;
    off.intensity = 0.0f;
    settings.lights = {off};

    // The same geometry twice, the second copy moved to the right.
    const DrawItem items[] = {{&red, Mat4::identity()}, {&blue, translate({2, 0, 0})}};
    Framebuffer fb(96, 96);
    const RenderStats stats = renderer.render(items, camera, settings, fb);
    CHECK(stats.trianglesSubmitted == 4);
    const Color8 left = pixelAt(fb, camera, {-1, 0, 0}), right = pixelAt(fb, camera, {1, 0, 0});
    CHECK(left.r > 200);
    CHECK(left.b == 0);
    CHECK(right.b > 200);
    CHECK(right.r == 0);

    // An empty list is an empty frame.
    renderer.render(std::span<const DrawItem>(), camera, settings, fb);
    for (const Color8& p : fb.pixels()) CHECK(p.a == 0);
}
