// Model loading: Wavefront OBJ (+MTL), glTF 2.0 (.gltf / .glb), STL and PLY
// built in; anything else through loaders registered at run time.
#pragma once

#include <functional>
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

// Load a model, choosing the loader from the file extension.
LoadResult loadModel(const std::string& path);

LoadResult loadObj(const std::string& path);
LoadResult loadGltf(const std::string& path);
LoadResult loadStl(const std::string& path);
LoadResult loadPly(const std::string& path);
#ifdef TERMESH_HAS_ASSIMP
// Anything the Assimp library can read (see assimp_loader.cpp). Only
// present in builds configured with -DTERMESH_ASSIMP=ON.
LoadResult loadWithAssimp(const std::string& path);
#endif

// The table loadModel looks loaders up in. The built-in formats are in it
// from the start; more can be added - by an optional backend compiled in
// (Assimp), or by a plugin - without loadModel knowing about them.
using ModelLoader = std::function<LoadResult(const std::string& path)>;

// Use `loader` for files with these extensions (lower case, no dot). A later
// registration for the same extension replaces an earlier one.
void registerModelLoader(const std::vector<std::string>& extensions, ModelLoader loader);

// A loader of last resort: tried for any extension nothing else has claimed.
void registerFallbackModelLoader(ModelLoader loader);

// The extensions that currently have a loader, sorted.
std::vector<std::string> modelExtensions();

namespace detail {

// Directory part of a path, including the trailing separator ("" if none).
std::string directoryOf(const std::string& path);
std::string fileNameOf(const std::string& path);
std::string lowercaseExtension(const std::string& path);

}  // namespace detail

}  // namespace termesh
