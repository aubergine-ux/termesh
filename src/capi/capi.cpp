// The C API (include/termesh.h): a thin, exception-proof wrapper around the
// C++ classes. Nothing here does any rendering of its own.
//
// Three rules keep it safe to call from C:
//
//   - no exception leaves a function: each body runs inside guard(), which
//     turns one into an error return and a message for termesh_last_error()
//   - no C++ type crosses the boundary: handles are opaque, and everything
//     else is plain numbers, arrays and strings
//   - structs filled in by the caller carry their own size, so a program
//     built against an older header (and so a shorter struct) keeps working
#include "termesh.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "app/options.hpp"
#include "output/backend.hpp"
#include "pipeline/renderer.hpp"
#include "pipeline/resolve.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"
#include "stage/scene_file.hpp"

using namespace termesh;

struct termesh_scene {
    Scene scene;
    bool lodsBuilt = false;
};

struct termesh_renderer {
    explicit termesh_renderer(unsigned threads) : pool(threads), renderer(pool) {}
    ThreadPool pool;
    Renderer renderer;
    Framebuffer fb;
    Image image;
};

namespace {

thread_local std::string lastError;

// Run `body`; if it throws, record why and return `failure` instead.
template <typename Result, typename Body>
Result guard(Result failure, const Body& body) noexcept {
    try {
        lastError.clear();
        return body();
    } catch (const std::exception& e) {
        try {
            lastError = e.what();
        } catch (...) {
        }
    } catch (...) {
        try {
            lastError = "unknown error";
        } catch (...) {
        }
    }
    return failure;
}

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, const char* message) {
    if (!condition) throw Error(message);
}

SceneObject& objectAt(termesh_scene* scene, int object) {
    require(scene != nullptr, "scene is NULL");
    require(object >= 0 && object < static_cast<int>(scene->scene.objects.size()), "no such object");
    return *scene->scene.objects[static_cast<std::size_t>(object)];
}
const SceneObject& objectAt(const termesh_scene* scene, int object) {
    return objectAt(const_cast<termesh_scene*>(scene), object);
}

AnimationPlayer& playerOf(termesh_scene* scene, int object) {
    SceneObject& o = objectAt(scene, object);
    require(o.player != nullptr, "the object is not animated");
    return *o.player;
}

// A caller's struct, as much of it as the caller's header knew about, on
// top of the defaults for the rest.
template <typename T>
T readStruct(const T* given, void (*init)(T*)) {
    T value;
    init(&value);
    if (given) {
        const std::size_t size = std::min(given->struct_size, sizeof(T));
        require(size >= sizeof(std::size_t), "struct_size is not set: initialise the struct with its _init function");
        std::memcpy(&value, given, size);
        value.struct_size = sizeof(T);
    }
    return value;
}

Vec3 vec3(const float v[3]) { return {v[0], v[1], v[2]}; }
void store(Vec3 v, float out[3]) { out[0] = v.x, out[1] = v.y, out[2] = v.z; }

int addModel(termesh_scene* scene, Model model, std::string name, std::string source) {
    SceneObject& object = scene->scene.add(std::move(model), std::move(name));
    object.source = std::move(source);
    scene->lodsBuilt = false;
    return static_cast<int>(scene->scene.objects.size()) - 1;
}

Model loadOrThrow(const std::string& path) {
    LoadResult loaded = loadModel(path);
    if (!loaded.ok) throw Error(loaded.error);
    return std::move(loaded.model);
}

CameraPose poseOf(const termesh_camera& camera) {
    CameraPose pose;
    pose.position = vec3(camera.position);
    pose.target = vec3(camera.target);
    pose.fov = radians(std::clamp(camera.fov, 1.0f, 170.0f));
    require(lengthSquared(pose.position - pose.target) > 0.0f, "the camera's position and target are the same point");
    return pose;
}

void storePose(const CameraPose& pose, termesh_camera* camera) {
    store(pose.position, camera->position);
    store(pose.target, camera->target);
    camera->fov = degrees(pose.fov);
}

template <typename Enum>
Enum enumValue(int value, int count, const char* message) {
    require(value >= 0 && value < count, message);
    return static_cast<Enum>(value);
}

// The renderer's settings for a frame of `scene`.
RenderSettings renderSettings(const termesh_settings& s, const Scene& scene) {
    RenderSettings r;
    r.shading = enumValue<ShadingMode>(s.shading, kShadingModeCount, "unknown shading mode");
    r.wireframe = s.wireframe != 0;
    r.textures = s.textures != 0;
    r.normalMaps = s.normal_maps != 0;
    r.cullBackfaces = s.cull_backfaces != 0;
    r.points = s.points != 0;
    r.pointScale = s.point_size > 0.0f ? s.point_size : 1.0f;
    r.background = {s.background[0], s.background[1], s.background[2], 255};
    r.shadows = s.shadows != 0;
    r.shadowMapSize = std::clamp(s.shadow_map_size, 64, 4096);
    r.shadowSoftness = std::clamp(s.shadow_softness, 1, 7);
    r.lights = scene.lights;
    r.environment = &scene.environment;
    r.skybox = s.skybox < 0 ? scene.skybox.value_or(scene.environment.hasMap()) : s.skybox != 0;
    r.skyboxBlur = std::clamp(s.sky_blur < 0.0f ? scene.skyBlur : s.sky_blur, 0.0f, 1.0f);
    r.post.ssao = s.ssao != 0;
    r.post.bloom = s.bloom != 0;
    r.post.fxaa = s.fxaa != 0;
    r.post.outline = s.outline < 0 ? r.shading == ShadingMode::Toon : s.outline != 0;
    r.post.toneMapper = enumValue<ToneMapper>(s.tone_mapper, 4, "unknown tone mapper");
    r.post.exposure = s.exposure;
    return r;
}

// Render the scene into renderer->image at width x height.
void renderImage(termesh_renderer* renderer, termesh_scene* scene, const termesh_camera* camera,
                 const termesh_settings& s, RenderSettings settings, int width, int height, float pixelAspect,
                 int defaultSupersampling) {
    require(camera != nullptr, "camera is NULL");
    require(width > 0 && height > 0, "the image has no pixels");
    require(static_cast<std::int64_t>(width) * height <= kMaxFramebufferPixels, "the image is too large");

    int ssaa = std::clamp(s.supersampling > 0 ? s.supersampling : defaultSupersampling, 1, 4);
    while (ssaa > 1 && static_cast<std::int64_t>(width) * ssaa * height * ssaa > kMaxFramebufferPixels) --ssaa;

    settings.wireWidth = static_cast<float>(ssaa);
    settings.pixelScale = static_cast<float>(ssaa);
    if (s.lod_tolerance > 0.0f) {
        if (!scene->lodsBuilt) {
            scene->scene.buildLods(&renderer->pool);
            scene->lodsBuilt = true;
        }
        settings.lodTolerance = s.lod_tolerance * static_cast<float>(ssaa);
    }

    const Bounds bounds = scene->scene.bounds();
    const float aspect = static_cast<float>(width) * pixelAspect / static_cast<float>(height);
    const Camera view = cameraFromPose(poseOf(*camera), aspect, bounds);
    const std::vector<DrawItem> items = scene->scene.drawItems();

    if (renderer->fb.width() != width * ssaa || renderer->fb.height() != height * ssaa)
        renderer->fb = Framebuffer(width * ssaa, height * ssaa);
    renderer->renderer.render(items, view, settings, renderer->fb);
    resolve(renderer->fb, ssaa, renderer->image, renderer->pool);
    if (settings.post.fxaa) applyFxaa(renderer->image, renderer->pool);
}

termesh_settings settingsFromOptions(const Options& o) {
    termesh_settings s{};
    s.struct_size = sizeof(s);
    s.shading = static_cast<int>(o.shading);
    s.wireframe = o.wireframe;
    s.textures = o.textures;
    s.normal_maps = o.normalMaps;
    s.cull_backfaces = o.cullBackfaces;
    s.points = o.points;
    s.point_size = o.pointSize;
    s.background[0] = o.background.r, s.background[1] = o.background.g, s.background[2] = o.background.b;
    s.background[3] = 255;
    s.shadows = o.shadows;
    s.shadow_map_size = o.shadowMapSize;
    s.shadow_softness = o.shadowSoftness;
    s.skybox = o.skybox ? static_cast<int>(*o.skybox) : -1;
    s.sky_blur = o.skyBlur > 0.0f ? o.skyBlur : -1.0f;
    s.ssao = o.ssao;
    s.bloom = o.bloom;
    s.outline = o.outline ? static_cast<int>(*o.outline) : -1;
    s.fxaa = o.fxaa;
    s.tone_mapper = static_cast<int>(o.toneMapper);
    s.exposure = o.exposure;
    s.supersampling = o.supersampling;
    s.lod_tolerance = o.lodTolerance.value_or(0.0f);
    s.dither = static_cast<int>(o.dither);
    return s;
}

}  // namespace

extern "C" {

const char* termesh_version(void) { return TERMESH_VERSION; }
int termesh_api_version(void) { return TERMESH_API_VERSION; }
const char* termesh_last_error(void) { return lastError.c_str(); }
void termesh_free(void* memory) { std::free(memory); }

// ------------------------------------------------------------------ scenes

termesh_scene* termesh_scene_create(void) {
    return guard<termesh_scene*>(nullptr, [] { return new termesh_scene; });
}

termesh_scene* termesh_scene_open(const char* path) {
    return guard<termesh_scene*>(nullptr, [&] {
        require(path != nullptr, "path is NULL");
        auto scene = std::make_unique<termesh_scene>();
        if (isSceneFile(path)) {
            SceneLoadResult loaded = loadSceneFile(path);
            if (!loaded.ok) throw Error(loaded.error);
            scene->scene = std::move(loaded.scene);
        } else {
            Model model = loadOrThrow(path);
            scene->scene.name = model.name;
            std::string name = model.name;
            addModel(scene.get(), std::move(model), std::move(name), path);
        }
        return scene.release();
    });
}

void termesh_scene_destroy(termesh_scene* scene) { delete scene; }

int termesh_scene_add_file(termesh_scene* scene, const char* path) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(path != nullptr, "path is NULL");
        Model model = loadOrThrow(path);
        std::string name = model.name;
        return addModel(scene, std::move(model), std::move(name), path);
    });
}

int termesh_scene_add_demo(termesh_scene* scene, const char* name) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(name != nullptr, "name is NULL");
        Model model = makeDemoModel(name);
        if (model.meshes.empty()) throw Error(std::string("no built-in model called '") + name + "'");
        return addModel(scene, std::move(model), name, std::string("demo:") + name);
    });
}

void termesh_mesh_init(termesh_mesh* mesh) {
    if (!mesh) return;
    *mesh = termesh_mesh{};
    mesh->struct_size = sizeof(*mesh);
    const Material defaults;
    mesh->base_color[0] = mesh->base_color[1] = mesh->base_color[2] = mesh->base_color[3] = 1.0f;
    mesh->metallic = defaults.metallic;
    mesh->roughness = defaults.roughness;
}

int termesh_scene_add_mesh(termesh_scene* scene, const termesh_mesh* given) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(given != nullptr, "mesh is NULL");
        const termesh_mesh m = readStruct(given, termesh_mesh_init);
        require(m.positions != nullptr && m.vertex_count > 0, "the mesh has no vertices");
        require(m.vertex_count <= 0xffffffffu, "the mesh has too many vertices");
        require(m.indices != nullptr || m.index_count == 0, "indices is NULL");
        require(m.index_count % 3 == 0, "index_count is not a multiple of three");
        for (std::size_t i = 0; i < m.index_count; ++i)
            require(m.indices[i] < m.vertex_count, "an index refers to a vertex that does not exist");

        Model model;
        model.name = m.name ? m.name : "mesh";
        Mesh mesh;
        mesh.name = model.name;
        mesh.vertices.resize(m.vertex_count);
        for (std::size_t i = 0; i < m.vertex_count; ++i) {
            Vertex& v = mesh.vertices[i];
            v.position = vec3(m.positions + i * 3);
            if (m.normals) v.normal = normalize(vec3(m.normals + i * 3));
            if (m.uvs) v.uv = {m.uvs[i * 2], m.uvs[i * 2 + 1]};
        }
        if (m.indices) mesh.indices.assign(m.indices, m.indices + m.index_count);
        if (m.colors) {
            mesh.colors.resize(m.vertex_count);
            for (std::size_t i = 0; i < m.vertex_count; ++i) mesh.colors[i] = vec3(m.colors + i * 3);
        }
        mesh.hasUVs = m.uvs != nullptr;
        if (!m.normals && !mesh.indices.empty()) computeNormals(mesh);
        if (mesh.hasUVs && !mesh.indices.empty()) computeTangents(mesh);

        Material material;
        material.name = model.name;
        material.baseColor = {m.base_color[0], m.base_color[1], m.base_color[2],
                              std::clamp(m.base_color[3], 0.0f, 1.0f)};
        material.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
        material.roughness = std::clamp(m.roughness, 0.0f, 1.0f);
        material.doubleSided = m.double_sided != 0;
        if (material.baseColor.w < 1.0f) material.alphaMode = AlphaMode::Blend;
        model.materials.push_back(std::move(material));
        model.meshes.push_back(std::move(mesh));
        finalizeModel(model);

        std::string name = model.name;
        return addModel(scene, std::move(model), std::move(name), "memory");
    });
}

int termesh_scene_object_count(const termesh_scene* scene) {
    return scene ? static_cast<int>(scene->scene.objects.size()) : 0;
}

const char* termesh_scene_object_name(const termesh_scene* scene, int object) {
    return guard<const char*>(nullptr, [&] { return objectAt(scene, object).name.c_str(); });
}

int termesh_scene_set_transform(termesh_scene* scene, int object, const float matrix[16]) {
    return guard(-1, [&] {
        require(matrix != nullptr, "matrix is NULL");
        Mat4 transform;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) transform(row, column) = matrix[column * 4 + row];
        objectAt(scene, object).transform = transform;
        return 0;
    });
}

int termesh_scene_set_visible(termesh_scene* scene, int object, int visible) {
    return guard(-1, [&] {
        objectAt(scene, object).visible = visible != 0;
        return 0;
    });
}

size_t termesh_scene_triangle_count(const termesh_scene* scene) {
    return guard<std::size_t>(0, [&] { return scene ? scene->scene.triangleCount() : 0; });
}

size_t termesh_scene_vertex_count(const termesh_scene* scene) {
    return guard<std::size_t>(0, [&] { return scene ? scene->scene.vertexCount() : 0; });
}

void termesh_scene_bounds(const termesh_scene* scene, float min[3], float max[3]) {
    guard(0, [&] {
        const Bounds bounds = scene ? scene->scene.bounds() : Bounds{};
        if (min) store(bounds.min, min);
        if (max) store(bounds.max, max);
        return 0;
    });
}

// --------------------------------------------------------------- animation

int termesh_scene_animation_count(const termesh_scene* scene, int object) {
    return guard(-1, [&] {
        const SceneObject& o = objectAt(scene, object);
        return o.player ? o.player->clipCount() : 0;
    });
}

const char* termesh_scene_animation_name(const termesh_scene* scene, int object, int animation) {
    return guard<const char*>(nullptr, [&] {
        const SceneObject& o = objectAt(scene, object);
        require(animation >= 0 && animation < static_cast<int>(o.model.rig.animations.size()), "no such animation");
        return o.model.rig.animations[static_cast<std::size_t>(animation)].name.c_str();
    });
}

float termesh_scene_animation_duration(const termesh_scene* scene, int object, int animation) {
    return guard(-1.0f, [&] {
        const SceneObject& o = objectAt(scene, object);
        require(animation >= 0 && animation < static_cast<int>(o.model.rig.animations.size()), "no such animation");
        return o.model.rig.animations[static_cast<std::size_t>(animation)].duration;
    });
}

int termesh_scene_play(termesh_scene* scene, int object, int animation, float fade_seconds) {
    return guard(-1, [&] {
        AnimationPlayer& player = playerOf(scene, object);
        require(animation >= 0 && animation < player.clipCount(), "no such animation");
        player.setClip(animation, std::max(fade_seconds, 0.0f));
        player.play();
        scene->scene.pose();
        return 0;
    });
}

int termesh_scene_seek(termesh_scene* scene, int object, float seconds) {
    return guard(-1, [&] {
        playerOf(scene, object).seek(seconds);
        scene->scene.pose();
        return 0;
    });
}

int termesh_scene_set_speed(termesh_scene* scene, int object, float speed) {
    return guard(-1, [&] {
        playerOf(scene, object).setSpeed(speed);
        return 0;
    });
}

void termesh_scene_advance(termesh_scene* scene, float dt) {
    guard(0, [&] {
        if (scene) scene->scene.update(dt);
        return 0;
    });
}

// ------------------------------------------------------------------ lights

void termesh_light_init(termesh_light* light) {
    if (!light) return;
    *light = termesh_light{};
    light->struct_size = sizeof(*light);
    const Light defaults;
    light->type = TERMESH_LIGHT_DIRECTIONAL;
    store(defaults.direction, light->direction);
    store(defaults.color, light->color);
    light->intensity = defaults.intensity;
    light->inner_cone = degrees(defaults.innerCone);
    light->outer_cone = degrees(defaults.outerCone);
}

int termesh_scene_add_light(termesh_scene* scene, const termesh_light* given) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(given != nullptr, "light is NULL");
        const termesh_light l = readStruct(given, termesh_light_init);
        Light light;
        light.type = enumValue<LightType>(l.type, 3, "unknown light type");
        light.direction = vec3(l.direction);
        require(light.type == LightType::Point || lengthSquared(light.direction) > 0.0f, "the light has no direction");
        light.position = vec3(l.position);
        light.color = vec3(l.color);
        light.intensity = l.intensity;
        light.range = std::max(l.range, 0.0f);
        light.outerCone = radians(std::clamp(l.outer_cone, 0.1f, 89.0f));
        light.innerCone = std::min(radians(std::max(l.inner_cone, 0.0f)), light.outerCone);
        light.castsShadow = l.casts_shadow != 0;
        light.cameraSpace = l.attached_to_camera != 0;
        scene->scene.lights.push_back(light);
        return static_cast<int>(scene->scene.lights.size()) - 1;
    });
}

void termesh_scene_clear_lights(termesh_scene* scene) {
    if (scene) scene->scene.lights.clear();
}

int termesh_scene_set_environment(termesh_scene* scene, const char* name_or_path) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(name_or_path != nullptr, "the environment's name is NULL");
        Environment environment;
        if (!makeBuiltinEnvironment(name_or_path, environment)) {
            std::string error;
            if (!loadEnvironment(name_or_path, environment, error)) throw Error(error);
        }
        scene->scene.environment = std::move(environment);
        scene->scene.hasEnvironment = true;
        return 0;
    });
}

// ------------------------------------------------------------------ camera

void termesh_camera_frame(const termesh_scene* scene, float yaw, float pitch, float zoom, float fov,
                          termesh_camera* camera) {
    guard(0, [&] {
        if (!camera) return 0;
        OrbitCamera orbit;
        orbit.fovY = radians(std::clamp(fov > 0.0f ? fov : 40.0f, 1.0f, 170.0f));
        Bounds bounds = scene ? scene->scene.bounds() : Bounds{};
        if (bounds.radius() <= 0.0f) bounds = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        orbit.frame(bounds);
        orbit.yaw = radians(yaw);
        orbit.pitch = radians(std::clamp(pitch, -89.0f, 89.0f));
        if (zoom > 0.0f) orbit.zoom(1.0f / zoom);
        storePose({orbit.position(), orbit.target, orbit.fovY}, camera);
        return 0;
    });
}

int termesh_scene_camera(const termesh_scene* scene, termesh_camera* camera) {
    return guard(0, [&] {
        if (!scene || !camera || !scene->scene.camera) return 0;
        storePose(*scene->scene.camera, camera);
        return 1;
    });
}

// ---------------------------------------------------------------- settings

void termesh_settings_init(termesh_settings* settings) {
    if (!settings) return;
    guard(0, [&] {
        *settings = settingsFromOptions(Options{});
        return 0;
    });
}

int termesh_settings_from_scene(const termesh_scene* scene, termesh_settings* settings) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(settings != nullptr, "settings is NULL");
        Options options;
        std::string error;
        if (!scene->scene.renderSettings.isNull() &&
            !applySceneRenderSettings(scene->scene.renderSettings, options, error))
            throw Error(error);
        *settings = settingsFromOptions(options);
        return 0;
    });
}

// --------------------------------------------------------------- rendering

termesh_renderer* termesh_renderer_create(int threads) {
    return guard<termesh_renderer*>(
        nullptr, [&] { return new termesh_renderer(static_cast<unsigned>(std::clamp(threads, 0, 256))); });
}

void termesh_renderer_destroy(termesh_renderer* renderer) { delete renderer; }

int termesh_render_pixels(termesh_renderer* renderer, termesh_scene* scene, const termesh_camera* camera,
                          const termesh_settings* given, int width, int height, uint8_t* rgba) {
    return guard(-1, [&] {
        require(renderer != nullptr, "renderer is NULL");
        require(scene != nullptr, "scene is NULL");
        require(rgba != nullptr, "rgba is NULL");
        const termesh_settings s = readStruct(given, termesh_settings_init);
        renderImage(renderer, scene, camera, s, renderSettings(s, scene->scene), width, height, 1.0f, 2);
        static_assert(sizeof(Color8) == 4, "pixels are copied out as bytes");
        std::memcpy(rgba, renderer->image.pixels.data(), renderer->image.pixels.size() * sizeof(Color8));
        return 0;
    });
}

char* termesh_render_text(termesh_renderer* renderer, termesh_scene* scene, const termesh_camera* camera,
                          const termesh_settings* given, int backendKind, int color, int cols, int rows,
                          size_t* length) {
    return guard<char*>(nullptr, [&] {
        require(renderer != nullptr, "renderer is NULL");
        require(scene != nullptr, "scene is NULL");
        require(cols > 0 && rows > 0 && cols <= 4096 && rows <= 4096, "cols and rows must be between 1 and 4096");
        const termesh_settings s = readStruct(given, termesh_settings_init);

        // The C enum lists the text backends first; map it by name rather
        // than by position.
        BackendKind kind = BackendKind::HalfBlock;
        switch (backendKind) {
            case TERMESH_BACKEND_HALFBLOCK: kind = BackendKind::HalfBlock; break;
            case TERMESH_BACKEND_BRAILLE: kind = BackendKind::Braille; break;
            case TERMESH_BACKEND_ASCII: kind = BackendKind::Ascii; break;
            case TERMESH_BACKEND_SIXEL: kind = BackendKind::Sixel; break;
            case TERMESH_BACKEND_KITTY: kind = BackendKind::Kitty; break;
            default: throw Error("unknown backend");
        }
        const auto backend = makeBackend(kind, enumValue<ColorDepth>(color, 3, "unknown colour depth"));
        backend->setDither(enumValue<DitherMode>(s.dither, 4, "unknown dither mode"));
        DrawArea area;
        area.cols = cols;
        area.rows = rows;
        int width = 0, height = 0;
        backend->configure(area, width, height);

        RenderSettings settings = renderSettings(s, scene->scene);
        settings.wireframe = settings.wireframe || backend->wantsWireframe();
        renderImage(renderer, scene, camera, s, std::move(settings), width, height, backend->pixelAspect(),
                    backend->defaultSupersampling());

        std::string out;
        backend->print(renderer->image.view(), out);
        char* text = static_cast<char*>(std::malloc(out.size() + 1));
        if (!text) throw std::bad_alloc();
        std::memcpy(text, out.data(), out.size());
        text[out.size()] = '\0';
        if (length) *length = out.size();
        return text;
    });
}

// ----------------------------------------------------------------- picking

int termesh_pick(termesh_scene* scene, const termesh_camera* camera, float aspect, float x, float y, termesh_hit* hit) {
    return guard(-1, [&] {
        require(scene != nullptr, "scene is NULL");
        require(camera != nullptr, "camera is NULL");
        require(aspect > 0.0f, "aspect must be positive");
        // Trees left out of date by an animation frame are rebuilt here, the
        // first time something asks for them.
        for (const auto& object : scene->scene.objects) object->accel.updateTrees(object->model);
        const std::vector<DrawItem> items = scene->scene.drawItems();
        const Camera view = cameraFromPose(poseOf(*camera), aspect, scene->scene.bounds());
        PickResult result;
        if (!pick(items, view, x, y, true, result)) return 0;
        if (hit) {
            hit->object = scene->scene.objectOfItem(result.item);
            hit->mesh = result.mesh;
            hit->triangle = result.triangle;
            hit->distance = result.distance;
            store(result.position, hit->position);
        }
        return 1;
    });
}

}  // extern "C"
