#include "anim/skinning.hpp"

#include <algorithm>

namespace termesh {

namespace {

constexpr int kVerticesPerChunk = 2048;

// Weighted sum of matrices. Blending matrices is what makes this *linear
// blend* skinning: cheap, and good enough that it is what nearly every game
// uses, at the cost of some volume loss where a joint twists a long way.
Mat4 blendMatrices(const Mat4* matrices, const std::array<std::uint16_t, 4>& joints, Vec4 weights) {
    Mat4 m;
    for (int c = 0; c < 4; ++c)
        m.c[c] = matrices[joints[0]].c[c] * weights.x + matrices[joints[1]].c[c] * weights.y +
                 matrices[joints[2]].c[c] * weights.z + matrices[joints[3]].c[c] * weights.w;
    return m;
}

}  // namespace

void applyPose(Model& model, const Pose& pose, ThreadPool* pool) {
    if (model.rig.empty() || pose.local.size() != model.rig.nodes.size()) return;

    std::vector<Mat4> world;
    computeWorldMatrices(model.rig, pose, world);
    std::vector<Mat4> jointMatrices;

    for (Mesh& mesh : model.meshes) {
        const MeshRig& rig = mesh.rig;
        const std::size_t count = mesh.vertices.size();
        if (rig.empty() || rig.positions.size() != count || rig.node < 0 ||
            static_cast<std::size_t>(rig.node) >= world.size())
            continue;

        // A vertex bound to a joint is first taken into that joint's space
        // as it was at bind time (the inverse bind matrix), then out again
        // through wherever the joint is now.
        const bool skinned = rig.skin >= 0 && static_cast<std::size_t>(rig.skin) < model.rig.skins.size() &&
                             rig.joints.size() == count && rig.weights.size() == count;
        if (skinned) {
            const Skin& skin = model.rig.skins[static_cast<std::size_t>(rig.skin)];
            jointMatrices.resize(skin.joints.size());
            for (std::size_t j = 0; j < skin.joints.size(); ++j)
                jointMatrices[j] = world[static_cast<std::size_t>(skin.joints[j])] * skin.inverseBind[j];
        }
        const Mat4 nodeWorld = world[static_cast<std::size_t>(rig.node)];
        const Mat4 nodeNormal = normalMatrix(nodeWorld);

        const std::vector<float>& weights = pose.weights[static_cast<std::size_t>(rig.node)];
        const std::size_t targetCount = std::min(rig.targets.size(), weights.size());
        const bool hasNormals = rig.normals.size() == count;
        const bool hasTangents = rig.tangents.size() == count;

        auto processChunk = [&](int chunk) {
            const std::size_t begin = static_cast<std::size_t>(chunk) * kVerticesPerChunk;
            const std::size_t end = std::min(begin + kVerticesPerChunk, count);
            for (std::size_t i = begin; i < end; ++i) {
                Vec3 p = rig.positions[i];
                Vec3 n = hasNormals ? rig.normals[i] : Vec3{0.0f, 0.0f, 1.0f};
                Vec3 t = hasTangents ? rig.tangents[i] : Vec3{1.0f, 0.0f, 0.0f};

                for (std::size_t k = 0; k < targetCount; ++k) {
                    const float w = weights[k];
                    if (w == 0.0f) continue;
                    const MorphTarget& target = rig.targets[k];
                    if (target.positions.size() == count) p += target.positions[i] * w;
                    if (target.normals.size() == count) n += target.normals[i] * w;
                    if (target.tangents.size() == count) t += target.tangents[i] * w;
                }

                Vertex& out = mesh.vertices[i];
                if (skinned) {
                    const Mat4 m = blendMatrices(jointMatrices.data(), rig.joints[i], rig.weights[i]);
                    out.position = transformPoint(m, p);
                    // Strictly, normals want the inverse transpose. Bones
                    // are almost always rotations with uniform scale, for
                    // which the matrix itself gives the same direction.
                    out.normal = normalize(transformDirection(m, n), out.normal);
                    out.tangent = Vec4(normalize(transformDirection(m, t), out.tangent.xyz()), out.tangent.w);
                } else {
                    out.position = transformPoint(nodeWorld, p);
                    out.normal = normalize(transformDirection(nodeNormal, n), out.normal);
                    out.tangent = Vec4(normalize(transformDirection(nodeWorld, t), out.tangent.xyz()), out.tangent.w);
                }
            }
        };

        const int chunks = static_cast<int>((count + kVerticesPerChunk - 1) / kVerticesPerChunk);
        if (pool && chunks > 1) pool->parallelFor(chunks, processChunk);
        else
            for (int chunk = 0; chunk < chunks; ++chunk) processChunk(chunk);
    }
}

Bounds computeMotionBounds(Model& model, ThreadPool* pool) {
    constexpr int kSamples = 8;
    const Pose rest = restPose(model.rig);
    applyPose(model, rest, pool);
    Bounds bounds = model.bounds();
    for (const Animation& animation : model.rig.animations) {
        for (int i = 0; i < kSamples; ++i) {
            Pose pose = rest;
            sampleAnimation(animation, animation.duration * static_cast<float>(i) / kSamples, pose);
            applyPose(model, pose, pool);
            const Bounds b = model.bounds();
            bounds.min = min(bounds.min, b.min);
            bounds.max = max(bounds.max, b.max);
        }
    }
    applyPose(model, rest, pool);
    return bounds;
}

}  // namespace termesh
