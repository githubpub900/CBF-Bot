#pragma once

#include "MacroFormat.hpp"
#include <Geode/Geode.hpp>
#include <string>

namespace cbf {

// ============================================================================
//  MacroPlayer
// ----------------------------------------------------------------------------
//  Streams a MacroData back into the game. The player subscribes to
//  CBFIntegration's tick callback and applies events as their tick arrives.
//
//  Memory model:
//    The full event list is loaded into memory ONCE (it's tiny - a 2-minute
//    level is ~3 KB). Playback then advances a single index forward; we
//    never scan the whole list per tick, so playback cost is O(events) total
//    regardless of macro length. This is what makes "infinite ticks" cheap.
//
//  Playback is deterministic because:
//    - With CBF: ticks advance at a fixed rate, independent of FPS.
//    - Without CBF: ticks advance per frame; the macro plays back at the
//      same FPS it was recorded at.
//
//  We push inputs through PlayerObject::pushButton / releaseButton so the
//  game's own input handling (squid mode, ship rotation, etc.) all behave
//  exactly as if a human pressed the button.
// ============================================================================
class MacroPlayer {
public:
    struct PlayStats {
        uint64_t startTick  = 0;
        uint64_t currentTick = 0;
        uint64_t totalTicks = 0;
        size_t   eventsApplied = 0;
        size_t   totalEvents   = 0;
        bool     finished = false;
    };

    static MacroPlayer& get();

    // Load a macro into memory. Replaces any previously loaded macro.
    void load(const MacroData& data);
    bool loadFile(const std::string& path);

    // Begin playback on the next PlayLayer enter (or immediately if a
    // PlayLayer is already active and at tick 0).
    void play();
    void stop();
    bool isPlaying() const { return m_playing; }
    bool isPending() const { return m_pendingStart; }

    const MacroData& data() const { return m_data; }
    const PlayStats& stats() const { return m_stats; }

    // Called by CBFIntegration on each physics tick.
    void onTick(uint64_t tick);

    // Called by MacroBot on PlayLayer init / destroy.
    void onLevelEnter();
    void onLevelExit();

private:
    MacroPlayer();

    void applyEvent(const Event& e);
    void resetCursor();

    MacroData  m_data;
    PlayStats  m_stats;
    bool       m_playing = false;
    bool       m_pendingStart = false;
    size_t     m_nextIdx = 0;
    uint64_t   m_nextTick = 0;     // absolute tick at which m_events[m_nextIdx] fires
    uint64_t   m_startTick = 0;    // tick at which playback began (usually 0)
    int        m_subHandle = 0;
    // Tracked held state so we only call pushButton/releaseButton when the
    // recorded state differs from the current state. Avoids spurious
    // re-presses when an event carries only the "held" bit (no edge).
    bool       m_p1Held = false;
    bool       m_p2Held = false;
};

} // namespace cbf
