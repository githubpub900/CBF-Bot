#pragma once

#include <Geode/Geode.hpp>
#include <string>
#include <atomic>
#include <functional>

namespace cbf {

// ============================================================================
//  CBFIntegration
// ----------------------------------------------------------------------------
//  Single source of truth for "what tick are we on right now?".
//
//  Two modes:
//    A. Syzzi's CBF loaded ("syzzi.cbf"):
//       The CBF mod decouples the physics tick rate from the render frame
//       rate. We hook the same physics-step entry point CBF uses, so our
//       tick counter increments once per physics step regardless of FPS.
//       A macro recorded this way plays back identically at 60 / 240 / 1000
//       FPS - that's the whole point of CBF.
//
//    B. No CBF loaded (RobTop built-in stepping / vanilla):
//       We increment once per PlayLayer::update call, i.e. once per render
//       frame. Macros are then frame-numbered and only framerate-stable at
//       the same FPS they were recorded. This is the same behavior as
//       classic macro bots and is provided as a graceful fallback.
//
//  The recorder and player never query GD directly - they ask this class.
// ============================================================================
class CBFIntegration {
public:
    using TickCallback = std::function<void(uint64_t /*newTick*/)>;

    static CBFIntegration& get();

    // Called from hooks. Idempotent.
    void detectCBF();

    // True iff Syzzi's CBF is loaded and active in the current session.
    bool isCBFActive() const { return m_cbfLoaded; }

    // True iff we should treat sub-frame ticks as authoritative. Always true
    // when CBF is loaded; false in vanilla fallback.
    bool isSubFrameAccurate() const { return m_cbfLoaded; }

    // Current physics tick for the active PlayLayer. Starts at 0 on level
    // enter and increments monotonically until the level exits.
    uint64_t getCurrentTick() const { return m_tick.load(std::memory_order_acquire); }

    // Reset tick to 0 (called on PlayLayer init / full reset).
    void resetTick();

    // Advance the tick by exactly one physics step. Called by our
    // physics-step hook. Triggers registered callbacks.
    void advanceTick();

    // Subscribe to tick advances (for the recorder/player). Returns a
    // handle that can be used to unsubscribe.
    int  subscribe(TickCallback cb);
    void unsubscribe(int handle);

    // Optional: if Syzzi's CBF exposes a global tick counter we can read
    // directly (faster than maintaining our own), we use it here. Returns
    // false if not available, in which case we fall back to our own.
    bool tryReadCBFOwnedTick(uint64_t& out);

    // Inform the integration of the configured fixed-step rate (ticks per
    // second) so the speedhack and UI can convert ticks <-> seconds.
    void setTicksPerSecond(double tps);
    double getTicksPerSecond() const { return m_tps; }

private:
    CBFIntegration();

    bool                     m_cbfLoaded = false;
    std::atomic<uint64_t>    m_tick{0};
    double                   m_tps = 60.0; // default for vanilla 60fps
    std::vector<TickCallback> m_callbacks;
    int                      m_nextHandle = 1;
};

} // namespace cbf
