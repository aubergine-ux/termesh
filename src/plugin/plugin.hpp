// Plugins: shared libraries that add file loaders and output backends at
// run time. The interface they are written against is the C header
// include/termesh_plugin.h; this is the program's side of it.
#pragma once

#include <string>
#include <vector>

namespace termesh {

struct PluginInfo {
    std::string path;
    std::string name;                     // the file's name, without directory or extension
    std::vector<std::string> extensions;  // file types it loads
    std::vector<std::string> backends;    // backends it provides
    std::vector<std::string> backendDescriptions;
};

// True if this build can load plugins at all (a fully static binary and
// the WebAssembly build cannot).
bool pluginsSupported();

// Load one plugin. On failure returns false and says why.
bool loadPlugin(const std::string& path, std::string& error);

// Load every plugin in a directory (files with the platform's shared
// library extension, in alphabetical order). Returns how many loaded;
// `errors` collects a line for each that did not. A directory that does not
// exist is not an error.
int loadPluginDirectory(const std::string& directory, std::vector<std::string>& errors);

// Where plugins are loaded from automatically: each directory in
// TERMESH_PLUGIN_PATH, then the user's own -
//   $XDG_CONFIG_HOME/termesh/plugins   (or ~/.config/termesh/plugins)
//   %APPDATA%\termesh\plugins          on Windows
std::vector<std::string> pluginDirectories();

// The plugins loaded so far, in order.
const std::vector<PluginInfo>& loadedPlugins();

// ".so", ".dylib" or ".dll".
const char* pluginExtension();

}  // namespace termesh
