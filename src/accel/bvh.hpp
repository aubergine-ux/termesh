// Bvh: a bounding volume hierarchy over the triangles of a mesh.
//
// Two questions come up that the rasterizer cannot answer cheaply:
//
//   "which triangle is under the mouse?"     a ray against every triangle
//   "which triangles can the camera see?"    a frustum against every triangle
//
// Both are searches, and a BVH turns them from "test everything" into "test
// a few boxes". The triangles are grouped into a tree of axis-aligned boxes:
// the root box holds them all, its two children each hold part, and so on
// down to leaves of a handful of triangles. If a ray misses a box, or a box
// lies wholly outside the frustum, everything inside it is dismissed in one
// test.
//
//                  ┌──────────────────────┐
//                  │ root: all triangles  │
//                  └──────┬────────┬──────┘
//                 ┌───────┴──┐  ┌──┴───────┐
//                 │ left half│  │right half│     ...down to leaves of
//                 └──┬────┬──┘  └──┬────┬──┘        <= 4 triangles
//                   ...  ...      ...  ...
#pragma once

#include <cstdint>
#include <vector>

#include "math/mat.hpp"
#include "scene/model.hpp"

namespace termesh {

// A ray: origin + t * direction. The direction need not be unit length; `t`
// is measured in multiples of it.
struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// Grow a box to include a point. `Bounds` (scene/model.hpp) is the box type
// used throughout: a minimum and a maximum corner.
inline void include(Bounds& box, Vec3 p) {
    box.min = min(box.min, p);
    box.max = max(box.max, p);
}

// A box that contains nothing: including any point makes it that point.
inline Bounds emptyBounds() { return {Vec3(3.4e38f), Vec3(-3.4e38f)}; }

// The box around a box moved by a transform (the box around its 8 corners).
Bounds transformBounds(const Bounds& box, const Mat4& transform);

// Does the ray pass through the box between distances 0 and tMax? The "slab"
// test: a box is the overlap of three slabs, one per axis; the ray is inside
// the box while it is inside all three at once.
bool rayHitsBox(const Ray& ray, Vec3 inverseDirection, const Bounds& box, float tMax);

// Where the ray crosses the triangle (a, b, c), if it does: the distance t,
// and the barycentric weights u (of b) and v (of c). Moller-Trumbore.
bool rayHitsTriangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c, float& t, float& u, float& v);

// The six planes of a view frustum, each as (normal, d) with the inside on
// the positive side: dot(normal, p) + d >= 0.
struct Frustum {
    Vec4 planes[6];

    // From any matrix that takes points to clip space. Given a
    // model-view-projection matrix the planes come out in model space,
    // which is where a mesh's BVH lives.
    //
    // A point is inside when -w <= x <= w (and the same for y, z). Written
    // out, "w + x >= 0" is the dot product of the point with (row 3 +
    // row 0) of the matrix: each plane is just a sum or difference of two
    // rows (Gribb and Hartmann).
    static Frustum fromMatrix(const Mat4& clipFromLocal);

    enum class Side { Outside, Intersecting, Inside };

    // Where a box lies. For each plane only one corner matters: the one
    // furthest along the plane's normal. If even that corner is behind the
    // plane the whole box is out.
    Side classify(const Bounds& box) const;
};

struct BvhHit {
    float t = 0.0f;              // distance along the ray, in multiples of its direction
    std::uint32_t triangle = 0;  // index of the triangle in the mesh (indices[3 * triangle ...])
    float u = 0.0f, v = 0.0f;    // barycentric weights of the triangle's second and third vertex
};

class Bvh {
public:
    struct Node {
        Bounds bounds;
        // A leaf (count > 0) holds triangles order_[first .. first + count).
        // An inner node (count == 0) has its children at `first` and
        // `first + 1`: siblings are stored side by side.
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    // Build over the mesh's triangles as they are now. The mesh is not kept:
    // pass the same one (unchanged) to the queries.
    void build(const Mesh& mesh);

    bool empty() const { return nodes_.empty(); }
    const Bounds& bounds() const { return nodes_[0].bounds; }
    std::size_t nodeCount() const { return nodes_.size(); }
    std::size_t triangleCount() const { return order_.size(); }
    const std::vector<Node>& nodes() const { return nodes_; }
    // Triangle indices, grouped so that each node's triangles are contiguous.
    const std::vector<std::uint32_t>& order() const { return order_; }

    // The nearest triangle the ray hits within distance tMax, if any.
    // With cullBackfaces, triangles facing away from the ray's origin are
    // passed through, as the renderer does.
    bool intersect(const Mesh& mesh, const Ray& ray, float tMax, bool cullBackfaces, BvhHit& hit) const;

    // Set bit i of `visible` for every triangle i that may be inside the
    // frustum (bits are only ever set, so clear it first). Conservative: a
    // marked triangle can still turn out to be off-screen, but no visible
    // one is missed.
    void markVisible(const Frustum& frustum, std::vector<std::uint64_t>& visible) const;

private:
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> order_;
};

}  // namespace termesh
