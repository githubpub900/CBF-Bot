// Bot.hpp
#pragma once
#include <Geode/Geode.hpp>
#include <vector>
#include <string>
#include <fstream>

using namespace geode::prelude;

/*
 * Recording format: absolute level-time + input flags.
 *
 * Why this is optimal:
 *  1) Not frame‑based → deterministic under CBS and CBF.
 *  2) Time‑stamps are taken directly from m_gameState.m_levelTime,
 *     which advances only with physics steps – perfect synchronisation.
 *  3) File size ∝ click count, not run length.
 *  4) Delta‑encoding during serialisation keeps files tiny.
 *  5) High‑resolution double preserves sub‑millisecond accuracy,
 *     making it compatible with Syzzi CBF’s enormous timing resolution.
 */

struct InputEvent {
    double time;      // absolute level time (from m_gameState.m_levelTime)
    bool down;        // true = press, false = release
    int button;       // 1 = jump (extendable)
};

class Bot {
public:
    // ---------- Recording ----------
    static void startRecording();
    static void stopRecording();
    static bool isRecording();

    // ---------- Playback ----------
    static void startPlayback();
    static void stopPlayback();
    static bool isPlaying();

    // ---------- Macro I/O ----------
    static void saveMacro(const std::string& path);
    static void loadMacro(const std::string& path);

    // ---------- UI ----------
    static void toggleUI(PlayLayer* pl);
    static bool isUIVisible();

    // ---------- Input capture (called from hook) ----------
    static void recordInput(double time, bool down, int button);

    // ---------- Dead‑input cleanup ----------
    static void onDeath();    // truncate events after last checkpoint
    static void onRestart();  // clear all events

    // ---------- Checkpoint ----------
    // Called when the game stores a checkpoint (we cannot reliably hook
    // the internal load function in GD 2.2; see comments in main.cpp).
    static void checkpointStored(CheckpointObject* cp);

    // ---------- Speedhack ----------
    static float getSpeedhack();
    static void setSpeedhack(float speed);
    static void speedhackUp();
    static void speedhackDown();

    // ---------- Status ----------
    enum class CBSMode { None, RobTop, Syzzi };
    static CBSMode getCBSMode();

    // UI callbacks – must be static
    static void onRecordButton(CCObject*);
    static void onPlayButton(CCObject*);
    static void onSaveButton(CCObject*);
    static void onLoadButton(CCObject*);

private:
    // Recording state
    static inline std::vector<InputEvent> m_events;
    static inline bool m_recording = false;
    static inline std::vector<size_t> m_checkpointEventIndices;

    // Playback state
    static inline bool m_playing = false;
    static inline size_t m_playIndex = 0;
    static inline double m_playAccum = 0.0;
    static inline double m_playBaseStep = 1.0 / 480.0; // 480 Hz – covers CBS limit
    static inline bool m_didInjectThisStep = false;

    // UI
    static inline bool m_uiVisible = false;
    static inline CCNode* m_uiNode = nullptr;

    // Speedhack
    static inline float m_speed = 1.0f;

    // Helper for injecting an input during playback
    static void injectInput(const InputEvent& ev);
    // Main playback stepping logic
    static void processPlaybackStep(PlayLayer* pl, float customDt);

    friend class PlayLayerHook;
    friend class SchedulerHook;
    friend class KeyboardHook;
};