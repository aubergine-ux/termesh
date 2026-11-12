#include "accel/simplify.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace termesh {

namespace {

// Quadric error metric (Garland and Heckbert, 1997).
//
// Which edge "changes the shape least"? Every vertex starts out lying on
// the planes of the triangles around it. After some collapses a vertex
// stands in for several original ones, and a good measure of the damage is
// the sum of its squared distances to all the planes it has inherited.
//
// The squared distance from a point p to the plane n.p + d = 0 is
// (n.p + d)^2, a quadratic in p, and a sum of quadratics is a quadratic:
// ten numbers per vertex, however many planes went into it. Merging two
// vertices adds their quadrics; evaluating the result at the surviving
// position gives the cost of the collapse.
struct Quadric {
    // The symmetric matrix [A b; b' c] of p' A p + 2 b.p + c, and the total
    // weight of the planes summed in (their triangles' areas).
    double xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
    double bx = 0, by = 0, bz = 0, c = 0;
    double weight = 0;

    void addPlane(Vec3 n, double d, double w) {
        xx += w * n.x * n.x, xy += w * n.x * n.y, xz += w * n.x * n.z;
        yy += w * n.y * n.y, yz += w * n.y * n.z, zz += w * n.z * n.z;
        bx += w * n.x * d, by += w * n.y * d, bz += w * n.z * d;
        c += w * d * d;
        weight += w;
    }
    Quadric& operator+=(const Quadric& o) {
        xx += o.xx, xy += o.xy, xz += o.xz, yy += o.yy, yz += o.yz, zz += o.zz;
        bx += o.bx, by += o.by, bz += o.bz, c += o.c;
        weight += o.weight;
        return *this;
    }
    double evaluate(Vec3 p) const {
        const double x = p.x, y = p.y, z = p.z;
        return xx * x * x + yy * y * y + zz * z * z + 2.0 * (xy * x * y + xz * x * z + yz * y * z) +
               2.0 * (bx * x + by * y + bz * z) + c;
    }
};

// Everything the simplifier knows about a mesh while it works on it.
//
// Vertices that share a position are grouped into a *site*. A mesh usually
// has several vertices at one position wherever the texture mapping has a
// seam (same place, different UVs); they must move together or the surface
// would tear, so collapses are decided site by site.
struct Simplifier {
    const Mesh& mesh;
    std::vector<std::uint32_t> siteOf;  // vertex -> site
    std::vector<Vec3> sitePosition;
    std::vector<Quadric> quadric;                      // per site
    std::vector<std::vector<std::uint32_t>> siteTris;  // per site: triangles touching it (may list dead ones)
    std::vector<std::uint32_t> version;                // per site: bumped whenever its surroundings change
    std::vector<char> siteAlive;
    std::vector<std::array<std::uint32_t, 3>> tris;  // corners as vertex indices
    std::vector<char> triAlive;
    std::size_t aliveTriangles = 0;
    double maxError = 0.0;

    struct Candidate {
        double cost;
        std::uint32_t from, to;
        std::uint32_t fromVersion, toVersion;
        // Cheapest first; ties broken by index so the result is the same on
        // every run and every platform.
        bool operator<(const Candidate& o) const {
            if (cost != o.cost) return cost > o.cost;
            if (from != o.from) return from > o.from;
            return to > o.to;
        }
    };
    std::priority_queue<Candidate> queue;

    explicit Simplifier(const Mesh& m) : mesh(m) {
        const std::uint32_t vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        const std::uint32_t triangleCount = static_cast<std::uint32_t>(mesh.indices.size() / 3);

        // Sites: sort the vertices by position and group the equal ones.
        std::vector<std::uint32_t> sorted(vertexCount);
        std::iota(sorted.begin(), sorted.end(), 0u);
        auto position = [&](std::uint32_t v) { return mesh.vertices[v].position; };
        std::sort(sorted.begin(), sorted.end(), [&](std::uint32_t a, std::uint32_t b) {
            const Vec3 p = position(a), q = position(b);
            if (p.x != q.x) return p.x < q.x;
            if (p.y != q.y) return p.y < q.y;
            if (p.z != q.z) return p.z < q.z;
            return a < b;
        });
        siteOf.resize(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i) {
            if (i == 0 || !(position(sorted[i]) == position(sorted[i - 1])))
                sitePosition.push_back(position(sorted[i]));
            siteOf[sorted[i]] = static_cast<std::uint32_t>(sitePosition.size() - 1);
        }
        const std::size_t siteCount = sitePosition.size();
        quadric.resize(siteCount);
        siteTris.resize(siteCount);
        version.assign(siteCount, 0);
        siteAlive.assign(siteCount, 1);

        // Triangles, their plane quadrics, and a count of how many triangles
        // use each edge.
        tris.resize(triangleCount);
        triAlive.assign(triangleCount, 0);
        std::unordered_map<std::uint64_t, int> edgeUse;
        auto edgeKey = [](std::uint32_t a, std::uint32_t b) {
            return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
        };
        for (std::uint32_t t = 0; t < triangleCount; ++t) {
            tris[t] = {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]};
            const std::uint32_t s[3] = {siteOf[tris[t][0]], siteOf[tris[t][1]], siteOf[tris[t][2]]};
            if (s[0] == s[1] || s[1] == s[2] || s[0] == s[2]) continue;  // degenerate from the start
            const Vec3 p0 = sitePosition[s[0]], p1 = sitePosition[s[1]], p2 = sitePosition[s[2]];
            const Vec3 cr = cross(p1 - p0, p2 - p0);
            const float twiceArea = length(cr);
            if (twiceArea <= 0.0f) continue;
            triAlive[t] = 1;
            ++aliveTriangles;
            const Vec3 n = cr / twiceArea;
            for (int k = 0; k < 3; ++k) {
                // Weighting by area keeps a cloud of tiny triangles from
                // outvoting one large one.
                quadric[s[k]].addPlane(n, -static_cast<double>(dot(n, p0)), twiceArea);
                siteTris[s[k]].push_back(t);
                ++edgeUse[edgeKey(s[k], s[(k + 1) % 3])];
            }
        }

        // An edge used by only one triangle is on the border of the surface
        // (the rim of a hole, the edge of a sheet). Nothing stops a collapse
        // from eating into a border - the plane quadrics see no error in
        // sliding along the surface - so each border edge adds a plane of
        // its own, standing on the edge at right angles to its triangle:
        // moving a vertex off the border now costs something.
        for (std::uint32_t t = 0; t < triangleCount; ++t) {
            if (!triAlive[t]) continue;
            const std::uint32_t s[3] = {siteOf[tris[t][0]], siteOf[tris[t][1]], siteOf[tris[t][2]]};
            const Vec3 faceNormal =
                normalize(cross(sitePosition[s[1]] - sitePosition[s[0]], sitePosition[s[2]] - sitePosition[s[0]]));
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t a = s[k], b = s[(k + 1) % 3];
                if (edgeUse[edgeKey(a, b)] != 1) continue;
                const Vec3 edge = sitePosition[b] - sitePosition[a];
                const Vec3 n = normalize(cross(edge, faceNormal));
                const double w = 10.0 * static_cast<double>(lengthSquared(edge));
                const double d = -static_cast<double>(dot(n, sitePosition[a]));
                quadric[a].addPlane(n, d, w);
                quadric[b].addPlane(n, d, w);
            }
        }

        for (std::uint32_t site = 0; site < siteCount; ++site) pushCandidates(site, false);
    }

    // The sites sharing a live triangle with `site`, dropping dead triangles
    // from its list on the way.
    void neighbours(std::uint32_t site, std::vector<std::uint32_t>& out) {
        out.clear();
        std::vector<std::uint32_t>& list = siteTris[site];
        std::size_t kept = 0;
        for (const std::uint32_t t : list) {
            if (!triAlive[t]) continue;
            list[kept++] = t;
            for (const std::uint32_t v : tris[t]) {
                const std::uint32_t s = siteOf[v];
                if (s != site && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
            }
        }
        list.resize(kept);
    }

    // For collapsing `from` onto `to`: which vertex of `to` each vertex of
    // `from` becomes. A vertex follows the edge it already shares with one
    // of `to`'s vertices - the one on its own side of any seam. The
    // collapse is refused (returns false) if some vertex has no such edge,
    // or if two vertices of `from` would land on the same vertex of `to`:
    // that would stitch the two sides of a seam together and smear the
    // texture across it.
    bool vertexMapping(std::uint32_t from, std::uint32_t to,
                       std::vector<std::pair<std::uint32_t, std::uint32_t>>& mapping) const {
        mapping.clear();
        for (const std::uint32_t t : siteTris[from]) {
            if (!triAlive[t]) continue;
            std::uint32_t source = 0, target = 0;
            bool hasTarget = false;
            for (const std::uint32_t v : tris[t]) {
                if (siteOf[v] == from) source = v;
                else if (siteOf[v] == to) target = v, hasTarget = true;
            }
            if (!hasTarget) continue;
            const auto known =
                std::find_if(mapping.begin(), mapping.end(), [&](const auto& m) { return m.first == source; });
            if (known == mapping.end()) mapping.emplace_back(source, target);
            else if (known->second != target) return false;  // attached to two different sides
        }
        for (const std::uint32_t t : siteTris[from]) {
            if (!triAlive[t]) continue;
            for (const std::uint32_t v : tris[t]) {
                if (siteOf[v] != from) continue;
                if (std::none_of(mapping.begin(), mapping.end(), [&](const auto& m) { return m.first == v; }))
                    return false;
            }
        }
        for (std::size_t i = 0; i < mapping.size(); ++i)
            for (std::size_t j = i + 1; j < mapping.size(); ++j)
                if (mapping[i].second == mapping[j].second) return false;
        return !mapping.empty();
    }

    // Cost of moving `from` onto `to`, or a negative number if not allowed.
    double collapseCost(std::uint32_t from, std::uint32_t to) const {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> mapping;
        if (!vertexMapping(from, to, mapping)) return -1.0;

        // No triangle may flip over (or collapse to a sliver): compare each
        // surviving triangle's normal before and after the move.
        const Vec3 target = sitePosition[to];
        for (const std::uint32_t t : siteTris[from]) {
            if (!triAlive[t]) continue;
            Vec3 before[3], after[3];
            bool touchesTarget = false;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t s = siteOf[tris[t][static_cast<std::size_t>(k)]];
                before[k] = sitePosition[s];
                after[k] = s == from ? target : before[k];
                touchesTarget = touchesTarget || s == to;
            }
            if (touchesTarget) continue;  // this one disappears
            const Vec3 n0 = cross(before[1] - before[0], before[2] - before[0]);
            const Vec3 n1 = cross(after[1] - after[0], after[2] - after[0]);
            if (dot(n0, n1) <= 0.2f * length(n0) * length(n1)) return -1.0;
        }

        Quadric merged = quadric[from];
        merged += quadric[to];
        return std::max(merged.evaluate(target), 0.0);
    }

    // Queue the collapses of `site` onto each of its neighbours and, with
    // `bothWays`, of each neighbour onto it.
    void pushCandidates(std::uint32_t site, bool bothWays) {
        std::vector<std::uint32_t> around;
        neighbours(site, around);
        auto push = [&](std::uint32_t from, std::uint32_t to) {
            const double cost = collapseCost(from, to);
            if (cost >= 0.0) queue.push({cost, from, to, version[from], version[to]});
        };
        for (const std::uint32_t other : around) {
            push(site, other);
            if (bothWays) push(other, site);
        }
    }

    // Perform the cheapest collapse still valid. Returns false when there
    // are none left.
    bool collapseOne() {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> mapping;
        std::vector<std::uint32_t> around;
        while (!queue.empty()) {
            const Candidate c = queue.top();
            queue.pop();
            // Entries are never removed from the queue when they go out of
            // date; they are recognised here by their version numbers.
            if (!siteAlive[c.from] || !siteAlive[c.to]) continue;
            if (version[c.from] != c.fromVersion || version[c.to] != c.toVersion) continue;
            if (!vertexMapping(c.from, c.to, mapping)) continue;

            for (const std::uint32_t t : siteTris[c.from]) {
                if (!triAlive[t]) continue;
                bool touchesTarget = false;
                for (const std::uint32_t v : tris[t]) touchesTarget = touchesTarget || siteOf[v] == c.to;
                if (touchesTarget) {
                    triAlive[t] = 0;  // squashed flat by the collapse
                    --aliveTriangles;
                    continue;
                }
                for (std::uint32_t& v : tris[t]) {
                    if (siteOf[v] != c.from) continue;
                    v = std::find_if(mapping.begin(), mapping.end(), [&](const auto& m) {
                            return m.first == v;
                        })->second;
                }
                siteTris[c.to].push_back(t);
            }
            siteTris[c.from].clear();
            siteAlive[c.from] = 0;

            quadric[c.to] += quadric[c.from];
            if (quadric[c.to].weight > 0.0) maxError = std::max(maxError, std::sqrt(c.cost / quadric[c.to].weight));

            // Everything around the survivor has changed: retire the old
            // candidates and make new ones.
            neighbours(c.to, around);
            ++version[c.to];
            for (const std::uint32_t s : around) ++version[s];
            pushCandidates(c.to, true);
            // The neighbours' collapses onto *their* other neighbours have
            // changed too: a triangle they shared with the survivor may be
            // gone, or have a new corner.
            for (const std::uint32_t s : around) pushCandidates(s, false);
            return true;
        }
        return false;
    }

    MeshLod snapshot() const {
        MeshLod lod;
        lod.indices.reserve(aliveTriangles * 3);
        for (std::size_t t = 0; t < tris.size(); ++t)
            if (triAlive[t]) lod.indices.insert(lod.indices.end(), tris[t].begin(), tris[t].end());
        lod.vertices = lod.indices;
        std::sort(lod.vertices.begin(), lod.vertices.end());
        lod.vertices.erase(std::unique(lod.vertices.begin(), lod.vertices.end()), lod.vertices.end());
        lod.error = static_cast<float>(maxError);
        return lod;
    }
};

}  // namespace

std::vector<MeshLod> buildLods(const Mesh& mesh, const SimplifyOptions& options) {
    std::vector<MeshLod> lods;
    const std::size_t original = mesh.indices.size() / 3;
    const float ratio = std::clamp(options.ratio, 0.05f, 0.95f);
    if (original < options.minTriangles || options.maxLevels <= 0) return lods;

    Simplifier simplifier(mesh);
    std::size_t previous = simplifier.aliveTriangles;
    for (int level = 0; level < options.maxLevels; ++level) {
        const std::size_t target = static_cast<std::size_t>(static_cast<float>(previous) * ratio);
        if (target < options.minTriangles) break;
        bool exhausted = false;
        while (simplifier.aliveTriangles > target && !exhausted) exhausted = !simplifier.collapseOne();
        // A level is only worth having if it is clearly smaller than the last.
        if (simplifier.aliveTriangles > previous * 9 / 10) break;
        lods.push_back(simplifier.snapshot());
        previous = simplifier.aliveTriangles;
        if (exhausted) break;
    }
    return lods;
}

}  // namespace termesh
