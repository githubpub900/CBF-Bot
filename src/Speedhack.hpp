#pragma once

#include <Geode/Geode.hpp>
#include <atomic>

namespace cbf {

// ============================================================================
//  Speedhack
// ----------------------------------------------------------------------------
//  Frame-rate-independent speedhack.
//
//  Problem with classic speedhacks (e.g. CCScheduler::setTimeScale):
//    They scale the dt passed to every scheduler target. At high multipliers
//    (10x, 50x, 100x) and low FPS, each frame tries to simulate many ticks
//    of physics in one go. The result is stuttering and dropped inputs.
//
//  Our approach:
//    We scale the dt that PlayLayer::update receives, but the actual
//    physics stepping is handled by CBF (or our practice-fix clamp). With
//    CBF active, scaled dt is broken into multiple sub-tick physics steps
//    automatically - so 10x speed at 30 FPS still produces 10 ticks per
//    frame, smoothly, because CBF is doing the sub-stepping.
//
//    Without CBF, we fall back to running multiple PlayLayer::update calls
//    per render frame ourselves (multi-stepping), which achieves the same
//    smoothness for speeds up to roughly (frame_dt / tick_dt)x. Beyond
//    that the simulation simply can't keep up - same physical limit as any
//    speedhack - but the bot itself never drops inputs because we record
//    and play back at tick granularity, not frame granularity.
//
//  This is "frame-rate-independent" in the sense that matters for a macro
//  bot: the macro's tick stream is identical at any FPS. The speedhack
//  just controls how fast those ticks elapse in wall-clock time.
// ============================================================================
class Speedhack {
public:
    static Speedhack& get();

    // Get / set the current speed multiplier (1.0 = normal).
    float getSpeed() const { return m_speed.load(std::memory_order_acquire); }
    void  setSpeed(float s);
    void  setDefaultFromSettings();

    // Given the raw frame dt and the configured speed, return the dt that
    // should be passed to PlayLayer::update this frame, AND the number of
    // physics sub-steps that should be executed (for the no-CBF multi-step
    // path).
    struct StepPlan {
        float    scaledDt;   // dt to pass to PlayLayer::update per call
        uint32_t steps;      // number of calls (1 if no multi-step needed)
    };
    StepPlan plan(float rawDt) const;

private:
    Speedhack();
    std::atomic<float> m_speed{1.0f};
};

} // namespace cbf
