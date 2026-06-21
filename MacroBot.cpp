#include "MacroBot.hpp"
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

namespace cbf {

MacroBot& MacroBot::get() {
    static MacroBot inst;
    return inst;
}

MacroBot::MacroBot() = default;

void MacroBot::initialize() {
    if (m_initialized) return;
    m_initialized = true;

    auto& cbf = CBFIntegration::get();
    cbf.detectCBF();
    PracticeFix::get().install();
    Speedhack::get().setDefaultFromSettings();

    // Ensure macro save directory exists.
    try {
        std::filesystem::create_directories(macroDir());
    } catch (const std::exception& e) {
        log::warn("[CBFMacroBot] Could not create macro dir: {}", e.what());
    }

    log::info("[CBFMacroBot] Initialized. CBF={}, save dir={}.",
              cbf.isCBFActive(), macroDir().string());
}

// ---------------------------------------------------------------------------
//  Recording
// ---------------------------------------------------------------------------
void MacroBot::startRecording() {
    // Recording implies stopping any active playback (they share the input
    // pipeline and would conflict).
    if (MacroPlayer::get().isPlaying()) stopPlayback();
    MacroRecorder::get().start();
}

void MacroBot::stopRecording() {
    MacroRecorder::get().stop();
}

bool MacroBot::isRecording() const {
    return MacroRecorder::get().isRecording();
}

// ---------------------------------------------------------------------------
//  Playback
// ---------------------------------------------------------------------------
bool MacroBot::playLastRecorded() {
    auto& rec = MacroRecorder::get();
    if (rec.data().events.empty()) {
        log::warn("[CBFMacroBot] No recorded macro to play.");
        return false;
    }
    MacroPlayer::get().load(rec.data());
    MacroPlayer::get().play();
    return true;
}

bool MacroBot::playFromFile(const std::string& path) {
    if (!MacroPlayer::get().loadFile(path)) return false;
    MacroPlayer::get().play();
    return true;
}

void MacroBot::stopPlayback() {
    MacroPlayer::get().stop();
}

bool MacroBot::isPlaying() const {
    return MacroPlayer::get().isPlaying();
}

// ---------------------------------------------------------------------------
//  Save / Load
// ---------------------------------------------------------------------------
std::filesystem::path MacroBot::macroDir() const {
    auto mod = Mod::get();
    auto base = mod->getSaveDir();
    auto folder = mod->getSettingValue<std::string>("save-folder");
    if (folder.empty()) folder = "cbf_macros";
    return base / folder;
}

bool MacroBot::saveLastRecorded(const std::string& filename) {
    auto& rec = MacroRecorder::get();
    if (rec.data().events.empty()) {
        log::warn("[CBFMacroBot] No recorded macro to save.");
        return false;
    }
    std::filesystem::path p = macroDir() / filename;
    if (p.extension() != ".cbfm") p += ".cbfm";
    bool ok = saveMacro(p.string(), rec.data());
    if (ok) log::info("[CBFMacroBot] Saved macro to {}", p.string());
    else    log::warn("[CBFMacroBot] Failed to save macro to {}", p.string());
    return ok;
}

std::vector<std::string> MacroBot::listSavedMacros() const {
    std::vector<std::string> out;
    try {
        if (!std::filesystem::exists(macroDir())) return out;
        for (auto& e : std::filesystem::directory_iterator(macroDir())) {
            if (!e.is_regular_file()) continue;
            if (e.path().extension() == ".cbfm") out.push_back(e.path().filename().string());
        }
    } catch (...) {}
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
//  Speedhack
// ---------------------------------------------------------------------------
void MacroBot::setSpeed(float s) { Speedhack::get().setSpeed(s); }
float MacroBot::getSpeed() const { return Speedhack::get().getSpeed(); }

// ---------------------------------------------------------------------------
//  Practice fix
// ---------------------------------------------------------------------------
void MacroBot::setPracticeFixEnabled(bool e) { PracticeFix::get().setEnabled(e); }
bool MacroBot::isPracticeFixEnabled() const  { return PracticeFix::get().isEnabled(); }

// ---------------------------------------------------------------------------
//  GD event routing
// ---------------------------------------------------------------------------
void MacroBot::onPlayLayerInit(bool isPracticeMode) {
    auto& cbf = CBFIntegration::get();
    cbf.detectCBF();
    cbf.resetTick();

    PracticeFix::get().onLevelStart(isPracticeMode);
    MacroRecorder::get().onLevelEnter(isPracticeMode);
    MacroPlayer::get().onLevelEnter();
}

void MacroBot::onPlayLayerDestroy(bool completed) {
    MacroRecorder::get().onLevelExit(completed);
    MacroPlayer::get().onLevelExit();
    PracticeFix::get().onLevelEnd();
}

void MacroBot::onPhysicsStep() {
    CBFIntegration::get().advanceTick();
}

void MacroBot::onPlayerButton(int player, bool push) {
    if (MacroRecorder::get().isRecording()) {
        MacroRecorder::get().onButtonPressed(player, push);
    }
}

MacroBot::State MacroBot::state() const {
    if (MacroRecorder::get().isRecording()) return State::Recording;
    if (MacroPlayer::get().isPlaying())     return State::Playing;
    if (MacroPlayer::get().isPending())     return State::PlayingPending;
    return State::Idle;
}

MacroRecorder::RecordStats MacroBot::recordStats() const {
    return MacroRecorder::get().stats();
}
MacroPlayer::PlayStats MacroBot::playStats() const {
    return MacroPlayer::get().stats();
}

} // namespace cbf
