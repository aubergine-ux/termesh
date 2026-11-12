#include "scene/model.hpp"

#include <algorithm>
#include <cmath>
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

float estimatePointRadius(const Mesh& mesh) {
    const std::size_t count = mesh.vertices.size();
    if (count < 2) return 0.01f;

    // A triangle mesh: its area shared out among its vertices.
    if (!mesh.indices.empty()) {
        double area = 0.0;
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const Vec3 a = mesh.vertices[mesh.indices[i]].position;
            area += 0.5 * length(cross(mesh.vertices[mesh.indices[i + 1]].position - a,
                                       mesh.vertices[mesh.indices[i + 2]].position - a));
        }
        return std::max(0.45f * std::sqrt(static_cast<float>(area / static_cast<double>(count))), 1e-6f);
    }

    // A point cloud: the typical distance from a point to its nearest
    // neighbour. Measuring that exactly needs a spatial index; an estimate
    // is enough here. Take a few hundred points and find each one's nearest
    // neighbour among a subset of the cloud. In a subset the neighbours are
    // further apart than in the whole - for points on a surface, by the
    // square root of the ratio of their numbers - so scale back by that.
    const std::size_t subset = std::min<std::size_t>(count, 20000), samples = std::min<std::size_t>(count, 400);
    const std::size_t subsetStep = count / subset, sampleStep = count / samples;
    std::vector<float> nearest;
    nearest.reserve(samples);
    for (std::size_t s = 0; s < samples; ++s) {
        const std::size_t self = s * sampleStep;
        const Vec3 p = mesh.vertices[self].position;
        float best = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < subset; ++k) {
            const std::size_t other = k * subsetStep;
            if (other == self) continue;
            const float d = lengthSquared(mesh.vertices[other].position - p);
            if (d > 0.0f && d < best) best = d;  // coincident points say nothing about spacing
        }
        if (best < std::numeric_limits<float>::max()) nearest.push_back(std::sqrt(best));
    }
    if (nearest.empty()) return 0.01f;
    std::nth_element(nearest.begin(), nearest.begin() + static_cast<std::ptrdiff_t>(nearest.size() / 2), nearest.end());
    const float spacing =
        nearest[nearest.size() / 2] * std::sqrt(static_cast<float>(subset) / static_cast<float>(count));
    // Half as much again as the typical gap: scattered points are unevenly
    // spaced, and discs of exactly the median gap would leave holes.
    return std::max(spacing * 1.5f, 1e-6f);
}

void shufflePoints(Mesh& mesh) {
    // Fisher-Yates, driven by a small fixed-seed generator so that the same
    // file always loads the same way.
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const bool hasColors = mesh.colors.size() == mesh.vertices.size();
    for (std::size_t i = mesh.vertices.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(next() % i);
        std::swap(mesh.vertices[i - 1], mesh.vertices[j]);
        if (hasColors) std::swap(mesh.colors[i - 1], mesh.colors[j]);
    }
}

void finalizeModel(Model& model) {
    if (model.materials.empty()) model.materials.emplace_back();
    const int count = static_cast<int>(model.materials.size());
    for (Mesh& mesh : model.meshes) {
        if (mesh.material < 0 || mesh.material >= count) mesh.material = 0;
        if (mesh.name.empty()) mesh.name = model.materials[static_cast<std::size_t>(mesh.material)].name;
        if (mesh.colors.size() != mesh.vertices.size()) mesh.colors.clear();
        if (mesh.pointRadius <= 0.0f) mesh.pointRadius = estimatePointRadius(mesh);
    }
    const int textures = static_cast<int>(model.textures.size());
    for (Material& m : model.materials) {
        for (int* slot : {&m.baseColorTexture, &m.normalTexture, &m.emissiveTexture, &m.metallicRoughnessTexture})
            if (*slot >= textures || (*slot >= 0 && model.textures[*slot].empty())) *slot = -1;
    }
}

}  // namespace termesh
