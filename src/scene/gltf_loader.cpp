// glTF 2.0, via cgltf.
//
// cgltf handles the container (JSON, .glb chunks, buffers, accessors); this
// file turns its scene graph into termesh's flat list of world-space meshes:
//
//   * copy the node hierarchy, skins and animations into a Rig;
//   * for each mesh primitive, read positions / normals / UVs / tangents in
//     the mesh's own space, plus bone weights and morph targets;
//   * pose the model once (anim/skinning.hpp) to get world-space vertices;
//   * translate metallic-roughness materials and load the textures they use.
//
// A file without animations has no further use for the rig, so it is dropped
// after that first pose and the model is exactly as if the transforms had
// been baked in directly.
//
// Not supported: cameras, lights, vertex colours, secondary UV sets, more
// than four bones per vertex and compressed geometry (Draco / meshopt).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cgltf/cgltf.h>

#include "anim/skinning.hpp"
#include "math/mat.hpp"
#include "scene/loader.hpp"

namespace termesh {

namespace {

WrapMode wrapModeFromGl(cgltf_int mode) {
    switch (mode) {
        case 33071: return WrapMode::Clamp;   // CLAMP_TO_EDGE
        case 33648: return WrapMode::Mirror;  // MIRRORED_REPEAT
        default: return WrapMode::Repeat;
    }
}

struct GltfContext {
    LoadResult& result;
    const cgltf_data* data;
    const cgltf_options* options;
    std::string directory;
    // The same image can be needed both as sRGB and as linear data.
    std::map<std::pair<const cgltf_texture*, bool>, int> textureCache;
    bool warnedCompression = false;
    bool warnedInfluences = false;
    std::vector<Mat4> restWorld;  // node-to-world matrices in the rest pose

    static Mat4 toMat4(const float* m) {  // column-major, like Mat4
        Mat4 out;
        for (int c = 0; c < 4; ++c) out.c[c] = {m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]};
        return out;
    }

    int nodeIndex(const cgltf_node* node) const { return node ? static_cast<int>(node - data->nodes) : -1; }

    // Copy the node hierarchy, skins and animations.
    void loadRig() {
        Rig& rig = result.model.rig;
        rig.nodes.resize(data->nodes_count);
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            const cgltf_node& src = data->nodes[i];
            RigNode& node = rig.nodes[i];
            if (src.name) node.name = src.name;
            node.parent = nodeIndex(src.parent);

            if (src.has_matrix) {
                // A node may give a matrix instead of separate translation,
                // rotation and scale. Take it apart: the last column is the
                // translation, the lengths of the first three are the scale,
                // and what is left after dividing those out is the rotation.
                // (A mirrored matrix gets one negative scale; shear, which
                // glTF forbids, would be lost.)
                Mat4 m = toMat4(src.matrix);
                node.rest.translation = m.c[3].xyz();
                Vec3 x = m.c[0].xyz(), y = m.c[1].xyz(), z = m.c[2].xyz();
                Vec3 scale{length(x), length(y), length(z)};
                if (dot(cross(x, y), z) < 0.0f) scale.x = -scale.x;
                node.rest.scale = scale;
                Mat4 r;
                r.c[0] = Vec4(scale.x != 0.0f ? x / scale.x : Vec3{1.0f, 0.0f, 0.0f}, 0.0f);
                r.c[1] = Vec4(scale.y != 0.0f ? y / scale.y : Vec3{0.0f, 1.0f, 0.0f}, 0.0f);
                r.c[2] = Vec4(scale.z != 0.0f ? z / scale.z : Vec3{0.0f, 0.0f, 1.0f}, 0.0f);
                node.rest.rotation = quatFromMat4(r);
            } else {
                if (src.has_translation)
                    node.rest.translation = {src.translation[0], src.translation[1], src.translation[2]};
                if (src.has_rotation)
                    node.rest.rotation =
                        normalize(Quat{src.rotation[0], src.rotation[1], src.rotation[2], src.rotation[3]});
                if (src.has_scale) node.rest.scale = {src.scale[0], src.scale[1], src.scale[2]};
            }

            // Default morph weights: the node's own, else the mesh's.
            if (src.weights_count) node.weights.assign(src.weights, src.weights + src.weights_count);
            else if (src.mesh && src.mesh->weights_count)
                node.weights.assign(src.mesh->weights, src.mesh->weights + src.mesh->weights_count);
            else if (src.mesh && src.mesh->primitives_count)
                node.weights.assign(src.mesh->primitives[0].targets_count, 0.0f);
        }

        // Parents before children. Validation has already rejected cycles.
        rig.order.reserve(rig.nodes.size());
        std::vector<int> stack;
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) stack.push_back(static_cast<int>(i));
        while (!stack.empty()) {
            const int index = stack.back();
            stack.pop_back();
            rig.order.push_back(index);
            const cgltf_node& node = data->nodes[index];
            for (cgltf_size c = 0; c < node.children_count; ++c) stack.push_back(nodeIndex(node.children[c]));
        }

        rig.skins.resize(data->skins_count);
        for (cgltf_size i = 0; i < data->skins_count; ++i) {
            const cgltf_skin& src = data->skins[i];
            Skin& skin = rig.skins[i];
            skin.joints.resize(src.joints_count);
            skin.inverseBind.resize(src.joints_count);  // identity when the file gives none
            for (cgltf_size j = 0; j < src.joints_count; ++j) {
                skin.joints[j] = nodeIndex(src.joints[j]);
                float m[16];
                if (src.inverse_bind_matrices && j < src.inverse_bind_matrices->count &&
                    cgltf_accessor_read_float(src.inverse_bind_matrices, j, m, 16))
                    skin.inverseBind[j] = toMat4(m);
            }
        }

        for (cgltf_size i = 0; i < data->animations_count; ++i) loadAnimation(data->animations[i]);
        computeWorldMatrices(rig, restPose(rig), restWorld);
    }

    void loadAnimation(const cgltf_animation& src) {
        Animation animation;
        if (src.name) animation.name = src.name;
        bool skipped = false;

        for (cgltf_size c = 0; c < src.channels_count; ++c) {
            const cgltf_animation_channel& ch = src.channels[c];
            const cgltf_animation_sampler* sampler = ch.sampler;
            if (!ch.target_node || !sampler || !sampler->input || !sampler->output) continue;  // drives an extension

            AnimationChannel channel;
            channel.node = nodeIndex(ch.target_node);
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: channel.path = AnimationPath::Translation; break;
                case cgltf_animation_path_type_rotation: channel.path = AnimationPath::Rotation; break;
                case cgltf_animation_path_type_scale: channel.path = AnimationPath::Scale; break;
                case cgltf_animation_path_type_weights: channel.path = AnimationPath::Weights; break;
                default: continue;
            }
            switch (sampler->interpolation) {
                case cgltf_interpolation_type_step: channel.interpolation = Interpolation::Step; break;
                case cgltf_interpolation_type_cubic_spline: channel.interpolation = Interpolation::CubicSpline; break;
                default: channel.interpolation = Interpolation::Linear; break;
            }

            const std::size_t keys = sampler->input->count;
            const std::size_t perKey = channel.interpolation == Interpolation::CubicSpline ? 3 : 1;
            const std::size_t floats = sampler->output->count * cgltf_num_components(sampler->output->type);
            if (keys == 0 || floats == 0 || floats % (keys * perKey) != 0) {
                skipped = true;
                continue;
            }
            channel.components = static_cast<int>(floats / (keys * perKey));
            const int expected =
                channel.path == AnimationPath::Rotation ? 4 : (channel.path == AnimationPath::Weights ? 0 : 3);
            if (expected && channel.components != expected) {
                skipped = true;
                continue;
            }

            channel.times.resize(keys);
            channel.values.resize(floats);
            if (cgltf_accessor_unpack_floats(sampler->input, channel.times.data(), keys) != keys ||
                cgltf_accessor_unpack_floats(sampler->output, channel.values.data(), floats) != floats ||
                !std::is_sorted(channel.times.begin(), channel.times.end())) {
                skipped = true;
                continue;
            }
            animation.duration = std::max(animation.duration, channel.times.back());
            animation.channels.push_back(std::move(channel));
        }

        if (skipped)
            result.warnings.push_back("animation '" +
                                      (animation.name.empty() ? std::string("(unnamed)") : animation.name) +
                                      "' has malformed channels; they were skipped");
        if (!animation.channels.empty()) result.model.rig.animations.push_back(std::move(animation));
    }

    Texture decodeImage(const cgltf_image& image, bool srgb) {
        // Embedded in a buffer (the normal case for .glb).
        if (image.buffer_view) {
            const cgltf_buffer_view* view = image.buffer_view;
            if (!view->buffer || !view->buffer->data) return {};
            const auto* bytes = static_cast<const unsigned char*>(view->buffer->data) + view->offset;
            return loadTextureFromMemory(bytes, view->size, srgb);
        }
        if (!image.uri) return {};

        // Embedded as a base64 data URI.
        if (std::strncmp(image.uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image.uri, ',');
            if (!comma) return {};
            const char* base64 = comma + 1;
            std::size_t length = std::strlen(base64);
            while (length > 0 && base64[length - 1] == '=') --length;
            const std::size_t size = length * 3 / 4;
            void* decoded = nullptr;
            if (cgltf_load_buffer_base64(options, size, base64, &decoded) != cgltf_result_success) return {};
            Texture tex = loadTextureFromMemory(static_cast<const unsigned char*>(decoded), size, srgb);
            std::free(decoded);
            return tex;
        }

        // An external file next to the .gltf. URIs are percent-encoded.
        std::string uri = image.uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::strlen(uri.c_str()));
        return loadTexture(directory + uri, srgb);
    }

    int texture(const cgltf_texture_view& view, bool srgb) {
        const cgltf_texture* tex = view.texture;
        if (!tex) return -1;
        const auto key = std::make_pair(tex, srgb);
        if (auto it = textureCache.find(key); it != textureCache.end()) return it->second;

        int index = -1;
        const cgltf_image* image = tex->image;
        if (image) {
            Texture decoded = decodeImage(*image, srgb);
            if (decoded.empty()) {
                const char* name =
                    image->name ? image->name
                                : (image->uri && std::strncmp(image->uri, "data:", 5) != 0 ? image->uri : "(embedded)");
                result.warnings.push_back(std::string("could not load texture '") + name + "'");
            } else {
                if (tex->sampler)
                    decoded.setWrap(wrapModeFromGl(tex->sampler->wrap_s), wrapModeFromGl(tex->sampler->wrap_t));
                if (image->name) decoded.name = image->name;
                else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) decoded.name = image->uri;
                index = static_cast<int>(result.model.textures.size());
                result.model.textures.push_back(std::move(decoded));
            }
        } else {
            result.warnings.push_back("texture uses an unsupported image format (KTX2 / WebP)");
        }
        textureCache[key] = index;
        return index;
    }

    void loadMaterials() {
        for (cgltf_size i = 0; i < data->materials_count; ++i) {
            const cgltf_material& src = data->materials[i];
            Material m;
            if (src.name) m.name = src.name;

            if (src.has_pbr_metallic_roughness) {
                const auto& pbr = src.pbr_metallic_roughness;
                // glTF colour factors are already linear.
                m.baseColor = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
                               pbr.base_color_factor[3]};
                m.metallic = pbr.metallic_factor;
                m.roughness = pbr.roughness_factor;
                m.baseColorTexture = texture(pbr.base_color_texture, true);
                m.metallicRoughnessTexture = texture(pbr.metallic_roughness_texture, false);
                // Occlusion is often packed into the red channel of the same
                // image ("ORM" textures); if so we get it for free.
                m.occlusionInRed = m.metallicRoughnessTexture >= 0 &&
                                   src.occlusion_texture.texture == pbr.metallic_roughness_texture.texture;
            } else if (src.has_pbr_specular_glossiness) {
                // The older specular-glossiness extension: use what maps
                // directly onto our model.
                const auto& sg = src.pbr_specular_glossiness;
                m.baseColor = {sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2], sg.diffuse_factor[3]};
                m.roughness = 1.0f - sg.glossiness_factor;
                m.baseColorTexture = texture(sg.diffuse_texture, true);
            }

            m.normalTexture = texture(src.normal_texture, false);
            m.normalScale = src.normal_texture.texture ? src.normal_texture.scale : 1.0f;

            float strength = 1.0f;
            if (src.has_emissive_strength) strength = src.emissive_strength.emissive_strength;
            m.emissive = Vec3{src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]} * strength;
            m.emissiveTexture = texture(src.emissive_texture, true);

            m.doubleSided = src.double_sided != 0;
            if (src.alpha_mode == cgltf_alpha_mode_mask) {
                m.alphaMode = AlphaMode::Mask;
                m.alphaCutoff = src.alpha_cutoff;
            } else if (src.alpha_mode == cgltf_alpha_mode_blend) {
                // There is no blending in this renderer. Treat translucent
                // materials as cut-outs that only drop (nearly) invisible
                // texels, so glass stays visible and decals keep their shape.
                m.alphaMode = AlphaMode::Mask;
                m.alphaCutoff = 0.1f;
                m.baseColor.w = std::max(m.baseColor.w, 0.11f);
            }
            result.model.materials.push_back(std::move(m));
        }
    }

    const cgltf_accessor* findAttribute(const cgltf_primitive& prim, cgltf_attribute_type type, int index = 0) {
        for (cgltf_size i = 0; i < prim.attributes_count; ++i)
            if (prim.attributes[i].type == type && prim.attributes[i].index == index) return prim.attributes[i].data;
        return nullptr;
    }

    void loadPrimitive(const cgltf_primitive& prim, const cgltf_node& node) {
        if (prim.has_draco_mesh_compression) {
            if (!warnedCompression) result.warnings.push_back("Draco-compressed meshes are not supported; skipped");
            warnedCompression = true;
            return;
        }
        if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
            prim.type != cgltf_primitive_type_triangle_fan)
            return;  // points and lines

        const cgltf_accessor* positions = findAttribute(prim, cgltf_attribute_type_position);
        if (!positions || positions->count == 0) return;
        const cgltf_accessor* normals = findAttribute(prim, cgltf_attribute_type_normal);
        const cgltf_accessor* uvs = findAttribute(prim, cgltf_attribute_type_texcoord);
        const cgltf_accessor* tangents = findAttribute(prim, cgltf_attribute_type_tangent);
        const std::size_t count = positions->count;

        // cgltf unpacks any accessor layout (interleaved, normalised
        // integers, sparse) into plain floats.
        std::vector<float> scratch;
        auto unpack = [&](const cgltf_accessor* accessor, int components) -> const float* {
            if (!accessor || accessor->count != count) return nullptr;
            scratch.resize(count * static_cast<std::size_t>(components));
            const cgltf_size got = cgltf_accessor_unpack_floats(accessor, scratch.data(), scratch.size());
            return got == scratch.size() ? scratch.data() : nullptr;
        };

        Mesh mesh;
        mesh.vertices.resize(count);
        mesh.material = prim.material ? static_cast<int>(prim.material - data->materials) : -1;

        const float* p = unpack(positions, 3);
        if (!p) return;
        // Everything is read in the mesh's own space; posing the model at
        // the end of loading moves it into world space.
        for (std::size_t i = 0; i < count; ++i) mesh.vertices[i].position = {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]};

        const bool hasNormals = normals && unpack(normals, 3);
        if (hasNormals)
            for (std::size_t i = 0; i < count; ++i)
                mesh.vertices[i].normal = normalize(Vec3{scratch[i * 3], scratch[i * 3 + 1], scratch[i * 3 + 2]});

        if (uvs && unpack(uvs, 2)) {
            mesh.hasUVs = true;
            for (std::size_t i = 0; i < count; ++i) mesh.vertices[i].uv = {scratch[i * 2], scratch[i * 2 + 1]};
        }

        const bool hasTangents = tangents && unpack(tangents, 4);
        if (hasTangents) {
            for (std::size_t i = 0; i < count; ++i) {
                const Vec3 t{scratch[i * 4], scratch[i * 4 + 1], scratch[i * 4 + 2]};
                mesh.vertices[i].tangent = Vec4(normalize(t, {1.0f, 0.0f, 0.0f}), scratch[i * 4 + 3]);
            }
        }

        // Indices - or, for non-indexed geometry, just 0, 1, 2, ...
        std::vector<std::uint32_t> raw;
        if (prim.indices) {
            raw.resize(prim.indices->count);
            for (cgltf_size i = 0; i < prim.indices->count; ++i) {
                raw[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i));
                if (raw[i] >= count) return;  // corrupt file: refuse rather than read out of bounds
            }
        } else {
            raw.resize(count);
            for (std::size_t i = 0; i < count; ++i) raw[i] = static_cast<std::uint32_t>(i);
        }

        if (prim.type == cgltf_primitive_type_triangles) {
            raw.resize(raw.size() / 3 * 3);
            mesh.indices = std::move(raw);
        } else if (prim.type == cgltf_primitive_type_triangle_strip) {
            for (std::size_t i = 0; i + 2 < raw.size(); ++i) {
                // Every other triangle in a strip is wound backwards.
                const bool odd = i & 1;
                mesh.indices.push_back(raw[i]);
                mesh.indices.push_back(raw[i + (odd ? 2 : 1)]);
                mesh.indices.push_back(raw[i + (odd ? 1 : 2)]);
            }
        } else {  // fan
            for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
                mesh.indices.push_back(raw[0]);
                mesh.indices.push_back(raw[i]);
                mesh.indices.push_back(raw[i + 1]);
            }
        }
        if (mesh.indices.empty()) return;

        if (!hasNormals) computeNormals(mesh);
        if (!hasTangents && mesh.hasUVs) computeTangents(mesh);

        // ---- rig data: the rest shape, bone weights and morph targets ----
        MeshRig& rig = mesh.rig;
        rig.node = nodeIndex(&node);
        rig.positions.resize(count);
        rig.normals.resize(count);
        rig.tangents.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            rig.positions[i] = mesh.vertices[i].position;
            rig.normals[i] = mesh.vertices[i].normal;
            rig.tangents[i] = mesh.vertices[i].tangent.xyz();
        }

        const cgltf_accessor* joints = findAttribute(prim, cgltf_attribute_type_joints);
        const cgltf_accessor* weights = findAttribute(prim, cgltf_attribute_type_weights);
        const Skin* skin =
            node.skin ? &result.model.rig.skins[static_cast<std::size_t>(node.skin - data->skins)] : nullptr;
        if (skin && !skin->joints.empty() && joints && joints->count == count && weights && unpack(weights, 4)) {
            rig.skin = static_cast<int>(node.skin - data->skins);
            rig.joints.resize(count);
            rig.weights.resize(count);
            const cgltf_uint lastJoint = static_cast<cgltf_uint>(skin->joints.size() - 1);
            for (std::size_t i = 0; i < count; ++i) {
                cgltf_uint j[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(joints, i, j, 4);
                Vec4 w{scratch[i * 4], scratch[i * 4 + 1], scratch[i * 4 + 2], scratch[i * 4 + 3]};
                // Weights should add up to one; files are not always careful.
                const float sum = w.x + w.y + w.z + w.w;
                w = sum > 1e-6f ? w / sum : Vec4{1.0f, 0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 4; ++k) rig.joints[i][k] = static_cast<std::uint16_t>(std::min(j[k], lastJoint));
                rig.weights[i] = w;
            }
            if (findAttribute(prim, cgltf_attribute_type_joints, 1) && !warnedInfluences) {
                result.warnings.push_back("more than four bones per vertex; only the first four are used");
                warnedInfluences = true;
            }
        }

        rig.targets.resize(prim.targets_count);
        for (cgltf_size t = 0; t < prim.targets_count; ++t) {
            const cgltf_morph_target& src = prim.targets[t];
            for (cgltf_size a = 0; a < src.attributes_count; ++a) {
                std::vector<Vec3>* dst = nullptr;
                switch (src.attributes[a].type) {
                    case cgltf_attribute_type_position: dst = &rig.targets[t].positions; break;
                    case cgltf_attribute_type_normal: dst = &rig.targets[t].normals; break;
                    case cgltf_attribute_type_tangent: dst = &rig.targets[t].tangents; break;
                    default: break;
                }
                if (!dst || src.attributes[a].index != 0 || !unpack(src.attributes[a].data, 3)) continue;
                dst->resize(count);
                for (std::size_t i = 0; i < count; ++i)
                    (*dst)[i] = {scratch[i * 3], scratch[i * 3 + 1], scratch[i * 3 + 2]};
            }
        }

        // A transform with a negative determinant mirrors the mesh, turning
        // counter-clockwise triangles clockwise. Swap them back. (Decided
        // once, from the rest pose: animations that mirror a mesh half-way
        // through are too rare to pay for per frame.)
        Mat4 world = restWorld[static_cast<std::size_t>(rig.node)];
        if (rig.skin >= 0) world = restWorld[static_cast<std::size_t>(skin->joints[0])] * skin->inverseBind[0];
        const Vec3 x = world.c[0].xyz(), y = world.c[1].xyz(), z = world.c[2].xyz();
        if (dot(cross(x, y), z) < 0.0f)
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);

        result.model.meshes.push_back(std::move(mesh));
    }

    void loadNode(const cgltf_node& node) {
        if (node.mesh)
            for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i) loadPrimitive(node.mesh->primitives[i], node);
        for (cgltf_size i = 0; i < node.children_count; ++i) loadNode(*node.children[i]);
    }
};

}  // namespace

LoadResult loadGltf(const std::string& path) {
    LoadResult result;
    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result status = cgltf_parse_file(&options, path.c_str(), &data);
    if (status != cgltf_result_success) {
        result.error = status == cgltf_result_file_not_found ? "cannot open '" + path + "'"
                                                             : "'" + path + "' is not a valid glTF file";
        return result;
    }
    status = cgltf_load_buffers(&options, data, path.c_str());
    if (status != cgltf_result_success) {
        result.error = "could not load the buffers referenced by '" + path + "'";
        cgltf_free(data);
        return result;
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        result.error = "'" + path + "' failed validation (corrupt or truncated)";
        cgltf_free(data);
        return result;
    }

    GltfContext ctx{result, data, &options, detail::directoryOf(path), {}, false, false, {}};
    result.model.name = detail::fileNameOf(path);
    ctx.loadMaterials();
    ctx.loadRig();

    // Draw the default scene, or everything if the file does not name one.
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) ctx.loadNode(*scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) ctx.loadNode(data->nodes[i]);
    }
    cgltf_free(data);

    if (result.model.meshes.empty()) {
        result.error = "'" + path + "' contains no triangle meshes";
        return result;
    }

    // Primitives without a material use glTF's default: plain white.
    const int fallback = static_cast<int>(result.model.materials.size());
    bool needFallback = false;
    for (Mesh& mesh : result.model.meshes)
        if (mesh.material < 0) {
            mesh.material = fallback;
            needFallback = true;
        }
    if (needFallback) {
        Material m;
        m.name = "default";
        m.baseColor = {0.8f, 0.8f, 0.8f, 1.0f};
        result.model.materials.push_back(m);
    }

    // Move everything into world space by posing the model as the file
    // describes it. Without animations that pose is final and the rig has
    // done its job.
    applyPose(result.model, restPose(result.model.rig));
    if (result.model.rig.animations.empty()) {
        result.model.rig = {};
        for (Mesh& mesh : result.model.meshes) mesh.rig = {};
    } else {
        result.model.motionBounds = computeMotionBounds(result.model);
    }

    finalizeModel(result.model);
    result.ok = true;
    return result;
}

}  // namespace termesh
