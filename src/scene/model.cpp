#include "scene/model.hpp"

#include <algorithm>
#include <limits>

namespace termesh {

std::size_t Model::triangleCount() const {
    std::size_t n = 0;
    for (const Mesh& mesh : meshes) n += mesh.indices.size() / 3;
    return n;
}

std::size_t Model::vertexCount() const {
    std::size_t n = 0;
    for (const Mesh& mesh : meshes) n += mesh.vertices.size();
    return n;
}

Bounds Model::bounds() const {
    Bounds b;
    b.min = Vec3(std::numeric_limits<float>::max());
    b.max = Vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    for (const Mesh& mesh : meshes)
        for (const Vertex& v : mesh.vertices) {
            b.min = min(b.min, v.position);
            b.max = max(b.max, v.position);
            any = true;
        }
    return any ? b : Bounds{};
}

void computeNormals(Mesh& mesh) {
    for (Vertex& v : mesh.vertices) v.normal = {0.0f, 0.0f, 0.0f};
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        Vertex& a = mesh.vertices[mesh.indices[i]];
        Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        // The cross product's length is twice the triangle's area, so adding
        // it un-normalised weights each face by its size.
        const Vec3 n = cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (Vertex& v : mesh.vertices) v.normal = normalize(v.normal, {0.0f, 1.0f, 0.0f});
}

// For each triangle, solve for the two surface directions T and B along which
// u and v increase:
//
//     edge1 = du1 * T + dv1 * B
//     edge2 = du2 * T + dv2 * B
//
// That is a 2x2 linear system per triangle. The per-triangle results are
// accumulated at the vertices, then T is made perpendicular to the vertex
// normal (Gram-Schmidt) and the handedness is recorded in w.
void computeTangents(Mesh& mesh) {
    std::vector<Vec3> tangents(mesh.vertices.size()), bitangents(mesh.vertices.size());

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        const Vertex& a = mesh.vertices[ia];
        const Vertex& b = mesh.vertices[ib];
        const Vertex& c = mesh.vertices[ic];

        const Vec3 e1 = b.position - a.position, e2 = c.position - a.position;
        const Vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
        const float det = cross(d1, d2);
        if (std::abs(det) < 1e-20f) continue;  // degenerate UVs
        const float inv = 1.0f / det;
        // Image-space v runs downwards, so the bitangent (the "up" direction
        // of the texture) is the direction of *decreasing* v.
        const Vec3 t = (e1 * d2.y - e2 * d1.y) * inv;
        const Vec3 bt = (e1 * d2.x - e2 * d1.x) * inv;

        for (std::uint32_t idx : {ia, ib, ic}) {
            tangents[idx] += t;
            bitangents[idx] += bt;
        }
    }

    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        Vertex& v = mesh.vertices[i];
        const Vec3 n = v.normal;
        Vec3 t = tangents[i] - n * dot(n, tangents[i]);
        if (lengthSquared(t) < 1e-20f) {
            // No usable UV gradient: pick any direction perpendicular to n.
            const Vec3 axis = std::abs(n.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
            t = cross(axis, n);
        }
        t = normalize(t);
        const float handedness = dot(cross(n, t), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        v.tangent = Vec4(t, handedness);
    }
}

void finalizeModel(Model& model) {
    if (model.materials.empty()) model.materials.emplace_back();
    const int count = static_cast<int>(model.materials.size());
    for (Mesh& mesh : model.meshes)
        if (mesh.material < 0 || mesh.material >= count) mesh.material = 0;
    const int textures = static_cast<int>(model.textures.size());
    for (Material& m : model.materials) {
        for (int* slot : {&m.baseColorTexture, &m.normalTexture, &m.emissiveTexture, &m.metallicRoughnessTexture})
            if (*slot >= textures || (*slot >= 0 && model.textures[*slot].empty())) *slot = -1;
    }
}

}  // namespace termesh
