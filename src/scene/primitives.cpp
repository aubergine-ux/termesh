#include "scene/primitives.hpp"

#include <algorithm>
#include <cmath>

#include "anim/skinning.hpp"
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
    model.textures.push_back(makeStudNormalMap(256, 2));
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

// A square of ground, two units on a side, for other models to stand on and
// cast shadows across. It is a grid rather than a single quad so that
// per-vertex shading has something to interpolate between.
Model makePlane() {
    constexpr int kCells = 16;
    Model model;
    model.name = "plane";
    Mesh mesh;
    mesh.hasUVs = true;
    for (int z = 0; z <= kCells; ++z)
        for (int x = 0; x <= kCells; ++x) {
            const float u = static_cast<float>(x) / kCells, v = static_cast<float>(z) / kCells;
            Vertex vert;
            vert.position = {u * 2.0f - 1.0f, 0.0f, v * 2.0f - 1.0f};
            vert.normal = {0.0f, 1.0f, 0.0f};
            vert.uv = {u * 4.0f, v * 4.0f};
            mesh.vertices.push_back(vert);
        }
    for (int z = 0; z < kCells; ++z)
        for (int x = 0; x < kCells; ++x) {
            const auto a = static_cast<std::uint32_t>(z * (kCells + 1) + x), b = a + 1;
            const auto c = a + static_cast<std::uint32_t>(kCells + 1), d = c + 1;
            // Counter-clockwise seen from above.
            for (std::uint32_t idx : {a, c, b, b, c, d}) mesh.indices.push_back(idx);
        }
    computeTangents(mesh);
    model.meshes.push_back(std::move(mesh));

    model.textures.push_back(makeGridTexture(256, 2, {0.42f, 0.44f, 0.48f}, {0.55f, 0.57f, 0.6f}));
    Material m;
    m.name = "ground";
    m.baseColorTexture = 0;
    m.roughness = 0.85f;
    m.doubleSided = true;
    model.materials.push_back(m);
    return model;
}

// An animated demo: a tapering tube with a chain of bones up its middle and
// two clips, so skinning, playback and cross-fading can be tried without any
// files. It is built the way a loader would build it - rest shape, bone
// weights, inverse bind matrices, keyframes - and then posed.
Model makeTentacle() {
    constexpr int kBones = 8, kRings = 64, kSides = 24;
    constexpr float kBoneLength = 0.5f, kHeight = kBones * kBoneLength;

    Model model;
    model.name = "tentacle";

    // Bones 0..7 form a chain along +y; the last node carries the mesh.
    Rig& rig = model.rig;
    rig.nodes.resize(kBones + 1);
    for (int i = 0; i < kBones; ++i) {
        rig.nodes[i].name = "bone " + std::to_string(i);
        rig.nodes[i].parent = i - 1;
        if (i > 0) rig.nodes[i].rest.translation = {0.0f, kBoneLength, 0.0f};
        rig.order.push_back(i);
    }
    rig.nodes[kBones].name = "tentacle";
    rig.order.push_back(kBones);

    // Each bone's inverse bind matrix undoes its rest position: bone i
    // starts i bone-lengths up the tube.
    Skin skin;
    for (int i = 0; i < kBones; ++i) {
        skin.joints.push_back(i);
        skin.inverseBind.push_back(translate({0.0f, -kBoneLength * static_cast<float>(i), 0.0f}));
    }
    rig.skins.push_back(std::move(skin));

    Mesh mesh;
    mesh.hasUVs = true;
    mesh.rig.node = kBones;
    mesh.rig.skin = 0;
    for (int i = 0; i <= kRings; ++i) {
        const float v = static_cast<float>(i) / kRings;
        const float y = v * kHeight;
        const float radius = 0.42f * (1.0f - v) + 0.03f;
        // A ring between two bones is shared between them in proportion to
        // how close it is to each, which is what makes the bend smooth.
        const float f = std::min(y / kBoneLength, static_cast<float>(kBones) - 1e-3f);
        const int bone = static_cast<int>(f);
        const float blend = bone + 1 < kBones ? f - static_cast<float>(bone) : 0.0f;
        for (int j = 0; j <= kSides; ++j) {
            const float a = static_cast<float>(j) / kSides * 2.0f * kPi;
            Vertex vert;
            // The slope of the taper tilts the normal slightly upwards.
            vert.normal = normalize(Vec3{std::cos(a), 0.42f / kHeight, std::sin(a)});
            vert.position = {std::cos(a) * radius, y, std::sin(a) * radius};
            vert.uv = {static_cast<float>(j) / kSides * 2.0f, (1.0f - v) * 4.0f};
            mesh.vertices.push_back(vert);
            mesh.rig.joints.push_back(
                {static_cast<std::uint16_t>(bone), static_cast<std::uint16_t>(std::min(bone + 1, kBones - 1)), 0, 0});
            mesh.rig.weights.push_back({1.0f - blend, blend, 0.0f, 0.0f});
        }
    }
    for (int i = 0; i < kRings; ++i)
        for (int j = 0; j < kSides; ++j) {
            const auto a = static_cast<std::uint32_t>(i * (kSides + 1) + j);
            const auto b = a + static_cast<std::uint32_t>(kSides + 1);
            for (std::uint32_t idx : {a, b, a + 1, a + 1, b, b + 1}) mesh.indices.push_back(idx);
        }
    computeTangents(mesh);
    for (const Vertex& v : mesh.vertices) {
        mesh.rig.positions.push_back(v.position);
        mesh.rig.normals.push_back(v.normal);
        mesh.rig.tangents.push_back(v.tangent.xyz());
    }
    model.meshes.push_back(std::move(mesh));

    // Keyframes. Both clips end on the pose they started with, so they loop.
    auto makeClip = [&](const char* name, float duration, Vec3 axis, auto angleAt) {
        constexpr int kKeys = 32;
        Animation clip;
        clip.name = name;
        clip.duration = duration;
        for (int bone = 0; bone < kBones; ++bone) {
            AnimationChannel channel;
            channel.node = bone;
            channel.path = AnimationPath::Rotation;
            channel.interpolation = Interpolation::Linear;
            channel.components = 4;
            for (int k = 0; k <= kKeys; ++k) {
                const float phase = static_cast<float>(k) / kKeys;
                const Quat q = Quat::fromAxisAngle(axis, angleAt(bone, phase));
                channel.times.push_back(phase * duration);
                channel.values.insert(channel.values.end(), {q.x, q.y, q.z, q.w});
            }
            clip.channels.push_back(std::move(channel));
        }
        return clip;
    };
    // A wave travelling up the tube: every bone swings like the one below
    // it, a little later.
    rig.animations.push_back(makeClip("wave", 2.4f, {0.0f, 0.0f, 1.0f}, [](int bone, float phase) {
        return 0.28f * std::sin(2.0f * kPi * phase - 0.7f * static_cast<float>(bone));
    }));
    // Curl up and straighten out again.
    rig.animations.push_back(makeClip("curl", 3.0f, {1.0f, 0.0f, 0.0f}, [](int bone, float phase) {
        return bone == 0 ? 0.0f : 0.55f * 0.5f * (1.0f - std::cos(2.0f * kPi * phase));
    }));

    model.textures.push_back(makeGridTexture(256, 2, {0.15f, 0.55f, 0.5f}, {0.95f, 0.85f, 0.6f}));
    model.textures.push_back(makeStudNormalMap(256, 2));
    Material m;
    m.name = "tentacle";
    m.baseColorTexture = 0;
    m.normalTexture = 1;
    m.roughness = 0.35f;
    m.doubleSided = true;  // the tube is open at the bottom
    model.materials.push_back(m);

    model.motionBounds = computeMotionBounds(model);
    return model;
}

}  // namespace

std::vector<std::string> demoModelNames() { return {"knot", "cube", "sphere", "tentacle", "plane"}; }

Model makeDemoModel(const std::string& name) {
    Model model;
    if (name == "cube") model = makeCube();
    else if (name == "sphere") model = makeSphere();
    else if (name == "knot") model = makeKnot();
    else if (name == "tentacle") model = makeTentacle();
    else if (name == "plane") model = makePlane();
    finalizeModel(model);
    return model;
}

}  // namespace termesh
