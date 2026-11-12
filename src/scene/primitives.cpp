#include "scene/primitives.hpp"

#include <cmath>

#include "core/color.hpp"

namespace termesh {

namespace {

// A grid of coloured squares with thin dark lines between them: reads well
// even at a few dozen pixels across.
Texture makeGridTexture(int size, int cells, Vec3 colorA, Vec3 colorB) {
    std::vector<Color8> pixels(static_cast<std::size_t>(size) * size);
    const int cell = size / cells;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int cx = x / cell, cy = y / cell;
            const bool line = x % cell < 2 || y % cell < 2;
            Vec3 c = ((cx + cy) & 1) ? colorA : colorB;
            if (line) c = c * 0.25f;
            pixels[static_cast<std::size_t>(y) * size + x] = linearToSrgb(c);
        }
    Texture t(size, size, std::move(pixels), true);
    t.name = "grid";
    return t;
}

// A normal map of round studs, one per grid cell, built from a height field.
Texture makeStudNormalMap(int size, int cells) {
    std::vector<Color8> height(static_cast<std::size_t>(size) * size);
    const float cell = static_cast<float>(size) / static_cast<float>(cells);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float fx = std::fmod(static_cast<float>(x), cell) / cell - 0.5f;
            const float fy = std::fmod(static_cast<float>(y), cell) / cell - 0.5f;
            const float r = std::sqrt(fx * fx + fy * fy) / 0.32f;
            const float h = r < 1.0f ? std::sqrt(1.0f - r * r) : 0.0f;  // a dome
            const auto v = static_cast<std::uint8_t>(h * 255.0f);
            height[static_cast<std::size_t>(y) * size + x] = {v, v, v, 255};
        }
    Texture t = heightToNormalMap(Texture(size, size, std::move(height), false), 6.0f);
    t.name = "studs";
    return t;
}

void addQuad(Mesh& mesh, Vec3 origin, Vec3 du, Vec3 dv) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    const Vec3 n = normalize(cross(du, dv));
    const Vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    const Vec3 corners[4] = {origin, origin + du, origin + du + dv, origin + dv};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = corners[i];
        v.normal = n;
        v.uv = uvs[i];
        mesh.vertices.push_back(v);
    }
    for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
}

Model makeCube() {
    Model model;
    model.name = "cube";
    Mesh mesh;
    mesh.hasUVs = true;
    addQuad(mesh, {-1, -1, 1}, {2, 0, 0}, {0, 2, 0});   // +Z
    addQuad(mesh, {1, -1, -1}, {-2, 0, 0}, {0, 2, 0});  // -Z
    addQuad(mesh, {1, -1, 1}, {0, 0, -2}, {0, 2, 0});   // +X
    addQuad(mesh, {-1, -1, -1}, {0, 0, 2}, {0, 2, 0});  // -X
    addQuad(mesh, {-1, 1, 1}, {2, 0, 0}, {0, 0, -2});   // +Y
    addQuad(mesh, {-1, -1, -1}, {2, 0, 0}, {0, 0, 2});  // -Y
    computeTangents(mesh);
    model.meshes.push_back(std::move(mesh));

    model.textures.push_back(makeGridTexture(256, 4, {0.9f, 0.45f, 0.1f}, {0.95f, 0.85f, 0.7f}));
    model.textures.push_back(makeStudNormalMap(256, 4));
    Material m;
    m.name = "crate";
    m.baseColorTexture = 0;
    m.normalTexture = 1;
    m.roughness = 0.45f;
    model.materials.push_back(m);
    return model;
}

Model makeSphere() {
    Model model;
    model.name = "sphere";
    Mesh mesh;
    mesh.hasUVs = true;
    const int slices = 48, stacks = 24;
    for (int j = 0; j <= stacks; ++j) {
        const float v = static_cast<float>(j) / stacks;
        const float phi = v * kPi;  // 0 at the north pole
        for (int i = 0; i <= slices; ++i) {
            const float u = static_cast<float>(i) / slices;
            const float theta = u * 2.0f * kPi;
            Vertex vert;
            vert.normal = {std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
            vert.position = vert.normal;
            vert.uv = {u * 2.0f, v};
            mesh.vertices.push_back(vert);
        }
    }
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i) {
            const auto a = static_cast<std::uint32_t>(j * (slices + 1) + i);
            const auto b = a + static_cast<std::uint32_t>(slices + 1);
            for (std::uint32_t idx : {a, b, a + 1, a + 1, b, b + 1}) mesh.indices.push_back(idx);
        }
    computeTangents(mesh);
    model.meshes.push_back(std::move(mesh));

    model.textures.push_back(makeGridTexture(512, 8, {0.1f, 0.45f, 0.8f}, {0.85f, 0.9f, 0.95f}));
    model.textures.push_back(makeStudNormalMap(512, 8));
    Material m;
    m.name = "ball";
    m.baseColorTexture = 0;
    m.normalTexture = 1;
    m.roughness = 0.35f;
    model.materials.push_back(m);
    return model;
}

// A (2,3) torus knot: a tube swept along a curve that winds twice around the
// torus's axis while circling its body three times.
Model makeKnot() {
    Model model;
    model.name = "knot";
    Mesh mesh;
    mesh.hasUVs = true;

    const int segments = 192, sides = 20;
    const float tubeRadius = 0.42f;
    auto curve = [](float t) {
        const float r = 2.0f + std::cos(3.0f * t);
        return Vec3{r * std::cos(2.0f * t), r * std::sin(2.0f * t), -std::sin(3.0f * t)};
    };

    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / segments * 2.0f * kPi;
        const Vec3 p = curve(t);
        // Build a frame around the curve: the tangent by finite differences,
        // and two perpendicular directions spanning the tube's cross-section.
        // The knot never passes through its own centre, so "away from the
        // origin" is always a safe reference direction.
        const Vec3 tangent = normalize(curve(t + 1e-3f) - curve(t - 1e-3f));
        const Vec3 side = normalize(cross(tangent, p));
        const Vec3 out = cross(side, tangent);
        for (int j = 0; j <= sides; ++j) {
            const float a = static_cast<float>(j) / sides * 2.0f * kPi;
            Vertex vert;
            vert.normal = out * std::cos(a) + side * std::sin(a);
            vert.position = p + vert.normal * tubeRadius;
            vert.uv = {static_cast<float>(i) / segments * 24.0f, static_cast<float>(j) / sides * 2.0f};
            mesh.vertices.push_back(vert);
        }
    }
    for (int i = 0; i < segments; ++i)
        for (int j = 0; j < sides; ++j) {
            const auto a = static_cast<std::uint32_t>(i * (sides + 1) + j);
            const auto b = a + static_cast<std::uint32_t>(sides + 1);
            for (std::uint32_t idx : {a, b, a + 1, a + 1, b, b + 1}) mesh.indices.push_back(idx);
        }
    computeTangents(mesh);
    model.meshes.push_back(std::move(mesh));

    model.textures.push_back(makeGridTexture(256, 2, {0.85f, 0.2f, 0.35f}, {0.95f, 0.8f, 0.3f}));
    model.textures.push_back(makeStudNormalMap(256, 2));
    Material m;
    m.name = "knot";
    m.baseColorTexture = 0;
    m.normalTexture = 1;
    m.roughness = 0.3f;
    model.materials.push_back(m);
    return model;
}

}  // namespace

std::vector<std::string> demoModelNames() { return {"knot", "cube", "sphere"}; }

Model makeDemoModel(const std::string& name) {
    Model model;
    if (name == "cube") model = makeCube();
    else if (name == "sphere") model = makeSphere();
    else if (name == "knot") model = makeKnot();
    finalizeModel(model);
    return model;
}

}  // namespace termesh
