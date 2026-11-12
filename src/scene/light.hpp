// Light: one light source.
#pragma once

#include "math/vec.hpp"

namespace termesh {

enum class LightType {
    Directional,  // parallel rays from infinitely far away: the sun
    Point,        // shines in all directions from a position: a bulb
    Spot,         // a point light restricted to a cone: a torch
};

struct Light {
    LightType type = LightType::Directional;
    Vec3 direction{0.0f, -1.0f, 0.0f};  // the way the light travels (unused for point lights)
    Vec3 position{0.0f, 0.0f, 0.0f};    // point and spot lights
    Vec3 color{1.0f, 1.0f, 1.0f};       // linear RGB
    float intensity = 1.0f;

    // Point and spot lights fade with the square of the distance, as real
    // ones do. With range > 0 they are also faded smoothly to nothing at
    // that distance, so a light has a definite reach.
    float range = 0.0f;

    // Spot cone, as angles from the axis in radians: full brightness inside
    // `innerCone`, none outside `outerCone`, a smooth edge between.
    float innerCone = 0.35f, outerCone = 0.5f;

    // Scales the highlights this light produces. Fill lights usually set it
    // to 0: they are there to lift the shadows, not to add a second glint.
    float specular = 1.0f;

    bool castsShadow = false;

    // Direction and position are relative to the camera rather than the
    // world, like a lamp mounted on it.
    bool cameraSpace = false;
};

}  // namespace termesh
