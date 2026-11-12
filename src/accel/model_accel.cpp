#include "accel/model_accel.hpp"

namespace termesh {

namespace {

Bounds meshBounds(const Mesh& mesh) {
    Bounds box = emptyBounds();
    for (const Vertex& v : mesh.vertices) include(box, v.position);
    return mesh.vertices.empty() ? Bounds{} : box;
}

}  // namespace

void ModelAccel::build(const Model& model, ThreadPool* pool) {
    entries_.assign(model.meshes.size(), {});
    refresh(model);
    updateTrees(model, pool);
}

void ModelAccel::buildLods(const Model& model, ThreadPool* pool) {
    if (entries_.size() != model.meshes.size()) entries_.assign(model.meshes.size(), {});
    auto buildOne = [&](int i) {
        entries_[static_cast<std::size_t>(i)].lods = termesh::buildLods(model.meshes[static_cast<std::size_t>(i)]);
    };
    const int count = static_cast<int>(entries_.size());
    if (pool && count > 1) pool->parallelFor(count, buildOne);
    else
        for (int i = 0; i < count; ++i) buildOne(i);
}

void ModelAccel::setLods(std::vector<std::vector<MeshLod>> lods) {
    if (lods.size() != entries_.size()) return;  // built for some other model
    for (std::size_t i = 0; i < entries_.size(); ++i) entries_[i].lods = std::move(lods[i]);
}

void ModelAccel::refresh(const Model& model) {
    if (entries_.size() != model.meshes.size()) entries_.assign(model.meshes.size(), {});
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].bounds = meshBounds(model.meshes[i]);
        entries_[i].stale = true;
    }
}

void ModelAccel::updateTrees(const Model& model, ThreadPool* pool) {
    auto buildOne = [&](int i) {
        Entry& entry = entries_[static_cast<std::size_t>(i)];
        if (!entry.stale) return;
        entry.bvh.build(model.meshes[static_cast<std::size_t>(i)]);
        entry.stale = false;
    };
    // Meshes are independent, so their trees can be built side by side.
    const int count = static_cast<int>(entries_.size());
    if (pool && count > 1) pool->parallelFor(count, buildOne);
    else
        for (int i = 0; i < count; ++i) buildOne(i);
}

Ray cameraRay(const Camera& camera, float ndcX, float ndcY) {
    // Undo the projection for the two ends of the pixel's line of sight:
    // its points on the near plane (NDC z = -1) and the far plane (z = +1).
    const Mat4 worldFromClip = inverse(camera.projection * camera.view);
    const Vec4 nearPoint = worldFromClip * Vec4(ndcX, ndcY, -1.0f, 1.0f);
    const Vec4 farPoint = worldFromClip * Vec4(ndcX, ndcY, 1.0f, 1.0f);
    const Vec3 origin = nearPoint.xyz() / nearPoint.w;
    return {origin, farPoint.xyz() / farPoint.w - origin};
}

bool pick(std::span<const DrawItem> items, const Camera& camera, float ndcX, float ndcY, bool cullBackfaces,
          PickResult& result) {
    const Ray worldRay = cameraRay(camera, ndcX, ndcY);
    float nearest = 1.0f;  // the far plane
    bool found = false;

    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
        const DrawItem& item = items[itemIndex];
        if (!item.model) continue;
        // Rather than move the model into the world, move the ray into the
        // model: one matrix inverse instead of transforming every vertex.
        // The direction is deliberately not renormalised, so t means the
        // same thing in both spaces and hits on different models compare.
        const Mat4 modelFromWorld = inverse(item.transform);
        const Ray ray{transformPoint(modelFromWorld, worldRay.origin),
                      transformDirection(modelFromWorld, worldRay.direction)};
        const Vec3 inverseDirection{1.0f / ray.direction.x, 1.0f / ray.direction.y, 1.0f / ray.direction.z};
        // A mirroring transform reverses which way triangles wind.
        const bool mirrored =
            dot(cross(item.transform.c[0].xyz(), item.transform.c[1].xyz()), item.transform.c[2].xyz()) < 0.0f;

        for (std::size_t meshIndex = 0; meshIndex < item.model->meshes.size(); ++meshIndex) {
            const Mesh& mesh = item.model->meshes[meshIndex];
            const Material& material = item.model->materials[static_cast<std::size_t>(mesh.material)];
            const bool cull = cullBackfaces && !material.doubleSided && !mirrored;
            const bool haveAccel = item.accel && item.accel->meshCount() == item.model->meshes.size();
            if (haveAccel && !rayHitsBox(ray, inverseDirection, item.accel->bounds(meshIndex), nearest)) continue;

            BvhHit hit;
            bool hitMesh = false;
            const Bvh* bvh = haveAccel ? item.accel->bvh(meshIndex) : nullptr;
            if (bvh) {
                hitMesh = bvh->intersect(mesh, ray, nearest, cull, hit);
            } else {
                // No tree (or one that is out of date): test every triangle.
                for (std::uint32_t tri = 0; tri * 3 + 2 < mesh.indices.size(); ++tri) {
                    const Vec3 a = mesh.vertices[mesh.indices[tri * 3]].position;
                    const Vec3 b = mesh.vertices[mesh.indices[tri * 3 + 1]].position;
                    const Vec3 c = mesh.vertices[mesh.indices[tri * 3 + 2]].position;
                    float t, u, v;
                    if (!rayHitsTriangle(ray, a, b, c, t, u, v) || t >= (hitMesh ? hit.t : nearest)) continue;
                    if (cull && dot(cross(b - a, c - a), ray.direction) >= 0.0f) continue;
                    hit = {t, tri, u, v};
                    hitMesh = true;
                }
            }
            if (!hitMesh) continue;
            nearest = hit.t;
            found = true;
            result.item = static_cast<int>(itemIndex);
            result.mesh = static_cast<int>(meshIndex);
            result.triangle = hit.triangle;
            result.position = worldRay.origin + worldRay.direction * hit.t;
            result.distance = length(result.position - camera.position);
        }
    }
    return found;
}

}  // namespace termesh
