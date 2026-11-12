// Wavefront OBJ and MTL.
//
// OBJ is a line-based text format. The lines that matter here:
//
//     v  x y z          a position
//     vt u v            a texture coordinate
//     vn x y z          a normal
//     f  a/b/c ...      a polygon; each corner indexes position/uv/normal
//     usemtl name       material for the faces that follow
//     mtllib file.mtl   where the materials are defined
//
// The awkward part is that a face corner indexes the three attribute arrays
// independently, while the renderer wants one index per vertex. So every
// distinct (position, uv, normal) triple becomes one output vertex.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/color.hpp"
#include "scene/loader.hpp"

namespace termesh {

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

// A tiny cursor over one line of text.
struct LineParser {
    const char* p;
    const char* end;

    void skipSpace() {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
    }
    bool atEnd() {
        skipSpace();
        return p >= end;
    }
    // The next whitespace-delimited word.
    std::string_view word() {
        skipSpace();
        const char* start = p;
        while (p < end && *p != ' ' && *p != '\t') ++p;
        return {start, static_cast<std::size_t>(p - start)};
    }
    float number(float fallback = 0.0f) {
        skipSpace();
        if (p >= end) return fallback;
        char* stop = nullptr;
        const float value = std::strtof(p, &stop);
        if (stop == p) {  // not a number: skip the token
            word();
            return fallback;
        }
        p = stop;
        return value;
    }
    // Everything up to the end of the line, without trailing spaces.
    std::string rest() {
        skipSpace();
        const char* stop = end;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t')) --stop;
        return {p, static_cast<std::size_t>(stop - p)};
    }
};

// Calls fn(parser) for every non-empty, non-comment line. The buffer must be
// NUL-terminated (std::string is), because strtof reads up to a terminator.
template <typename Fn>
void forEachLine(const std::string& text, Fn&& fn) {
    const char* const data = text.data();
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        std::size_t end = lineEnd;
        if (end > pos && text[end - 1] == '\r') --end;
        // Look for a comment inside this line only: searching the rest of the
        // file for every line would make loading quadratic.
        if (end > pos)
            if (const void* hash = std::memchr(data + pos, '#', end - pos))
                end = static_cast<std::size_t>(static_cast<const char*>(hash) - data);
        LineParser line{data + pos, data + end};
        if (!line.atEnd()) fn(line);
        pos = lineEnd + 1;
    }
}

// MTL colours are authored by eye on an sRGB display.
Vec3 decodeColor(float r, float g, float b) {
    auto channel = [](float c) { return detail::srgbDecode(std::clamp(c, 0.0f, 1.0f)); };
    return {channel(r), channel(g), channel(b)};
}

// Texture statements can carry options ("-bm 0.5 -s 1 1 1 file.png"). Skip
// them and return the file name plus the bump multiplier if one was given.
std::string parseTextureStatement(LineParser& line, float* bumpScale = nullptr) {
    for (;;) {
        line.skipSpace();
        if (line.p >= line.end || *line.p != '-') break;
        const std::string_view option = line.word();
        int args = 1;
        if (option == "-s" || option == "-o" || option == "-t") args = 3;
        else if (option == "-mm") args = 2;
        else if (option == "-bm") {
            const float value = line.number(1.0f);
            if (bumpScale) *bumpScale = value;
            args = 0;
        }
        for (int i = 0; i < args; ++i) line.word();
    }
    std::string name = line.rest();
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

struct ObjContext {
    LoadResult& result;
    std::string directory;
    std::unordered_map<std::string, int> materialByName;
    std::unordered_map<std::string, int> textureByKey;

    // Load a texture once, however many materials refer to it.
    int texture(const std::string& file, bool srgb, bool asNormalMap) {
        if (file.empty()) return -1;
        const std::string key = file + (asNormalMap ? "|n" : srgb ? "|s" : "|l");
        if (auto it = textureByKey.find(key); it != textureByKey.end()) return it->second;

        Texture tex = loadTexture(directory + file, srgb);
        int index = -1;
        if (tex.empty()) {
            result.warnings.push_back("could not load texture '" + file + "'");
        } else {
            // "Bump maps" in the wild are either real normal maps or
            // greyscale height maps; convert the latter.
            if (asNormalMap && tex.isGreyscale()) tex = heightToNormalMap(tex);
            index = static_cast<int>(result.model.textures.size());
            result.model.textures.push_back(std::move(tex));
        }
        textureByKey[key] = index;
        return index;
    }

    void loadMaterials(const std::string& file) {
        std::string text;
        if (!readFile(directory + file, text)) {
            result.warnings.push_back("could not open material library '" + file + "'");
            return;
        }
        Material* current = nullptr;
        bool explicitRoughness = false;
        forEachLine(text, [&](LineParser& line) {
            const std::string_view key = line.word();
            if (key == "newmtl") {
                const std::string name = line.rest();
                materialByName[name] = static_cast<int>(result.model.materials.size());
                current = &result.model.materials.emplace_back();
                current->name = name;
                explicitRoughness = false;
                return;
            }
            if (!current) return;

            if (key == "Kd") {
                const float r = line.number(), g = line.number(r), b = line.number(r);
                const Vec3 c = decodeColor(r, g, b);
                current->baseColor = {c.x, c.y, c.z, current->baseColor.w};
            } else if (key == "Ke") {
                const float r = line.number(), g = line.number(r), b = line.number(r);
                current->emissive = decodeColor(r, g, b);
            } else if (key == "Ns") {
                // Invert shininessFromRoughness(): s = 2 / r^4 - 2.
                if (!explicitRoughness) {
                    const float shininess = std::max(line.number(), 0.0f);
                    current->roughness = std::clamp(std::pow(2.0f / (shininess + 2.0f), 0.25f), 0.05f, 1.0f);
                }
            } else if (key == "Pr") {
                current->roughness = std::clamp(line.number(), 0.0f, 1.0f);
                explicitRoughness = true;
            } else if (key == "Pm") {
                current->metallic = std::clamp(line.number(), 0.0f, 1.0f);
            } else if (key == "d") {
                current->baseColor.w = line.number(1.0f);
            } else if (key == "Tr") {
                current->baseColor.w = 1.0f - line.number(0.0f);
            } else if (key == "map_Kd") {
                current->baseColorTexture = texture(parseTextureStatement(line), true, false);
                // A base colour texture with holes in it is a cut-out.
                if (current->baseColorTexture >= 0) {
                    const auto& pixels = result.model.textures[current->baseColorTexture].pixels();
                    const bool hasHoles = std::any_of(pixels.begin(), pixels.end(), [](Color8 c) { return c.a < 128; });
                    if (hasHoles) current->alphaMode = AlphaMode::Mask;
                }
            } else if (key == "map_Ke") {
                current->emissiveTexture = texture(parseTextureStatement(line), true, false);
                if (current->emissiveTexture >= 0 && lengthSquared(current->emissive) == 0.0f)
                    current->emissive = Vec3(1.0f);
            } else if (key == "map_Bump" || key == "map_bump" || key == "bump" || key == "norm" || key == "map_Kn") {
                float scale = 1.0f;
                const std::string name = parseTextureStatement(line, &scale);
                current->normalTexture = texture(name, false, true);
                current->normalScale = scale;
            }
        });
    }
};

struct CornerKey {
    int position, uv, normal;
    bool operator==(const CornerKey&) const = default;
};
struct CornerHash {
    std::size_t operator()(const CornerKey& k) const {
        std::size_t h = static_cast<std::size_t>(k.position) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::size_t>(k.uv + 1) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.normal + 1) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return h;
    }
};

// One output mesh per material, each with its own vertex de-duplication map.
struct MeshBuilder {
    Mesh mesh;
    std::unordered_map<CornerKey, std::uint32_t, CornerHash> lookup;
    bool missingNormals = false;
};

}  // namespace

LoadResult loadObj(const std::string& path) {
    LoadResult result;
    std::string text;
    if (!readFile(path, text)) {
        result.error = "cannot open '" + path + "'";
        return result;
    }

    ObjContext ctx{result, detail::directoryOf(path), {}, {}};
    result.model.name = detail::fileNameOf(path);

    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    std::vector<MeshBuilder> builders;
    std::unordered_map<int, int> builderByMaterial;
    int currentMaterial = -1;  // -1: faces before any usemtl
    MeshBuilder* builder = nullptr;

    auto selectBuilder = [&] {
        auto [it, inserted] = builderByMaterial.try_emplace(currentMaterial, static_cast<int>(builders.size()));
        if (inserted) {
            builders.emplace_back();
            builders.back().mesh.material = currentMaterial;
        }
        builder = &builders[it->second];
    };

    // OBJ indices are 1-based; negative ones count back from the end of the
    // array as it stands when the face is read.
    auto resolveIndex = [](long index, std::size_t count) -> int {
        if (index > 0) return index <= static_cast<long>(count) ? static_cast<int>(index - 1) : -1;
        if (index < 0)
            return -index <= static_cast<long>(count) ? static_cast<int>(static_cast<long>(count) + index) : -1;
        return -1;
    };

    std::vector<std::uint32_t> corners;  // reused per face

    forEachLine(text, [&](LineParser& line) {
        const std::string_view key = line.word();
        if (key == "v") {
            const float x = line.number(), y = line.number(), z = line.number();
            positions.push_back({x, y, z});
        } else if (key == "vt") {
            const float u = line.number(), v = line.number();
            // OBJ puts v = 0 at the bottom of the image; we use the top.
            uvs.push_back({u, 1.0f - v});
        } else if (key == "vn") {
            const float x = line.number(), y = line.number(), z = line.number();
            normals.push_back({x, y, z});
        } else if (key == "f") {
            if (!builder) selectBuilder();
            corners.clear();
            while (!line.atEnd()) {
                // A corner is "p", "p/t", "p//n" or "p/t/n".
                const std::string_view token = line.word();
                long idx[3] = {0, 0, 0};
                const char* p = token.data();
                const char* const end = p + token.size();
                for (int slot = 0; slot < 3 && p < end; ++slot) {
                    char* stop = nullptr;
                    idx[slot] = std::strtol(p, &stop, 10);
                    p = stop;
                    if (p < end && *p == '/') ++p;
                    else break;
                }
                CornerKey corner{resolveIndex(idx[0], positions.size()), resolveIndex(idx[1], uvs.size()),
                                 resolveIndex(idx[2], normals.size())};
                if (corner.position < 0) continue;  // malformed: skip the corner

                auto [it, inserted] =
                    builder->lookup.try_emplace(corner, static_cast<std::uint32_t>(builder->mesh.vertices.size()));
                if (inserted) {
                    Vertex v;
                    v.position = positions[corner.position];
                    if (corner.uv >= 0) {
                        v.uv = uvs[corner.uv];
                        builder->mesh.hasUVs = true;
                    }
                    if (corner.normal >= 0) v.normal = normalize(normals[corner.normal]);
                    else builder->missingNormals = true;
                    builder->mesh.vertices.push_back(v);
                }
                corners.push_back(it->second);
            }
            // Polygons with more than three corners become a triangle fan.
            for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
                builder->mesh.indices.push_back(corners[0]);
                builder->mesh.indices.push_back(corners[i]);
                builder->mesh.indices.push_back(corners[i + 1]);
            }
        } else if (key == "usemtl") {
            const auto it = ctx.materialByName.find(line.rest());
            currentMaterial = it != ctx.materialByName.end() ? it->second : -1;
            selectBuilder();
        } else if (key == "mtllib") {
            if (const std::string file = line.rest(); !file.empty()) ctx.loadMaterials(file);
            builder = nullptr;  // material indices may have changed meaning
        }
    });

    for (MeshBuilder& b : builders) {
        if (b.mesh.indices.empty()) continue;
        if (b.missingNormals) computeNormals(b.mesh);
        if (b.mesh.hasUVs) computeTangents(b.mesh);
        result.model.meshes.push_back(std::move(b.mesh));
    }

    if (result.model.meshes.empty()) {
        result.error = "'" + path + "' contains no faces";
        return result;
    }

    // Faces with no material get a neutral default, appended last so the
    // indices handed out above stay valid.
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

    finalizeModel(result.model);
    result.ok = true;
    return result;
}

}  // namespace termesh
