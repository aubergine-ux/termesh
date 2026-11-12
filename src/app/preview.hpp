// Preview mode support: `termesh --preview`, for file managers.
//
// A file manager's preview pane asks for a picture of a file every time the
// cursor lands on it, and expects an answer at once. Rendering one frame is
// quick; loading the model is what takes the time - a large glTF file is
// mostly compressed textures to decode. So previews are remembered: the
// finished output (the escape sequences themselves) is written to a cache
// keyed by everything it depends on, and the next request for the same
// file at the same size is a single file read.
#pragma once

#include <cstdint>
#include <string>

namespace termesh {

// Where previews are cached: $XDG_CACHE_HOME/termesh/previews, or the
// platform's equivalent. Empty if there is nowhere sensible.
std::string previewCacheDirectory();

// A name for the cached preview of `path` rendered with `parameters` (the
// size, backend and anything else that affects the picture, as a string).
// The file's size and modification time are part of it, so editing the file
// makes its old previews unreachable. Empty if the file cannot be examined.
std::string previewCacheKey(const std::string& path, const std::string& parameters);

// Fetch a cached preview. False if there is none.
bool readPreviewCache(const std::string& directory, const std::string& key, std::string& out);

// Store one, creating the directory if need be. Failure is silent: a cache
// that cannot be written is only a slower preview next time. When the
// cache has grown past `maxEntries` files, the oldest half are removed.
void writePreviewCache(const std::string& directory, const std::string& key, const std::string& data,
                       std::size_t maxEntries = 400);

// The size of a file in bytes, or 0 if it cannot be examined.
std::uint64_t fileSize(const std::string& path);

}  // namespace termesh
