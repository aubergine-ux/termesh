// Model loading: Wavefront OBJ (+MTL) and glTF 2.0 (.gltf / .glb).
#pragma once

#include <string>
#include <vector>

#include "scene/model.hpp"

namespace termesh {

struct LoadResult {
    Model model;
    bool ok = false;
    std::string error;                  // set when !ok
    std::vector<std::string> warnings;  // non-fatal problems (missing textures, ...)
};

// Load a model, choosing the format from the file extension.
LoadResult loadModel(const std::string& path);

LoadResult loadObj(const std::string& path);
LoadResult loadGltf(const std::string& path);

namespace detail {

// Directory part of a path, including the trailing separator ("" if none).
std::string directoryOf(const std::string& path);
std::string fileNameOf(const std::string& path);
std::string lowercaseExtension(const std::string& path);

}  // namespace detail

}  // namespace termesh
