// QualityGovernor: trading picture quality for frame rate.
//
// While something is moving, each frame is on screen for a few hundredths
// of a second and smoothness matters more than detail. The governor watches
// how long frames take and, when they run over budget, steps down a ladder
// of cheaper settings; when there is time to spare it climbs back up. The
// viewer draws a full-quality frame as soon as the motion stops, so the
// reduced settings are only ever seen in passing.
#pragma once

#include <algorithm>

namespace termesh {

class QualityGovernor {
public:
    // One rung of the ladder. What is given up, least noticeable first:
    // coarser levels of detail, then supersamples, then resolution.
    struct Level {
        float lodScale;     // multiplies the allowed level-of-detail error
        int ssaaDrop;       // supersampling steps given up
        float renderScale;  // fraction of the display resolution rendered
    };
    static constexpr int kLevelCount = 8;
    static constexpr Level kLevels[kLevelCount] = {
        {1.0f, 0, 1.0f}, {2.0f, 0, 1.0f}, {4.0f, 0, 1.0f},  {4.0f, 1, 1.0f},
        {4.0f, 2, 1.0f}, {8.0f, 2, 1.0f}, {8.0f, 2, 0.75f}, {8.0f, 2, 0.5f},
    };
    static const Level& level(int index) { return kLevels[std::clamp(index, 0, kLevelCount - 1)]; }

    // Frames in a row over budget before stepping down, and frames in a row
    // with time to spare before stepping back up. Recovery is deliberately
    // much slower: a level restored too eagerly would be dropped again at
    // once, and the picture would flicker between the two.
    static constexpr int kSlowFramesToDrop = 6;
    static constexpr int kFastFramesToRecover = 45;

    int current() const { return level_; }  // 0 = full quality

    // Report how long a frame took against its budget (both in
    // milliseconds). `baseSupersampling` is the factor used at full
    // quality, so that rungs which would change nothing - a backend with no
    // supersampling has none to give up - can be skipped. Returns true if
    // the level changed.
    bool frame(double frameMs, double budgetMs, int baseSupersampling) {
        if (budgetMs <= 0.0) return false;
        if (frameMs > budgetMs) {
            fast_ = 0;
            if (++slow_ < kSlowFramesToDrop || level_ + 1 >= kLevelCount) return false;
            slow_ = 0;
            int next = level_ + 1;
            while (next + 1 < kLevelCount && sameEffect(next, level_, baseSupersampling)) ++next;
            if (sameEffect(next, level_, baseSupersampling)) return false;  // nothing left to give
            level_ = next;
            return true;
        }
        if (frameMs < budgetMs * 0.5) {
            slow_ = 0;
            if (++fast_ < kFastFramesToRecover || level_ == 0) return false;
            fast_ = 0;
            int previous = level_ - 1;
            while (previous > 0 && sameEffect(previous, level_, baseSupersampling)) --previous;
            level_ = previous;
            return true;
        }
        // Comfortably inside the budget but with nothing to spare: stay put.
        slow_ = fast_ = 0;
        return false;
    }

private:
    static bool sameEffect(int a, int b, int baseSupersampling) {
        const Level &x = kLevels[a], &y = kLevels[b];
        return x.lodScale == y.lodScale && x.renderScale == y.renderScale &&
               std::max(baseSupersampling - x.ssaaDrop, 1) == std::max(baseSupersampling - y.ssaaDrop, 1);
    }

    int level_ = 0;
    int slow_ = 0, fast_ = 0;
};

}  // namespace termesh
