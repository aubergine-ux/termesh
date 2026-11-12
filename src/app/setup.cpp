#include "app/setup.hpp"

#include <cstdio>
#include <memory>
#include <string>

#include "physics/demo.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"
#include "stage/scene_file.hpp"

namespace termesh {

bool buildScene(int argc, const char* const* argv, Options& options, ThreadPool& pool, Scene& scene) {
    if (options.demo == "physics") {
        scene.driver = std::make_shared<PhysicsDemo>(scene, options.bodies);
        // Shadows are what make things look as if they rest on the floor:
        // on for this demo, unless the command line says otherwise.
        Options defaults;
        defaults.shadows = true;
        options = parseOptions(argc, argv, defaults).options;
        return true;
    }
    if (!options.demo.empty()) {
        Model model = makeDemoModel(options.demo);
        if (model.meshes.empty()) {
            std::fprintf(stderr, "termesh: unknown demo '%s'\nTry 'termesh --help'.\n", options.demo.c_str());
            return false;
        }
        scene.name = model.name;
        scene.add(std::move(model), options.demo, Mat4::identity(), &pool).source = "demo:" + options.demo;
        return true;
    }

    if (isSceneFile(options.inputs.front())) {
        SceneLoadResult loaded = loadSceneFile(options.inputs.front(), &pool);
        for (const std::string& warning : loaded.warnings)
            std::fprintf(stderr, "termesh: warning: %s\n", warning.c_str());
        if (!loaded.ok) {
            std::fprintf(stderr, "termesh: %s\n", loaded.error.c_str());
            return false;
        }
        scene = std::move(loaded.scene);

        // The file's "render" settings sit between the built-in defaults and
        // the command line: read the command line again on top of them.
        Options defaults;
        if (std::string error; !applySceneRenderSettings(scene.renderSettings, defaults, error)) {
            std::fprintf(stderr, "termesh: '%s': %s\n", options.inputs.front().c_str(), error.c_str());
            return false;
        }
        options = parseOptions(argc, argv, defaults).options;
        return true;
    }

    for (const std::string& path : options.inputs) {
        LoadResult loaded = loadModel(path);
        for (const std::string& warning : loaded.warnings)
            std::fprintf(stderr, "termesh: warning: %s\n", warning.c_str());
        if (!loaded.ok) {
            std::fprintf(stderr, "termesh: %s\n", loaded.error.c_str());
            return false;
        }
        const std::string name = loaded.model.name;
        scene.add(std::move(loaded.model), name, Mat4::identity(), &pool).source = path;
    }
    scene.name = scene.objects.front()->name;
    if (scene.objects.size() > 1) {
        // Models from different files were not made to share a space.
        scene.name += " +" + std::to_string(scene.objects.size() - 1);
        scene.arrangeInRow();
    }
    return true;
}

bool setUpAnimations(const Options& options, Scene& scene, ThreadPool& pool) {
    bool matched = false;
    for (const auto& object : scene.objects) {
        if (!object->player) continue;
        AnimationPlayer& player = *object->player;
        if (!options.animation.empty()) {
            const int clip = findAnimation(object->model, options.animation);
            if (clip >= 0) {
                player.setClip(clip);
                matched = true;
            }
        }
        if (options.animationSpeedSet) player.setSpeed(options.animationSpeed);
        if (options.animationTimeSet) player.seek(options.animationTime);
        if (options.animationPaused) player.pause();
    }
    scene.pose(&pool);
    if (options.animation.empty() || matched) return true;
    if (scene.animated()) {
        std::fprintf(stderr, "termesh: no animation '%s' in %s (try --list-animations)\n", options.animation.c_str(),
                     scene.name.c_str());
        return false;
    }
    std::fprintf(stderr, "termesh: warning: %s has no animations; --animation ignored\n", scene.name.c_str());
    return true;
}

}  // namespace termesh
