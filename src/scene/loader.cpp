#include "scene/loader.hpp"

#include <algorithm>
#include <cctype>

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

LoadResult loadModel(const std::string& path) {
    const std::string ext = detail::lowercaseExtension(path);
    if (ext == "obj") return loadObj(path);
    if (ext == "gltf" || ext == "glb") return loadGltf(path);

    LoadResult result;
    result.error = "unsupported file type '" + path + "' (expected .obj, .gltf or .glb)";
    return result;
}

}  // namespace termesh
