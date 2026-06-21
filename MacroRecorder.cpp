#include "MacroRecorder.hpp"
#include "CBFIntegration.hpp"
#include <Geode/loader/Mod.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>

using namespace geode::prelude;

namespace cbf {

MacroRecorder& MacroRecorder::get() {
    static MacroRecorder inst;
    return inst;
}

MacroRecorder::MacroRecorder() {
    m_data.reset();
}

void MacroRecorder::start() {
    if (m_recording) return;
    m_data.reset();

    auto& cbf = CBFIntegration::get();
    cbf.detectCBF();
    cbf.resetTick();

    m_recording = true;
    m_lastEventTick = 0;
    m_stats = RecordStats{};

    // Header is filled in as we go.
    m_data.header.start_tick = 0;
    if (cbf.isCBFActive())   m_data.header.flags |= FlagCBFMode;

    log::info("[CBFMacroBot] Recording started (CBF={}, tps={}).",
              cbf.isCBFActive(), cbf.getTicksPerSecond());
}

void MacroRecorder::stop() {
    if (!m_recording) return;
    m_recording = false;

    m_data.header.end_tick = CBFIntegration::get().getCurrentTick();
    m_stats.lastTick = m_data.header.end_tick;
    m_stats.eventCount = static_cast<uint32_t>(m_data.events.size());

    log::info("[CBFMacroBot] Recording stopped. {} events, ticks 0..{}.",
              m_stats.eventCount, m_data.header.end_tick);

    if (m_stopCb) m_stopCb(m_data);
}

void MacroRecorder::emitEvent(int64_t deltaTicks, uint8_t action) {
    m_data.events.push_back({ deltaTicks, action });
    m_stats.eventCount = static_cast<uint32_t>(m_data.events.size());
}

void MacroRecorder::onButtonPressed(int player, bool push) {
    if (!m_recording) return;

    auto& cbf = CBFIntegration::get();
    uint64_t now = cbf.getCurrentTick();
    int64_t delta = static_cast<int64_t>(now) - static_cast<int64_t>(m_lastEventTick);
    m_lastEventTick = now;

    // Update held state and build action byte.
    uint8_t action = 0;
    bool& held = (player == 1) ? m_stats.p1Held : m_stats.p2Held;

    if (push && !held) {
        held = true;
        action |= (player == 1) ? (ActP1Held | ActP1PressEdge)
                                : (ActP2Held | ActP2PressEdge);
    } else if (!push && held) {
        held = false;
        action |= (player == 1) ? ActP1RelEdge : ActP2RelEdge;
    } else {
        // Redundant event (already in that state) - ignore to keep file
        // compact. This shouldn't happen in normal play.
        return;
    }

    // Carry the OTHER player's held state into this event's byte so a
    // player reading only edges still sees consistent state.
    if (m_stats.p1Held && player != 1) action |= ActP1Held;
    if (m_stats.p2Held && player != 2) action |= ActP2Held;

    emitEvent(delta, action);

    // Track two-player flag.
    if (player == 2) m_data.header.flags |= FlagTwoPlayer;
}

void MacroRecorder::onLevelEnter(bool isPracticeMode) {
    // Always reset on level enter - even if we weren't recording.
    m_data.reset();
    m_stats = RecordStats{};
    m_lastEventTick = 0;

    auto& cbf = CBFIntegration::get();
    cbf.detectCBF();
    cbf.resetTick();

    // Record which level this is.
    auto pl = PlayLayer::get();
    if (pl && pl->m_level) {
        m_data.header.level_id = pl->m_level->m_levelID;
    }
    m_data.header.recorded_fps = static_cast<float>(cbf.getTicksPerSecond());
    m_data.header.speed = 1.0f;

    // Auto-record in practice mode if the setting is on.
    if (isPracticeMode) {
        bool autoRecord = Mod::get()->getSettingValue<bool>("auto-record");
        if (autoRecord) start();
    }
}

void MacroRecorder::onLevelExit(bool completed) {
    if (!m_recording) return;
    bool autoStop = Mod::get()->getSettingValue<bool>("auto-stop-at-end");
    if (completed && autoStop) {
        stop();
    } else if (!completed) {
        // Level exited without completion (quit). Discard or stop?
        // We stop so the user can still save the partial macro.
        stop();
    }
}

} // namespace cbf
