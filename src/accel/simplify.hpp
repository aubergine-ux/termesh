// Mesh simplification: fewer triangles, (nearly) the same shape.
//
// A 100,000-triangle model drawn into a 160 x 100 terminal has several
// triangles per pixel; most of the work of setting them up is wasted.
// Level of detail keeps a few progressively coarser versions of each mesh
// and draws the coarsest one whose error would be smaller than a pixel.
//
// The coarser versions are made by *edge collapse*: pick an edge, slide one
// of its ends onto the other, and the two triangles that shared the edge
// disappear. Repeat, always collapsing the edge that changes the shape
// least.
//
//        +-----+-----+              +-----+-----+
//       / .   / .   / .            / .    |    / .
//      /   . /   . /   .    ->    /   .   |   /   .       a is moved onto b:
//     +-----a-----b-----+        +-----.--b--/-----+       two triangles fewer
//      .   / .   / .   /          .     . | /     /
//       . /   . /   . /            .     .|/     /
//        +-----+-----+              +-----+-----+
//
// Because a vertex is always moved onto an *existing* vertex, a simplified
// mesh uses a subset of the original's vertices. So a level of detail is
// nothing more than a shorter index list into the same vertex array - which
// means skinning, morph targets and everything else that works on vertices
// applies to every level for free.
#pragma once

#include <cstdint>
#include <vector>

#include "scene/model.hpp"

namespace termesh {

struct MeshLod {
    // Three per triangle, into the original mesh's vertices. Triangles keep
    // their relative order from the original.
    std::vector<std::uint32_t> indices;
    // The vertices those indices use, ascending: lets the renderer
    // transform only what this level needs.
    std::vector<std::uint32_t> vertices;
    // How far, at most, the surface has moved from the original: the largest
    // root-mean-square distance, in the mesh's units, between a collapsed
    // vertex's new position and the planes of the faces it stood for.
    float error = 0.0f;
};

struct SimplifyOptions {
    // Each level aims for this fraction of the previous one's triangles.
    float ratio = 0.5f;
    int maxLevels = 6;
    // Stop when a level would have fewer triangles than this.
    std::size_t minTriangles = 64;
};

// Build a chain of levels of detail for a mesh, finest first. The original
// is not included; an empty result means the mesh is too small, or too
// constrained, to simplify.
std::vector<MeshLod> buildLods(const Mesh& mesh, const SimplifyOptions& options = {});

}  // namespace termesh
