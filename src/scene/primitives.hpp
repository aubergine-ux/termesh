// Built-in demo models with procedural textures, so the viewer has something
// to show without any files on disk.
#pragma once

#include <string>
#include <vector>

#include "scene/model.hpp"

namespace termesh {

// Names accepted by makeDemoModel.
std::vector<std::string> demoModelNames();

// Returns a model with no meshes if the name is unknown.
Model makeDemoModel(const std::string& name);

}  // namespace termesh
