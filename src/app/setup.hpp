// Turning a command line into a scene ready to show. Shared by the program's
// main() and the WebAssembly build's entry points.
#pragma once

#include "app/options.hpp"
#include "core/thread_pool.hpp"
#include "stage/scene.hpp"

namespace termesh {

// Put together the scene the command line asks for: a scene file, a
// built-in demo, or one or more model files. Returns false (having said
// why on stderr) if that cannot be done. `options` may be replaced: a scene
// file's render settings become the defaults underneath the command line.
bool buildScene(int argc, const char* const* argv, Options& options, ThreadPool& pool, Scene& scene);

// Apply the animation options to every animated model in the scene.
bool setUpAnimations(const Options& options, Scene& scene, ThreadPool& pool);

}  // namespace termesh
