#pragma once

#include <Geode/Geode.hpp>
#include <string>
#include <atomic>
#include <functional>

namespace cbf {

// ============================================================================
//  CBFIntegration
// ----------------------------------------------------------------------------
//  GD 2.2081 ships with BUILT-IN Click Between Frames support:
//    - GJBaseGameLayer::m_clickBetweenSteps  (bool: CBF enabled)
//    - GJBaseGameLayer::m_clickOnSteps       (bool: click on step boundaries)
//    - GJBaseGameLayer::m_isBetweenSteps     (bool: currently between steps)
//    - GJBaseGameLayer::m_tickIndex          (int: GD's own tick counter!)
//    - GJBaseGameLayer::m_currentStep        (int: step counter)
//    - GJBaseGameLayer::m_randomSeed         (uint64_t: deterministic RNG)
//    - GJBaseGameLayer::m_replayRandSeed     (uint64_t: replay RNG)
//
//  We read GD's own m_tickIndex as our authoritative tick source. This is
//  better than maintaining our own counter because:
//    1. It's the SAME counter GD uses for its built-in replay system.
//    2. It's guaranteed to be in sync with physics steps.
//    3. It works identically with or without Syzzi's CBF mod.
//
//  If Syzzi's CBF is loaded, it may increase the physics step rate (running
//  more steps per render frame). Our tick counter naturally follows because
//  we read m_tickIndex which CBF also increments.
// ============================================================================
class CBFIntegration {
public:
    using TickCallback = std::function<void(uint64_t /*newTick*/)>;

    static CBFIntegration& get();

    // Called from $execute and on PlayLayer init. Idempotent.
    void detectCBF();

    // True iff Syzzi's CBF mod is loaded.
    bool isSyzziCBFLoaded() const { return m_syzziLoaded; }

    // True iff GD's built-in CBF (m_clickBetweenSteps) is active.
    bool isBuiltinCBFActive() const;

    // True iff ANY form of sub-frame tick precision is available.
    bool isSubFrameAccurate() const { return isSyzziCBFLoaded() || isBuiltinCBFActive(); }

    // Read GD's own m_tickIndex from the active PlayLayer. Returns 0 if no
    // PlayLayer is active.
    uint64_t getCurrentTick() const;

    // Reset our cached tick (called on PlayLayer init). GD's m_tickIndex is
    // reset by GD itself on level (re)start.
    void resetTick();

    // Advance our local tick counter. Called from the PlayerObject::update
    // hook (once per physics step for player 1). We also read m_tickIndex
    // directly when available, so this is mainly a fallback.
    void advanceTick();

    // Subscribe to tick advances. Returns a handle for unsubscribing.
    int  subscribe(TickCallback cb);
    void unsubscribe(int handle);

    // Enable / disable GD's built-in CBF on the active PlayLayer.
    void setBuiltinCBF(bool enabled);

    // Configure the physics ticks-per-second. With CBF this is typically
    // 240 Hz. Without CBF it matches the render FPS (usually 60).
    void setTicksPerSecond(double tps);
    double getTicksPerSecond() const { return m_tps; }

    // The maximum physics rate the user has configured (from settings).
    // Used by the Speedhack to know how high it can push the physics rate
    // for smooth slow-motion.
    double getMaxPhysicsRate() const { return m_maxRate; }
    void setMaxPhysicsRate(double hz);

private:
    CBFIntegration();

    bool                      m_syzziLoaded = false;
    std::atomic<uint64_t>     m_tick{0};
    double                    m_tps = 240.0;
    double                    m_maxRate = 240.0;
    std::vector<TickCallback> m_callbacks;
    int                       m_nextHandle = 1;
};

} // namespace cbf
