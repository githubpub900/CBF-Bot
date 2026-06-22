#pragma once

#include <Geode/Geode.hpp>
#include <atomic>

namespace cbf {

// ============================================================================
//  Speedhack — frame-rate-independent, choppy-free slow-motion
// ----------------------------------------------------------------------------
//  THE PROBLEM WITH TRADITIONAL SPEEDHACKS:
//    Scaling dt (CCScheduler::setTimeScale) makes the physics tick rate
//    drop at slow speeds. At 0.01x with a 240 Hz physics baseline, physics
//    only runs at 2.4 Hz wall-clock — that's a new physics step every 417ms,
//    which looks like a slideshow.
//
//  OUR SOLUTION — DYNAMIC PHYSICS POLLING RATE:
//    Instead of scaling dt, we keep dt-per-step FIXED (1/baseRate seconds of
//    game-time, preserving physics determinism) and change how many steps
//    run per second of WALL-CLOCK time.
//
//      speed = (wall-clock steps per second) / (base physics rate)
//
//    So at 0.5x speed with a 240 Hz base: run 120 physics steps per second
//    wall-clock (each advancing 1/240 sec game-time) → 0.5 sec game-time
//    per sec wall-clock. 120 Hz is still smooth at 60 fps render.
//
//    At very slow speeds we hit a floor: we can't run fewer than ~60 steps
//    per second without choppiness. To go slower without choppiness, we
//    RAISE the base physics rate:
//
//      effective_baseRate = max(baseRate, min(60 / speed, maxPhysicsRate))
//
//    Example with maxPhysicsRate = 24000 (24 kHz):
//      1.0x  → baseRate 240 Hz,  240 steps/sec wall-clock  ✓ smooth
//      0.1x  → baseRate 600 Hz,  60  steps/sec wall-clock  ✓ smooth
//      0.01x → baseRate 6000 Hz, 60  steps/sec wall-clock  ✓ smooth
//      0.001x→ baseRate 24000 Hz, 60 steps/sec wall-clock  ✓ smooth (capped)
//
//    Each physics step still advances (1 / effective_baseRate) sec of
//    game-time, so the macro's tick stream is consistent at any speed.
//    The macro is tagged with ticks, not wall-clock time, so playback at
//    any speed fires inputs at the correct tick.
//
//  WHY THIS IS "INFINITE":
//    The user asked for a speedhack that "doesn't get choppy when slowed"
//    and is "infinite" in range. With a 24 kHz max physics rate, we can go
//    down to ~0.003x speed while maintaining 60+ physics steps per second
//    wall-clock. Below that the cap kicks in and choppiness gradually
//    returns, but 0.003x is already 333x slower than real-time — far beyond
//    what any human needs.
//
//  FAST-FORWARD (speed > 1):
//    We scale dt up and let CBF (or our multi-step fallback) handle the
//    extra ticks per frame. Capped at 64 sub-steps per frame to protect CPU.
// ============================================================================
class Speedhack {
public:
    static Speedhack& get();

    // Get / set the current speed multiplier (1.0 = normal).
    float getSpeed() const { return m_speed.load(std::memory_order_acquire); }
    void  setSpeed(float s);
    void  setDefaultFromSettings();

    // The effective base physics rate for the current speed. This is what
    // the PlayLayer::update hook should use as "ticks per second" when
    // deciding dt-per-step and steps-per-frame.
    double getEffectiveBaseRate() const;

    // Given the raw render-frame dt, compute the playback plan:
    //   - dtPerStep: game-time seconds per physics step (fixed for determinism)
    //   - stepsThisFrame: how many physics steps to run this render frame
    //   - frameDt: the dt to pass to each PlayLayer::update call
    struct StepPlan {
        float    dtPerStep;     // game-time per physics step
        uint32_t stepsThisFrame; // number of physics steps to run
        float    frameDt;       // dt to pass to PlayLayer::update per call
    };
    StepPlan plan(float rawRenderDt) const;

private:
    Speedhack();
    std::atomic<float> m_speed{1.0f};
};

} // namespace cbf
