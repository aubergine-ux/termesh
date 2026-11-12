#include "app/inspector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace termesh {

namespace {

std::string count(std::size_t n) {
    char buf[32];
    if (n >= 1'000'000) std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1e6);
    else if (n >= 10'000) std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1e3);
    else std::snprintf(buf, sizeof(buf), "%zu", n);
    return buf;
}

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3g", static_cast<double>(v));
    return buf;
}

std::string vector(Vec3 v) { return number(v.x) + ", " + number(v.y) + ", " + number(v.z); }

// How many columns a string takes: one per character. UTF-8 continuation
// bytes (10xxxxxx) are the second and later bytes of a character.
int columns(const std::string& text) {
    int n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// The first `width` columns of a string, never cutting a character in half.
std::string clip(const std::string& text, int width) {
    int n = 0;
    std::size_t end = 0;
    while (end < text.size()) {
        const bool starts = (static_cast<unsigned char>(text[end]) & 0xC0) != 0x80;
        if (starts && n == width) break;
        n += starts;
        ++end;
    }
    return text.substr(0, end);
}

// `left` and `right` on one line of exactly `width` columns. The left part
// gives way (with an ellipsis) if both do not fit.
std::string fit(const std::string& left, const std::string& right, int width, bool unicode) {
    if (width <= 0) return {};
    const int rightWidth = std::min(columns(right), width);
    const int room = width - rightWidth - (rightWidth ? 1 : 0);
    std::string out = left;
    if (columns(out) > room)
        out = room > 1 ? clip(out, room - 1) + (unicode ? "\xe2\x80\xa6" : "~") : clip(out, std::max(room, 0));
    out += std::string(static_cast<std::size_t>(std::max(width - columns(out) - rightWidth, 0)), ' ');
    out += clip(right, rightWidth);
    return out;
}

const char* lightTypeName(LightType type) {
    switch (type) {
        case LightType::Point: return "point";
        case LightType::Spot: return "spot";
        default: return "directional";
    }
}

std::string textureLine(const Model& model, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= model.textures.size()) return "none";
    const Texture& texture = model.textures[static_cast<std::size_t>(index)];
    std::string out = std::to_string(texture.width()) + "x" + std::to_string(texture.height());
    if (!texture.name.empty()) out += " " + texture.name;
    return out;
}

}  // namespace

void Inspector::rebuild(const Scene& scene) {
    // The first time round, open things up to a sensible depth: every
    // object if there are only a few, and a lone object's meshes.
    if (!initialised_) {
        initialised_ = true;
        if (scene.objects.size() <= 4)
            for (std::size_t i = 0; i < scene.objects.size(); ++i) expanded_.insert("o" + std::to_string(i));
        if (scene.objects.size() == 1) expanded_.insert("o0/meshes");
        expanded_.insert("lights");
    }

    const std::string previous = selectedRow() ? selectedRow()->key : std::string();
    rows_.clear();
    auto add = [&](InspectorRow::Kind kind, int depth, std::string label, std::string value, std::string key,
                   int object, int index, bool expandable) -> bool {
        InspectorRow row;
        row.kind = kind;
        row.depth = depth;
        row.label = std::move(label);
        row.value = std::move(value);
        row.key = std::move(key);
        row.object = object;
        row.index = index;
        row.expandable = expandable;
        row.expanded = expandable && expanded_.count(row.key) > 0;
        rows_.push_back(std::move(row));
        return rows_.back().expanded;
    };

    for (std::size_t o = 0; o < scene.objects.size(); ++o) {
        const SceneObject& object = *scene.objects[o];
        const Model& model = object.model;
        const int oi = static_cast<int>(o);
        const std::string base = "o" + std::to_string(o);
        std::string state = count(model.triangleCount()) + " tris";
        if (!object.visible) state = "hidden";
        if (!add(InspectorRow::Kind::Object, 0, object.name.empty() ? "(unnamed)" : object.name, state, base, oi, -1,
                 true))
            continue;

        if (add(InspectorRow::Kind::Group, 1, "meshes", std::to_string(model.meshes.size()), base + "/meshes", oi, -1,
                !model.meshes.empty()))
            for (std::size_t m = 0; m < model.meshes.size(); ++m) {
                const Mesh& mesh = model.meshes[m];
                add(InspectorRow::Kind::Mesh, 2, mesh.name.empty() ? "mesh " + std::to_string(m + 1) : mesh.name,
                    count(mesh.indices.size() / 3), base + "/mesh" + std::to_string(m), oi, static_cast<int>(m), false);
            }

        if (add(InspectorRow::Kind::Group, 1, "materials", std::to_string(model.materials.size()), base + "/materials",
                oi, -1, !model.materials.empty()))
            for (std::size_t m = 0; m < model.materials.size(); ++m) {
                const Material& material = model.materials[m];
                add(InspectorRow::Kind::Material, 2,
                    material.name.empty() ? "material " + std::to_string(m + 1) : material.name, "",
                    base + "/material" + std::to_string(m), oi, static_cast<int>(m), false);
            }

        // The skeleton, for models that kept theirs (animated ones): each
        // node indented under its parent.
        if (!model.rig.nodes.empty() && add(InspectorRow::Kind::Group, 1, "nodes",
                                            std::to_string(model.rig.nodes.size()), base + "/nodes", oi, -1, true)) {
            std::vector<int> depth(model.rig.nodes.size(), 0);
            for (const int n : model.rig.order) {
                const RigNode& node = model.rig.nodes[static_cast<std::size_t>(n)];
                depth[static_cast<std::size_t>(n)] =
                    node.parent >= 0 ? depth[static_cast<std::size_t>(node.parent)] + 1 : 0;
            }
            // `order` lists a node before its children, but not necessarily
            // next to them; walk the tree to keep families together.
            std::vector<std::vector<int>> children(model.rig.nodes.size());
            std::vector<int> roots;
            for (std::size_t n = 0; n < model.rig.nodes.size(); ++n) {
                const int parent = model.rig.nodes[n].parent;
                (parent >= 0 ? children[static_cast<std::size_t>(parent)] : roots).push_back(static_cast<int>(n));
            }
            std::vector<int> stack(roots.rbegin(), roots.rend());
            while (!stack.empty()) {
                const int n = stack.back();
                stack.pop_back();
                const RigNode& node = model.rig.nodes[static_cast<std::size_t>(n)];
                add(InspectorRow::Kind::Node, 2 + std::min(depth[static_cast<std::size_t>(n)], 12),
                    node.name.empty() ? "node " + std::to_string(n) : node.name, "", base + "/node" + std::to_string(n),
                    oi, n, false);
                const std::vector<int>& kids = children[static_cast<std::size_t>(n)];
                stack.insert(stack.end(), kids.rbegin(), kids.rend());
            }
        }
    }

    const std::string lightCount = scene.lights.empty() ? "default rig" : std::to_string(scene.lights.size());
    if (add(InspectorRow::Kind::Group, 0, "lights", lightCount, "lights", -1, -1, !scene.lights.empty()))
        for (std::size_t l = 0; l < scene.lights.size(); ++l)
            add(InspectorRow::Kind::Light, 1, lightTypeName(scene.lights[l].type),
                scene.lights[l].castsShadow ? "shadows" : "", "light" + std::to_string(l), -1, static_cast<int>(l),
                false);
    add(InspectorRow::Kind::Environment, 0, "environment", scene.environment.name, "environment", -1, -1, false);
    std::string camera = "orbit";
    if (!scene.cameraPath.empty()) camera = "path " + number(scene.cameraPath.duration) + "s";
    else if (scene.camera) camera = "fixed";
    add(InspectorRow::Kind::Camera, 0, "camera", camera, "camera", -1, -1, false);

    // Stay on the same row if it still exists.
    selected_ = std::clamp(selected_, 0, std::max(static_cast<int>(rows_.size()) - 1, 0));
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].key == previous) selected_ = static_cast<int>(i);
}

void Inspector::move(int delta) { select(selected_ + delta); }

void Inspector::select(int row) { selected_ = std::clamp(row, 0, std::max(static_cast<int>(rows_.size()) - 1, 0)); }

void Inspector::setExpanded(const Scene& scene, int open) {
    const InspectorRow* row = selectedRow();
    if (!row) return;
    if (!row->expandable) {
        // Closing a leaf closes the heading it sits under, which is what
        // "left" means in every tree view.
        if (open != 0 || row->depth == 0) return;
        for (int i = selected_ - 1; i >= 0; --i)
            if (rows_[static_cast<std::size_t>(i)].depth < row->depth &&
                rows_[static_cast<std::size_t>(i)].expandable) {
                selected_ = i;
                break;
            }
        return;
    }
    const bool want = open < 0 ? !row->expanded : open > 0;
    if (want) expanded_.insert(row->key);
    else expanded_.erase(row->key);
    rebuild(scene);
}

void Inspector::selectMesh(const Scene& scene, int object, int mesh) {
    if (object < 0) return;
    const std::string base = "o" + std::to_string(object);
    expanded_.insert(base);
    if (mesh >= 0) expanded_.insert(base + "/meshes");
    rebuild(scene);
    const std::string key = mesh >= 0 ? base + "/mesh" + std::to_string(mesh) : base;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].key == key) selected_ = static_cast<int>(i);
}

void Inspector::selection(int& object, int& mesh) const {
    object = mesh = -1;
    const InspectorRow* row = selectedRow();
    if (!row || row->object < 0) return;
    object = row->object;
    if (row->kind == InspectorRow::Kind::Mesh) mesh = row->index;
}

std::vector<std::string> Inspector::describe(const Scene& scene, const InspectorRow& row) {
    std::vector<std::string> out;
    auto line = [&](const std::string& name, const std::string& value) { out.push_back(name + ": " + value); };
    const SceneObject* object = row.object >= 0 && static_cast<std::size_t>(row.object) < scene.objects.size()
                                    ? scene.objects[static_cast<std::size_t>(row.object)].get()
                                    : nullptr;

    switch (row.kind) {
        case InspectorRow::Kind::Object: {
            if (!object) break;
            const Model& model = object->model;
            line("source", object->source.empty() ? "(built in code)" : object->source);
            line("triangles", count(model.triangleCount()));
            line("vertices", count(model.vertexCount()));
            line("meshes", std::to_string(model.meshes.size()));
            line("materials", std::to_string(model.materials.size()));
            line("textures", std::to_string(model.textures.size()));
            line("position", vector(object->transform.c[3].xyz()));
            const Bounds box = transformBounds(model.bounds(), object->transform);
            line("size", vector(box.max - box.min));
            if (object->player) {
                const AnimationPlayer& player = *object->player;
                line("animation", player.clipName(player.clip()) + " (" + std::to_string(player.clip() + 1) + " of " +
                                      std::to_string(player.clipCount()) + ")");
                line("time", number(player.time()) + " / " + number(player.duration()) + " s" +
                                 (player.playing() ? "" : ", paused"));
            }
            break;
        }
        case InspectorRow::Kind::Group:
            if (object) line("of", object->name);
            break;
        case InspectorRow::Kind::Mesh: {
            if (!object || row.index < 0 || static_cast<std::size_t>(row.index) >= object->model.meshes.size()) break;
            const Mesh& mesh = object->model.meshes[static_cast<std::size_t>(row.index)];
            line("triangles", count(mesh.indices.size() / 3));
            line("vertices", count(mesh.vertices.size()));
            line("material", object->model.materials[static_cast<std::size_t>(mesh.material)].name);
            line("texture coords", mesh.hasUVs ? "yes" : "no");
            if (!mesh.rig.empty()) {
                line("skinned", mesh.rig.skin >= 0 ? "yes" : "no");
                if (!mesh.rig.targets.empty()) line("morph targets", std::to_string(mesh.rig.targets.size()));
            }
            // Levels of detail, once they have been built.
            if (static_cast<std::size_t>(row.index) < object->accel.meshCount()) {
                std::string lods;
                for (const MeshLod& lod : object->accel.lods(static_cast<std::size_t>(row.index)))
                    lods += (lods.empty() ? "" : ", ") + count(lod.indices.size() / 3);
                if (!lods.empty()) line("levels of detail", lods);
                const Bounds& box = object->accel.bounds(static_cast<std::size_t>(row.index));
                line("size", vector(box.max - box.min));
            }
            break;
        }
        case InspectorRow::Kind::Material: {
            if (!object || row.index < 0 || static_cast<std::size_t>(row.index) >= object->model.materials.size())
                break;
            const Model& model = object->model;
            const Material& m = model.materials[static_cast<std::size_t>(row.index)];
            line("base colour", vector(m.baseColor.xyz()) + ", alpha " + number(m.baseColor.w));
            line("metallic", number(m.metallic));
            line("roughness", number(m.roughness));
            if (lengthSquared(m.emissive) > 0.0f) line("emissive", vector(m.emissive));
            line("alpha", m.alphaMode == AlphaMode::Opaque ? "opaque"
                          : m.alphaMode == AlphaMode::Mask ? "cut-out at " + number(m.alphaCutoff)
                                                           : "blended");
            line("double sided", m.doubleSided ? "yes" : "no");
            line("colour map", textureLine(model, m.baseColorTexture));
            line("normal map", textureLine(model, m.normalTexture));
            line("metal/rough map", textureLine(model, m.metallicRoughnessTexture));
            line("emissive map", textureLine(model, m.emissiveTexture));
            break;
        }
        case InspectorRow::Kind::Node: {
            if (!object || row.index < 0 || static_cast<std::size_t>(row.index) >= object->model.rig.nodes.size())
                break;
            const RigNode& node = object->model.rig.nodes[static_cast<std::size_t>(row.index)];
            line("parent",
                 node.parent >= 0 ? object->model.rig.nodes[static_cast<std::size_t>(node.parent)].name : "(root)");
            line("translation", vector(node.rest.translation));
            const Quat q = node.rest.rotation;
            line("rotation", number(q.x) + ", " + number(q.y) + ", " + number(q.z) + ", " + number(q.w));
            line("scale", vector(node.rest.scale));
            if (!node.weights.empty()) line("morph weights", std::to_string(node.weights.size()));
            break;
        }
        case InspectorRow::Kind::Light: {
            if (row.index < 0 || static_cast<std::size_t>(row.index) >= scene.lights.size()) break;
            const Light& light = scene.lights[static_cast<std::size_t>(row.index)];
            line("type", lightTypeName(light.type));
            line("colour", vector(light.color));
            line("intensity", number(light.intensity));
            if (light.type != LightType::Point) line("direction", vector(light.direction));
            if (light.type != LightType::Directional) {
                line("position", vector(light.position));
                line("range", light.range > 0.0f ? number(light.range) : "unlimited");
            }
            if (light.type == LightType::Spot)
                line("cone", number(degrees(light.innerCone)) + " to " + number(degrees(light.outerCone)) + " degrees");
            line("shadows", light.castsShadow ? "yes" : "no");
            break;
        }
        case InspectorRow::Kind::Environment:
            line("name", scene.environment.name);
            line("kind", scene.environment.hasMap() ? "panorama (image-based lighting)" : "two colours");
            line("intensity", number(scene.environment.intensity));
            line("rotation", number(degrees(scene.environment.rotation)) + " degrees");
            break;
        case InspectorRow::Kind::Camera:
            if (!scene.cameraPath.empty()) {
                line("path", std::to_string(scene.cameraPath.keys.size()) + " keys, " +
                                 number(scene.cameraPath.duration) + " s");
                line("interpolation", scene.cameraPath.smooth ? "smooth" : "linear");
                line("loops", scene.cameraPath.loop ? "yes" : "no");
            } else if (scene.camera) {
                line("position", vector(scene.camera->position));
                line("target", vector(scene.camera->target));
                line("field of view", number(degrees(scene.camera->fov)) + " degrees");
            } else {
                line("kind", "orbit, framed on the scene");
            }
            break;
    }
    return out;
}

Inspector::Frame Inspector::render(const Scene& scene, const RenderStats& stats, int width, int height, bool unicode) {
    Frame frame;
    if (width <= 0 || height <= 0) return frame;
    const std::string rule(static_cast<std::size_t>(width), '-');
    const char* open = unicode ? "\xe2\x96\xbe " : "v ";    // a down-pointing triangle
    const char* closed = unicode ? "\xe2\x96\xb8 " : "> ";  // a right-pointing one
    const char* leaf = "  ";

    // The bottom of the panel: the details of the selected row, then the
    // totals. Whatever is left above belongs to the outline.
    std::vector<std::string> bottom;
    const InspectorRow* current = selectedRow();
    if (current) {
        bottom.push_back(rule);
        frame.headings.push_back(-static_cast<int>(bottom.size()));  // fixed up below
        bottom.push_back(fit(" " + current->label, "", width, unicode));
        for (const std::string& detail : describe(scene, *current))
            bottom.push_back(fit("  " + detail, "", width, unicode));
    }
    bottom.push_back(rule);
    std::size_t textures = 0;
    for (const auto& object : scene.objects) textures += object->model.textures.size();
    bottom.push_back(fit(" scene",
                         count(scene.objects.size()) + (scene.objects.size() == 1 ? " model, " : " models, ") +
                             count(scene.triangleCount()) + " tris",
                         width, unicode));
    bottom.push_back(fit(" ", count(scene.vertexCount()) + " verts, " + count(textures) + " textures", width, unicode));
    std::string drawn = count(stats.trianglesDrawn) + " drawn";
    if (stats.trianglesCulled) drawn += ", " + count(stats.trianglesCulled) + " culled";
    if (stats.trianglesSimplified) drawn += ", " + count(stats.trianglesSimplified) + " lod";
    bottom.push_back(fit(" frame", drawn, width, unicode));

    // Give the outline at least a few lines, even in a short window.
    const int headerLines = 1;
    int bottomLines = std::min(static_cast<int>(bottom.size()), std::max(height - headerLines - 3, 0));
    const int treeLines = std::max(height - headerLines - bottomLines, 0);
    visibleRows_ = treeLines;

    // Scroll just far enough to keep the selection in view.
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + treeLines) scroll_ = selected_ - treeLines + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(static_cast<int>(rows_.size()) - treeLines, 0));

    frame.lines.push_back(fit(" " + (scene.name.empty() ? std::string("scene") : scene.name), "", width, unicode));
    frame.headings.insert(frame.headings.begin(), 0);
    for (int i = 0; i < treeLines; ++i) {
        const int index = scroll_ + i;
        if (index >= static_cast<int>(rows_.size())) {
            frame.lines.push_back(std::string(static_cast<std::size_t>(width), ' '));
            continue;
        }
        const InspectorRow& row = rows_[static_cast<std::size_t>(index)];
        const std::string indent(static_cast<std::size_t>(1 + row.depth * 2), ' ');
        frame.lines.push_back(fit(indent + (row.expandable ? (row.expanded ? open : closed) : leaf) + row.label,
                                  row.value + " ", width, unicode));
        if (index == selected_) frame.selectedLine = static_cast<int>(frame.lines.size()) - 1;
    }
    frame.treeLines = treeLines;

    // The last `bottomLines` of the bottom block: if it does not all fit,
    // it is the details that are cut short, not the totals.
    const int skip = static_cast<int>(bottom.size()) - bottomLines;
    for (int i = 0; i < bottomLines; ++i) {
        const int source = i < 2 ? i : i + skip;  // keep the rule and the heading, drop details after them
        frame.lines.push_back(bottom[static_cast<std::size_t>(std::min(source, static_cast<int>(bottom.size()) - 1))]);
    }
    // The details heading sits two lines into the bottom block.
    for (int& heading : frame.headings)
        if (heading < 0) heading = headerLines + treeLines + 1;
    if (!current || bottomLines < 2) frame.headings.resize(1);

    frame.lines.resize(static_cast<std::size_t>(height), std::string(static_cast<std::size_t>(width), ' '));
    return frame;
}

int Inspector::rowAtLine(int line) const {
    const int index = scroll_ + line - 1;  // line 0 is the title
    if (line < 1 || line > visibleRows_ || index >= static_cast<int>(rows_.size())) return -1;
    return index;
}

}  // namespace termesh
