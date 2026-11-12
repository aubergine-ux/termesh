// Scene files: a scene described in JSON.
//
//   {
//     "name": "showroom",
//     "models": [
//       { "file": "helmet.glb", "position": [0, 0, 0], "rotation": [0, 45, 0], "scale": 1.5 },
//       { "demo": "plane", "scale": 6, "material": { "color": "#808080", "roughness": 0.9 } }
//     ],
//     "lights": [
//       { "type": "directional", "direction": [-1, -2, -1], "intensity": 2, "shadows": true },
//       { "type": "spot", "position": [0, 4, 3], "target": [0, 0, 0], "angle": 30, "intensity": 40 }
//     ],
//     "environment": { "name": "sky", "intensity": 1, "skybox": true },
//     "camera": { "turntable": { "seconds": 12, "elevation": 20 } },
//     "render": { "shading": "pbr", "shadows": true, "ssao": true }
//   }
//
// Every part is optional except "models". docs/scenes.md describes each
// field. Paths are relative to the scene file.
#pragma once

#include <string>
#include <vector>

#include "stage/scene.hpp"

namespace termesh {

struct SceneLoadResult {
    Scene scene;
    bool ok = false;
    std::string error;                  // set when !ok
    std::vector<std::string> warnings;  // non-fatal problems
};

// Read a scene file and everything it refers to.
SceneLoadResult loadSceneFile(const std::string& path, ThreadPool* pool = nullptr);

// The same from text already in memory. `directory` (with a trailing
// separator, or empty) is where relative paths are looked up.
SceneLoadResult loadSceneText(const std::string& text, const std::string& directory, ThreadPool* pool = nullptr);

// "#rrggbb" or [r, g, b] in 0-1, as linear RGB. False if it is neither.
bool parseSceneColor(const Json& value, Vec3& linear);

}  // namespace termesh
