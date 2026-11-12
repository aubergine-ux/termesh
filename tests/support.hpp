// Helpers shared by the test files.
#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

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
