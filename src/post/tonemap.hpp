// Tone mapping: from the renderer's unbounded light values to displayable
// colour.
#pragma once

#include <algorithm>
#include <cmath>

#include "math/vec.hpp"

namespace termesh {

// Squeeze the unbounded range of computed light into the displayable
// [0, 1]. A hard clamp turns every bright area into a flat white patch; a
// tone curve instead rolls highlights off smoothly, the way film does.

enum class ToneMapper {
    Aces,      // Narkowicz's fit of the ACES filmic curve: contrasty, the default
    Reinhard,  // x / (1 + x): the simplest curve that never clips
    Filmic,    // Hable's "Uncharted 2" curve: softer shoulder, lifted toe
    Linear,    // no curve: clamp at 1
};

inline const char* toneMapperName(ToneMapper mapper) {
    switch (mapper) {
        case ToneMapper::Reinhard: return "reinhard";
        case ToneMapper::Filmic: return "filmic";
        case ToneMapper::Linear: return "linear";
        default: return "aces";
    }
}

inline float toneMapAces(float x) {
    x = std::max(x, 0.0f);
    return std::min((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 1.0f);
}

inline float toneMapFilmic(float x) {
    auto curve = [](float v) {
        const float a = 0.15f, b = 0.50f, c = 0.10f, d = 0.20f, e = 0.02f, f = 0.30f;
        return (v * (a * v + c * b) + d * e) / (v * (a * v + b) + d * f) - e / f;
    };
    // Scaled so that an input of 11.2 ("white") maps to exactly 1.
    return std::min(curve(std::max(x, 0.0f) * 2.0f) / curve(11.2f), 1.0f);
}

inline float toneMap(float x, ToneMapper mapper = ToneMapper::Aces) {
    switch (mapper) {
        case ToneMapper::Reinhard: return std::max(x, 0.0f) / (1.0f + std::max(x, 0.0f));
        case ToneMapper::Filmic: return toneMapFilmic(x);
        case ToneMapper::Linear: return std::clamp(x, 0.0f, 1.0f);
        default: return toneMapAces(x);
    }
}
inline Vec3 toneMap(Vec3 c, ToneMapper mapper = ToneMapper::Aces) {
    // Choose the curve once per colour rather than once per channel; the
    // default gets a straight-line path the compiler can vectorise.
    if (mapper == ToneMapper::Aces) return {toneMapAces(c.x), toneMapAces(c.y), toneMapAces(c.z)};
    return {toneMap(c.x, mapper), toneMap(c.y, mapper), toneMap(c.z, mapper)};
}

}  // namespace termesh
