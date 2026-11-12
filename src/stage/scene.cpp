#include "stage/scene.hpp"

#include <algorithm>

#include "anim/skinning.hpp"

namespace termesh {

SceneObject& Scene::add(Model model, std::string objectName, const Mat4& transform, ThreadPool* pool) {
    auto object = std::make_unique<SceneObject>();
    object->name = std::move(objectName);
    object->model = std::move(model);
    object->transform = transform;
    object->accel.build(object->model, pool);
    // The player keeps a pointer to the rig, which is why objects are held
    // by pointer: the model must never move again.
    if (object->model.animated()) object->player = std::make_unique<AnimationPlayer>(object->model.rig);
    objects.push_back(std::move(object));
    return *objects.back();
}

std::vector<DrawItem> Scene::drawItems() const {
    std::vector<DrawItem> items;
    for (const auto& object : objects)
        if (object->visible) items.push_back({&object->model, object->transform, &object->accel});
    return items;
}

int Scene::objectOfItem(int item) const {
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (objects[i]->visible && item-- == 0) return static_cast<int>(i);
    return -1;
}

int Scene::itemOfObject(int object) const {
    if (object < 0 || static_cast<std::size_t>(object) >= objects.size() ||
        !objects[static_cast<std::size_t>(object)]->visible)
        return -1;
    int item = 0;
    for (int i = 0; i < object; ++i) item += objects[static_cast<std::size_t>(i)]->visible;
    return item;
}

Bounds Scene::bounds() const {
    Bounds box = emptyBounds();
    bool any = false;
    for (const auto& object : objects) {
        if (!object->visible || object->model.meshes.empty()) continue;
        const Bounds moved = transformBounds(object->model.framingBounds(), object->transform);
        include(box, moved.min);
        include(box, moved.max);
        any = true;
    }
    return any ? box : Bounds{};
}

std::size_t Scene::triangleCount() const {
    std::size_t n = 0;
    for (const auto& object : objects) n += object->model.triangleCount();
    return n;
}

std::size_t Scene::vertexCount() const {
    std::size_t n = 0;
    for (const auto& object : objects) n += object->model.vertexCount();
    return n;
}

bool Scene::animated() const {
    return std::any_of(objects.begin(), objects.end(), [](const auto& object) { return object->player != nullptr; });
}

bool Scene::playing() const {
    return std::any_of(objects.begin(), objects.end(), [](const auto& object) {
        return object->player && (object->player->playing() || object->player->fading());
    });
}

bool Scene::update(float dt, ThreadPool* pool) {
    bool moved = false;
    for (const auto& object : objects) {
        if (!object->player || !object->player->advance(dt)) continue;
        applyPose(object->model, object->player->pose(), pool);
        object->accel.refresh(object->model);  // the boxes follow; trees are rebuilt only when needed
        moved = true;
    }
    return moved;
}

void Scene::pose(ThreadPool* pool) {
    for (const auto& object : objects) {
        if (!object->player) continue;
        applyPose(object->model, object->player->pose(), pool);
        object->accel.refresh(object->model);
    }
}

void Scene::buildLods(ThreadPool* pool) {
    for (const auto& object : objects) object->accel.buildLods(object->model, pool);
}

void Scene::arrangeInRow() {
    float x = 0.0f;
    for (const auto& object : objects) {
        const Bounds box = object->model.framingBounds();
        const float radius = std::max(box.radius(), 1e-6f);
        // Unit size, centred over the origin, feet on the ground...
        const float scaleBy = 1.0f / radius;
        const Vec3 size = (box.max - box.min) * scaleBy;
        const Mat4 normalise = scale(Vec3(scaleBy)) * translate({-box.center().x, -box.min.y, -box.center().z});
        // ...then moved along the row by half its own width, plus a gap.
        x += size.x * 0.5f;
        object->transform = translate({x, 0.0f, 0.0f}) * normalise;
        x += size.x * 0.5f + 0.4f;
    }
    // Centre the row on the origin.
    const float shift = objects.empty() ? 0.0f : (x - 0.4f) * 0.5f;
    for (const auto& object : objects) object->transform = translate({-shift, 0.0f, 0.0f}) * object->transform;
}

}  // namespace termesh
