// Helpers shared by the test files.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// A scratch directory that cleans up after itself.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / ("termesh_test_" + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string write(const std::string& name, const std::string& contents) const {
        const std::filesystem::path file = path / name;
        std::ofstream(file, std::ios::binary) << contents;
        return file.string();
    }
    std::string file(const std::string& name) const { return (path / name).string(); }
};

// Builds the binary buffer of a glTF file and the JSON that describes it.
struct GltfBuffer {
    std::string bytes, views, accessors;
    int count = 0;

    // Append `n` elements of `type` and return the accessor index.
    int add(const void* data, std::size_t size, int componentType, std::size_t n, const char* type,
            const char* extra = "") {
        while (bytes.size() % 4) bytes.push_back('\0');
        const std::size_t offset = bytes.size();
        bytes.append(static_cast<const char*>(data), size);
        if (count) views += ",", accessors += ",";
        views +=
            "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(size) + "}";
        accessors += "{\"bufferView\":" + std::to_string(count) +
                     ",\"componentType\":" + std::to_string(componentType) + ",\"count\":" + std::to_string(n) +
                     ",\"type\":\"" + type + "\"" + extra + "}";
        return count++;
    }
    int floats(const std::vector<float>& v, int components, const char* type, const char* extra = "") {
        return add(v.data(), v.size() * sizeof(float), 5126, v.size() / static_cast<std::size_t>(components), type,
                   extra);
    }

    static std::string base64(const std::string& in) {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < in.size(); i += 3) {
            const std::uint32_t a = static_cast<unsigned char>(in[i]);
            const std::uint32_t b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
            const std::uint32_t c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
            const std::uint32_t v = (a << 16) | (b << 8) | c;
            out.push_back(table[v >> 18]);
            out.push_back(table[(v >> 12) & 63]);
            out.push_back(i + 1 < in.size() ? table[(v >> 6) & 63] : '=');
            out.push_back(i + 2 < in.size() ? table[v & 63] : '=');
        }
        return out;
    }

    // The "buffers", "bufferViews" and "accessors" members of the file.
    std::string json() const {
        return "\"buffers\":[{\"byteLength\":" + std::to_string(bytes.size()) +
               ",\"uri\":\"data:application/octet-stream;base64," + base64(bytes) + "\"}],\"bufferViews\":[" + views +
               "],\"accessors\":[" + accessors + "]";
    }
};
