#include "app/preview.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace termesh {

namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// FNV-1a, as sixteen hex digits: a file name that is the same for the same
// inputs on every run.
std::string hashName(const std::string& text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char c : text) hash = (hash ^ c) * 0x100000001b3ull;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

}  // namespace

std::string previewCacheDirectory() {
    fs::path base;
    if (const std::string xdg = env("XDG_CACHE_HOME"); !xdg.empty()) base = xdg;
    else if (const std::string local = env("LOCALAPPDATA"); !local.empty()) base = local;  // Windows
    else if (const std::string home = env("HOME"); !home.empty()) base = fs::path(home) / ".cache";
    else return {};
    return (base / "termesh" / "previews").string();
}

std::uint64_t fileSize(const std::string& path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
}

std::string previewCacheKey(const std::string& path, const std::string& parameters) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    if (error) return {};
    const std::uintmax_t size = fs::file_size(absolute, error);
    if (error) return {};
    const auto modified = fs::last_write_time(absolute, error);
    if (error) return {};

    std::ostringstream identity;
    identity << TERMESH_VERSION << '\n'
             << absolute.string() << '\n'
             << size
             << '\n'
             // (Some standard libraries count file times in a type wider
             // than any that streams know how to print.)
             << static_cast<long long>(modified.time_since_epoch().count()) << '\n'
             << parameters;
    return hashName(identity.str());
}

bool readPreviewCache(const std::string& directory, const std::string& key, std::string& out) {
    if (directory.empty() || key.empty()) return false;
    std::ifstream file(fs::path(directory) / key, std::ios::binary);
    if (!file) return false;
    std::ostringstream contents;
    contents << file.rdbuf();
    out = contents.str();
    return !out.empty();
}

void writePreviewCache(const std::string& directory, const std::string& key, const std::string& data,
                       std::size_t maxEntries) {
    if (directory.empty() || key.empty() || data.empty()) return;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return;

    // Write under a temporary name and rename, so that a second preview
    // running at the same moment never reads half a file.
    const fs::path target = fs::path(directory) / key;
    const fs::path temporary = fs::path(directory) / (key + ".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) return;
    }
    fs::rename(temporary, target, error);
    if (error) {
        fs::remove(temporary, error);
        return;
    }

    // Keep the cache from growing for ever.
    std::vector<std::pair<fs::file_time_type, fs::path>> entries;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        std::error_code ignored;
        if (entry.is_regular_file(ignored)) entries.emplace_back(entry.last_write_time(ignored), entry.path());
    }
    if (entries.size() <= maxEntries) return;
    std::sort(entries.begin(), entries.end());
    for (std::size_t i = 0; i < entries.size() / 2; ++i) fs::remove(entries[i].second, error);
}

}  // namespace termesh
