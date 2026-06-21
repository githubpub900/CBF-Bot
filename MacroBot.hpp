#pragma once

#include "MacroRecorder.hpp"
#include "MacroPlayer.hpp"
#include "CBFIntegration.hpp"
#include "PracticeFix.hpp"
#include "Speedhack.hpp"
#include <Geode/Geode.hpp>
#include <string>
#include <vector>
#include <filesystem>

namespace cbf {

// ============================================================================
//  MacroBot
// ----------------------------------------------------------------------------
//  High-level facade that the UI and Geode hooks talk to. Owns the lifecycle
//  of every subsystem and exposes simple commands (record / play / speed).
//
//  All hooks in main.cpp route their events through here so the rest of the
//  mod never has to know about GD's internal classes.
// ============================================================================
class MacroBot {
public:
    enum class State {
        Idle,
        Recording,
        Playing,
        PlayingPending, // queued - will start on next level (re)start
    };

    static MacroBot& get();

    // Called from the Geode $execute block.
    void initialize();

    // ---- Recording ----
    void startRecording();
    void stopRecording();
    bool isRecording() const;

    // ---- Playback ----
    // Load by path or use whatever was last recorded (in-memory).
    bool playLastRecorded();
    bool playFromFile(const std::string& path);
    void stopPlayback();
    bool isPlaying() const;

    // ---- Save / Load ----
    bool saveLastRecorded(const std::string& filename);
    std::vector<std::string> listSavedMacros() const;
    std::filesystem::path macroDir() const;

    // ---- Speedhack ----
    void setSpeed(float s);
    float getSpeed() const;

    // ---- Practice fix ----
    void setPracticeFixEnabled(bool e);
    bool isPracticeFixEnabled() const;

    // ---- GD event routing (called by hooks in main.cpp) ----
    void onPlayLayerInit(bool isPracticeMode);
    void onPlayLayerDestroy(bool completed);
    void onPhysicsStep();                       // called per physics sub-step
    void onPlayerButton(int player, bool push); // player 1 or 2

    // ---- State for UI ----
    State state() const;
    MacroRecorder::RecordStats recordStats() const;
    MacroPlayer::PlayStats     playStats() const;

private:
    MacroBot();
    bool m_initialized = false;
};

} // namespace cbf
