// Model: the geometry, materials and textures loaded from a file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/texture.hpp"
#include "math/vec.hpp"
#include "scene/rig.hpp"

namespace termesh {

struct Vertex {
    Vec3 position;
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Vec2 uv;
    // Tangent: the surface direction in which the texture's u coordinate
    // increases. w is +1 or -1 and gives the handedness of the bitangent,
    // which is reconstructed as cross(normal, tangent) * w.
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

enum class AlphaMode {
    Opaque,  // alpha is ignored
    Mask,    // a cut-out: pixels below alphaCutoff are not drawn at all
    Blend,   // translucent: mixed with whatever is behind
};

struct Material {
    std::string name;
    Vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};  // linear RGB + alpha
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.6f;
    float normalScale = 1.0f;

    // Indices into Model::textures, or -1 for none.
    int baseColorTexture = -1;
    int normalTexture = -1;
    int emissiveTexture = -1;
    int metallicRoughnessTexture = -1;  // G = roughness, B = metallic (glTF)
    bool occlusionInRed = false;        // ...and R = ambient occlusion

    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

// One draw call's worth of triangles sharing a material. Vertices are stored
// in world space: loaders bake node transforms in. Meshes of an animated
// model also carry `rig`, from which the vertices are recomputed every time
// the pose changes (see anim/skinning.hpp).
struct Mesh {
    std::string name;  // from the file where it gives one; otherwise the material's name
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;  // three per triangle, CCW front faces
    int material = 0;
    bool hasUVs = false;
    MeshRig rig;  // empty for static meshes

    // Optional per-vertex colours (linear RGB), one per vertex. They
    // multiply the material's base colour. Scans and point clouds usually
    // carry their colour this way rather than in a texture.
    std::vector<Vec3> colors;

    // A mesh with vertices but no triangles is a point cloud: each vertex is
    // drawn as a small disc facing the camera (a "splat") of this radius,
    // in the mesh's units. Filled in by finalizeModel if left at 0.
    float pointRadius = 0.0f;
    bool isPointCloud() const { return indices.empty() && !vertices.empty(); }
};

struct Bounds {
    Vec3 min{0.0f, 0.0f, 0.0f}, max{0.0f, 0.0f, 0.0f};
    Vec3 center() const { return (min + max) * 0.5f; }
    float radius() const { return length(max - min) * 0.5f; }
};

struct Model {
    std::string name;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;  // never empty after loading
    std::vector<Texture> textures;
    Rig rig;  // empty unless the model is animated
    // For animated models: a box around everything the animations reach, so
    // the camera can be placed where the model never leaves the picture.
    Bounds motionBounds;

    bool animated() const { return !rig.animations.empty(); }
    // What the camera should fit in view.
    Bounds framingBounds() const { return animated() ? motionBounds : bounds(); }

    std::size_t triangleCount() const;
    std::size_t vertexCount() const;
    Bounds bounds() const;
};

// Fill in smooth vertex normals by averaging the (area-weighted) normals of
// the faces around each vertex.
void computeNormals(Mesh& mesh);

// Fill in vertex tangents from the UV layout, for normal mapping.
void computeTangents(Mesh& mesh);

// Replace every number in the model that is infinite, not a number, or too
// large to calculate with by something harmless, and return how many were.
// Files are not to be trusted: a damaged one can hold any bit pattern where
// a coordinate should be, and one such value would otherwise spread through
// everything computed from it. Called by finalizeModel.
std::size_t sanitizeModel(Model& model);

// Make sure the model has at least one material and that every mesh refers to
// a valid one, and work out a splat radius for each mesh.
void finalizeModel(Model& model);

// A radius for drawing a mesh's vertices as splats: about the typical gap
// between neighbouring points, so that the discs just overlap.
float estimatePointRadius(const Mesh& mesh);

// Put a point cloud's points in random order. Any prefix of a shuffled
// cloud is an even sample of the whole, which is what lets a distant cloud
// be drawn with only some of its points.
void shufflePoints(Mesh& mesh);

}  // namespace termesh
