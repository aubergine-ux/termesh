// Plugins: the example plugins are loaded for real, as shared libraries.
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "output/backend.hpp"
#include "plugin/plugin.hpp"
#include "scene/loader.hpp"
#include "support.hpp"

using namespace termesh;

TEST_CASE("plugins: where they are looked for") {
    CHECK(std::string(pluginExtension()).front() == '.');
    // Whatever else is on the list, it does not throw and has no empty entries.
    for (const std::string& directory : pluginDirectories()) CHECK(!directory.empty());
}

TEST_CASE("plugins: a custom backend can be registered by anything") {
    CHECK(!isRegisteredBackend("test-ascii"));
    registerBackend("test-ascii", [](ColorDepth color) { return makeBackend(BackendKind::Ascii, color); });
    CHECK(isRegisteredBackend("test-ascii"));
    const std::vector<std::string> names = registeredBackendNames();
    CHECK(std::find(names.begin(), names.end(), "test-ascii") != names.end());
    CHECK(makeBackend(BackendKind::Custom, ColorDepth::None, "test-ascii")->kind() == BackendKind::Ascii);
    // An unknown name falls back to something that works rather than nothing.
    CHECK(makeBackend(BackendKind::Custom, ColorDepth::None, "no-such-backend") != nullptr);
    // The built-in names are not in the table: they cannot be shadowed.
    CHECK(!isRegisteredBackend("halfblock"));
    CHECK(std::string(backendName(BackendKind::Custom)) == "custom");
}

#ifdef TERMESH_TEST_PLUGIN_DIR

namespace {

std::string pluginPath(const char* name) {
    return std::string(TERMESH_TEST_PLUGIN_DIR) + "/" + name + TERMESH_TEST_PLUGIN_SUFFIX;
}

Image gradient(int width, int height) {
    Image image(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.row(y)[x] = {static_cast<std::uint8_t>(x * 255 / std::max(width - 1, 1)),
                               static_cast<std::uint8_t>(y * 255 / std::max(height - 1, 1)), 80, 255};
    return image;
}

}  // namespace

TEST_CASE("plugins: failures to load are reported") {
    REQUIRE(pluginsSupported());
    std::string error;
    CHECK(!loadPlugin("/no/such/plugin.so", error));
    CHECK(!error.empty());

    // A file that is not a shared library at all.
    TempDir dir;
    const std::string text = dir.write(std::string("not-a-plugin") + pluginExtension(), "hello");
    error.clear();
    CHECK(!loadPlugin(text, error));
    CHECK(!error.empty());

    // A directory that does not exist is simply empty.
    std::vector<std::string> errors;
    CHECK(loadPluginDirectory(dir.file("missing"), errors) == 0);
    CHECK(errors.empty());
    // One holding something that will not load says so, once.
    CHECK(loadPluginDirectory(dir.path.string(), errors) == 0);
    CHECK(errors.size() == 1);
}

TEST_CASE("plugins: a loader plugin adds a file format") {
    TempDir dir;
    const std::string cloud = dir.write("cloud.xyz",
                                        "# a comment\n"
                                        "0 0 0 255 0 0\n"
                                        "1 0 0 0 255 0\n"
                                        "\n"
                                        "0 1 0 0 0 255\n"
                                        "1 1 1 255 255 255\n"
                                        "not a point\n");
    const std::vector<std::string> before = modelExtensions();
    if (std::find(before.begin(), before.end(), "xyz") == before.end()) {
        const LoadResult refused = loadModel(cloud);
        CHECK(!refused.ok);  // nothing reads .xyz yet
    }

    std::string error;
    REQUIRE_MESSAGE(loadPlugin(pluginPath("xyz"), error), error);
    // Loading it twice is harmless.
    CHECK(loadPlugin(pluginPath("xyz"), error));
    const auto& plugins = loadedPlugins();
    const auto found =
        std::find_if(plugins.begin(), plugins.end(), [](const PluginInfo& p) { return p.name == "xyz"; });
    REQUIRE(found != plugins.end());
    CHECK(found->extensions == std::vector<std::string>{"xyz"});
    CHECK(std::count_if(plugins.begin(), plugins.end(), [](const PluginInfo& p) { return p.name == "xyz"; }) == 1);
    const std::vector<std::string> after = modelExtensions();
    CHECK(std::find(after.begin(), after.end(), "xyz") != after.end());

    const LoadResult loaded = loadModel(cloud);
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    REQUIRE(loaded.model.meshes.size() == 1);
    const Mesh& mesh = loaded.model.meshes[0];
    CHECK(mesh.isPointCloud());
    CHECK(mesh.vertices.size() == 4);
    REQUIRE(mesh.colors.size() == 4);
    CHECK(mesh.pointRadius > 0.0f);
    CHECK(loaded.model.name == "cloud.xyz");
    const Bounds bounds = loaded.model.bounds();
    CHECK(bounds.max.x == doctest::Approx(1.0f));
    CHECK(bounds.max.z == doctest::Approx(1.0f));
    // The colours came through (in some order: clouds are shuffled).
    int red = 0;
    for (const Vec3& color : mesh.colors) red += color.x > 0.99f && color.y < 0.01f && color.z < 0.01f;
    CHECK(red == 1);

    // The plugin's own errors come back as load errors.
    const LoadResult empty = loadModel(dir.write("empty.xyz", "# nothing here\n"));
    CHECK(!empty.ok);
    CHECK(empty.error.find("no points") != std::string::npos);
    const LoadResult missing = loadModel(dir.file("missing.xyz"));
    CHECK(!missing.ok);
    CHECK(missing.error.find("missing.xyz") != std::string::npos);
}

TEST_CASE("plugins: a backend plugin adds a way of drawing") {
    std::string error;
    REQUIRE_MESSAGE(loadPlugin(pluginPath("quadrant"), error), error);
    REQUIRE(isRegisteredBackend("quadrant"));

    const auto backend = makeBackend(BackendKind::Custom, ColorDepth::TrueColor, "quadrant");
    REQUIRE(backend);
    CHECK(backend->kind() == BackendKind::Custom);
    CHECK(std::string(backend->name()) == "quadrant");
    CHECK(backend->defaultSupersampling() == 2);
    CHECK(!backend->wantsWireframe());

    DrawArea area;
    area.cols = 20;
    area.rows = 6;
    int width = 0, height = 0;
    backend->configure(area, width, height);
    CHECK(width == 40);
    CHECK(height == 12);
    // A 10 x 20 cell holding 2 x 2 pixels: each is half as wide as tall.
    CHECK(backend->pixelAspect() == doctest::Approx(0.5f));

    const Image image = gradient(width, height);
    std::string printed;
    backend->print(image.view(), printed);
    CHECK(std::count(printed.begin(), printed.end(), '\n') == 6);
    CHECK(printed.find("\x1b[38;2;") != std::string::npos);
    CHECK(printed.find("\x1b[48;2;") != std::string::npos);
    // A quadrant or half-block character: U+2580 - U+259F all begin E2 96.
    CHECK(printed.find("\xe2\x96") != std::string::npos);

    // On screen: the first frame draws every row, the same frame again
    // draws nothing, and a change redraws only the rows it touches.
    std::string first, again, changed;
    backend->present(image.view(), first);
    CHECK(first.find("\x1b[1;1H") != std::string::npos);
    CHECK(first.find("\x1b[6;1H") != std::string::npos);
    backend->present(image.view(), again);
    CHECK(again.empty());
    Image edited = image;
    for (int x = 0; x < width; ++x) edited.row(0)[x] = {255, 255, 255, 255};
    backend->present(edited.view(), changed);
    CHECK(changed.find("\x1b[1;1H") != std::string::npos);
    CHECK(changed.find("\x1b[2;1H") == std::string::npos);
    // After the screen has been cleared, everything is sent again.
    backend->invalidate();
    std::string redrawn;
    backend->present(edited.view(), redrawn);
    CHECK(redrawn.find("\x1b[6;1H") != std::string::npos);

    // Fewer colours, and none.
    const auto indexed = makeBackend(BackendKind::Custom, ColorDepth::Indexed256, "quadrant");
    indexed->configure(area, width, height);
    std::string palette;
    indexed->print(image.view(), palette);
    CHECK(palette.find("\x1b[38;5;") != std::string::npos);
    CHECK(palette.find("\x1b[38;2;") == std::string::npos);

    const auto plain = makeBackend(BackendKind::Custom, ColorDepth::None, "quadrant");
    plain->configure(area, width, height);
    std::string mono;
    plain->print(image.view(), mono);
    CHECK(mono.find("\x1b[38;") == std::string::npos);
    CHECK(mono.find("\x1b[48;") == std::string::npos);
    CHECK(mono.find("\xe2\x96") != std::string::npos);

    const auto& plugins = loadedPlugins();
    const auto found =
        std::find_if(plugins.begin(), plugins.end(), [](const PluginInfo& p) { return p.name == "quadrant"; });
    REQUIRE(found != plugins.end());
    CHECK(found->backends == std::vector<std::string>{"quadrant"});
    CHECK(!found->backendDescriptions[0].empty());
}

#endif  // TERMESH_TEST_PLUGIN_DIR
