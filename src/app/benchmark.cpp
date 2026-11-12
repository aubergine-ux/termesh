#include "app/benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>

#include "accel/model_accel.hpp"
#include "anim/player.hpp"
#include "anim/skinning.hpp"
#include "output/backend.hpp"
#include "pipeline/renderer.hpp"
#include "pipeline/resolve.hpp"
#include "raster/simd.hpp"
#include "scene/primitives.hpp"

namespace termesh {

namespace {

// What a scene is made of. Everything not mentioned keeps its default.
struct SceneSpec {
    SceneSpec(const char* name_, const char* description_, const char* model_, int width_ = 320, int height_ = 180)
        : name(name_), description(description_), model(model_), width(width_), height(height_) {}

    const char* name;
    const char* description;
    const char* model;  // a built-in demo model
    int width, height;  // output size: pixels, or cells if there is a backend
    int supersampling = 1;
    float zoom = 1.0f;                              // > 1 is closer
    bool accelerate = false;                        // boxes and BVHs (frustum culling)
    float lodTolerance = 0.0f;                      // > 0: build and use levels of detail
    bool animate = false;                           // advance the model's animation every frame
    const char* environment = nullptr;              // a built-in environment
    const char* backend = nullptr;                  // also encode each frame for this backend
    std::function<void(RenderSettings&)> settings;  // changes to the render settings
};

const std::vector<SceneSpec>& scenes() {
    static const std::vector<SceneSpec> list = [] {
        std::vector<SceneSpec> s;
        s.push_back({"knot-phong", "the default: Phong shading, textures, normal map", "knot"});
        s.push_back({"knot-supersampled", "the same at 3x3 supersampling, resolved", "knot"});
        s.back().supersampling = 3;
        s.push_back({"cube-flat-wire", "flat shading and hidden-line wireframe", "cube"});
        s.back().settings = [](RenderSettings& r) {
            r.shading = ShadingMode::Flat;
            r.wireframe = true;
        };
        s.push_back({"sphere-gouraud", "per-vertex lighting", "sphere"});
        s.back().settings = [](RenderSettings& r) { r.shading = ShadingMode::Gouraud; };
        s.push_back({"knot-pbr-ibl-shadows", "PBR lit by a panorama, with a shadow map", "knot"});
        s.back().environment = "sky";
        s.back().settings = [](RenderSettings& r) {
            r.shading = ShadingMode::Pbr;
            r.shadows = true;
            r.skybox = true;
        };
        s.push_back({"knot-toon-outline", "cel shading with ink outlines", "knot"});
        s.back().settings = [](RenderSettings& r) {
            r.shading = ShadingMode::Toon;
            r.post.outline = true;
        };
        s.push_back({"knot-post", "SSAO, bloom and FXAA", "knot"});
        s.back().settings = [](RenderSettings& r) {
            r.post.ssao = true;
            r.post.bloom = true;
            r.post.fxaa = true;
        };
        s.push_back({"tentacle-skinned", "skeletal animation: pose and skin every frame", "tentacle"});
        s.back().animate = true;
        s.push_back({"knot-zoomed-culled", "close up: BVH frustum culling", "knot"});
        s.back().zoom = 5.0f;
        s.back().accelerate = true;
        s.push_back({"knot-lod", "far away: levels of detail", "knot"});
        s.back().zoom = 0.35f;
        s.back().accelerate = true;
        s.back().lodTolerance = 1.0f;
        s.push_back({"present-halfblock", "encode for the half-block backend, changed cells only", "knot", 160, 50});
        s.back().supersampling = 3;
        s.back().backend = "halfblock";
        s.push_back({"present-sixel", "encode as sixel", "knot", 60, 20});
        s.back().backend = "sixel";
        s.push_back({"present-ascii", "encode for the ASCII backend", "knot", 160, 50});
        s.back().supersampling = 3;
        s.back().backend = "ascii";
        return s;
    }();
    return list;
}

// FNV-1a: a simple hash, good enough to tell "the same bytes" from "not".
struct Checksum {
    std::uint64_t value = 0xcbf29ce484222325ull;
    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 0x100000001b3ull;
    }
};

}  // namespace

std::vector<std::string> benchmarkSceneNames() {
    std::vector<std::string> names;
    for (const SceneSpec& scene : scenes()) names.emplace_back(scene.name);
    return names;
}

bool runBenchmarkScene(const std::string& name, int frames, ThreadPool& pool, BenchmarkResult& result) {
    const auto found =
        std::find_if(scenes().begin(), scenes().end(), [&](const SceneSpec& s) { return name == s.name; });
    if (found == scenes().end()) return false;
    const SceneSpec& spec = *found;
    frames = std::max(frames, 1);

    Model model = makeDemoModel(spec.model);
    ModelAccel accel;
    if (spec.accelerate) accel.build(model, &pool);
    if (spec.lodTolerance > 0.0f) accel.buildLods(model, &pool);
    const DrawItem item{&model, Mat4::identity(), spec.accelerate ? &accel : nullptr};

    Environment environment;
    if (spec.environment) makeBuiltinEnvironment(spec.environment, environment);

    RenderSettings settings;
    settings.environment = &environment;
    if (spec.settings) spec.settings(settings);
    const int ssaa = spec.supersampling;
    settings.wireWidth = static_cast<float>(ssaa);
    settings.pixelScale = static_cast<float>(ssaa);
    settings.lodTolerance = spec.lodTolerance * static_cast<float>(ssaa);

    // With a backend, the image is whatever size it wants for a terminal
    // area of the scene's dimensions (given in cells).
    int width = spec.width, height = spec.height;
    std::unique_ptr<Backend> backend;
    float pixelAspect = 1.0f;
    if (spec.backend) {
        backend = makeBackend(*backendFromName(spec.backend), ColorDepth::TrueColor);
        DrawArea area;
        area.cols = spec.width;
        area.rows = spec.height;
        backend->configure(area, width, height);
        pixelAspect = backend->pixelAspect();
    }

    OrbitCamera camera;
    camera.frame(model.framingBounds());
    camera.zoom(1.0f / spec.zoom);
    const float aspect = static_cast<float>(width) * pixelAspect / static_cast<float>(height);

    AnimationPlayer player(model.rig);
    Renderer renderer(pool);
    Framebuffer fb(width * ssaa, height * ssaa);
    Image image;
    std::string output;
    Checksum checksum;
    std::vector<double> times;

    result = {};
    result.name = spec.name;
    result.description = spec.description;
    result.frames = frames;

    for (int frame = 0; frame < frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        // One full turn over the run, whatever its length: every frame count
        // sees the model from all sides.
        camera.yaw = radians(30.0f) + 2.0f * kPi * static_cast<float>(frame) / static_cast<float>(frames);
        if (spec.animate) {
            player.seek(player.duration() * static_cast<float>(frame) / static_cast<float>(frames));
            applyPose(model, player.pose(), &pool);
            if (spec.accelerate) accel.refresh(model);
        }

        const RenderStats stats =
            renderer.render(std::span<const DrawItem>(&item, 1), camera.camera(aspect), settings, fb);
        resolve(fb, ssaa, image, pool);
        if (settings.post.fxaa) applyFxaa(image, pool);
        if (backend) {
            output.clear();
            backend->present(image.view(), output);
            result.bytes += output.size();
            checksum.add(output.data(), output.size());
        }
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

        result.triangles += stats.trianglesDrawn;
        for (const Color8& p : image.pixels) result.pixels += p.a != 0;
        checksum.add(image.pixels.data(), image.pixels.size() * sizeof(Color8));
    }

    std::sort(times.begin(), times.end());
    result.minMs = times.front();
    result.medianMs = times[times.size() / 2];
    result.checksum = checksum.value;
    return true;
}

std::string benchmarkTable(const std::vector<BenchmarkResult>& results) {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line), "%-22s %6s %10s %10s %10s %9s %9s  %s\n", "scene", "frames", "triangles",
                  "pixels", "bytes", "ms/frame", "best", "checksum");
    out += line;
    for (const BenchmarkResult& r : results) {
        std::snprintf(line, sizeof(line), "%-22s %6d %10llu %10llu %10llu %9.3f %9.3f  %016llx\n", r.name.c_str(),
                      r.frames, static_cast<unsigned long long>(r.triangles), static_cast<unsigned long long>(r.pixels),
                      static_cast<unsigned long long>(r.bytes), r.medianMs, r.minMs,
                      static_cast<unsigned long long>(r.checksum));
        out += line;
    }
    return out;
}

std::string benchmarkJson(const std::vector<BenchmarkResult>& results, unsigned threads) {
    std::string out = "{\n  \"version\": \"" TERMESH_VERSION "\",\n  \"simd\": \"";
    out += simd::kName;
    out += "\",\n  \"threads\": " + std::to_string(threads) + ",\n  \"scenes\": [\n";
    char line[512];
    for (std::size_t i = 0; i < results.size(); ++i) {
        const BenchmarkResult& r = results[i];
        std::snprintf(line, sizeof(line),
                      "    {\"name\": \"%s\", \"frames\": %d, \"triangles\": %llu, \"pixels\": %llu, \"bytes\": %llu, "
                      "\"checksum\": \"%016llx\", \"ms_median\": %.4f, \"ms_min\": %.4f}%s\n",
                      r.name.c_str(), r.frames, static_cast<unsigned long long>(r.triangles),
                      static_cast<unsigned long long>(r.pixels), static_cast<unsigned long long>(r.bytes),
                      static_cast<unsigned long long>(r.checksum), r.medianMs, r.minMs,
                      i + 1 < results.size() ? "," : "");
        out += line;
    }
    out += "  ]\n}\n";
    return out;
}

}  // namespace termesh
