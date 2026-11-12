// Pose: where every node of a rig is at one instant.
//
// Animation is a pipeline of its own, upstream of the renderer:
//
//   Animation + time ──► sampleAnimation ──► Pose (local transforms, weights)
//                                              │   blendPoses mixes two of them
//                                              ▼
//                         computeWorldMatrices ──► one matrix per node
//                                              ▼
//                         applyPose (skinning.hpp) ──► world-space vertices
#pragma once

#include <vector>

#include "scene/rig.hpp"

namespace termesh {

struct Pose {
    std::vector<Transform> local;             // one per node, relative to its parent
    std::vector<std::vector<float>> weights;  // morph target weights, per node
};

// The pose stored in the file: every node at its rest transform.
Pose restPose(const Rig& rig);

// Evaluate one channel at `time` into `out` (channel.components floats).
// Times before the first keyframe or after the last clamp to that keyframe.
// Returns false if the channel has no usable keyframes.
bool sampleChannel(const AnimationChannel& channel, float time, float* out);

// Overwrite the properties of `pose` that `animation` drives with their
// values at `time`. Everything the animation does not touch is left alone,
// so start from restPose().
void sampleAnimation(const Animation& animation, float time, Pose& pose);

// Mix two poses of the same rig: t = 0 gives a, t = 1 gives b. Translations,
// scales and morph weights are interpolated linearly, rotations along the
// shortest arc.
void blendPoses(const Pose& a, const Pose& b, float t, Pose& out);

// Walk the hierarchy and turn local transforms into node-to-world matrices.
void computeWorldMatrices(const Rig& rig, const Pose& pose, std::vector<Mat4>& world);

}  // namespace termesh
