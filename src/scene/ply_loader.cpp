// PLY: the format of 3D scanners, and of point clouds.
//
// A PLY file describes itself. Its header lists "elements" (vertex, face,
// ...) and, for each, how many there are and which properties each one has,
// with their types; the data follows in exactly that order, as text or as
// raw binary of either byte order:
//
//     ply
//     format binary_little_endian 1.0
//     element vertex 1250
//     property float x
//     property float y
//     property float z
//     property uchar red
//     property uchar green
//     property uchar blue
//     element face 2400
//     property list uchar int vertex_indices
//     end_header
//     ...data...
//
// So the reader has no fixed layout to assume: it builds a table of
// properties from the header and walks the data with it. Properties it has
// no use for are stepped over; properties it knows by name are collected:
//
//     x y z                           position
//     nx ny nz                        normal
//     red green blue                  colour (also diffuse_red, ...)
//     s t / u v / texture_u texture_v texture coordinates
//     f_dc_0 f_dc_1 f_dc_2            colour, as written by Gaussian
//                                     splatting tools
//     vertex_indices / vertex_index   a face's corners
//
// A file with vertices but no faces is a point cloud.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/color.hpp"
#include "scene/loader.hpp"

namespace termesh {

namespace {

enum class Type { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Invalid };

Type typeFromName(const std::string& name) {
    if (name == "char" || name == "int8") return Type::Int8;
    if (name == "uchar" || name == "uint8") return Type::UInt8;
    if (name == "short" || name == "int16") return Type::Int16;
    if (name == "ushort" || name == "uint16") return Type::UInt16;
    if (name == "int" || name == "int32") return Type::Int32;
    if (name == "uint" || name == "uint32") return Type::UInt32;
    if (name == "float" || name == "float32") return Type::Float32;
    if (name == "double" || name == "float64") return Type::Float64;
    return Type::Invalid;
}

std::size_t sizeOf(Type type) {
    switch (type) {
        case Type::Int8:
        case Type::UInt8: return 1;
        case Type::Int16:
        case Type::UInt16: return 2;
        case Type::Float64: return 8;
        default: return 4;
    }
}

struct Property {
    std::string name;
    Type type = Type::Invalid;
    bool isList = false;
    Type countType = Type::Invalid;  // lists only: the type of the length that precedes the items
};

struct Element {
    std::string name;
    std::size_t count = 0;
    std::vector<Property> properties;
};

// The data section, read one value at a time in whichever encoding the
// header announced.
class Reader {
public:
    enum class Format { Ascii, LittleEndian, BigEndian };

    Reader(const std::vector<char>& data, std::size_t start, Format format)
        : data_(data), pos_(start), format_(format) {}

    bool failed() const { return failed_; }

    double value(Type type) {
        if (format_ == Format::Ascii) return ascii();
        const std::size_t size = sizeOf(type);
        if (pos_ + size > data_.size()) {
            failed_ = true;
            return 0.0;
        }
        // Collect the bytes into an integer, least significant first.
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < size; ++i) {
            const std::size_t at = format_ == Format::LittleEndian ? i : size - 1 - i;
            bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(data_[pos_ + at])) << (8 * i);
        }
        pos_ += size;
        switch (type) {
            case Type::Int8: return static_cast<std::int8_t>(bits);
            case Type::UInt8: return static_cast<std::uint8_t>(bits);
            case Type::Int16: return static_cast<std::int16_t>(bits);
            case Type::UInt16: return static_cast<std::uint16_t>(bits);
            case Type::Int32: return static_cast<std::int32_t>(bits);
            case Type::UInt32: return static_cast<std::uint32_t>(bits);
            case Type::Float32: {
                const auto narrow = static_cast<std::uint32_t>(bits);
                float f;
                std::memcpy(&f, &narrow, sizeof(f));
                return f;
            }
            default: {
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                return d;
            }
        }
    }

private:
    double ascii() {
        while (pos_ < data_.size() && std::isspace(static_cast<unsigned char>(data_[pos_]))) ++pos_;
        if (pos_ >= data_.size()) {
            failed_ = true;
            return 0.0;
        }
        // strtod needs a terminated string; copy the token out.
        char token[64];
        std::size_t length = 0;
        while (pos_ < data_.size() && !std::isspace(static_cast<unsigned char>(data_[pos_])) &&
               length + 1 < sizeof(token))
            token[length++] = data_[pos_++];
        token[length] = '\0';
        char* end = nullptr;
        const double v = std::strtod(token, &end);
        if (end == token) failed_ = true;
        return v;
    }

    const std::vector<char>& data_;
    std::size_t pos_;
    Format format_;
    bool failed_ = false;
};

// 8-bit colours are sRGB; so, in practice, are colours stored as floats.
float decodeColor(double value, Type type) {
    const double unit = (type == Type::Float32 || type == Type::Float64) ? value : value / 255.0;
    return detail::srgbDecode(static_cast<float>(std::clamp(unit, 0.0, 1.0)));
}

}  // namespace

LoadResult loadPly(const std::string& path) {
    LoadResult result;
    auto fail = [&](const std::string& message) {
        result.error = "'" + path + "': " + message;
        return result;
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "cannot open '" + path + "'";
        return result;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // ------------------------------------------------------------- header
    std::vector<Element> elements;
    Reader::Format format = Reader::Format::Ascii;
    bool sawFormat = false, sawEnd = false;
    std::size_t pos = 0;
    int lineNumber = 0;
    while (pos < data.size() && !sawEnd) {
        std::size_t end = pos;
        while (end < data.size() && data[end] != '\n') ++end;
        std::string line(data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(end));
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string keyword;
        words >> keyword;

        if (lineNumber++ == 0) {
            if (keyword != "ply") return fail("not a PLY file");
            continue;
        }
        if (keyword == "format") {
            std::string name;
            words >> name;
            if (name == "ascii") format = Reader::Format::Ascii;
            else if (name == "binary_little_endian") format = Reader::Format::LittleEndian;
            else if (name == "binary_big_endian") format = Reader::Format::BigEndian;
            else return fail("unknown format '" + name + "'");
            sawFormat = true;
        } else if (keyword == "element") {
            Element element;
            long long count = -1;
            words >> element.name >> count;
            if (words.fail() || count < 0) return fail("malformed element line in the header");
            element.count = static_cast<std::size_t>(count);
            elements.push_back(std::move(element));
        } else if (keyword == "property") {
            if (elements.empty()) return fail("a property before any element in the header");
            Property property;
            std::string type;
            words >> type;
            if (type == "list") {
                std::string countType, itemType;
                words >> countType >> itemType >> property.name;
                property.isList = true;
                property.countType = typeFromName(countType);
                property.type = typeFromName(itemType);
                if (property.countType == Type::Invalid) return fail("unknown type '" + countType + "' in the header");
            } else {
                property.type = typeFromName(type);
                words >> property.name;
            }
            if (words.fail() || property.type == Type::Invalid) return fail("malformed property line in the header");
            elements.back().properties.push_back(std::move(property));
        } else if (keyword == "end_header") {
            sawEnd = true;
        }
        // "comment" and "obj_info" lines, and anything unrecognised, are skipped.
    }
    if (!sawEnd || !sawFormat) return fail("the header is incomplete");

    // A header can claim any number of elements; before believing it, check
    // that the file could possibly hold that many.
    const std::size_t remaining = data.size() - std::min(pos, data.size());
    for (const Element& element : elements)
        if (element.count > remaining)
            return fail("the file is too short for the " + element.name + "s its header promises");

    // --------------------------------------------------------------- data
    Reader reader(data, pos, format);
    Mesh mesh;
    bool hasNormals = false, hasColors = false;

    for (const Element& element : elements) {
        if (element.name == "vertex") {
            // Which property feeds which part of a vertex.
            enum Slot { X, Y, Z, NX, NY, NZ, R, G, B, U, V, DC0, DC1, DC2, None };
            std::vector<Slot> slots;
            bool hasUVs = false, hasDc = false;
            for (const Property& property : element.properties) {
                const std::string& n = property.name;
                Slot slot = None;
                if (property.isList) slot = None;
                else if (n == "x") slot = X;
                else if (n == "y") slot = Y;
                else if (n == "z") slot = Z;
                else if (n == "nx") slot = NX;
                else if (n == "ny") slot = NY;
                else if (n == "nz") slot = NZ;
                else if (n == "red" || n == "diffuse_red" || n == "r") slot = R;
                else if (n == "green" || n == "diffuse_green" || n == "g") slot = G;
                else if (n == "blue" || n == "diffuse_blue" || n == "b") slot = B;
                else if (n == "s" || n == "u" || n == "texture_u") slot = U;
                else if (n == "t" || n == "v" || n == "texture_v") slot = V;
                else if (n == "f_dc_0") slot = DC0;
                else if (n == "f_dc_1") slot = DC1;
                else if (n == "f_dc_2") slot = DC2;
                slots.push_back(slot);
                hasNormals = hasNormals || slot == NX;
                hasColors = hasColors || slot == R;
                hasUVs = hasUVs || slot == U;
                hasDc = hasDc || slot == DC0;
            }
            hasColors = hasColors || hasDc;

            mesh.vertices.resize(element.count);
            if (hasColors) mesh.colors.assign(element.count, Vec3(1.0f));
            mesh.hasUVs = hasUVs;
            for (std::size_t i = 0; i < element.count && !reader.failed(); ++i) {
                Vertex& vertex = mesh.vertices[i];
                // No normal in the file is recorded as a zero normal, which
                // the renderer takes to mean "facing the viewer".
                vertex.normal = {0.0f, 0.0f, 0.0f};
                Vec3 color{1.0f, 1.0f, 1.0f};
                for (std::size_t p = 0; p < element.properties.size(); ++p) {
                    const Property& property = element.properties[p];
                    if (property.isList) {
                        const double n = reader.value(property.countType);
                        for (double k = 0; k < n && !reader.failed(); ++k) reader.value(property.type);
                        continue;
                    }
                    const double value = reader.value(property.type);
                    const auto f = static_cast<float>(value);
                    switch (slots[p]) {
                        case X: vertex.position.x = f; break;
                        case Y: vertex.position.y = f; break;
                        case Z: vertex.position.z = f; break;
                        case NX: vertex.normal.x = f; break;
                        case NY: vertex.normal.y = f; break;
                        case NZ: vertex.normal.z = f; break;
                        case R: color.x = decodeColor(value, property.type); break;
                        case G: color.y = decodeColor(value, property.type); break;
                        case B: color.z = decodeColor(value, property.type); break;
                        case U: vertex.uv.x = f; break;
                        case V:
                            vertex.uv.y = 1.0f - f;
                            break;  // PLY's v runs upwards; images run down
                        // Gaussian splat files store colour as the constant
                        // term of a spherical-harmonic series: 0.5 + c * 0.282.
                        case DC0: color.x = decodeColor(0.5 + 0.2820948 * value, Type::Float32); break;
                        case DC1: color.y = decodeColor(0.5 + 0.2820948 * value, Type::Float32); break;
                        case DC2: color.z = decodeColor(0.5 + 0.2820948 * value, Type::Float32); break;
                        default: break;
                    }
                }
                if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
                    !std::isfinite(vertex.position.z))
                    vertex.position = {0.0f, 0.0f, 0.0f};
                if (hasNormals) vertex.normal = normalize(vertex.normal, {0.0f, 0.0f, 0.0f});
                if (hasColors) mesh.colors[i] = color;
            }
        } else if (element.name == "face") {
            std::vector<std::uint32_t> corners;
            bool badIndex = false;
            for (std::size_t i = 0; i < element.count && !reader.failed(); ++i) {
                for (const Property& property : element.properties) {
                    if (!property.isList) {
                        reader.value(property.type);
                        continue;
                    }
                    const bool indices = property.name == "vertex_indices" || property.name == "vertex_index";
                    const double n = reader.value(property.countType);
                    corners.clear();
                    for (double k = 0; k < n && !reader.failed(); ++k) {
                        const double index = reader.value(property.type);
                        if (indices) corners.push_back(index >= 0.0 ? static_cast<std::uint32_t>(index) : 0xFFFFFFFFu);
                    }
                    if (!indices) continue;
                    bool valid = corners.size() >= 3;
                    for (const std::uint32_t c : corners) valid = valid && c < mesh.vertices.size();
                    if (!valid) {
                        badIndex = badIndex || corners.size() >= 3;
                        continue;
                    }
                    // Polygons with more than three corners become a fan.
                    for (std::size_t k = 1; k + 1 < corners.size(); ++k)
                        mesh.indices.insert(mesh.indices.end(), {corners[0], corners[k], corners[k + 1]});
                }
            }
            if (badIndex) result.warnings.push_back("faces referring to vertices that do not exist were skipped");
        } else {
            // An element this loader has no use for (edges, materials, ...):
            // read past it.
            for (std::size_t i = 0; i < element.count && !reader.failed(); ++i)
                for (const Property& property : element.properties) {
                    if (!property.isList) {
                        reader.value(property.type);
                        continue;
                    }
                    const double n = reader.value(property.countType);
                    for (double k = 0; k < n && !reader.failed(); ++k) reader.value(property.type);
                }
        }
        if (reader.failed()) return fail("the data ends early, or is not what the header describes");
    }

    if (mesh.vertices.empty()) return fail("contains no vertices");

    Material material;
    material.name = "ply";
    // Scans rarely have consistent winding; draw both sides.
    material.doubleSided = true;
    material.roughness = 0.7f;
    material.baseColor = hasColors ? Vec4{1.0f, 1.0f, 1.0f, 1.0f} : Vec4{0.78f, 0.78f, 0.8f, 1.0f};

    if (mesh.indices.empty()) {
        mesh.name = "points";
        shufflePoints(mesh);
    } else if (!hasNormals) {
        computeNormals(mesh);
    }

    result.model.name = detail::fileNameOf(path);
    result.model.materials.push_back(material);
    result.model.meshes.push_back(std::move(mesh));
    finalizeModel(result.model);
    result.ok = true;
    return result;
}

}  // namespace termesh
