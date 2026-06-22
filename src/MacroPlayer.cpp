#include "MacroPlayer.hpp"
#include "CBFIntegration.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/loader/Mod.hpp>

using namespace geode::prelude;

namespace cbf {

MacroPlayer& MacroPlayer::get() {
    static MacroPlayer inst;
    return inst;
}

MacroPlayer::MacroPlayer() {
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
        m_pendingStart = true;
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
    m_startTick = CBFIntegration::get().getCurrentTick();
    resetCursor();

    // Enable GD's built-in CBF for sub-frame accuracy during playback.
    bool useBuiltin = Mod::get()->getSettingValue<bool>("use-builtin-cbf");
    if (useBuiltin) CBFIntegration::get().setBuiltinCBF(true);

    log::info("[CBFMacroBot] Playback started at tick {}.", m_startTick);
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

    // Apply edges by calling pushButton / releaseButton with the correct
    // PlayerButton enum. GD 2.2081's pushButton takes PlayerButton (not int).
    // PlayerButton::Jump is the primary action button.
    auto pushFor = [pl](int player, bool push) {
        PlayerObject* p = (player == 1) ? pl->m_player1 : pl->m_player2;
        if (!p) return;
        if (push) p->pushButton(PlayerButton::Jump);
        else      p->releaseButton(PlayerButton::Jump);
    };

    // Update tracked held state and apply edges.
    if (e.action & ActP1PressEdge) { m_p1Held = true;  pushFor(1, true);  }
    if (e.action & ActP1RelEdge)   { m_p1Held = false; pushFor(1, false); }
    if (e.action & ActP2PressEdge) { m_p2Held = true;  pushFor(2, true);  }
    if (e.action & ActP2RelEdge)   { m_p2Held = false; pushFor(2, false); }

    // Held-only reconciliation (no edge): for the rare case where recording
    // started mid-hold.
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
            int64_t d = std::max<int64_t>(0, m_data.events[m_nextIdx].delta_ticks);
            m_nextTick += static_cast<uint64_t>(d);
        }
    }

    if (m_nextIdx >= m_data.events.size()) {
        m_stats.finished = true;
        m_playing = false;
        log::info("[CBFMacroBot] Macro finished at tick {} ({} events applied).",
                  tick, m_stats.eventsApplied);
    }
}

} // namespace cbf
