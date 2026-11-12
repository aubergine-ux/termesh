#include "plugin/plugin.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include "output/backend.hpp"
#include "scene/loader.hpp"
#include "termesh_plugin.h"

#if defined(TERMESH_NO_PLUGINS) || defined(__EMSCRIPTEN__)
#define TERMESH_PLUGINS_UNAVAILABLE
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// What the opaque types of the C header are, on this side.
struct termesh_plugin_model {
    termesh::Model model;
    std::string error;
};

struct termesh_plugin_output {
    std::vector<std::string> rows;
};

namespace termesh {

namespace {

namespace fs = std::filesystem;

// The table of functions handed to every plugin (defined further down).
extern const termesh_host kHost;

std::vector<PluginInfo>& plugins() {
    static std::vector<PluginInfo> list;
    return list;
}

// The plugin whose termesh_plugin_init is running, so that registrations
// can be credited to it.
PluginInfo* loading = nullptr;

// As much of a plugin's struct as the plugin knew about, over defaults.
template <typename T>
bool readStruct(const T* given, void (*init)(T*), T& out) {
    init(&out);
    if (!given || given->struct_size < sizeof(std::size_t)) return false;
    std::memcpy(&out, given, std::min(given->struct_size, sizeof(T)));
    out.struct_size = sizeof(T);
    return true;
}

// ------------------------------------------------------------ loader side ---

void meshInit(termesh_plugin_mesh* mesh) {
    if (!mesh) return;
    *mesh = termesh_plugin_mesh{};
    mesh->struct_size = sizeof(*mesh);
    const Material defaults;
    mesh->base_color[0] = mesh->base_color[1] = mesh->base_color[2] = mesh->base_color[3] = 1.0f;
    mesh->metallic = defaults.metallic;
    mesh->roughness = defaults.roughness;
}

void setError(termesh_plugin_model* model, const char* message) {
    if (model) model->error = message ? message : "";
}

int addMesh(termesh_plugin_model* model, const termesh_plugin_mesh* given) {
    if (!model) return 1;
    const auto fail = [&](const char* message) {
        model->error = message;
        return 1;
    };
    try {
        termesh_plugin_mesh m;
        if (!readStruct(given, meshInit, m)) return fail("the mesh was not initialised with mesh_init");
        if (!m.positions || m.vertex_count == 0) return fail("the mesh has no vertices");
        if (m.vertex_count > 0xffffffffu) return fail("the mesh has too many vertices");
        if (!m.indices && m.index_count != 0) return fail("the mesh has an index count but no indices");
        if (m.index_count % 3 != 0) return fail("the mesh's index count is not a multiple of three");
        for (std::size_t i = 0; i < m.index_count; ++i)
            if (m.indices[i] >= m.vertex_count) return fail("an index refers to a vertex that does not exist");

        Mesh mesh;
        mesh.name = m.name ? m.name : "";
        mesh.vertices.resize(m.vertex_count);
        for (std::size_t i = 0; i < m.vertex_count; ++i) {
            Vertex& v = mesh.vertices[i];
            v.position = {m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]};
            if (m.normals) v.normal = normalize(Vec3{m.normals[i * 3], m.normals[i * 3 + 1], m.normals[i * 3 + 2]});
            if (m.uvs) v.uv = {m.uvs[i * 2], m.uvs[i * 2 + 1]};
        }
        if (m.indices) mesh.indices.assign(m.indices, m.indices + m.index_count);
        if (m.colors) {
            mesh.colors.resize(m.vertex_count);
            for (std::size_t i = 0; i < m.vertex_count; ++i)
                mesh.colors[i] = {m.colors[i * 3], m.colors[i * 3 + 1], m.colors[i * 3 + 2]};
        }
        mesh.hasUVs = m.uvs != nullptr;
        if (!m.normals && !mesh.indices.empty()) computeNormals(mesh);
        if (mesh.hasUVs && !mesh.indices.empty()) computeTangents(mesh);

        Material material;
        material.name = mesh.name;
        material.baseColor = {m.base_color[0], m.base_color[1], m.base_color[2],
                              std::clamp(m.base_color[3], 0.0f, 1.0f)};
        material.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
        material.roughness = std::clamp(m.roughness, 0.0f, 1.0f);
        material.doubleSided = m.double_sided != 0;
        if (material.baseColor.w < 1.0f) material.alphaMode = AlphaMode::Blend;
        mesh.material = static_cast<int>(model->model.materials.size());
        model->model.materials.push_back(std::move(material));
        model->model.meshes.push_back(std::move(mesh));
        return 0;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

int registerLoader(const termesh_host*, const char* extensions, termesh_plugin_load_fn load, void* user) {
    if (!extensions || !load || !loading) return 1;
    std::vector<std::string> list;
    std::string current;
    for (const char* p = extensions;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!current.empty()) list.push_back(current);
            current.clear();
            if (*p == '\0') break;
        } else if (*p != ' ' && *p != '.') {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
    }
    if (list.empty()) return 1;

    const std::string pluginName = loading->name;
    registerModelLoader(list, [load, user, pluginName](const std::string& path) {
        LoadResult result;
        termesh_plugin_model building;
        int status = 1;
        try {
            status = load(user, path.c_str(), &building, &kHost);
        } catch (...) {
            // A plugin written in C++ must not let exceptions out, but one
            // that does should not take the program down with it.
            building.error = "the plugin threw an exception";
        }
        if (status != 0) {
            result.error =
                "'" + path +
                "': " + (building.error.empty() ? "the " + pluginName + " plugin could not read it" : building.error);
            return result;
        }
        if (building.model.meshes.empty()) {
            result.error = "'" + path + "': the " + pluginName + " plugin found nothing in it";
            return result;
        }
        result.model = std::move(building.model);
        result.model.name = detail::fileNameOf(path);
        finalizeModel(result.model);
        for (Mesh& mesh : result.model.meshes)
            if (mesh.isPointCloud()) shufflePoints(mesh);
        result.ok = true;
        return result;
    });
    loading->extensions.insert(loading->extensions.end(), list.begin(), list.end());
    return 0;
}

// ----------------------------------------------------------- backend side ---

void backendInit(termesh_plugin_backend* backend) {
    if (!backend) return;
    *backend = termesh_plugin_backend{};
    backend->struct_size = sizeof(*backend);
    backend->cell_pixels_x = 1;
    backend->cell_pixels_y = 1;
}

void writeRow(termesh_plugin_output* out, int row, const char* utf8, std::size_t length) {
    if (!out || row < 0 || row >= static_cast<int>(out->rows.size()) || (!utf8 && length)) return;
    out->rows[static_cast<std::size_t>(row)].assign(utf8 ? utf8 : "", length);
}

// A plugin's backend, seen by the rest of the program as any other.
class PluginBackend : public Backend {
public:
    PluginBackend(const termesh_plugin_backend& description, ColorDepth color)
        : description_(description), color_(color) {}

    BackendKind kind() const override { return BackendKind::Custom; }
    const char* name() const override { return description_.name; }

    void configure(const DrawArea& area, int& width, int& height) override {
        area_ = area;
        width = area.cols * description_.cell_pixels_x;
        height = area.rows * description_.cell_pixels_y;
        invalidate();
    }

    float pixelAspect() const override {
        return (static_cast<float>(area_.cellWidth) / static_cast<float>(description_.cell_pixels_x)) /
               (static_cast<float>(area_.cellHeight) / static_cast<float>(description_.cell_pixels_y));
    }

    int defaultSupersampling() const override {
        return description_.supersampling > 0 ? std::min(description_.supersampling, 4) : 2;
    }
    bool wantsWireframe() const override { return description_.wants_wireframe != 0; }

    void present(ImageView image, std::string& out) override {
        draw(image);
        // Only the rows that differ from what is on screen are sent.
        shown_.resize(rows_.rows.size());
        for (std::size_t row = 0; row < rows_.rows.size(); ++row) {
            if (valid_ && rows_.rows[row] == shown_[row]) continue;
            out += "\x1b[" + std::to_string(row + 1) + ";1H\x1b[0m";
            out += rows_.rows[row];
            out += "\x1b[0m";
            shown_[row] = rows_.rows[row];
        }
        valid_ = true;
    }

    void print(ImageView image, std::string& out) override {
        draw(image);
        for (const std::string& row : rows_.rows) {
            out += row;
            out += "\x1b[0m\n";
        }
    }

    void invalidate() override { valid_ = false; }

private:
    void draw(ImageView image) {
        rows_.rows.assign(static_cast<std::size_t>(std::max(area_.rows, 0)), std::string());
        termesh_plugin_frame frame{};
        static_assert(sizeof(Color8) == 4, "frames are handed to plugins as bytes");
        frame.rgba = reinterpret_cast<const std::uint8_t*>(image.data);
        frame.width = image.width;
        frame.height = image.height;
        frame.cols = area_.cols;
        frame.rows = area_.rows;
        frame.color = static_cast<int>(color_);
        try {
            description_.draw(description_.user, &frame, &rows_, &kHost);
        } catch (...) {
            // See the loader: contain a misbehaving plugin.
        }
    }

    termesh_plugin_backend description_;
    ColorDepth color_;
    DrawArea area_;
    termesh_plugin_output rows_;
    std::vector<std::string> shown_;  // what present() last put on screen
    bool valid_ = false;
};

int registerBackendFromPlugin(const termesh_host*, const termesh_plugin_backend* given) {
    termesh_plugin_backend description;
    if (!loading || !readStruct(given, backendInit, description)) return 1;
    if (!description.name || !*description.name || !description.draw) return 1;
    if (description.cell_pixels_x < 1 || description.cell_pixels_y < 1 || description.cell_pixels_x > 16 ||
        description.cell_pixels_y > 16)
        return 1;
    // A plugin cannot take the place of a built-in backend.
    if (backendFromName(description.name)) return 1;

    registerBackend(description.name,
                    [description](ColorDepth color) { return std::make_unique<PluginBackend>(description, color); });
    loading->backends.emplace_back(description.name);
    loading->backendDescriptions.emplace_back(description.description ? description.description : "");
    return 0;
}

const termesh_host kHost = {
    sizeof(termesh_host),
    TERMESH_PLUGIN_API_VERSION,
    TERMESH_VERSION,
    registerLoader,
    registerBackendFromPlugin,
    meshInit,
    addMesh,
    setError,
    backendInit,
    writeRow,
    nullptr,
};

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

}  // namespace

// ----------------------------------------------------------------- loading ---

const char* pluginExtension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

bool pluginsSupported() {
#ifdef TERMESH_PLUGINS_UNAVAILABLE
    return false;
#else
    return true;
#endif
}

const std::vector<PluginInfo>& loadedPlugins() { return plugins(); }

bool loadPlugin(const std::string& path, std::string& error) {
#ifdef TERMESH_PLUGINS_UNAVAILABLE
    error = "'" + path + "': this build of termesh cannot load plugins";
    return false;
#else
    std::error_code ec;
    const std::string absolute = fs::absolute(path, ec).lexically_normal().string();
    for (const PluginInfo& plugin : plugins())
        if (plugin.path == absolute) return true;  // already loaded

    using InitFunction = int (*)(const termesh_host*);
    InitFunction init = nullptr;
#if defined(_WIN32)
    // (Without this, Windows may report a file that is not a library in a
    // dialog box rather than by returning.)
    const UINT previousMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    const HMODULE library = LoadLibraryA(absolute.c_str());
    SetErrorMode(previousMode);
    if (!library) {
        error = "'" + path + "': could not be loaded (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    init = reinterpret_cast<InitFunction>(reinterpret_cast<void*>(GetProcAddress(library, "termesh_plugin_init")));
#else
    // RTLD_LOCAL: a plugin's symbols are its own business.
    void* library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char* reason = dlerror();
        error = reason ? reason : "'" + path + "': could not be loaded";
        return false;
    }
    init = reinterpret_cast<InitFunction>(dlsym(library, "termesh_plugin_init"));
#endif
    if (!init) {
        error = "'" + path + "': not a termesh plugin (it has no termesh_plugin_init)";
        return false;
    }

    PluginInfo info;
    info.path = absolute;
    info.name = fs::path(absolute).stem().string();
    loading = &info;
    int status = 1;
    try {
        status = init(&kHost);
    } catch (...) {
    }
    loading = nullptr;
    // The library stays loaded either way: what it registered points into it.
    if (status != 0) {
        error = "'" + path + "': the plugin failed to start";
        return false;
    }
    plugins().push_back(std::move(info));
    return true;
#endif
}

int loadPluginDirectory(const std::string& directory, std::vector<std::string>& errors) {
    std::error_code ec;
    std::vector<std::string> files;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string extension = it->path().extension().string();
        // (On macOS a plugin may be a .dylib or, as CMake names them, a .so.)
        const bool isLibrary =
            extension == pluginExtension() || (std::string(pluginExtension()) == ".dylib" && extension == ".so");
        if (it->is_regular_file(ec) && isLibrary) files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());
    int loaded = 0;
    for (const std::string& file : files) {
        std::string error;
        if (loadPlugin(file, error)) ++loaded;
        else errors.push_back(error);
    }
    return loaded;
}

std::vector<std::string> pluginDirectories() {
    std::vector<std::string> directories;
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const std::string listed = environment("TERMESH_PLUGIN_PATH");
    for (std::size_t begin = 0; begin <= listed.size();) {
        std::size_t end = listed.find(separator, begin);
        if (end == std::string::npos) end = listed.size();
        if (end > begin) directories.push_back(listed.substr(begin, end - begin));
        begin = end + 1;
    }
#ifdef _WIN32
    if (const std::string appData = environment("APPDATA"); !appData.empty())
        directories.push_back((fs::path(appData) / "termesh" / "plugins").string());
#else
    if (const std::string config = environment("XDG_CONFIG_HOME"); !config.empty())
        directories.push_back((fs::path(config) / "termesh" / "plugins").string());
    else if (const std::string home = environment("HOME"); !home.empty())
        directories.push_back((fs::path(home) / ".config" / "termesh" / "plugins").string());
#endif
    return directories;
}

}  // namespace termesh
