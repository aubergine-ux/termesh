#include "scene/loader.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace termesh {

namespace detail {

std::string directoryOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string lowercaseExtension(const std::string& path) {
    const std::string name = fileNameOf(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace detail

namespace {

struct LoaderTable {
    std::map<std::string, ModelLoader> byExtension;
    ModelLoader fallback;
};

// Built on first use rather than at program start, so that code which
// registers a loader from a static initialiser finds the table ready.
LoaderTable& loaders() {
    static LoaderTable table = [] {
        LoaderTable t;
        t.byExtension["obj"] = loadObj;
        t.byExtension["gltf"] = loadGltf;
        t.byExtension["glb"] = loadGltf;
        t.byExtension["stl"] = loadStl;
        t.byExtension["ply"] = loadPly;
#ifdef TERMESH_HAS_ASSIMP
        // Whatever the built-in loaders do not claim goes to Assimp.
        t.fallback = loadWithAssimp;
#endif
        return t;
    }();
    return table;
}

}  // namespace

void registerModelLoader(const std::vector<std::string>& extensions, ModelLoader loader) {
    for (std::string extension : extensions) {
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!extension.empty() && extension[0] == '.') extension.erase(0, 1);
        if (!extension.empty()) loaders().byExtension[extension] = loader;
    }
}

void registerFallbackModelLoader(ModelLoader loader) { loaders().fallback = std::move(loader); }

std::vector<std::string> modelExtensions() {
    std::vector<std::string> out;
    for (const auto& entry : loaders().byExtension) out.push_back(entry.first);
    return out;  // a map iterates in sorted order
}

LoadResult loadModel(const std::string& path) {
    const std::string ext = detail::lowercaseExtension(path);
    const LoaderTable& table = loaders();
    if (const auto found = table.byExtension.find(ext); found != table.byExtension.end()) return found->second(path);
    if (table.fallback) return table.fallback(path);

    LoadResult result;
    std::string known;
    for (const std::string& extension : modelExtensions()) known += (known.empty() ? "." : ", .") + extension;
    result.error = "unsupported file type '" + path + "' (expected one of " + known + ")";
    return result;
}

}  // namespace termesh
