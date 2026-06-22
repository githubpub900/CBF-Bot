// Bot.hpp
#pragma once
#include <Geode/Geode.hpp>
#include <vector>
#include <string>

using namespace geode::prelude;

/*
 * Chosen recording format: time‑stamped input events (absolute game time, double precision).
 *
 * Rationale:
 *  - Not frame‑based, not tied to rendering FPS.
 *  - Works naturally with CBS and CBF because inputs are recorded exactly when
 *    they are processed by the game (inside handleButton calls).
 *  - File size scales with click count, not run duration.
 *  - Delta‑encoding during serialisation keeps files extremely small.
 *  - High‑precision double timestamps preserve sub‑millisecond accuracy,
 *    compatible with Syzzi CBF’s massive input resolution.
 */

struct InputEvent {
    double time;      // absolute level time (from m_gameState.m_levelTime)
    bool down;        // true = press, false = release
    int button;       // 1 = jump (can be extended)
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
    static void checkpointStored(CheckpointObject* cp);
    static void checkpointLoaded(CheckpointObject* cp);

    // ---------- Speedhack ----------
    static float getSpeedhack();
    static void setSpeedhack(float speed);
    static void speedhackUp();
    static void speedhackDown();

    // ---------- Status ----------
    enum class CBSMode { None, RobTop, Syzzi };
    static CBSMode getCBSMode();

private:
    // Recording state
    static inline std::vector<InputEvent> m_events;
    static inline bool m_recording = false;
    static inline std::vector<size_t> m_checkpointEventIndices; // event count at each checkpoint

    // Playback state
    static inline bool m_playing = false;
    static inline size_t m_playIndex = 0;
    static inline double m_playAccum = 0.0;         // time accumulator inside manual stepping
    static inline double m_playBaseStep = 1.0/480.0; // base physics step for playback (480 Hz)
    static inline bool m_didInjectThisStep = false;

    // UI
    static inline bool m_uiVisible = false;
    static inline CCNode* m_uiNode = nullptr;

    // Speedhack
    static inline float m_speed = 1.0f;

    // Checkpoint extra data
    struct ExtraCPData {
        // Universal
        bool dual;
        bool gravityFlipped;
        float sizeMod;
        int speedPortal; // 0=normal,1=slow,2=fast,3=veryfast?
        // Gamemode specifics – store every relevant field from PlayerObject
        // We’ll store full copies of the vehicle state members
        // (offsets taken from GD 2.2081)
        float shipRotation;
        float ballRotation;
        float ufoRotation;
        float waveRotation;
        float robotRotation;
        float spiderRotation;
        float swingRotation;
        // Add more as needed – this set covers all gamemodes
    };
    static inline std::unordered_map<CheckpointObject*, ExtraCPData> m_cpExtra;

    // Helpers
    static void injectInput(const InputEvent& ev);
    static void processPlaybackStep(PlayLayer* pl, float customDt);
    friend class PlayLayerHook;
    friend class SchedulerHook;
    friend class KeyboardHook;
};