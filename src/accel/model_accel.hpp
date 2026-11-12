// ModelAccel: the boxes and BVHs that go with a model, and picking.
#pragma once

#include <span>
#include <vector>

#include "accel/bvh.hpp"
#include "accel/simplify.hpp"
#include "core/thread_pool.hpp"
#include "scene/camera.hpp"
#include "scene/draw_item.hpp"

namespace termesh {

// One bounding box and one BVH per mesh. Kept beside the model rather than
// inside it: a model is data that comes from a file, this is derived from
// it and has to be redone whenever the vertices move.
class ModelAccel {
public:
    ModelAccel() = default;
    explicit ModelAccel(const Model& model, ThreadPool* pool = nullptr) { build(model, pool); }

    // Build everything from the model as it is now.
    void build(const Model& model, ThreadPool* pool = nullptr);

    // The model's vertices have moved (an animation frame). Recomputes the
    // boxes, which is cheap; the trees are only marked out of date and are
    // rebuilt by the next call to updateTrees().
    void refresh(const Model& model);

    // Rebuild any trees that refresh() left out of date.
    void updateTrees(const Model& model, ThreadPool* pool = nullptr);

    // Build levels of detail for the larger meshes (see simplify.hpp).
    // Unlike the trees these survive animation: a level is a list of
    // triangles, and stays valid wherever the vertices go.
    void buildLods(const Model& model, ThreadPool* pool = nullptr);
    // Install levels built elsewhere (one list per mesh), for example on a
    // background thread.
    void setLods(std::vector<std::vector<MeshLod>> lods);
    // The coarser versions of a mesh, finest first; empty if it has none.
    const std::vector<MeshLod>& lods(std::size_t mesh) const { return entries_[mesh].lods; }

    std::size_t meshCount() const { return entries_.size(); }
    const Bounds& bounds(std::size_t mesh) const { return entries_[mesh].bounds; }
    // The mesh's tree, or null while it is out of date.
    const Bvh* bvh(std::size_t mesh) const { return entries_[mesh].stale ? nullptr : &entries_[mesh].bvh; }

private:
    struct Entry {
        Bounds bounds;
        Bvh bvh;
        bool stale = true;
        std::vector<MeshLod> lods;
    };
    std::vector<Entry> entries_;
};

struct PickResult {
    int item = -1;               // index into the draw list
    int mesh = -1;               // index into that model's meshes
    std::uint32_t triangle = 0;  // index of the triangle within the mesh
    float distance = 0.0f;       // from the camera, in world units
    Vec3 position;               // world space
};

// The ray leaving the camera through a point of the image. (x, y) are in
// normalised device coordinates: -1 .. 1, with +y up. The ray runs from the
// near plane to the far plane as t goes from 0 to 1.
Ray cameraRay(const Camera& camera, float ndcX, float ndcY);

// What is under that point of the image: the nearest triangle of any item
// along the camera ray. With cullBackfaces, back faces of single-sided
// materials are passed through, exactly as they are when drawing.
bool pick(std::span<const DrawItem> items, const Camera& camera, float ndcX, float ndcY, bool cullBackfaces,
          PickResult& result);

}  // namespace termesh
