// The Python module: termesh._termesh.
//
// It is written against the C API (include/termesh.h) and nothing else, so
// it is both a binding and a second client of that interface. What it adds
// is what Python expects: exceptions instead of error codes, objects that
// clean up after themselves, NumPy arrays in and out, and names instead of
// numbered enums. The package's __init__.py puts a few conveniences on top.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "termesh.h"

namespace py = pybind11;

namespace {

// Raised for anything the library reports: termesh.Error.
PyObject* errorType = nullptr;

[[noreturn]] void fail() {
    PyErr_SetString(errorType, termesh_last_error());
    throw py::error_already_set();
}

void check(int result) {
    if (result < 0) fail();
}

// Names for the numbered options, in the order termesh.h numbers them.
struct Names {
    const char* what;
    std::vector<const char*> names;

    int parse(const std::string& name) const {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (name == names[i]) return static_cast<int>(i);
        std::string message = std::string("unknown ") + what + " '" + name + "': expected one of";
        for (const char* n : names) message += std::string(" ") + n;
        throw py::value_error(message);
    }
    const char* name(int value) const {
        return value >= 0 && value < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(value)] : "?";
    }
};

const Names kShading{"shading mode", {"flat", "gouraud", "phong", "pbr", "toon"}};
const Names kToneMappers{"tone mapper", {"aces", "reinhard", "filmic", "linear"}};
const Names kDither{"dither mode", {"auto", "none", "ordered", "floyd-steinberg"}};
const Names kBackends{"backend", {"halfblock", "braille", "ascii", "sixel", "kitty"}};
const Names kColors{"colour depth", {"truecolor", "256", "none"}};
const Names kLights{"light type", {"directional", "point", "spot"}};

using Vec3 = std::array<float, 3>;

struct Camera {
    termesh_camera c{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, 40.0f};
};

struct Settings {
    Settings() { termesh_settings_init(&s); }
    termesh_settings s;
};

struct Hit {
    termesh_hit h;
};

struct Scene {
    Scene() : handle(termesh_scene_create()) {
        if (!handle) fail();
    }
    explicit Scene(termesh_scene* taken) : handle(taken) {}
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    ~Scene() { termesh_scene_destroy(handle); }

    void object(int index) const {
        if (index < 0 || index >= termesh_scene_object_count(handle)) throw py::index_error("no such object");
    }

    termesh_scene* handle;
};

struct Renderer {
    explicit Renderer(int threads) : handle(termesh_renderer_create(threads)) {
        if (!handle) fail();
    }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer() { termesh_renderer_destroy(handle); }

    termesh_renderer* handle;
};

Vec3 vec3(const float v[3]) { return {v[0], v[1], v[2]}; }

// An (n, 3) or (n, 2) float array, or None.
using FloatArray = py::array_t<float, py::array::c_style | py::array::forcecast>;

const float* rows(const FloatArray& array, std::size_t count, int width, const char* name) {
    if (array.ndim() != 2 || array.shape(1) != width || static_cast<std::size_t>(array.shape(0)) != count)
        throw py::value_error(std::string(name) + " must have shape (" + std::to_string(count) + ", " +
                              std::to_string(width) + ")");
    return array.data();
}

}  // namespace

PYBIND11_MODULE(_termesh, m) {
    m.doc() = "termesh: software-rasterised 3D rendering to pixels and terminal text";
    m.attr("__version__") = termesh_version();
    m.attr("api_version") = termesh_api_version();

    errorType = PyErr_NewException("termesh.Error", PyExc_RuntimeError, nullptr);
    m.add_object("Error", py::reinterpret_borrow<py::object>(errorType));

    m.attr("SHADING_MODES") = kShading.names;
    m.attr("TONE_MAPPERS") = kToneMappers.names;
    m.attr("DITHER_MODES") = kDither.names;
    m.attr("BACKENDS") = kBackends.names;
    m.attr("COLOR_DEPTHS") = kColors.names;

    // ------------------------------------------------------------- Camera
    py::class_<Camera>(m, "Camera", "Where the picture is taken from.")
        .def(py::init([](Vec3 position, Vec3 target, float fov) {
                 Camera camera;
                 std::memcpy(camera.c.position, position.data(), sizeof(camera.c.position));
                 std::memcpy(camera.c.target, target.data(), sizeof(camera.c.target));
                 camera.c.fov = fov;
                 return camera;
             }),
             py::arg("position") = Vec3{0.0f, 0.0f, 5.0f}, py::arg("target") = Vec3{0.0f, 0.0f, 0.0f},
             py::arg("fov") = 40.0f)
        .def_property(
            "position", [](const Camera& c) { return vec3(c.c.position); },
            [](Camera& c, Vec3 v) { std::memcpy(c.c.position, v.data(), sizeof(c.c.position)); })
        .def_property(
            "target", [](const Camera& c) { return vec3(c.c.target); },
            [](Camera& c, Vec3 v) { std::memcpy(c.c.target, v.data(), sizeof(c.c.target)); })
        .def_property(
            "fov", [](const Camera& c) { return c.c.fov; }, [](Camera& c, float fov) { c.c.fov = fov; },
            "Vertical field of view, in degrees.")
        .def("__repr__", [](const Camera& c) {
            return py::str("Camera(position=({:g}, {:g}, {:g}), target=({:g}, {:g}, {:g}), fov={:g})")
                .format(c.c.position[0], c.c.position[1], c.c.position[2], c.c.target[0], c.c.target[1], c.c.target[2],
                        c.c.fov);
        });

    // ----------------------------------------------------------- Settings
    py::class_<Settings> settings(m, "Settings", "How to draw: shading, shadows and effects.");
    settings.def(py::init<>());
    settings.def("copy", [](const Settings& s) { return s; });
#define TERMESH_FLAG(field, doc) \
    settings.def_property(       \
        #field, [](const Settings& s) { return s.s.field != 0; }, [](Settings& s, bool v) { s.s.field = v; }, doc)
#define TERMESH_VALUE(field, type, doc) \
    settings.def_property(              \
        #field, [](const Settings& s) { return s.s.field; }, [](Settings& s, type v) { s.s.field = v; }, doc)
#define TERMESH_NAMED(field, names, doc)                                 \
    settings.def_property(                                               \
        #field, [](const Settings& s) { return names.name(s.s.field); }, \
        [](Settings& s, const std::string& v) { s.s.field = names.parse(v); }, doc)
// A flag the scene can be left to decide: None, True or False.
#define TERMESH_OPTIONAL_FLAG(field, doc)         \
    settings.def_property(                        \
        #field,                                   \
        [](const Settings& s) -> py::object {     \
            if (s.s.field < 0) return py::none(); \
            return py::bool_(s.s.field != 0);     \
        },                                        \
        [](Settings& s, const py::object& v) { s.s.field = v.is_none() ? -1 : (v.cast<bool>() ? 1 : 0); }, doc)

    TERMESH_NAMED(shading, kShading, "'flat', 'gouraud', 'phong', 'pbr' or 'toon'.");
    TERMESH_FLAG(wireframe, "Draw edges only.");
    TERMESH_FLAG(textures, "Use the model's textures.");
    TERMESH_FLAG(normal_maps, "Use the model's normal maps.");
    TERMESH_FLAG(cull_backfaces, "Skip triangles facing away from the camera.");
    TERMESH_FLAG(points, "Draw every mesh as points.");
    TERMESH_VALUE(point_size, float, "Scale of point splats.");
    settings.def_property(
        "background",
        [](const Settings& s) { return std::array<int, 3>{s.s.background[0], s.s.background[1], s.s.background[2]}; },
        [](Settings& s, std::array<int, 3> rgb) {
            for (int i = 0; i < 3; ++i) {
                if (rgb[static_cast<std::size_t>(i)] < 0 || rgb[static_cast<std::size_t>(i)] > 255)
                    throw py::value_error("background components must be between 0 and 255");
                s.s.background[i] = static_cast<std::uint8_t>(rgb[static_cast<std::size_t>(i)]);
            }
        },
        "Background colour: (r, g, b), each 0-255.");
    TERMESH_FLAG(shadows, "Shadows from directional and spot lights.");
    TERMESH_VALUE(shadow_map_size, int, "Resolution of each shadow map.");
    TERMESH_VALUE(shadow_softness, int, "Width of the shadow filter: 1 (hard) to 7.");
    TERMESH_OPTIONAL_FLAG(skybox, "Draw the environment as the background. None leaves it to the scene.");
    settings.def_property(
        "sky_blur",
        [](const Settings& s) -> py::object {
            if (s.s.sky_blur < 0.0f) return py::none();
            return py::float_(s.s.sky_blur);
        },
        [](Settings& s, const py::object& v) { s.s.sky_blur = v.is_none() ? -1.0f : v.cast<float>(); },
        "Blur of the skybox, 0-1. None leaves it to the scene.");
    TERMESH_FLAG(ssao, "Ambient occlusion.");
    TERMESH_FLAG(bloom, "Glow around very bright areas.");
    TERMESH_OPTIONAL_FLAG(outline, "Ink outlines. None draws them with toon shading only.");
    TERMESH_FLAG(fxaa, "Smooth jagged edges after rendering.");
    TERMESH_NAMED(tone_mapper, kToneMappers, "'aces', 'reinhard', 'filmic' or 'linear'.");
    TERMESH_VALUE(exposure, float, "Brightness, in stops.");
    TERMESH_VALUE(supersampling, int, "Render at 1-4 times the resolution and average down. 0 picks a default.");
    TERMESH_VALUE(lod_tolerance, float, "Level of detail: the error allowed, in pixels. 0 turns it off.");
    TERMESH_NAMED(dither, kDither, "For text output: 'auto', 'none', 'ordered' or 'floyd-steinberg'.");
#undef TERMESH_FLAG
#undef TERMESH_VALUE
#undef TERMESH_NAMED
#undef TERMESH_OPTIONAL_FLAG

    // ---------------------------------------------------------------- Hit
    py::class_<Hit>(m, "Hit", "What Scene.pick found.")
        .def_property_readonly(
            "object", [](const Hit& h) { return h.h.object; }, "Index of the object.")
        .def_property_readonly(
            "mesh", [](const Hit& h) { return h.h.mesh; }, "Index of the mesh within the object.")
        .def_property_readonly(
            "triangle", [](const Hit& h) { return h.h.triangle; }, "Index of the triangle within the mesh.")
        .def_property_readonly(
            "distance", [](const Hit& h) { return h.h.distance; }, "Distance from the camera.")
        .def_property_readonly(
            "position", [](const Hit& h) { return vec3(h.h.position); }, "Where the ray hit, in world space.")
        .def("__repr__", [](const Hit& h) {
            return py::str("Hit(object={}, mesh={}, triangle={}, distance={:g})")
                .format(h.h.object, h.h.mesh, h.h.triangle, h.h.distance);
        });

    // -------------------------------------------------------------- Scene
    py::class_<Scene>(m, "Scene", "Models, lights and an environment.")
        .def(py::init<>(), "An empty scene.")
        .def_static(
            "open",
            [](const std::string& path) {
                termesh_scene* scene;
                {
                    py::gil_scoped_release release;
                    scene = termesh_scene_open(path.c_str());
                }
                if (!scene) fail();
                return std::make_unique<Scene>(scene);
            },
            py::arg("path"), "Load a model (.obj, .gltf, .glb, .stl, .ply) or a scene file (.json).")
        .def(
            "add_file",
            [](Scene& s, const std::string& path) {
                int index;
                {
                    py::gil_scoped_release release;
                    index = termesh_scene_add_file(s.handle, path.c_str());
                }
                check(index);
                return index;
            },
            py::arg("path"), "Add a model from a file. Returns the new object's index.")
        .def(
            "add_demo",
            [](Scene& s, const std::string& name) {
                const int index = termesh_scene_add_demo(s.handle, name.c_str());
                check(index);
                return index;
            },
            py::arg("name"), "Add a built-in model: 'knot', 'cube', 'sphere', 'tentacle' or 'plane'.")
        .def(
            "add_mesh",
            [](Scene& s, const FloatArray& positions, const py::object& indices, const py::object& normals,
               const py::object& uvs, const py::object& colors, std::array<float, 4> color, float metallic,
               float roughness, bool doubleSided, const std::string& name) {
                if (positions.ndim() != 2 || positions.shape(1) != 3)
                    throw py::value_error("positions must have shape (n, 3)");
                termesh_mesh mesh;
                termesh_mesh_init(&mesh);
                mesh.positions = positions.data();
                mesh.vertex_count = static_cast<std::size_t>(positions.shape(0));

                // The converted arrays have to stay alive until the call.
                py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast> indexArray;
                FloatArray normalArray, uvArray, colorArray;
                if (!indices.is_none()) {
                    indexArray = indices.cast<py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>>();
                    mesh.indices = indexArray.data();
                    mesh.index_count = static_cast<std::size_t>(indexArray.size());
                }
                if (!normals.is_none()) {
                    normalArray = normals.cast<FloatArray>();
                    mesh.normals = rows(normalArray, mesh.vertex_count, 3, "normals");
                }
                if (!uvs.is_none()) {
                    uvArray = uvs.cast<FloatArray>();
                    mesh.uvs = rows(uvArray, mesh.vertex_count, 2, "uvs");
                }
                if (!colors.is_none()) {
                    colorArray = colors.cast<FloatArray>();
                    mesh.colors = rows(colorArray, mesh.vertex_count, 3, "colors");
                }
                std::memcpy(mesh.base_color, color.data(), sizeof(mesh.base_color));
                mesh.metallic = metallic;
                mesh.roughness = roughness;
                mesh.double_sided = doubleSided;
                mesh.name = name.c_str();
                const int index = termesh_scene_add_mesh(s.handle, &mesh);
                check(index);
                return index;
            },
            py::arg("positions"), py::arg("indices") = py::none(), py::kw_only(), py::arg("normals") = py::none(),
            py::arg("uvs") = py::none(), py::arg("colors") = py::none(),
            py::arg("color") = std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}, py::arg("metallic") = 0.0f,
            py::arg("roughness") = 0.6f, py::arg("double_sided") = false, py::arg("name") = "mesh",
            "Add geometry from arrays: positions (n, 3) and triangle indices (m, 3).\n\n"
            "Without indices the vertices are drawn as a point cloud. normals (n, 3), uvs (n, 2)\n"
            "and per-vertex colors (n, 3) are optional; color is the material's (r, g, b, a).\n"
            "Returns the new object's index.")
        .def("__len__", [](const Scene& s) { return termesh_scene_object_count(s.handle); })
        .def_property_readonly(
            "objects",
            [](const Scene& s) {
                std::vector<std::string> names;
                for (int i = 0; i < termesh_scene_object_count(s.handle); ++i)
                    names.emplace_back(termesh_scene_object_name(s.handle, i));
                return names;
            },
            "The name of each object, in order.")
        .def(
            "set_transform",
            [](Scene& s, int object, const FloatArray& matrix) {
                s.object(object);
                if (matrix.ndim() != 2 || matrix.shape(0) != 4 || matrix.shape(1) != 4)
                    throw py::value_error("the transform must be a 4 x 4 matrix");
                // NumPy matrices are stored by rows; the library wants columns.
                float columns[16];
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column) columns[column * 4 + row] = matrix.at(row, column);
                check(termesh_scene_set_transform(s.handle, object, columns));
            },
            py::arg("object"), py::arg("matrix"),
            "Place an object: a 4 x 4 model-to-world matrix, as NumPy writes them\n"
            "(translation in the last column).")
        .def(
            "set_visible",
            [](Scene& s, int object, bool visible) {
                s.object(object);
                check(termesh_scene_set_visible(s.handle, object, visible));
            },
            py::arg("object"), py::arg("visible"))
        .def_property_readonly("triangle_count", [](const Scene& s) { return termesh_scene_triangle_count(s.handle); })
        .def_property_readonly("vertex_count", [](const Scene& s) { return termesh_scene_vertex_count(s.handle); })
        .def_property_readonly(
            "bounds",
            [](const Scene& s) {
                float lo[3], hi[3];
                termesh_scene_bounds(s.handle, lo, hi);
                return std::make_pair(vec3(lo), vec3(hi));
            },
            "The box around everything visible: (min, max).")
        // Animation.
        .def(
            "animations",
            [](const Scene& s, int object) {
                s.object(object);
                std::vector<std::pair<std::string, float>> clips;
                const int count = termesh_scene_animation_count(s.handle, object);
                for (int i = 0; i < count; ++i)
                    clips.emplace_back(termesh_scene_animation_name(s.handle, object, i),
                                       termesh_scene_animation_duration(s.handle, object, i));
                return clips;
            },
            py::arg("object") = 0, "An object's animations: a list of (name, seconds).")
        .def(
            "play",
            [](Scene& s, const py::object& animation, int object, float fade) {
                s.object(object);
                int index = -1;
                if (py::isinstance<py::str>(animation)) {
                    const std::string name = animation.cast<std::string>();
                    const int count = termesh_scene_animation_count(s.handle, object);
                    for (int i = 0; i < count && index < 0; ++i)
                        if (name == termesh_scene_animation_name(s.handle, object, i)) index = i;
                    if (index < 0) throw py::key_error("no animation called '" + name + "'");
                } else {
                    index = animation.cast<int>();
                }
                check(termesh_scene_play(s.handle, object, index, fade));
            },
            py::arg("animation"), py::arg("object") = 0, py::arg("fade") = 0.0f,
            "Switch to an animation, by name or number, cross-fading over `fade` seconds.")
        .def(
            "seek",
            [](Scene& s, float seconds, int object) {
                s.object(object);
                check(termesh_scene_seek(s.handle, object, seconds));
            },
            py::arg("seconds"), py::arg("object") = 0, "Jump to a time in the current animation.")
        .def(
            "set_speed",
            [](Scene& s, float speed, int object) {
                s.object(object);
                check(termesh_scene_set_speed(s.handle, object, speed));
            },
            py::arg("speed"), py::arg("object") = 0)
        .def(
            "advance", [](Scene& s, float dt) { termesh_scene_advance(s.handle, dt); }, py::arg("dt"),
            "Move every animation on by dt seconds.")
        // Lighting.
        .def(
            "add_light",
            [](Scene& s, const std::string& type, Vec3 direction, Vec3 position, Vec3 color, float intensity,
               float range, float innerCone, float outerCone, bool shadows, bool attached) {
                termesh_light light;
                termesh_light_init(&light);
                light.type = kLights.parse(type);
                std::memcpy(light.direction, direction.data(), sizeof(light.direction));
                std::memcpy(light.position, position.data(), sizeof(light.position));
                std::memcpy(light.color, color.data(), sizeof(light.color));
                light.intensity = intensity;
                light.range = range;
                light.inner_cone = innerCone;
                light.outer_cone = outerCone;
                light.casts_shadow = shadows;
                light.attached_to_camera = attached;
                const int index = termesh_scene_add_light(s.handle, &light);
                check(index);
                return index;
            },
            py::arg("type") = "directional", py::kw_only(), py::arg("direction") = Vec3{0.0f, -1.0f, 0.0f},
            py::arg("position") = Vec3{0.0f, 0.0f, 0.0f}, py::arg("color") = Vec3{1.0f, 1.0f, 1.0f},
            py::arg("intensity") = 1.0f, py::arg("range") = 0.0f, py::arg("inner_cone") = 20.0f,
            py::arg("outer_cone") = 30.0f, py::arg("shadows") = false, py::arg("attached_to_camera") = false,
            "Add a 'directional', 'point' or 'spot' light. With no lights of its own a scene\n"
            "is lit by a default pair attached to the camera.")
        .def("clear_lights", [](Scene& s) { termesh_scene_clear_lights(s.handle); })
        .def(
            "set_environment",
            [](Scene& s, const std::string& name) { check(termesh_scene_set_environment(s.handle, name.c_str())); },
            py::arg("name_or_path"), "Ambient light and reflections: 'studio', 'sky', 'sunset' or an .hdr panorama.")
        // Cameras and settings.
        .def(
            "frame",
            [](const Scene& s, float yaw, float pitch, float zoom, float fov) {
                Camera camera;
                termesh_camera_frame(s.handle, yaw, pitch, zoom, fov, &camera.c);
                return camera;
            },
            py::arg("yaw") = 30.0f, py::arg("pitch") = 15.0f, py::arg("zoom") = 1.0f, py::arg("fov") = 40.0f,
            "A camera that fits the whole scene in view, turned by yaw and raised by pitch (degrees).")
        .def_property_readonly(
            "camera",
            [](const Scene& s) -> py::object {
                Camera camera;
                if (!termesh_scene_camera(s.handle, &camera.c)) return py::none();
                return py::cast(camera);
            },
            "The camera a scene file asked for, or None.")
        .def(
            "settings",
            [](const Scene& s) {
                Settings settings;
                check(termesh_settings_from_scene(s.handle, &settings.s));
                return settings;
            },
            "The default settings, with the scene file's own render settings applied.")
        .def(
            "pick",
            [](Scene& s, const Camera& camera, float x, float y, float aspect) -> py::object {
                Hit hit;
                const int found = termesh_pick(s.handle, &camera.c, aspect, x, y, &hit.h);
                check(found);
                if (!found) return py::none();
                return py::cast(hit);
            },
            py::arg("camera"), py::arg("x"), py::arg("y"), py::arg("aspect") = 1.0f,
            "What is under a point of the image, or None. x and y run from -1 to 1 with +y up;\n"
            "aspect is the image's width / height.")
        .def("__repr__", [](const Scene& s) {
            return py::str("<termesh.Scene: {} objects, {} triangles>")
                .format(termesh_scene_object_count(s.handle), termesh_scene_triangle_count(s.handle));
        });

    // ----------------------------------------------------------- Renderer
    py::class_<Renderer>(m, "Renderer", "Draws scenes. Keeps its threads and memory between frames.")
        .def(py::init<int>(), py::arg("threads") = 0, "threads: how many to render with; 0 uses every core.")
        .def(
            "render",
            [](Renderer& r, Scene& scene, const Camera& camera, int width, int height, const Settings* settings) {
                if (width <= 0 || height <= 0) throw py::value_error("width and height must be positive");
                if (static_cast<long long>(width) * height > (1LL << 23))
                    throw py::value_error("the image is too large");
                py::array_t<std::uint8_t> image(
                    {static_cast<py::ssize_t>(height), static_cast<py::ssize_t>(width), static_cast<py::ssize_t>(4)});
                int result;
                {
                    py::gil_scoped_release release;
                    result = termesh_render_pixels(r.handle, scene.handle, &camera.c, settings ? &settings->s : nullptr,
                                                   width, height, image.mutable_data());
                }
                check(result);
                return image;
            },
            py::arg("scene"), py::arg("camera"), py::arg("width"), py::arg("height"),
            py::arg("settings") = static_cast<const Settings*>(nullptr),
            "Render to pixels: a (height, width, 4) uint8 array of sRGB r, g, b and coverage.")
        .def(
            "render_text",
            [](Renderer& r, Scene& scene, const Camera& camera, int cols, int rows, const std::string& backend,
               const std::string& color, const Settings* settings) {
                const int backendIndex = kBackends.parse(backend), colorIndex = kColors.parse(color);
                char* text;
                std::size_t length = 0;
                {
                    py::gil_scoped_release release;
                    text = termesh_render_text(r.handle, scene.handle, &camera.c, settings ? &settings->s : nullptr,
                                               backendIndex, colorIndex, cols, rows, &length);
                }
                if (!text) fail();
                py::str result(text, length);
                termesh_free(text);
                return result;
            },
            py::arg("scene"), py::arg("camera"), py::arg("cols") = 80, py::arg("rows") = 24,
            py::arg("backend") = "halfblock", py::arg("color") = "truecolor",
            py::arg("settings") = static_cast<const Settings*>(nullptr),
            "Render to the text a terminal would display, for a cols x rows area.\n\n"
            "backend: 'halfblock', 'braille', 'ascii', 'sixel' or 'kitty'.\n"
            "color: 'truecolor', '256' or 'none'.");
}
