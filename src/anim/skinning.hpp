// Skinning: turn a pose into the world-space vertices the renderer draws.
#pragma once

#include "anim/pose.hpp"
#include "core/thread_pool.hpp"
#include "scene/model.hpp"

namespace termesh {

// Recompute Mesh::vertices (positions, normals, tangents) of every rigged
// mesh in the model for the given pose:
//
//   1. morph targets   rest shape + sum of weight_k * target_k
//   2. skinning        each vertex follows a weighted blend of up to four
//                      bones (linear blend skinning)
//      or, for rigid meshes, the world matrix of the node they hang from.
//
// Meshes without rig data are left untouched. With a thread pool the
// vertices are processed in parallel; pass nullptr to stay on this thread.
void applyPose(Model& model, const Pose& pose, ThreadPool* pool = nullptr);

// The box enclosing the model over the whole of every animation, found by
// posing it at a handful of evenly spaced times. Leaves the model in its
// rest pose.
Bounds computeMotionBounds(Model& model, ThreadPool* pool = nullptr);

}  // namespace termesh
