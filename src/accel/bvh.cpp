#include "accel/bvh.hpp"

#include <algorithm>
#include <cmath>

namespace termesh {

namespace {

constexpr std::uint32_t kLeafSize = 4;
constexpr int kBins = 12;
// Beyond this depth nodes are simply halved. Halving cannot go deeper than
// another 32 levels (there are fewer than 2^32 triangles), which bounds the
// tree's depth and lets the queries use a fixed-size stack.
constexpr int kMaxGuidedDepth = 48;
constexpr int kStackSize = kMaxGuidedDepth + 32 + 8;

float surfaceArea(const Bounds& box) {
    const Vec3 d = box.max - box.min;
    if (d.x < 0.0f) return 0.0f;  // empty
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

}  // namespace

Bounds transformBounds(const Bounds& box, const Mat4& transform) {
    Bounds out = emptyBounds();
    for (int corner = 0; corner < 8; ++corner)
        include(out, transformPoint(transform, {corner & 1 ? box.max.x : box.min.x, corner & 2 ? box.max.y : box.min.y,
                                                corner & 4 ? box.max.z : box.min.z}));
    return out;
}

bool rayHitsBox(const Ray& ray, Vec3 inverseDirection, const Bounds& box, float tMax) {
    float tEnter = 0.0f, tExit = tMax;
    for (int axis = 0; axis < 3; ++axis) {
        // Where the ray crosses the two planes of this slab. Dividing by a
        // zero direction gives infinities of the right signs, and the
        // min / max below then leave the interval alone - unless the origin
        // lies exactly on a plane (0 * inf = NaN), which the comparisons
        // also survive because NaN never replaces a bound.
        const float t0 = (box.min[axis] - ray.origin[axis]) * inverseDirection[axis];
        const float t1 = (box.max[axis] - ray.origin[axis]) * inverseDirection[axis];
        const float tNear = t0 < t1 ? t0 : t1, tFar = t0 < t1 ? t1 : t0;
        if (tNear > tEnter) tEnter = tNear;
        if (tFar < tExit) tExit = tFar;
        if (tEnter > tExit) return false;
    }
    return true;
}

// Moller-Trumbore. Write the hit point both ways,
//
//     origin + t * direction  =  a + u * (b - a) + v * (c - a)
//
// and solve the 3x3 system for (t, u, v) with Cramer's rule, sharing the
// cross products between the determinants. The ray hits the triangle when
// u >= 0, v >= 0 and u + v <= 1.
bool rayHitsTriangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c, float& t, float& u, float& v) {
    const Vec3 edge1 = b - a, edge2 = c - a;
    const Vec3 p = cross(ray.direction, edge2);
    const float det = dot(edge1, p);
    if (std::abs(det) < 1e-20f) return false;  // the ray is parallel to the triangle
    const float invDet = 1.0f / det;
    const Vec3 s = ray.origin - a;
    u = dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross(s, edge1);
    v = dot(ray.direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(edge2, q) * invDet;
    return t >= 0.0f;
}

Frustum Frustum::fromMatrix(const Mat4& m) {
    const Vec4 x = m.row(0), y = m.row(1), z = m.row(2), w = m.row(3);
    Frustum f;
    f.planes[0] = w + x;  // left
    f.planes[1] = w - x;  // right
    f.planes[2] = w + y;  // bottom
    f.planes[3] = w - y;  // top
    f.planes[4] = w + z;  // near
    f.planes[5] = w - z;  // far
    return f;
}

Frustum::Side Frustum::classify(const Bounds& box) const {
    bool inside = true;
    for (const Vec4& plane : planes) {
        // The corner furthest along the normal, and the one furthest against it.
        const Vec3 far{plane.x >= 0.0f ? box.max.x : box.min.x, plane.y >= 0.0f ? box.max.y : box.min.y,
                       plane.z >= 0.0f ? box.max.z : box.min.z};
        const Vec3 near{plane.x >= 0.0f ? box.min.x : box.max.x, plane.y >= 0.0f ? box.min.y : box.max.y,
                        plane.z >= 0.0f ? box.min.z : box.max.z};
        if (dot(plane.xyz(), far) + plane.w < 0.0f) return Side::Outside;
        if (dot(plane.xyz(), near) + plane.w < 0.0f) inside = false;
    }
    return inside ? Side::Inside : Side::Intersecting;
}

// Building the tree.
//
// Start with every triangle in one node and split recursively. Where to
// split is what separates a good tree from a bad one: the aim is children
// whose boxes are small (so rays rarely enter them) without putting
// everything on one side. The *surface area heuristic* scores a split by
//
//     area(left box) * count(left) + area(right box) * count(right)
//
// - the chance a random ray hits each child is proportional to its box's
// surface area, and the work inside is proportional to its triangle count.
// Trying every possible split would be slow, so each node drops its
// triangles into a dozen bins along each axis by centroid and only the
// eleven boundaries between bins are tried.
void Bvh::build(const Mesh& mesh) {
    const std::uint32_t triangleCount = static_cast<std::uint32_t>(mesh.indices.size() / 3);
    nodes_.clear();
    order_.resize(triangleCount);
    if (triangleCount == 0) return;

    // Each triangle's box and centroid, computed once.
    std::vector<Bounds> boxes(triangleCount);
    std::vector<Vec3> centroids(triangleCount);
    for (std::uint32_t i = 0; i < triangleCount; ++i) {
        order_[i] = i;
        Bounds box = emptyBounds();
        for (int k = 0; k < 3; ++k)
            include(box, mesh.vertices[mesh.indices[i * 3 + static_cast<std::uint32_t>(k)]].position);
        boxes[i] = box;
        centroids[i] = box.center();
    }

    nodes_.reserve(static_cast<std::size_t>(triangleCount) * 2);
    nodes_.emplace_back();
    nodes_[0].first = 0;
    nodes_[0].count = triangleCount;

    struct Pending {
        std::uint32_t node;
        int depth;
    };
    std::vector<Pending> pending{{0, 0}};
    while (!pending.empty()) {
        const std::uint32_t nodeIndex = pending.back().node;
        const int depth = pending.back().depth;
        pending.pop_back();
        const std::uint32_t first = nodes_[nodeIndex].first, count = nodes_[nodeIndex].count;

        Bounds box = emptyBounds(), centroidBox = emptyBounds();
        for (std::uint32_t i = first; i < first + count; ++i) {
            const std::uint32_t tri = order_[i];
            include(box, boxes[tri].min);
            include(box, boxes[tri].max);
            include(centroidBox, centroids[tri]);
        }
        nodes_[nodeIndex].bounds = box;
        if (count <= kLeafSize) continue;

        // Find the cheapest of the binned splits over the three axes.
        float bestCost = 3.4e38f;
        int bestAxis = -1, bestSplit = 0;
        for (int axis = 0; axis < 3 && depth < kMaxGuidedDepth; ++axis) {
            const float lo = centroidBox.min[axis], extent = centroidBox.max[axis] - lo;
            if (extent <= 0.0f) continue;  // all centroids coincide on this axis
            Bounds binBox[kBins];
            std::uint32_t binCount[kBins] = {};
            for (Bounds& b : binBox) b = emptyBounds();
            const float scale = static_cast<float>(kBins) / extent;
            for (std::uint32_t i = first; i < first + count; ++i) {
                const std::uint32_t tri = order_[i];
                // (Written so that a centroid that is not a number lands in
                // the first bin rather than being converted to an integer.)
                const float position = (centroids[tri][axis] - lo) * scale;
                const int bin = position >= 1.0f ? std::min(static_cast<int>(std::min(position, 1e6f)), kBins - 1) : 0;
                ++binCount[bin];
                include(binBox[bin], boxes[tri].min);
                include(binBox[bin], boxes[tri].max);
            }
            // Sweep from the right to get each suffix's area, then from the
            // left to score every boundary.
            float rightArea[kBins];
            Bounds accumulated = emptyBounds();
            for (int bin = kBins - 1; bin > 0; --bin) {
                include(accumulated, binBox[bin].min);
                include(accumulated, binBox[bin].max);
                rightArea[bin] = surfaceArea(accumulated);
            }
            accumulated = emptyBounds();
            std::uint32_t leftCount = 0;
            for (int split = 1; split < kBins; ++split) {
                include(accumulated, binBox[split - 1].min);
                include(accumulated, binBox[split - 1].max);
                leftCount += binCount[split - 1];
                if (leftCount == 0 || leftCount == count) continue;
                const float cost = surfaceArea(accumulated) * static_cast<float>(leftCount) +
                                   rightArea[split] * static_cast<float>(count - leftCount);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = split;
                }
            }
        }

        // Partition the node's triangles around the chosen boundary. If no
        // split separates anything (every centroid identical), fall back to
        // halving the list so the tree still has bounded depth.
        std::uint32_t middle = first + count / 2;
        if (bestAxis >= 0) {
            const float lo = centroidBox.min[bestAxis];
            const float scale = static_cast<float>(kBins) / (centroidBox.max[bestAxis] - lo);
            const auto mid =
                std::partition(order_.begin() + first, order_.begin() + first + count, [&](std::uint32_t tri) {
                    return std::min(static_cast<int>((centroids[tri][bestAxis] - lo) * scale), kBins - 1) < bestSplit;
                });
            middle = static_cast<std::uint32_t>(mid - order_.begin());
        }

        const std::uint32_t leftIndex = static_cast<std::uint32_t>(nodes_.size());
        nodes_.emplace_back();
        nodes_.emplace_back();
        nodes_[leftIndex].first = first;
        nodes_[leftIndex].count = middle - first;
        nodes_[leftIndex + 1].first = middle;
        nodes_[leftIndex + 1].count = first + count - middle;
        nodes_[nodeIndex].first = leftIndex;
        nodes_[nodeIndex].count = 0;
        pending.push_back({leftIndex, depth + 1});
        pending.push_back({leftIndex + 1, depth + 1});
    }
}

bool Bvh::intersect(const Mesh& mesh, const Ray& ray, float tMax, bool cullBackfaces, BvhHit& hit) const {
    if (nodes_.empty()) return false;
    const Vec3 inverse{1.0f / ray.direction.x, 1.0f / ray.direction.y, 1.0f / ray.direction.z};
    bool found = false;
    float nearest = tMax;

    // Depth-first, with an explicit stack. Once a hit is found, `nearest`
    // shrinks and boxes beyond it are skipped: most of the tree is never
    // visited.
    std::uint32_t stack[kStackSize];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = nodes_[stack[--top]];
        if (!rayHitsBox(ray, inverse, node.bounds, nearest)) continue;
        if (node.count == 0) {
            stack[top++] = node.first;
            stack[top++] = node.first + 1;
            continue;
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const std::uint32_t tri = order_[i];
            const Vec3 a = mesh.vertices[mesh.indices[tri * 3]].position;
            const Vec3 b = mesh.vertices[mesh.indices[tri * 3 + 1]].position;
            const Vec3 c = mesh.vertices[mesh.indices[tri * 3 + 2]].position;
            float t, u, v;
            if (!rayHitsTriangle(ray, a, b, c, t, u, v) || t >= nearest) continue;
            // Front faces wind counter-clockwise seen from outside, so their
            // normal points back along the ray.
            if (cullBackfaces && dot(cross(b - a, c - a), ray.direction) >= 0.0f) continue;
            nearest = t;
            hit = {t, tri, u, v};
            found = true;
        }
    }
    return found;
}

void Bvh::markVisible(const Frustum& frustum, std::vector<std::uint64_t>& visible) const {
    if (nodes_.empty()) return;
    auto mark = [&](std::uint32_t first, std::uint32_t count) {
        for (std::uint32_t i = first; i < first + count; ++i)
            visible[order_[i] >> 6] |= std::uint64_t{1} << (order_[i] & 63);
    };

    std::uint32_t stack[kStackSize];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const std::uint32_t index = stack[--top];
        const Node& node = nodes_[index];
        const Frustum::Side side = frustum.classify(node.bounds);
        if (side == Frustum::Side::Outside) continue;
        if (node.count > 0) {
            mark(node.first, node.count);
        } else if (side == Frustum::Side::Inside) {
            // Everything below is visible: no more plane tests needed, just
            // walk down to the leaves.
            std::uint32_t inner[kStackSize];
            int innerTop = 0;
            inner[innerTop++] = index;
            while (innerTop > 0) {
                const Node& n = nodes_[inner[--innerTop]];
                if (n.count > 0) {
                    mark(n.first, n.count);
                } else {
                    inner[innerTop++] = n.first;
                    inner[innerTop++] = n.first + 1;
                }
            }
        } else {
            stack[top++] = node.first;
            stack[top++] = node.first + 1;
        }
    }
}

}  // namespace termesh
