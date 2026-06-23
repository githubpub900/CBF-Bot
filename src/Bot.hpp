// ═══════════════════════════════════════════════════════════════
// Bot.hpp — GDBot Core Declarations
// ═══════════════════════════════════════════════════════════════
#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

// ── Enums ──────────────────────────────────────────────────────

enum class CBFStatus : uint8_t {
    None       = 0,  // Red  — no CBF at all
    RobTopCBS  = 1,  // Yellow — built-in Click Between Steps (480 fps cap)
    SyzziCBF   = 2   // Green  — syzzi.click_between_frames (practically infinite)
};

enum class BotState : uint8_t {
    Idle      = 0,
    Recording = 1,
    Playing   = 2
};

// ── Data Structures ────────────────────────────────────────────

// A single input event — only stores *actions*, never per-frame snapshots.
// Even a 10-minute level with 8 000 clicks is only ~88 KB.
struct MacroInput {
    double  time;    // seconds into the level (high-precision double)
    bool    isPress; // true = pushButton, false = releaseButton
    uint8_t player;  // 0 = P1, 1 = P2 (dual)
    uint8_t button;  // maps to PlayerButton: 1 = Jump, 2 = Left, 3 = Right
};

// Full physics snapshot stored at every checkpoint — fixes the
// "practice bug" where checkpoints don't save velocity / state
// for ship, ball, UFO, wave, robot, spider, swing.
struct PlayerCheckpointState {
    double time;
    float  x, y;
    float  yAccel, jumpAccel;
    float  playerSpeed;
    float  rotation;
    // gamemode flags
    bool   isShip, isBall, isBird, isDart, isRobot, isSpider, isSwing;
    // physics flags
    bool   isGravityFlipped;
    bool   isOnGround;
    bool   isMini;
    bool   isSecondPlayer;
    float  playerScale;
};

struct CheckpointEntry {
    double time;
    int    stateIndex;
};

// ── MacroBot Singleton ─────────────────────────────────────────

class MacroBot {
public:
    static MacroBot& get() { static MacroBot i; return i; }

    // --- state ---
    BotState  m_state     = BotState::Idle;
    CBFStatus m_cbfStatus = CBFStatus::None;

    // --- macro data ---
    std::vector<MacroInput>            m_inputs;
    std::vector<PlayerCheckpointState> m_checkpointStates;   // P1
    std::vector<PlayerCheckpointState> m_checkpointStatesP2; // P2 (dual)
    std::vector<CheckpointEntry>       m_checkpointEntries;

    // --- playback cursor ---
    int    m_nextInputIndex     = 0;
    double m_levelTime          = 0.0;   // accumulated level-time (seconds)
    double m_recordingStartOff  = 0.0;   // time of first recorded input
    int64_t m_levelID           = 0;

    // --- speedhack ---
    float m_speedMultiplier = 1.0f;

    // --- practice-bug fix state ---
    bool m_needsStateRestore   = false;
    int  m_restoreCheckpointIdx = -1;
    int  m_restoreFrameCount    = 0;

    // --- gui ---
    bool        m_guiOpen       = false;
    bool        m_isDispatching = false;  // re-entrancy guard for playback
    std::string m_statusMessage = "Ready";
    std::string m_saveName      = "macro";
    void*       m_panelPtr      = nullptr; // BotPanel* (avoids header dep)

    // --- core ---
    void init();
    void updateCBFStatus();
    bool hasCBF() const { return m_cbfStatus != CBFStatus::None; }

    // --- recording ---
    void startRecording(PlayLayer* pl);
    void stopRecording();
    void recordInput(double time, bool press, uint8_t player, uint8_t button);
    void onDeath(PlayLayer* pl);
    void onCheckpoint(PlayLayer* pl);
    void onRestartFromPause();
    void pruneDeadInputs();

    // --- playback ---
    void startPlayback(PlayLayer* pl);
    void stopPlayback();
    void processFrame(PlayLayer* pl, double dt);

    // --- file I/O ---
    bool saveMacro(const std::string& filename);
    bool loadMacro(const std::string& filename);
    std::vector<std::string> listMacroFiles();

    // --- speedhack ---
    void setSpeed(float s);

    // --- practice fix ---
    PlayerCheckpointState captureState(PlayerObject* p, double time);
    void applyState(PlayerObject* p, const PlayerCheckpointState& s);

    // --- gui ---
    void toggleGUI();
    std::string cbfText() const;
    cocos2d::ccColor3B cbfColor() const;
    std::string stateText() const;
};