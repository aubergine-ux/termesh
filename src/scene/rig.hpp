// Rig: everything a model needs in order to move.
//
// A static model is just world-space triangles. An animated one also keeps
// the structure those triangles came from:
//
//   nodes       a hierarchy of transforms (translation, rotation, scale)
//   skins       lists of nodes acting as bones, for skeletal animation
//   animations  keyframe tracks that drive node transforms and morph weights
//
// and each mesh keeps its vertices as they were authored (the "rest" shape,
// in the mesh's own space) next to the per-vertex bone weights and morph
// targets. Posing the model (src/anim/) recomputes the world-space vertices
// the renderer draws from those; the renderer itself never sees any of this.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "math/mat.hpp"
#include "math/quat.hpp"
#include "math/vec.hpp"

namespace termesh {

// A transform kept as separate translation, rotation and scale, because
// those - unlike the entries of a matrix - can be interpolated sensibly.
struct Transform {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};

    // Scale first, then rotate, then translate.
    Mat4 matrix() const { return termesh::translate(translation) * toMat4(rotation) * termesh::scale(scale); }
};

struct RigNode {
    std::string name;
    int parent = -1;  // index into Rig::nodes, or -1 for a root
    Transform rest;   // the transform stored in the file
    // Default morph target weights for the mesh on this node, if it has one.
    std::vector<float> weights;
};

// A skin binds a mesh to a set of nodes ("joints", the bones). Each joint has
// an inverse bind matrix: the transform from the mesh's space into that
// joint's local space at the moment the mesh was attached to the skeleton.
struct Skin {
    std::vector<int> joints;  // indices into Rig::nodes
    std::vector<Mat4> inverseBind;
};

enum class AnimationPath { Translation, Rotation, Scale, Weights };
enum class Interpolation { Step, Linear, CubicSpline };

// One animated property of one node: keyframe times and the values at them.
struct AnimationChannel {
    int node = -1;
    AnimationPath path = AnimationPath::Translation;
    Interpolation interpolation = Interpolation::Linear;
    // Number of floats per keyframe value: 3 for translation and scale, 4
    // for rotation, the number of morph targets for weights.
    int components = 3;
    std::vector<float> times;  // seconds, ascending
    // `components` floats per keyframe - or, for cubic splines, three times
    // that: in-tangent, value, out-tangent.
    std::vector<float> values;
};

struct Animation {
    std::string name;
    float duration = 0.0f;  // seconds; the latest keyframe of any channel
    std::vector<AnimationChannel> channels;
};

struct Rig {
    std::vector<RigNode> nodes;
    // Node indices ordered so that every parent comes before its children,
    // which lets world matrices be computed in a single pass.
    std::vector<int> order;
    std::vector<Skin> skins;
    std::vector<Animation> animations;

    bool empty() const { return nodes.empty(); }
};

// A morph target ("blend shape"): per-vertex offsets that are added to the
// rest shape, scaled by an animated weight. Unused attributes stay empty.
struct MorphTarget {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> tangents;
};

// The parts of a mesh that only animation needs. Lives next to the mesh's
// world-space vertices, which are regenerated from this whenever the pose
// changes.
struct MeshRig {
    int node = -1;  // the node this mesh hangs from
    int skin = -1;  // index into Rig::skins, or -1 if the mesh is rigid

    // Rest shape, in the mesh's own space. Same layout as Mesh::vertices.
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> tangents;

    // Skinning: up to four bones per vertex. `joints` index Skin::joints.
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<Vec4> weights;

    std::vector<MorphTarget> targets;

    bool empty() const { return positions.empty(); }
};

}  // namespace termesh
