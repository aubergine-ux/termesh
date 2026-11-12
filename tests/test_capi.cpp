// The C API, exercised the way a client would: through termesh.h and the
// library, with no access to anything inside.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <termesh.h>

#include "support.hpp"

namespace {

struct SceneHandle {
    termesh_scene* scene;
    explicit SceneHandle(termesh_scene* s) : scene(s) {}
    ~SceneHandle() { termesh_scene_destroy(scene); }
    operator termesh_scene*() const { return scene; }
};

struct RendererHandle {
    termesh_renderer* renderer = termesh_renderer_create(2);
    ~RendererHandle() { termesh_renderer_destroy(renderer); }
    operator termesh_renderer*() const { return renderer; }
};

// How many pixels of an RGBA image have anything drawn on them.
int covered(const std::vector<std::uint8_t>& rgba) {
    int count = 0;
    for (std::size_t i = 3; i < rgba.size(); i += 4) count += rgba[i] > 0;
    return count;
}

termesh_mesh triangleMesh(const float* positions, const std::uint32_t* indices) {
    termesh_mesh mesh;
    termesh_mesh_init(&mesh);
    mesh.positions = positions;
    mesh.vertex_count = 3;
    mesh.indices = indices;
    mesh.index_count = 3;
    return mesh;
}

const float kTriangle[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
const std::uint32_t kTriangleIndices[] = {0, 1, 2};

}  // namespace

TEST_CASE("capi: version") {
    CHECK(std::string(termesh_version()).find('.') != std::string::npos);
    CHECK(termesh_api_version() == TERMESH_API_VERSION);
}

TEST_CASE("capi: a demo scene renders to pixels") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(scene.scene);
    REQUIRE(termesh_scene_add_demo(scene, "knot") == 0);
    CHECK(termesh_scene_object_count(scene) == 1);
    CHECK(std::string(termesh_scene_object_name(scene, 0)) == "knot");
    CHECK(termesh_scene_triangle_count(scene) > 100);
    CHECK(termesh_scene_vertex_count(scene) > 100);

    float lo[3], hi[3];
    termesh_scene_bounds(scene, lo, hi);
    CHECK(hi[0] > lo[0]);

    termesh_camera camera;
    termesh_camera_frame(scene, 30, 15, 1, 40, &camera);
    CHECK(camera.fov == doctest::Approx(40.0f));

    RendererHandle renderer;
    REQUIRE(renderer.renderer);
    std::vector<std::uint8_t> rgba(64 * 48 * 4, 7);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 64, 48, rgba.data()) == 0);
    const int drawn = covered(rgba);
    CHECK(drawn > 64 * 48 / 10);
    CHECK(drawn < 64 * 48);
    // The corner is background: the default colour, with no coverage.
    CHECK(rgba[0] == 16);
    CHECK(rgba[3] == 0);

    // The same frame twice is the same bytes.
    std::vector<std::uint8_t> again(rgba.size());
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 64, 48, again.data()) == 0);
    CHECK(rgba == again);
}

TEST_CASE("capi: settings change the picture") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(termesh_scene_add_demo(scene, "sphere") == 0);
    termesh_camera camera;
    termesh_camera_frame(scene, 0, 0, 1, 40, &camera);
    RendererHandle renderer;

    termesh_settings settings;
    termesh_settings_init(&settings);
    CHECK(settings.struct_size == sizeof(settings));
    CHECK(settings.shading == TERMESH_SHADING_PHONG);
    CHECK(settings.textures == 1);

    std::vector<std::uint8_t> phong(48 * 48 * 4), toon(phong.size()), red(phong.size());
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 48, 48, phong.data()) == 0);
    settings.shading = TERMESH_SHADING_TOON;
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 48, 48, toon.data()) == 0);
    CHECK(phong != toon);

    settings.background[0] = 200, settings.background[1] = 0, settings.background[2] = 0;
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 48, 48, red.data()) == 0);
    CHECK(red[0] == 200);
    CHECK(red[1] == 0);

    settings.shading = 99;
    CHECK(termesh_render_pixels(renderer, scene, &camera, &settings, 48, 48, red.data()) < 0);
    CHECK(std::string(termesh_last_error()).find("shading") != std::string::npos);
}

TEST_CASE("capi: a struct from an older header still works") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(termesh_scene_add_demo(scene, "cube") == 0);
    termesh_camera camera;
    termesh_camera_frame(scene, 30, 15, 1, 40, &camera);
    RendererHandle renderer;
    std::vector<std::uint8_t> full(32 * 32 * 4), shorter(full.size());

    termesh_settings settings;
    termesh_settings_init(&settings);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 32, 32, full.data()) == 0);

    // Pretend the caller's struct ends before the last few fields, and put
    // rubbish where they would be: it must not be read.
    settings.struct_size = offsetof(termesh_settings, tone_mapper);
    settings.tone_mapper = 1234;
    settings.dither = -5;
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 32, 32, shorter.data()) == 0);
    CHECK(full == shorter);

    // A struct that was never initialised is refused rather than trusted.
    settings.struct_size = 0;
    CHECK(termesh_render_pixels(renderer, scene, &camera, &settings, 32, 32, shorter.data()) < 0);
}

TEST_CASE("capi: geometry from memory") {
    SceneHandle scene(termesh_scene_create());
    termesh_mesh mesh = triangleMesh(kTriangle, kTriangleIndices);
    mesh.name = "tri";
    mesh.base_color[0] = 1, mesh.base_color[1] = 0, mesh.base_color[2] = 0;
    mesh.double_sided = 1;
    REQUIRE(termesh_scene_add_mesh(scene, &mesh) == 0);
    CHECK(termesh_scene_triangle_count(scene) == 1);
    CHECK(std::string(termesh_scene_object_name(scene, 0)) == "tri");

    termesh_camera camera = {{0, 0, 4}, {0, 0, 0}, 40};
    RendererHandle renderer;
    std::vector<std::uint8_t> rgba(40 * 40 * 4);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 40, 40, rgba.data()) == 0);
    // The middle of the image is on the triangle, and red.
    const std::uint8_t* centre = &rgba[(20 * 40 + 20) * 4];
    CHECK(centre[3] == 255);
    CHECK(centre[0] > centre[1] + 40);
    CHECK(centre[0] > centre[2] + 40);

    SUBCASE("moving it out of view") {
        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 0, 0, 1};  // column-major: translate x
        REQUIRE(termesh_scene_set_transform(scene, 0, matrix) == 0);
        float lo[3], hi[3];
        termesh_scene_bounds(scene, lo, hi);
        CHECK(lo[0] == doctest::Approx(99.0f));
        CHECK(hi[0] == doctest::Approx(101.0f));
    }
    SUBCASE("hiding it") {
        REQUIRE(termesh_scene_set_visible(scene, 0, 0) == 0);
        REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 40, 40, rgba.data()) == 0);
        CHECK(covered(rgba) == 0);
    }
    SUBCASE("points") {
        termesh_mesh points = triangleMesh(kTriangle, nullptr);
        points.index_count = 0;
        REQUIRE(termesh_scene_add_mesh(scene, &points) == 1);
        CHECK(termesh_scene_triangle_count(scene) == 1);
        CHECK(termesh_scene_vertex_count(scene) == 6);
    }
    SUBCASE("bad input is refused") {
        const std::uint32_t outOfRange[] = {0, 1, 3};
        termesh_mesh bad = triangleMesh(kTriangle, outOfRange);
        CHECK(termesh_scene_add_mesh(scene, &bad) < 0);
        CHECK(std::strlen(termesh_last_error()) > 0);
        bad = triangleMesh(kTriangle, kTriangleIndices);
        bad.index_count = 2;
        CHECK(termesh_scene_add_mesh(scene, &bad) < 0);
        bad = triangleMesh(nullptr, kTriangleIndices);
        CHECK(termesh_scene_add_mesh(scene, &bad) < 0);
        CHECK(termesh_scene_add_mesh(scene, nullptr) < 0);
        CHECK(termesh_scene_object_count(scene) == 1);
        CHECK(termesh_scene_set_visible(scene, 5, 1) < 0);
        CHECK(termesh_scene_object_name(scene, -1) == nullptr);
    }
}

TEST_CASE("capi: errors are reported, not thrown") {
    CHECK(termesh_scene_open("/no/such/file.obj") == nullptr);
    CHECK(std::strlen(termesh_last_error()) > 0);
    CHECK(termesh_scene_open(nullptr) == nullptr);

    SceneHandle scene(termesh_scene_create());
    CHECK(termesh_scene_add_demo(scene, "teapot") < 0);
    CHECK(std::string(termesh_last_error()).find("teapot") != std::string::npos);
    CHECK(termesh_scene_add_file(scene, "/no/such/file.glb") < 0);
    CHECK(termesh_scene_set_environment(scene, "/no/such/sky.hdr") < 0);
    // A success clears the message.
    CHECK(termesh_scene_add_demo(scene, "cube") == 0);
    CHECK(std::strlen(termesh_last_error()) == 0);

    termesh_camera camera;
    termesh_camera_frame(scene, 0, 0, 1, 40, &camera);
    RendererHandle renderer;
    std::vector<std::uint8_t> rgba(16);
    CHECK(termesh_render_pixels(renderer, scene, &camera, nullptr, 0, 10, rgba.data()) < 0);
    CHECK(termesh_render_pixels(renderer, scene, &camera, nullptr, 100000, 100000, rgba.data()) < 0);
    CHECK(termesh_render_pixels(renderer, scene, nullptr, nullptr, 2, 2, rgba.data()) < 0);
    CHECK(termesh_render_pixels(nullptr, scene, &camera, nullptr, 2, 2, rgba.data()) < 0);
    termesh_camera nowhere = {{1, 1, 1}, {1, 1, 1}, 40};
    CHECK(termesh_render_pixels(renderer, scene, &nowhere, nullptr, 2, 2, rgba.data()) < 0);
    CHECK(termesh_render_text(renderer, scene, &camera, nullptr, 42, TERMESH_COLOR_NONE, 10, 5, nullptr) == nullptr);
    CHECK(termesh_render_text(renderer, scene, &camera, nullptr, TERMESH_BACKEND_ASCII, 9, 10, 5, nullptr) == nullptr);

    // Destroying nothing is allowed, like free().
    termesh_scene_destroy(nullptr);
    termesh_renderer_destroy(nullptr);
    termesh_free(nullptr);
}

TEST_CASE("capi: an empty scene renders its background") {
    SceneHandle scene(termesh_scene_create());
    termesh_camera camera;
    termesh_camera_frame(scene, 0, 0, 1, 40, &camera);
    RendererHandle renderer;
    std::vector<std::uint8_t> rgba(8 * 8 * 4, 9);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 8, 8, rgba.data()) == 0);
    CHECK(covered(rgba) == 0);
    CHECK(rgba[0] == 16);
}

TEST_CASE("capi: text for a terminal") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(termesh_scene_add_demo(scene, "knot") == 0);
    termesh_camera camera;
    termesh_camera_frame(scene, 30, 15, 1, 40, &camera);
    RendererHandle renderer;

    const auto lines = [](const char* text) {
        int count = 0;
        for (const char* p = text; *p; ++p) count += *p == '\n';
        return count;
    };

    std::size_t length = 0;
    char* ascii = termesh_render_text(renderer, scene, &camera, nullptr, TERMESH_BACKEND_ASCII, TERMESH_COLOR_NONE, 40,
                                      12, &length);
    REQUIRE(ascii);
    CHECK(length == std::strlen(ascii));
    CHECK(lines(ascii) == 12);
    CHECK(std::string(ascii).find("\x1b[38;") == std::string::npos);  // no colour: nothing sets one
    termesh_free(ascii);

    char* blocks = termesh_render_text(renderer, scene, &camera, nullptr, TERMESH_BACKEND_HALFBLOCK,
                                       TERMESH_COLOR_TRUECOLOR, 40, 12, nullptr);
    REQUIRE(blocks);
    CHECK(lines(blocks) == 12);
    CHECK(std::string(blocks).find("\x1b[38;2;") != std::string::npos);
    CHECK(std::string(blocks).find("\xe2\x96\x80") != std::string::npos);  // U+2580, the upper half block
    termesh_free(blocks);

    char* braille = termesh_render_text(renderer, scene, &camera, nullptr, TERMESH_BACKEND_BRAILLE, TERMESH_COLOR_NONE,
                                        40, 12, nullptr);
    REQUIRE(braille);
    CHECK(std::string(braille).find("\xe2\xa0") != std::string::npos);  // somewhere in U+2800..U+283F
    termesh_free(braille);

    for (int backend : {TERMESH_BACKEND_SIXEL, TERMESH_BACKEND_KITTY}) {
        char* pixels =
            termesh_render_text(renderer, scene, &camera, nullptr, backend, TERMESH_COLOR_TRUECOLOR, 20, 6, &length);
        REQUIRE(pixels);
        CHECK(length > 100);
        CHECK(pixels[0] == '\x1b');
        termesh_free(pixels);
    }
}

TEST_CASE("capi: lights and environments") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(termesh_scene_add_demo(scene, "sphere") == 0);
    termesh_camera camera;
    termesh_camera_frame(scene, 0, 0, 1, 40, &camera);
    RendererHandle renderer;
    std::vector<std::uint8_t> before(32 * 32 * 4), lit(before.size()), sky(before.size());
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 32, 32, before.data()) == 0);

    termesh_light light;
    termesh_light_init(&light);
    CHECK(light.intensity == 1.0f);
    light.color[0] = 1, light.color[1] = 0, light.color[2] = 0;
    light.direction[0] = 0, light.direction[1] = 0, light.direction[2] = -1;
    light.intensity = 4;
    REQUIRE(termesh_scene_add_light(scene, &light) == 0);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 32, 32, lit.data()) == 0);
    const std::size_t centre = (16 * 32 + 16) * 4;
    CHECK(lit[centre] > lit[centre + 2] + 30);  // red light: a red sphere

    light.type = 7;
    CHECK(termesh_scene_add_light(scene, &light) < 0);
    light.type = TERMESH_LIGHT_SPOT;
    light.direction[2] = 0;
    CHECK(termesh_scene_add_light(scene, &light) < 0);  // a spot light needs a direction

    termesh_scene_clear_lights(scene);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 32, 32, lit.data()) == 0);
    CHECK(lit == before);

    REQUIRE(termesh_scene_set_environment(scene, "sunset") == 0);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 32, 32, sky.data()) == 0);
    CHECK(sky != before);
    // A panorama is shown as the background unless the settings say not to.
    CHECK(sky[0] != 16);
    termesh_settings settings;
    termesh_settings_init(&settings);
    settings.skybox = 0;
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 32, 32, sky.data()) == 0);
    CHECK(sky[0] == 16);
}

TEST_CASE("capi: animation") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(termesh_scene_add_demo(scene, "cube") == 0);
    REQUIRE(termesh_scene_add_demo(scene, "tentacle") == 1);
    CHECK(termesh_scene_animation_count(scene, 0) == 0);
    const int clips = termesh_scene_animation_count(scene, 1);
    REQUIRE(clips >= 1);
    CHECK(termesh_scene_animation_name(scene, 1, 0) != nullptr);
    const float duration = termesh_scene_animation_duration(scene, 1, 0);
    CHECK(duration > 0.0f);
    CHECK(termesh_scene_animation_name(scene, 1, clips) == nullptr);
    CHECK(termesh_scene_play(scene, 0, 0, 0) < 0);  // the cube has nothing to play
    CHECK(termesh_scene_seek(scene, 0, 1) < 0);

    REQUIRE(termesh_scene_set_visible(scene, 0, 0) == 0);
    termesh_camera camera;
    termesh_camera_frame(scene, 30, 15, 1, 40, &camera);
    RendererHandle renderer;
    std::vector<std::uint8_t> start(48 * 48 * 4), later(start.size()), back(start.size());
    REQUIRE(termesh_scene_seek(scene, 1, 0.0f) == 0);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 48, 48, start.data()) == 0);
    termesh_scene_advance(scene, duration * 0.3f);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 48, 48, later.data()) == 0);
    CHECK(start != later);
    REQUIRE(termesh_scene_seek(scene, 1, 0.0f) == 0);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 48, 48, back.data()) == 0);
    CHECK(start == back);

    REQUIRE(termesh_scene_set_speed(scene, 1, 0.0f) == 0);
    termesh_scene_advance(scene, 1.0f);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, nullptr, 48, 48, later.data()) == 0);
    CHECK(start == later);
    if (clips > 1) CHECK(termesh_scene_play(scene, 1, 1, 0.2f) == 0);
}

TEST_CASE("capi: picking") {
    SceneHandle scene(termesh_scene_create());
    termesh_mesh mesh = triangleMesh(kTriangle, kTriangleIndices);
    REQUIRE(termesh_scene_add_mesh(scene, &mesh) == 0);
    REQUIRE(termesh_scene_add_demo(scene, "tentacle") == 1);
    REQUIRE(termesh_scene_set_visible(scene, 1, 0) == 0);

    termesh_camera camera = {{0, 0, 4}, {0, 0, 0}, 40};
    termesh_hit hit;
    REQUIRE(termesh_pick(scene, &camera, 1.0f, 0.0f, 0.0f, &hit) == 1);
    CHECK(hit.object == 0);
    CHECK(hit.mesh == 0);
    CHECK(hit.triangle == 0);
    CHECK(hit.distance == doctest::Approx(4.0f).epsilon(0.01));
    CHECK(hit.position[2] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(termesh_pick(scene, &camera, 1.0f, 0.95f, 0.95f, &hit) == 0);
    CHECK(termesh_pick(scene, &camera, 1.0f, 0.0f, 0.0f, nullptr) == 1);
    CHECK(termesh_pick(scene, nullptr, 1.0f, 0.0f, 0.0f, &hit) < 0);

    // An animated model can still be picked after it has moved.
    REQUIRE(termesh_scene_set_visible(scene, 0, 0) == 0);
    REQUIRE(termesh_scene_set_visible(scene, 1, 1) == 0);
    termesh_scene_advance(scene, 0.4f);
    termesh_camera_frame(scene, 0, 0, 1, 40, &camera);
    bool found = false;
    for (float y = -0.8f; y <= 0.8f && !found; y += 0.1f)
        for (float x = -0.8f; x <= 0.8f && !found; x += 0.1f)
            found = termesh_pick(scene, &camera, 1.0f, x, y, &hit) == 1 && hit.object == 1;
    CHECK(found);
}

TEST_CASE("capi: files") {
    TempDir dir;
    const std::string obj = dir.write("tri.obj", "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n");
    {
        SceneHandle scene(termesh_scene_open(obj.c_str()));
        REQUIRE(scene.scene);
        CHECK(termesh_scene_object_count(scene) == 1);
        CHECK(termesh_scene_triangle_count(scene) == 1);
        CHECK(termesh_scene_add_file(scene, obj.c_str()) == 1);
        termesh_camera camera;
        CHECK(termesh_scene_camera(scene, &camera) == 0);  // a model has no camera of its own
    }

    const std::string file = dir.write("scene.json", R"({
            "models": [ { "file": "tri.obj" }, { "demo": "cube", "position": [3, 0, 0] } ],
            "lights": [ { "type": "directional", "direction": [0, -1, -1] } ],
            "camera": { "position": [0, 1, 9], "target": [1, 0, 0], "fov": 50 },
            "render": { "shading": "pbr", "ssao": true, "background": "#102030" }
        })");
    SceneHandle scene(termesh_scene_open(file.c_str()));
    REQUIRE_MESSAGE(scene.scene, termesh_last_error());
    CHECK(termesh_scene_object_count(scene) == 2);

    termesh_camera camera;
    REQUIRE(termesh_scene_camera(scene, &camera) == 1);
    CHECK(camera.position[2] == doctest::Approx(9.0f));
    CHECK(camera.target[0] == doctest::Approx(1.0f));
    CHECK(camera.fov == doctest::Approx(50.0f));

    termesh_settings settings;
    REQUIRE(termesh_settings_from_scene(scene, &settings) == 0);
    CHECK(settings.shading == TERMESH_SHADING_PBR);
    CHECK(settings.ssao == 1);
    CHECK(settings.bloom == 0);
    CHECK(settings.background[0] == 0x10);
    CHECK(settings.background[2] == 0x30);

    RendererHandle renderer;
    std::vector<std::uint8_t> rgba(48 * 32 * 4);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 48, 32, rgba.data()) == 0);
    CHECK(covered(rgba) > 20);
    CHECK(rgba[0] == 0x10);

    const std::string broken = dir.write("broken.json", "{ \"models\": [ { \"file\": \"missing.obj\" } ] }");
    CHECK(termesh_scene_open(broken.c_str()) == nullptr);
    CHECK(std::string(termesh_last_error()).find("missing.obj") != std::string::npos);
}

TEST_CASE("capi: level of detail and the heavier effects") {
    SceneHandle scene(termesh_scene_create());
    REQUIRE(termesh_scene_add_demo(scene, "knot") == 0);
    REQUIRE(termesh_scene_add_demo(scene, "plane") == 1);
    termesh_camera camera;
    termesh_camera_frame(scene, 30, 25, 1, 40, &camera);
    RendererHandle renderer;
    termesh_settings settings;
    termesh_settings_init(&settings);
    settings.shading = TERMESH_SHADING_PBR;
    settings.shadows = 1;
    settings.ssao = 1;
    settings.bloom = 1;
    settings.fxaa = 1;
    settings.outline = 1;
    settings.lod_tolerance = 2.0f;
    settings.supersampling = 1;
    std::vector<std::uint8_t> rgba(96 * 64 * 4);
    REQUIRE(termesh_render_pixels(renderer, scene, &camera, &settings, 96, 64, rgba.data()) == 0);
    CHECK(covered(rgba) > 96 * 64 / 10);
}

TEST_CASE("capi: errors are per thread, and scenes can be used on different threads") {
    CHECK(termesh_scene_open("/no/such/file.obj") == nullptr);
    const std::string mine = termesh_last_error();
    REQUIRE(!mine.empty());

    std::string theirs = "unset";
    int drawn = 0;
    std::thread other([&] {
        theirs = termesh_last_error();
        SceneHandle scene(termesh_scene_create());
        termesh_scene_add_demo(scene, "cube");
        termesh_camera camera;
        termesh_camera_frame(scene, 30, 15, 1, 40, &camera);
        RendererHandle renderer;
        std::vector<std::uint8_t> rgba(24 * 24 * 4);
        if (termesh_render_pixels(renderer, scene, &camera, nullptr, 24, 24, rgba.data()) == 0) drawn = covered(rgba);
    });
    other.join();
    CHECK(theirs.empty());
    CHECK(drawn > 0);
    CHECK(mine == termesh_last_error());
}
