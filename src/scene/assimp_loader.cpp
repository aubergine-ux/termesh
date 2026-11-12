// The optional Assimp backend: FBX, COLLADA, 3DS, Blender and several
// dozen more formats, by way of the Open Asset Import Library.
//
// termesh's own loaders are written out in full because reading a format is
// part of what this project is for. This file is the opposite bargain: one
// dependency in exchange for formats that would each be a project of their
// own. It is compiled only with -DTERMESH_ASSIMP=ON, and loadModel turns to
// it only for extensions that nothing built in can read.
//
// What comes through: geometry (with every node's transform applied),
// normals, texture coordinates, tangents, vertex colours, and materials
// with their base colour, normal and emissive textures. What does not:
// animation and skinning - Assimp is asked to flatten the scene, which is
// what makes a file from any of those formats look like a termesh Model.
#include <algorithm>
#include <map>

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "scene/loader.hpp"

namespace termesh {

namespace {

struct AssimpContext {
    LoadResult& result;
    const aiScene* scene;
    std::string directory;
    std::map<std::pair<std::string, bool>, int> textureCache;

    // A texture is either stored inside the file or named by a path.
    int texture(const aiMaterial& material, aiTextureType type, bool srgb) {
        aiString name;
        if (material.GetTextureCount(type) == 0 || material.GetTexture(type, 0, &name) != AI_SUCCESS) return -1;
        const auto key = std::make_pair(std::string(name.C_Str()), srgb);
        if (const auto found = textureCache.find(key); found != textureCache.end()) return found->second;

        Texture decoded;
        if (const aiTexture* embedded = scene->GetEmbeddedTexture(name.C_Str())) {
            if (embedded->mHeight == 0) {
                // A whole image file (PNG, JPEG, ...) of mWidth bytes.
                decoded = loadTextureFromMemory(reinterpret_cast<const unsigned char*>(embedded->pcData),
                                                embedded->mWidth, srgb);
            } else {
                // Raw texels, stored blue-green-red-alpha.
                std::vector<Color8> pixels(static_cast<std::size_t>(embedded->mWidth) * embedded->mHeight);
                for (std::size_t i = 0; i < pixels.size(); ++i) {
                    const aiTexel& t = embedded->pcData[i];
                    pixels[i] = {t.r, t.g, t.b, t.a};
                }
                decoded = Texture(static_cast<int>(embedded->mWidth), static_cast<int>(embedded->mHeight),
                                  std::move(pixels), srgb);
            }
        } else {
            std::string path = name.C_Str();
            std::replace(path.begin(), path.end(), '\\', '/');
            decoded = loadTexture(directory + path, srgb);
            // Files exported on another machine often carry absolute paths;
            // try the bare file name next to the model.
            if (decoded.empty()) decoded = loadTexture(directory + detail::fileNameOf(path), srgb);
        }

        int index = -1;
        if (decoded.empty()) {
            result.warnings.push_back(std::string("could not load texture '") + name.C_Str() + "'");
        } else {
            decoded.name = detail::fileNameOf(name.C_Str());
            index = static_cast<int>(result.model.textures.size());
            result.model.textures.push_back(std::move(decoded));
        }
        textureCache[key] = index;
        return index;
    }

    void materials() {
        for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial& src = *scene->mMaterials[i];
            Material m;
            aiString name;
            if (src.Get(AI_MATKEY_NAME, name) == AI_SUCCESS) m.name = name.C_Str();

            // Newer formats describe a PBR material; older ones a diffuse
            // colour, which is the nearest thing to a base colour.
            aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
            bool haveColor = false;
#ifdef AI_MATKEY_BASE_COLOR
            haveColor = src.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS;
#endif
            if (!haveColor) haveColor = src.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS;
            if (haveColor) m.baseColor = {color.r, color.g, color.b, color.a};

#ifdef AI_MATKEY_METALLIC_FACTOR
            float metallic = 0.0f, roughness = 0.6f;
            if (src.Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) m.metallic = metallic;
            if (src.Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) m.roughness = roughness;
#endif
            aiColor3D emissive(0.0f, 0.0f, 0.0f);
            if (src.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
                m.emissive = {emissive.r, emissive.g, emissive.b};

            float opacity = 1.0f;
            if (src.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f) {
                m.baseColor.w *= opacity;
                m.alphaMode = AlphaMode::Blend;
            }
            int twoSided = 0;
            if (src.Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS) m.doubleSided = twoSided != 0;

            m.baseColorTexture = texture(src, aiTextureType_BASE_COLOR, true);
            if (m.baseColorTexture < 0) m.baseColorTexture = texture(src, aiTextureType_DIFFUSE, true);
            m.normalTexture = texture(src, aiTextureType_NORMALS, false);
            m.emissiveTexture = texture(src, aiTextureType_EMISSIVE, true);
            result.model.materials.push_back(std::move(m));
        }
    }

    void meshes() {
        for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh& src = *scene->mMeshes[i];
            if (!(src.mPrimitiveTypes & aiPrimitiveType_TRIANGLE) || !src.HasPositions()) continue;

            Mesh mesh;
            mesh.name = src.mName.C_Str();
            mesh.material = static_cast<int>(src.mMaterialIndex);
            mesh.hasUVs = src.HasTextureCoords(0);
            mesh.vertices.resize(src.mNumVertices);
            if (src.HasVertexColors(0)) mesh.colors.resize(src.mNumVertices);
            for (unsigned v = 0; v < src.mNumVertices; ++v) {
                Vertex& out = mesh.vertices[v];
                out.position = {src.mVertices[v].x, src.mVertices[v].y, src.mVertices[v].z};
                if (src.HasNormals())
                    out.normal = normalize(Vec3{src.mNormals[v].x, src.mNormals[v].y, src.mNormals[v].z});
                if (mesh.hasUVs) out.uv = {src.mTextureCoords[0][v].x, src.mTextureCoords[0][v].y};
                if (src.HasTangentsAndBitangents()) {
                    const Vec3 t{src.mTangents[v].x, src.mTangents[v].y, src.mTangents[v].z};
                    const Vec3 b{src.mBitangents[v].x, src.mBitangents[v].y, src.mBitangents[v].z};
                    // The sign records whether the bitangent is the cross
                    // product of normal and tangent, or its opposite.
                    out.tangent =
                        Vec4(normalize(t, {1.0f, 0.0f, 0.0f}), dot(cross(out.normal, t), b) < 0.0f ? -1.0f : 1.0f);
                }
                if (!mesh.colors.empty())
                    mesh.colors[v] = {src.mColors[0][v].r, src.mColors[0][v].g, src.mColors[0][v].b};
            }
            for (unsigned f = 0; f < src.mNumFaces; ++f) {
                const aiFace& face = src.mFaces[f];
                if (face.mNumIndices != 3) continue;
                bool valid = true;
                for (unsigned k = 0; k < 3; ++k) valid = valid && face.mIndices[k] < src.mNumVertices;
                if (valid)
                    mesh.indices.insert(mesh.indices.end(), {face.mIndices[0], face.mIndices[1], face.mIndices[2]});
            }
            if (mesh.indices.empty()) continue;
            if (!src.HasNormals()) computeNormals(mesh);
            if (!src.HasTangentsAndBitangents() && mesh.hasUVs) computeTangents(mesh);
            result.model.meshes.push_back(std::move(mesh));
        }
    }
};

}  // namespace

LoadResult loadWithAssimp(const std::string& path) {
    LoadResult result;
    Assimp::Importer importer;
    // Triangulate everything, apply every node's transform to its vertices
    // (which also discards the node hierarchy and animation), split meshes
    // by primitive type so points and lines can be dropped, and flip the
    // texture coordinates to termesh's top-left origin.
    const aiScene* scene = importer.ReadFile(
        path, aiProcess_Triangulate | aiProcess_PreTransformVertices | aiProcess_JoinIdenticalVertices |
                  aiProcess_SortByPType | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace | aiProcess_FlipUVs);
    if (!scene || !scene->mRootNode) {
        result.error = "cannot load '" + path + "': " + importer.GetErrorString();
        return result;
    }

    AssimpContext ctx{result, scene, detail::directoryOf(path), {}};
    result.model.name = detail::fileNameOf(path);
    ctx.materials();
    ctx.meshes();
    if (result.model.meshes.empty()) {
        result.error = "'" + path + "' contains no triangle meshes";
        return result;
    }
    if (scene->mNumAnimations > 0)
        result.warnings.push_back("animations are not imported through the Assimp backend; the model is shown at rest");

    finalizeModel(result.model);
    result.ok = true;
    return result;
}

}  // namespace termesh
