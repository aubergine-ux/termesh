#include "anim/player.hpp"

#include <algorithm>
#include <cmath>

namespace termesh {

AnimationPlayer::AnimationPlayer(const Rig& rig) : rig_(&rig), rest_(restPose(rig)) {
    if (!rig.animations.empty()) clip_ = 0;
    pose_ = rest_;
}

std::string AnimationPlayer::clipName(int index) const {
    if (index < 0 || index >= clipCount()) return {};
    const std::string& name = rig_->animations[static_cast<std::size_t>(index)].name;
    return name.empty() ? "animation " + std::to_string(index + 1) : name;
}

float AnimationPlayer::duration() const {
    return clip_ >= 0 ? rig_->animations[static_cast<std::size_t>(clip_)].duration : 0.0f;
}

float AnimationPlayer::wrap(float time, float duration) const {
    if (duration <= 0.0f) return 0.0f;
    if (!loop_) return std::clamp(time, 0.0f, duration);
    time = std::fmod(time, duration);
    return time < 0.0f ? time + duration : time;
}

void AnimationPlayer::setClip(int index, float fadeSeconds) {
    if (clipCount() == 0) return;
    index = std::clamp(index, 0, clipCount() - 1);
    if (index == clip_ && !fading()) {
        seek(0.0f);
        return;
    }
    if (fadeSeconds > 0.0f && clip_ >= 0) {
        previousClip_ = clip_;
        previousTime_ = time_;
        fadeDuration_ = fadeSeconds;
        fadeElapsed_ = 0.0f;
    } else {
        fadeDuration_ = 0.0f;
    }
    clip_ = index;
    time_ = 0.0f;
    stale_ = true;
}

void AnimationPlayer::seek(float seconds) {
    time_ = wrap(seconds, duration());
    fadeDuration_ = 0.0f;  // scrubbing shows exactly the requested frame
    stale_ = true;
}

bool AnimationPlayer::advance(float dt) {
    if (clip_ < 0 || dt <= 0.0f) return stale_;
    bool changed = false;

    if (fading()) {
        // The fade runs on real time, so it completes even while paused or
        // at a very slow playback speed.
        fadeElapsed_ += dt;
        if (fadeElapsed_ >= fadeDuration_) fadeDuration_ = 0.0f;
        changed = true;
    }
    if (playing_ && speed_ != 0.0f) {
        const float before = time_;
        time_ = wrap(time_ + dt * speed_, duration());
        if (fading()) {
            const float previousDuration = rig_->animations[static_cast<std::size_t>(previousClip_)].duration;
            previousTime_ = wrap(previousTime_ + dt * speed_, previousDuration);
        }
        // A finished, non-looping clip sits on its last frame.
        changed = changed || time_ != before;
    }
    stale_ = stale_ || changed;
    return stale_;
}

const Pose& AnimationPlayer::pose() {
    if (!stale_) return pose_;
    stale_ = false;
    if (clip_ < 0) return pose_ = rest_;

    currentPose_ = rest_;
    sampleAnimation(rig_->animations[static_cast<std::size_t>(clip_)], time_, currentPose_);
    if (!fading()) return pose_ = currentPose_;

    previousPose_ = rest_;
    sampleAnimation(rig_->animations[static_cast<std::size_t>(previousClip_)], previousTime_, previousPose_);
    // Smoothstep eases the blend in and out instead of starting and stopping
    // it at full speed.
    const float u = std::clamp(fadeElapsed_ / fadeDuration_, 0.0f, 1.0f);
    blendPoses(previousPose_, currentPose_, u * u * (3.0f - 2.0f * u), pose_);
    return pose_;
}

}  // namespace termesh
