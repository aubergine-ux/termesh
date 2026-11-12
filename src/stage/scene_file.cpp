#include "stage/scene_file.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "anim/skinning.hpp"
#include "core/color.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"

namespace termesh {

namespace {

// Reading a scene is a long list of "this field, if present, must look like
// that". The first thing that does not is reported with the path to it
// ("models[1].position: expected three numbers") and reading stops.
struct Reader {
    SceneLoadResult& result;
    std::string directory;
    ThreadPool* pool;

    bool fail(const std::string& where, const std::string& what) {
        if (result.error.empty()) result.error = where + ": " + what;
        return false;
    }

    bool number(const Json& json, const std::string& where, float& out) {
        if (json.isNull()) return true;  // absent: keep the default
        if (!json.isNumber() || !std::isfinite(json.number())) return fail(where, "expected a number");
        out = static_cast<float>(json.number());
        return true;
    }

    bool boolean(const Json& json, const std::string& where, bool& out) {
        if (json.isNull()) return true;
        if (!json.isBool()) return fail(where, "expected true or false");
        out = json.boolean();
        return true;
    }

    bool vec3(const Json& json, const std::string& where, Vec3& out) {
        if (json.isNull()) return true;
        if (!json.isArray() || json.size() != 3) return fail(where, "expected three numbers");
        for (std::size_t i = 0; i < 3; ++i) {
            if (!json[i].isNumber() || !std::isfinite(json[i].number())) return fail(where, "expected three numbers");
            out[static_cast<int>(i)] = static_cast<float>(json[i].number());
        }
        return true;
    }

    bool color(const Json& json, const std::string& where, Vec3& out) {
        if (json.isNull()) return true;
        if (!parseSceneColor(json, out)) return fail(where, "expected a colour: \"#rrggbb\" or [r, g, b]");
        return true;
    }

    // ------------------------------------------------------------- models ---

    bool transform(const Json& json, const std::string& where, Mat4& out) {
        Vec3 position{0.0f, 0.0f, 0.0f}, degrees{0.0f, 0.0f, 0.0f}, size{1.0f, 1.0f, 1.0f};
        if (!vec3(json["position"], where + ".position", position)) return false;
        if (!vec3(json["rotation"], where + ".rotation", degrees)) return false;
        const Json& s = json["scale"];
        if (s.isNumber()) size = Vec3(static_cast<float>(s.number()));
        else if (!vec3(s, where + ".scale", size)) return fail(where + ".scale", "expected a number or three numbers");
        // Rotation is given as degrees about x, y and z. Applied as yaw
        // (about y), then pitch (about x), then roll (about z) - the order
        // in which "turn it round, then tilt it" does what it says.
        out = translate(position) * rotate({0.0f, 1.0f, 0.0f}, radians(degrees.y)) *
              rotate({1.0f, 0.0f, 0.0f}, radians(degrees.x)) * rotate({0.0f, 0.0f, 1.0f}, radians(degrees.z)) *
              scale(size);
        return true;
    }

    bool materialOverride(const Json& json, const std::string& where, Model& model) {
        if (json.isNull()) return true;
        if (!json.isObject()) return fail(where, "expected an object");
        for (Material& material : model.materials) {
            Vec3 base = material.baseColor.xyz();
            if (!json["color"].isNull()) {
                if (!color(json["color"], where + ".color", base)) return false;
                material.baseColor = Vec4(base, material.baseColor.w);
                material.baseColorTexture = -1;  // a flat colour replaces the texture
            }
            if (!number(json["metallic"], where + ".metallic", material.metallic)) return false;
            if (!number(json["roughness"], where + ".roughness", material.roughness)) return false;
            if (!color(json["emissive"], where + ".emissive", material.emissive)) return false;
            float opacity = material.baseColor.w;
            if (!number(json["opacity"], where + ".opacity", opacity)) return false;
            if (opacity < 1.0f) {
                material.baseColor.w = std::max(opacity, 0.0f);
                material.alphaMode = AlphaMode::Blend;
            }
            bool doubleSided = material.doubleSided;
            if (!boolean(json["doubleSided"], where + ".doubleSided", doubleSided)) return false;
            material.doubleSided = doubleSided;
        }
        return true;
    }

    bool model(const Json& json, const std::string& where) {
        if (!json.isObject()) return fail(where, "expected an object");
        Model loaded;
        std::string source, name = json["name"].string();

        if (json["file"].isString()) {
            const std::string file = json["file"].string();
            LoadResult load = loadModel(directory + file);
            for (const std::string& warning : load.warnings) result.warnings.push_back(file + ": " + warning);
            if (!load.ok) return fail(where + ".file", load.error);
            loaded = std::move(load.model);
            source = directory + file;
            if (name.empty()) name = detail::fileNameOf(file);
        } else if (json["demo"].isString()) {
            const std::string demo = json["demo"].string();
            loaded = makeDemoModel(demo);
            if (loaded.meshes.empty()) return fail(where + ".demo", "there is no built-in model called '" + demo + "'");
            source = "demo:" + demo;
            if (name.empty()) name = demo;
        } else {
            return fail(where, "needs a \"file\" or a \"demo\"");
        }

        if (!materialOverride(json["material"], where + ".material", loaded)) return false;
        Mat4 placement;
        if (!transform(json, where, placement)) return false;

        SceneObject& object = result.scene.add(std::move(loaded), name, placement, pool);
        object.source = source;
        bool visible = true;
        if (!boolean(json["visible"], where + ".visible", visible)) return false;
        object.visible = visible;

        // Which animation to play, and how.
        if (!object.player) {
            if (!json["animation"].isNull())
                result.warnings.push_back(where + ": '" + name + "' has no animations; \"animation\" ignored");
            return true;
        }
        AnimationPlayer& player = *object.player;
        const Json& animation = json["animation"];
        if (animation.isNumber()) {
            const int index = static_cast<int>(animation.number());
            if (index < 1 || index > player.clipCount())
                return fail(where + ".animation",
                            "'" + name + "' has " + std::to_string(player.clipCount()) + " animations");
            player.setClip(index - 1);
        } else if (animation.isString()) {
            int found = -1;
            for (int i = 0; i < player.clipCount(); ++i)
                if (object.model.rig.animations[static_cast<std::size_t>(i)].name == animation.string()) found = i;
            if (found < 0)
                return fail(where + ".animation",
                            "'" + name + "' has no animation called '" + animation.string() + "'");
            player.setClip(found);
        } else if (!animation.isNull()) {
            return fail(where + ".animation", "expected a name or a number");
        }
        float speed = 1.0f, time = 0.0f;
        bool paused = false;
        if (!number(json["animationSpeed"], where + ".animationSpeed", speed)) return false;
        if (!number(json["animationTime"], where + ".animationTime", time)) return false;
        if (!boolean(json["paused"], where + ".paused", paused)) return false;
        player.setSpeed(speed);
        player.seek(time);
        if (paused) player.pause();
        applyPose(object.model, player.pose(), pool);
        object.accel.refresh(object.model);
        object.accel.updateTrees(object.model, pool);
        return true;
    }

    // ------------------------------------------------------------- lights ---

    bool light(const Json& json, const std::string& where) {
        if (!json.isObject()) return fail(where, "expected an object");
        Light out;
        const std::string type = json["type"].string("directional");
        if (type == "directional" || type == "sun") out.type = LightType::Directional;
        else if (type == "point") out.type = LightType::Point;
        else if (type == "spot") out.type = LightType::Spot;
        else return fail(where + ".type", "expected \"directional\", \"point\" or \"spot\"");

        if (!vec3(json["position"], where + ".position", out.position)) return false;
        if (!vec3(json["direction"], where + ".direction", out.direction)) return false;
        // "target" is an easier way to aim a spot light than a direction.
        if (!json["target"].isNull()) {
            Vec3 target;
            if (!vec3(json["target"], where + ".target", target)) return false;
            out.direction = target - out.position;
        }
        if (lengthSquared(out.direction) < 1e-12f) return fail(where, "the light's direction has no length");

        if (!color(json["color"], where + ".color", out.color)) return false;
        if (!number(json["intensity"], where + ".intensity", out.intensity)) return false;
        if (!number(json["range"], where + ".range", out.range)) return false;
        if (!number(json["specular"], where + ".specular", out.specular)) return false;
        bool shadows = false, attached = false;
        if (!boolean(json["shadows"], where + ".shadows", shadows)) return false;
        if (!boolean(json["attachedToCamera"], where + ".attachedToCamera", attached)) return false;
        out.castsShadow = shadows;
        out.cameraSpace = attached;

        // The cone: "angle" is its half-width in degrees, "blend" the
        // fraction of that over which the edge fades.
        float angle = 30.0f, blend = 0.2f;
        if (!number(json["angle"], where + ".angle", angle)) return false;
        if (!number(json["blend"], where + ".blend", blend)) return false;
        angle = std::clamp(angle, 1.0f, 85.0f);
        out.outerCone = radians(angle);
        out.innerCone = radians(angle * (1.0f - std::clamp(blend, 0.0f, 1.0f)));

        if (out.intensity < 0.0f || out.range < 0.0f) return fail(where, "intensity and range cannot be negative");
        result.scene.lights.push_back(out);
        return true;
    }

    // -------------------------------------------------------- environment ---

    bool environment(const Json& json, const std::string& where) {
        if (json.isNull()) return true;
        if (!json.isObject()) return fail(where, "expected an object");
        Scene& scene = result.scene;
        if (json["file"].isString()) {
            std::string error;
            if (!loadEnvironment(directory + json["file"].string(), scene.environment, error))
                return fail(where + ".file", error);
            scene.hasEnvironment = true;
        } else if (json["name"].isString()) {
            if (!makeBuiltinEnvironment(json["name"].string(), scene.environment))
                return fail(where + ".name", "there is no built-in environment called '" + json["name"].string() + "'");
            scene.hasEnvironment = true;
        } else if (!json["sky"].isNull() || !json["ground"].isNull()) {
            // Two colours: the simplest environment there is.
            Vec3 sky{0.42f, 0.47f, 0.55f}, ground{0.14f, 0.12f, 0.11f};
            if (!color(json["sky"], where + ".sky", sky) || !color(json["ground"], where + ".ground", ground))
                return false;
            scene.environment = Environment::hemisphere(sky, ground);
            scene.environment.name = "custom";
            scene.hasEnvironment = true;
        }
        float rotation = 0.0f;
        if (!number(json["intensity"], where + ".intensity", scene.environment.intensity)) return false;
        if (!number(json["rotation"], where + ".rotation", rotation)) return false;
        scene.environment.rotation = radians(rotation);
        if (!json["skybox"].isNull()) {
            bool skybox = false;
            if (!boolean(json["skybox"], where + ".skybox", skybox)) return false;
            scene.skybox = skybox;
        }
        return number(json["blur"], where + ".blur", scene.skyBlur);
    }

    // ------------------------------------------------------------- camera ---

    bool pose(const Json& json, const std::string& where, CameraPose& out) {
        float fov = degrees(out.fov);
        if (!vec3(json["position"], where + ".position", out.position)) return false;
        if (!vec3(json["target"], where + ".target", out.target)) return false;
        if (!number(json["fov"], where + ".fov", fov)) return false;
        if (fov < 5.0f || fov > 150.0f) return fail(where + ".fov", "expected an angle between 5 and 150 degrees");
        out.fov = radians(fov);
        return true;
    }

    bool camera(const Json& json, const std::string& where) {
        if (json.isNull()) return true;
        if (!json.isObject()) return fail(where, "expected an object");
        Scene& scene = result.scene;
        const Bounds bounds = scene.bounds();

        // A camera with a position is a fixed view. Everything else starts
        // from the default framing of the scene.
        CameraPose fixed;
        fixed.target = bounds.center();
        if (!pose(json, where, fixed)) return false;
        if (!json["position"].isNull()) scene.camera = fixed;

        const Json& turntable = json["turntable"];
        const Json& path = json["path"];
        if (!turntable.isNull() && !path.isNull()) return fail(where, "has both a \"turntable\" and a \"path\"");

        if (!turntable.isNull()) {
            if (!turntable.isObject()) return fail(where + ".turntable", "expected an object");
            float seconds = 12.0f, elevation = 15.0f, distance = 1.0f;
            const std::string w = where + ".turntable";
            if (!number(turntable["seconds"], w + ".seconds", seconds)) return false;
            if (!number(turntable["elevation"], w + ".elevation", elevation)) return false;
            if (!number(turntable["distance"], w + ".distance", distance)) return false;
            if (seconds <= 0.0f) return fail(w + ".seconds", "must be more than zero");
            if (distance <= 0.0f) return fail(w + ".distance", "must be more than zero");
            scene.cameraPath =
                makeTurntable(bounds, seconds, radians(std::clamp(elevation, -89.0f, 89.0f)), distance, fixed.fov);
        }

        if (!path.isNull()) {
            if (!path.isObject()) return fail(where + ".path", "expected an object");
            const std::string w = where + ".path";
            const Json& keys = path["keys"];
            if (!keys.isArray() || keys.size() == 0) return fail(w + ".keys", "expected a list of keyframes");
            CameraPath out;
            bool loop = false;
            if (!boolean(path["loop"], w + ".loop", loop)) return false;
            out.loop = loop;
            if (!number(path["duration"], w + ".duration", out.duration)) return false;
            const std::string interpolation = path["interpolation"].string("smooth");
            if (interpolation == "smooth") out.smooth = true;
            else if (interpolation == "linear") out.smooth = false;
            else return fail(w + ".interpolation", "expected \"smooth\" or \"linear\"");

            float previous = -1.0f;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const std::string k = w + ".keys[" + std::to_string(i) + "]";
                if (!keys[i].isObject()) return fail(k, "expected an object");
                CameraKey key;
                // A key inherits whatever it leaves out from the one before.
                key.pose = out.keys.empty() ? fixed : out.keys.back().pose;
                key.time = out.keys.empty() ? 0.0f : out.keys.back().time + 1.0f;
                if (!number(keys[i]["time"], k + ".time", key.time)) return false;
                if (key.time < 0.0f || key.time <= previous) return fail(k + ".time", "keyframe times must increase");
                previous = key.time;
                if (!pose(keys[i], k, key.pose)) return false;
                out.keys.push_back(key);
            }
            out.finish();
            scene.cameraPath = std::move(out);
        }
        return true;
    }

    bool document(const Json& root) {
        if (!root.isObject()) return fail("scene", "expected an object at the top level");
        Scene& scene = result.scene;
        scene.name = root["name"].string();

        const Json& models = root["models"];
        if (!models.isArray() || models.size() == 0) return fail("models", "a scene needs at least one model");
        for (std::size_t i = 0; i < models.size(); ++i)
            if (!model(models[i], "models[" + std::to_string(i) + "]")) return false;

        const Json& lights = root["lights"];
        if (!lights.isNull() && !lights.isArray()) return fail("lights", "expected a list");
        for (std::size_t i = 0; i < lights.size(); ++i)
            if (!light(lights[i], "lights[" + std::to_string(i) + "]")) return false;

        if (!environment(root["environment"], "environment")) return false;
        if (!camera(root["camera"], "camera")) return false;

        const Json& render = root["render"];
        if (!render.isNull() && !render.isObject()) return fail("render", "expected an object");
        scene.renderSettings = render;

        // A misspelt section would otherwise be silently ignored.
        for (const auto& member : root.members()) {
            const std::string& key = member.first;
            if (key != "name" && key != "models" && key != "lights" && key != "environment" && key != "camera" &&
                key != "render")
                result.warnings.push_back("unknown section \"" + key + "\" ignored");
        }
        return true;
    }
};

}  // namespace

bool parseSceneColor(const Json& value, Vec3& linear) {
    if (value.isString()) {
        std::string text = value.string();
        if (!text.empty() && text[0] == '#') text.erase(0, 1);
        if (text.size() != 6 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
        const unsigned long rgb = std::strtoul(text.c_str(), nullptr, 16);
        // Hex colours are sRGB, as everywhere else; lighting works in linear light.
        linear = srgbToLinear(Color8{static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
                                     static_cast<std::uint8_t>(rgb), 255});
        return true;
    }
    if (!value.isArray() || value.size() != 3) return false;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value[i].isNumber() || !std::isfinite(value[i].number()) || value[i].number() < 0.0) return false;
        linear[static_cast<int>(i)] = static_cast<float>(value[i].number());
    }
    return true;
}

SceneLoadResult loadSceneText(const std::string& text, const std::string& directory, ThreadPool* pool) {
    SceneLoadResult result;
    Json root;
    std::string error;
    if (!Json::parse(text, root, error)) {
        result.error = error;
        return result;
    }
    Reader reader{result, directory, pool};
    result.ok = reader.document(root);
    return result;
}

SceneLoadResult loadSceneFile(const std::string& path, ThreadPool* pool) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SceneLoadResult result;
        result.error = "cannot open '" + path + "'";
        return result;
    }
    std::ostringstream text;
    text << file.rdbuf();
    SceneLoadResult result = loadSceneText(text.str(), detail::directoryOf(path), pool);
    if (!result.ok) result.error = "'" + path + "': " + result.error;
    if (result.scene.name.empty()) result.scene.name = detail::fileNameOf(path);
    return result;
}

}  // namespace termesh
