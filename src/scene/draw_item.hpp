// DrawItem: one thing to draw.
#pragma once

#include "math/mat.hpp"
#include "scene/model.hpp"

namespace termesh {

class ModelAccel;

struct DrawItem {
    const Model* model = nullptr;
    Mat4 transform;  // model space -> world space
    // Optional boxes and BVHs for the model (accel/model_accel.hpp). With
    // them, meshes and triangles outside the view are skipped early and
    // picking is fast; without them everything still works.
    const ModelAccel* accel = nullptr;
};

}  // namespace termesh
