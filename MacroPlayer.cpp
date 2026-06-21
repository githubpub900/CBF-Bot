#include "MacroPlayer.hpp"
#include "CBFIntegration.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/loader/Mod.hpp>

using namespace geode::prelude;

namespace cbf {

MacroPlayer& MacroPlayer::get() {
    static MacroPlayer inst;
    return inst;
}

MacroPlayer::MacroPlayer() {
    // Subscribe to physics-tick callbacks once. The callback body is a no-op
    // when not playing.
    m_subHandle = CBFIntegration::get().subscribe(
        [this](uint64_t t) { this->onTick(t); }
    );
}

void MacroPlayer::resetCursor() {
    m_nextIdx = 0;
    m_nextTick = m_startTick;
    if (!m_data.events.empty()) {
        m_nextTick = m_startTick +
            static_cast<uint64_t>(std::max<int64_t>(0, m_data.events[0].delta_ticks));
    }
    m_stats = PlayStats{};
    m_stats.totalTicks  = macroTickSpan(m_data);
    m_stats.totalEvents = m_data.events.size();
    m_p1Held = false;
    m_p2Held = false;
}

void MacroPlayer::load(const MacroData& data) {
    m_data = data;
    resetCursor();
    log::info("[CBFMacroBot] Loaded macro: {} events, {} ticks, CBF={}.",
              m_data.events.size(), m_stats.totalTicks, m_data.isCBF());
}

bool MacroPlayer::loadFile(const std::string& path) {
    MacroData tmp;
    if (!loadMacro(path, tmp)) {
        log::warn("[CBFMacroBot] Failed to load macro from {}", path);
        return false;
    }
    load(tmp);
    return true;
}

void MacroPlayer::play() {
    if (m_playing) return;
    if (m_data.events.empty()) {
        log::warn("[CBFMacroBot] No macro loaded - nothing to play.");
        return;
    }

    auto pl = PlayLayer::get();
    if (pl && !pl->m_isPaused) {
        // Level is already running. We require a fresh start to keep timing
        // deterministic, so request a restart.
        m_pendingStart = true;
        // MacroBot will handle the actual restart when it processes the
        // pending flag (we don't restart from inside the player to avoid
        // reentrancy during update).
        log::info("[CBFMacroBot] Playback queued - will start on next level (re)start.");
    } else {
        m_pendingStart = true;
        log::info("[CBFMacroBot] Playback queued - will start when a level is entered.");
    }
}

void MacroPlayer::stop() {
    if (!m_playing && !m_pendingStart) return;
    m_playing = false;
    m_pendingStart = false;
    resetCursor();
    log::info("[CBFMacroBot] Playback stopped.");
}

void MacroPlayer::onLevelEnter() {
    if (!m_pendingStart) return;
    m_pendingStart = false;
    m_playing = true;
    m_startTick = 0; // we always play from the level's start
    resetCursor();
    log::info("[CBFMacroBot] Playback started.");
}

void MacroPlayer::onLevelExit() {
    if (m_playing) {
        m_playing = false;
        m_stats.finished = true;
        log::info("[CBFMacroBot] Playback ended (level exited). Applied {}/{} events.",
                  m_stats.eventsApplied, m_stats.totalEvents);
    }
    resetCursor();
}

void MacroPlayer::applyEvent(const Event& e) {
    auto pl = PlayLayer::get();
    if (!pl) return;

    // Apply edges first - this is what makes the input feel native to GD's
    // input pipeline. We call pushButton / releaseButton directly on the
    // PlayerObject so all downstream game logic (jump buffering, ship
    // rotation, etc.) runs identically to a real click.
    auto pushFor = [pl](int player, bool push) {
        PlayerObject* p = (player == 1) ? pl->m_player1 : pl->m_player2;
        if (!p) return;
        // The int argument corresponds to PlayerButton::Jump (1) in GD 2.2.
        // Different bindings expose this as int or enum; we use int.
        if (push) p->pushButton(1);
        else      p->releaseButton(1);
    };

    // Update tracked held state and apply edges. If only the "held" bit is
    // set with no edge (e.g. macro started mid-hold), reconcile our tracked
    // state with the recorded state to avoid spurious re-presses.
    if (e.action & ActP1PressEdge) { m_p1Held = true;  pushFor(1, true);  }
    if (e.action & ActP1RelEdge)   { m_p1Held = false; pushFor(1, false); }
    if (e.action & ActP2PressEdge) { m_p2Held = true;  pushFor(2, true);  }
    if (e.action & ActP2RelEdge)   { m_p2Held = false; pushFor(2, false); }

    // Held-only reconciliation (no edge): only push/release if our tracked
    // state disagrees with the recorded state. This handles the rare case
    // where recording started while a button was already held.
    if (!(e.action & (ActP1PressEdge | ActP1RelEdge))) {
        bool wantHeld = (e.action & ActP1Held) != 0;
        if (wantHeld != m_p1Held) {
            pushFor(1, wantHeld);
            m_p1Held = wantHeld;
        }
    }
    if (!(e.action & (ActP2PressEdge | ActP2RelEdge))) {
        bool wantHeld = (e.action & ActP2Held) != 0;
        if (wantHeld != m_p2Held) {
            pushFor(2, wantHeld);
            m_p2Held = wantHeld;
        }
    }
}

void MacroPlayer::onTick(uint64_t tick) {
    if (!m_playing) return;
    m_stats.currentTick = tick;

    // Apply every event whose absolute tick <= current tick. Multiple
    // events can fire on the same tick (delta=0), which is normal for
    // frame-perfect dual-player inputs.
    while (m_nextIdx < m_data.events.size() && m_nextTick <= tick) {
        const Event& e = m_data.events[m_nextIdx];
        applyEvent(e);
        m_stats.eventsApplied++;
        m_nextIdx++;

        if (m_nextIdx < m_data.events.size()) {
            // delta_ticks is signed; clamp negatives to 0 (defensive).
            int64_t d = std::max<int64_t>(0, m_data.events[m_nextIdx].delta_ticks);
            m_nextTick += static_cast<uint64_t>(d);
        }
    }

    // Finished?
    if (m_nextIdx >= m_data.events.size()) {
        m_stats.finished = true;
        m_playing = false;
        log::info("[CBFMacroBot] Macro finished at tick {} ({} events applied).",
                  tick, m_stats.eventsApplied);
    }
}

} // namespace cbf
