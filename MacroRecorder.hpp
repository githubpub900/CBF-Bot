#pragma once

#include "MacroFormat.hpp"
#include <Geode/Geode.hpp>
#include <string>
#include <functional>

namespace cbf {

class CBFIntegration;

// ============================================================================
//  MacroRecorder
// ----------------------------------------------------------------------------
//  Captures every press/release of the jump button (for both players in
//  dual mode) tagged with the current physics tick from CBFIntegration.
//  Produces a MacroData ready to be saved to disk.
//
//  Recording lifecycle:
//    start()  -> armed, waits for first input or first tick
//    onButtonPressed(...)  -> called from PlayerObject hooks
//    stop()   -> finalizes the macro
//
//  The recorder is a singleton because GD's hook targets are global - we
//  can't easily pass a `this` pointer through RobTop's vtables.
// ============================================================================
class MacroRecorder {
public:
    struct RecordStats {
        uint64_t startTick = 0;
        uint64_t lastTick  = 0;
        uint32_t eventCount = 0;
        bool     p1Held = false;
        bool     p2Held = false;
    };

    using StopCallback = std::function<void(const MacroData&)>;

    static MacroRecorder& get();

    // Begin a fresh recording. Resets all state.
    void start();

    // Stop recording and finalize (compute end_tick, etc.).
    void stop();

    bool isRecording() const { return m_recording; }

    const MacroData& data() const { return m_data; }
    const RecordStats& stats() const { return m_stats; }

    // Called from input hooks. `player` is 1 or 2. `push` true = press,
    // false = release.
    void onButtonPressed(int player, bool push);

    // Called on PlayLayer init (level entered) - resets state and may
    // auto-start if the auto-record setting is on.
    void onLevelEnter(bool isPracticeMode);

    // Called on PlayLayer exit / level complete - stops recording if active.
    void onLevelExit(bool completed);

    // Register a callback fired when recording stops.
    void setStopCallback(StopCallback cb) { m_stopCb = std::move(cb); }

private:
    MacroRecorder();

    void emitEvent(int64_t deltaTicks, uint8_t action);

    MacroData    m_data;
    RecordStats  m_stats;
    bool         m_recording = false;
    uint64_t     m_lastEventTick = 0;
    StopCallback m_stopCb;
};

} // namespace cbf
