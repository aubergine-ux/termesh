// Model: the geometry, materials and textures loaded from a file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/texture.hpp"
#include "math/vec.hpp"

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

enum class AlphaMode { Opaque, Mask };

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
    bool occlusionInRed = false;         // ...and R = ambient occlusion

    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

// One draw call's worth of triangles sharing a material. Vertices are stored
// in world space: loaders bake node transforms in.
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;  // three per triangle, CCW front faces
    int material = 0;
    bool hasUVs = false;
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

    std::size_t triangleCount() const;
    std::size_t vertexCount() const;
    Bounds bounds() const;
};

// Fill in smooth vertex normals by averaging the (area-weighted) normals of
// the faces around each vertex.
void computeNormals(Mesh& mesh);

// Fill in vertex tangents from the UV layout, for normal mapping.
void computeTangents(Mesh& mesh);

// Make sure the model has at least one material and that every mesh refers to
// a valid one.
void finalizeModel(Model& model);

}  // namespace termesh
