// AnimationPlayer: the timeline. Owns "which clip, at what time, how fast",
// and produces the pose for the current instant.
#pragma once

#include <string>

#include "anim/pose.hpp"

namespace termesh {

class AnimationPlayer {
public:
    // The rig must outlive the player.
    explicit AnimationPlayer(const Rig& rig);

    int clipCount() const { return static_cast<int>(rig_->animations.size()); }
    int clip() const { return clip_; }  // -1 when the rig has no animations
    std::string clipName(int index) const;

    // Switch to another clip, starting from its beginning. With fadeSeconds
    // > 0 the old clip keeps playing while the new one is blended in over
    // that time (a cross-fade), so the model does not snap between poses.
    void setClip(int index, float fadeSeconds = 0.0f);

    void play() { playing_ = true; }
    void pause() { playing_ = false; }
    bool playing() const { return playing_ && clip_ >= 0; }

    float time() const { return time_; }
    float duration() const;  // of the current clip
    // Jump to a time. Wraps around when looping, clamps otherwise.
    void seek(float seconds);

    // Playback rate: 1 is real time, 2 double speed, negative runs backwards.
    void setSpeed(float speed) { speed_ = speed; }
    float speed() const { return speed_; }

    void setLooping(bool loop) { loop_ = loop; }
    bool looping() const { return loop_; }

    // True while a cross-fade is in progress.
    bool fading() const { return fadeDuration_ > 0.0f; }

    // Move the clock forward by dt seconds of real time. Returns true if the
    // pose changed and the model needs to be re-skinned.
    bool advance(float dt);

    // The pose at the current time.
    const Pose& pose();

private:
    float wrap(float time, float duration) const;

    const Rig* rig_;
    int clip_ = -1;
    float time_ = 0.0f;
    float speed_ = 1.0f;
    bool playing_ = true;
    bool loop_ = true;

    // Cross-fade state: the clip being faded out and where it has got to.
    int previousClip_ = -1;
    float previousTime_ = 0.0f;
    float fadeElapsed_ = 0.0f, fadeDuration_ = 0.0f;

    Pose rest_, pose_, previousPose_, currentPose_;
    bool stale_ = true;  // pose_ needs re-evaluating
};

}  // namespace termesh
