// Environment: the light arriving from everywhere around the scene.
//
// Lights are points; the environment is everything else - the sky, the
// ground, the room. Surfaces pick it up in two ways:
//
//   diffuse   a matte surface averages all the light in the hemisphere it
//             faces (its "irradiance")
//   specular  a shiny surface mirrors the light from one direction, blurred
//             more the rougher it is
//
// An environment is either a simple two-colour hemisphere (the default, and
// free to evaluate) or an HDR panorama loaded from a file: image-based
// lighting.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "math/vec.hpp"

namespace termesh {

class Environment {
public:
    // The default "studio": light grey-blue from above, dim brown from below.
    Environment() = default;

    static Environment hemisphere(Vec3 sky, Vec3 ground);

    // Build from an equirectangular panorama: `width` x `height` linear RGB
    // pixels, row 0 at the top (straight up), the horizon half-way down and
    // longitude running across. Large images are reduced first.
    static Environment fromEquirect(int width, int height, std::vector<Vec3> pixels);

    // True when backed by a panorama rather than two colours.
    bool hasMap() const { return !levels_.empty(); }

    // Diffuse light reaching a surface with unit normal `n`, already divided
    // by pi: multiply by the surface colour to get what it reflects.
    Vec3 irradiance(Vec3 n) const {
        // The two-colour case is on the per-pixel path of every default
        // frame, so it stays here in the header where it can be inlined.
        if (!hasMap()) return lerp(ground_, sky_, n.y * 0.5f + 0.5f) * intensity;
        return mapIrradiance(n);
    }

    // Light arriving from around direction `r`, blurred to suit a surface of
    // the given roughness (0 = mirror, 1 = fully rough).
    Vec3 specular(Vec3 r, float roughness) const {
        if (!hasMap()) return lerp(ground_, sky_, r.y * 0.5f + 0.5f) * intensity;
        return mapSpecular(r, roughness);
    }

    // What the camera sees in direction `dir`: the background. `blur` (0-1)
    // softens it, which keeps a busy panorama from upstaging the model.
    Vec3 background(Vec3 dir, float blur) const;

    float intensity = 1.0f;  // brightness multiplier
    float rotation = 0.0f;   // turn about the vertical axis, in radians

    std::string name = "studio";

private:
    struct Level {
        int width = 0, height = 0;
        std::vector<Vec3> pixels;
    };

    Vec3 mapIrradiance(Vec3 n) const;
    Vec3 mapSpecular(Vec3 r, float roughness) const;
    Vec3 sampleLevel(Vec3 dir, float level) const;
    Vec3 sampleBilinear(const Level& level, float u, float v) const;
    void projectIrradiance();

    Vec3 sky_{0.42f, 0.47f, 0.55f}, ground_{0.14f, 0.12f, 0.11f};
    std::vector<Level> levels_;  // mip chain: each level half the size, and so twice as blurred
    // Irradiance as nine spherical-harmonic coefficients per colour channel.
    std::array<Vec3, 9> sh_{};
};

// Load a Radiance .hdr panorama. On failure returns false and sets `error`.
bool loadEnvironment(const std::string& path, Environment& out, std::string& error);

// Built-in environments, so image-based lighting can be tried without a
// file. Returns false for an unknown name.
//   studio  the default two-colour hemisphere
//   sky     a procedural daytime sky with a sun
//   sunset  the same sky with a low, warm sun
bool makeBuiltinEnvironment(const std::string& name, Environment& out);
std::vector<std::string> builtinEnvironmentNames();

}  // namespace termesh
