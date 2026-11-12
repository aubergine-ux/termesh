// STL: the format of 3D printing. Nothing but triangles.
//
// There are two encodings and no reliable way to tell them apart from the
// first bytes (a binary file's 80-byte header is free text and often begins
// with "solid", which is also how every ASCII file begins):
//
//   binary   80 bytes of header, a 32-bit triangle count, then 50 bytes per
//            triangle: a normal, three corners (twelve 32-bit floats in
//            all) and two bytes of padding.
//   ASCII    solid NAME / facet normal nx ny nz / outer loop /
//            vertex x y z (three times) / endloop / endfacet / endsolid
//
// The test that works is arithmetic: if the file is exactly as long as its
// triangle count says a binary file should be, it is binary.
//
// STL has no shared vertices, no texture coordinates and no materials. Each
// triangle's three corners become three vertices carrying the triangle's
// own normal, so a model looks the way its facets are: flat.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "scene/loader.hpp"

namespace termesh {

namespace {

// Add a triangle with its geometric normal. STL files store a normal per
// facet, but it is redundant - the corner order already says which way the
// facet faces - and often wrong or zero, so it is recomputed.
void addFacet(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 cr = cross(b - a, c - a);
    if (lengthSquared(cr) <= 0.0f) return;  // no area: nothing to draw
    const Vec3 n = normalize(cr);
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const Vec3 p : {a, b, c}) {
        Vertex v;
        v.position = p;
        v.normal = n;
        mesh.vertices.push_back(v);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

float readFloat(const unsigned char* p) {
    // Little-endian IEEE 754, assembled explicitly so the result does not
    // depend on the machine doing the reading.
    const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

LoadResult loadStl(const std::string& path) {
    LoadResult result;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "cannot open '" + path + "'";
        return result;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t size = data.size();

    Mesh mesh;
    bool binary = false;
    if (size >= 84) {
        const std::uint64_t count =
            static_cast<std::uint64_t>(bytes[80]) | (static_cast<std::uint64_t>(bytes[81]) << 8) |
            (static_cast<std::uint64_t>(bytes[82]) << 16) | (static_cast<std::uint64_t>(bytes[83]) << 24);
        binary = size == 84 + count * 50;
        if (binary) {
            mesh.vertices.reserve(static_cast<std::size_t>(count) * 3);
            mesh.indices.reserve(static_cast<std::size_t>(count) * 3);
            for (std::uint64_t i = 0; i < count; ++i) {
                const unsigned char* p = bytes + 84 + i * 50 + 12;  // skip the stored normal
                Vec3 corner[3];
                for (int k = 0; k < 3; ++k)
                    corner[k] = {readFloat(p + k * 12), readFloat(p + k * 12 + 4), readFloat(p + k * 12 + 8)};
                if (finite(corner[0]) && finite(corner[1]) && finite(corner[2]))
                    addFacet(mesh, corner[0], corner[1], corner[2]);
            }
        }
    }

    if (!binary) {
        // ASCII. Only the "vertex" lines matter: every three make a facet.
        const std::string text(data.begin(), data.end());
        if (text.find("facet") == std::string::npos) {
            result.error = "'" + path + "' is not an STL file";
            return result;
        }
        Vec3 corner[3];
        int have = 0;
        std::size_t pos = 0;
        while ((pos = text.find("vertex", pos)) != std::string::npos) {
            pos += 6;
            const char* cursor = text.c_str() + pos;
            Vec3 v;
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                char* end = nullptr;
                v[k] = std::strtof(cursor, &end);
                ok = end != cursor;
                cursor = end;
            }
            if (!ok || !finite(v)) {
                result.warnings.push_back("a malformed vertex was skipped");
                have = 0;
                continue;
            }
            corner[have++] = v;
            if (have == 3) {
                addFacet(mesh, corner[0], corner[1], corner[2]);
                have = 0;
            }
        }
        if (have != 0) result.warnings.push_back("the file ends in the middle of a facet");
    }

    if (mesh.indices.empty()) {
        result.error = "'" + path + "' contains no triangles";
        return result;
    }

    Material material;
    material.name = "stl";
    material.baseColor = {0.72f, 0.74f, 0.78f, 1.0f};
    material.roughness = 0.55f;
    result.model.name = detail::fileNameOf(path);
    result.model.materials.push_back(material);
    result.model.meshes.push_back(std::move(mesh));
    finalizeModel(result.model);
    result.ok = true;
    return result;
}

}  // namespace termesh
