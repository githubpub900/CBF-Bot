// ============================================================================
//  GD Macro Bot — Bot.hpp
//  Target : Geometry Dash 2.2081
//  Geode  : v5.7.1
//
//  This header is the entire bot core: data structures + Bot singleton.
//  Hooks, UI, F8 menu, and Bot method implementations live in main.cpp.
//
//  ┌─ Why two files? ──────────────────────────────────────────────────────┐
//  │  Bot.hpp  — pure data structures + Bot class declaration.             │
//  │  main.cpp — Geode $modify hooks, Cocos2d UI, Bot method bodies.       │
//  └───────────────────────────────────────────────────────────────────────┘
// ============================================================================

#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <string>
#include <cstdint>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <utility>

using namespace geode::prelude;

namespace macrobot {

// ============================================================================
//  Constants
// ============================================================================
inline constexpr const char* SYZZI_CBF_MOD_ID   = "syzzi.click_between_frames";
inline constexpr const char* MACRO_MAGIC         = "GDBOTv1";
inline constexpr uint32_t    MACRO_VERSION       = 1;
inline constexpr float       CBS_EFFECTIVE_MAX_FPS = 480.f; // see CB detection notes

// ============================================================================
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │              RECORDING FORMAT — X-POSITION BASED                     │
//  ├──────────────────────────────────────────────────────────────────────┤
//  │                                                                       │
//  │  Each input is one 8-byte record:                                     │
//  │     struct InputRecord { float x; u8 action; u8 player; u16 pad; }    │
//  │                                                                       │
//  │  WHY X-POSITION IS OPTIMAL FOR CBF/CBS GAMEPLAY                       │
//  │                                                                       │
//  │  1. PLAYBACK ACCURACY                                                 │
//  │     GD physics is a deterministic function of (level layout, input    │
//  │     sequence at X positions). Two runs reaching the same X with the   │
//  │     same inputs produce identical outcomes. That is exactly the       │
//  │     invariant a macro bot needs to exploit.                           │
//  │                                                                       │
//  │  2. DETERMINISM                                                       │
//  │     X is a property of the game state, not the renderer. It is        │
//  │     invariant under FPS, refresh-rate, and speedhack changes.         │
//  │     Frame-based recording breaks under all three.                     │
//  │                                                                       │
//  │  3. SMALL FILE SIZE                                                   │
//  │     File size scales as O(click_count), NOT O(run_duration).          │
//  │     A 2-minute level with 100 clicks ≈ 828 bytes (header + body).    │
//  │     A 60-fps frame-based format for the same run ≈ 7.2 MB.            │
//  │                                                                       │
//  │  4. FAST LOAD / SAVE                                                  │
//  │     Fixed-size records → trivial memcpy serialization, no parsing.    │
//  │                                                                       │
//  │  5. CBF / CBS COMPATIBILITY                                           │
//  │     Syzzi CBF allows inputs to register BETWEEN rendered frames.      │
//  │     The X position at input time is still well-defined regardless     │
//  │     of where in the render cycle the input was processed — we store   │
//  │     the exact X, preserving full CBF precision without quantization.  │
//  │     CBS (RobTop) caps effective input rate at ~480 FPS, but X-based   │
//  │     recording still captures whatever X the engine settles on.        │
//  │                                                                       │
//  │  6. HIGH CPS RECORDINGS                                              │
//  │     Multiple inputs at the same X (rare, but possible under CBF)      │
//  │     are stored as multiple records; ordering is preserved by index.   │
//  │                                                                       │
//  │  TRADE-OFF                                                            │
//  │     Y position is not stored. In practice this is fine because Y is   │
//  │     a deterministic function of (level + X-input sequence) for every  │
//  │     vanilla GD gamemode. Physics-state-based recording would also     │
//  │     work, but X-position is strictly smaller on disk and equally      │
//  │     deterministic. We pick X-position.                                │
//  │                                                                       │
//  │  FILE LAYOUT                                                          │
//  │     [MacroHeader : 28 bytes]                                          │
//  │     [InputRecord * inputCount]                                        │
//  │                                                                       │
//  └──────────────────────────────────────────────────────────────────────┘
// ============================================================================

enum class InputAction : uint8_t {
    Press   = 0,
    Release = 1,
};

enum class PlayerSel : uint8_t {
    Player1 = 0,
    Player2 = 1,
};

#pragma pack(push, 1)
struct InputRecord {
    float   x;         // Player X (world space) at the moment of the input
    uint8_t action;    // InputAction
    uint8_t player;    // PlayerSel
    uint16_t reserved; // Alignment to 8 bytes; reserved for future flags

    InputRecord() : x(0.f), action(0), player(0), reserved(0) {}
    InputRecord(float x_, InputAction a, PlayerSel p)
        : x(x_), action(static_cast<uint8_t>(a)),
          player(static_cast<uint8_t>(p)), reserved(0) {}
};
#pragma pack(pop)
static_assert(sizeof(InputRecord) == 8, "InputRecord must be 8 bytes");

#pragma pack(push, 1)
struct MacroHeader {
    char     magic[8];    // "GDBOTv1\0"
    uint32_t version;     // format version
    int32_t  levelId;     // for sanity check on load
    float    recFps;      // recording FPS — REFERENCE ONLY, not used for playback
    uint8_t  cbMode;      // CBImplementation recorded under
    uint8_t  pad[3];      // alignment
    uint32_t inputCount;
};
#pragma pack(pop)
static_assert(sizeof(MacroHeader) == 28, "MacroHeader must be 28 bytes");

// ============================================================================
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │       PRACTICE BUG FIX — CHECKPOINT SNAPSHOT                         │
//  ├──────────────────────────────────────────────────────────────────────┤
//  │                                                                       │
//  │  GD's built-in CheckpointObject captures most state, but several     │
//  │  fields are known to be missing or incorrectly restored — this is    │
//  │  the classic "practice bug". We maintain a parallel snapshot stack   │
//  │  keyed by checkpoint order and restore our snapshot on load.         │
//  │                                                                       │
//  │  Universal state captured:                                            │
//  │    - Position, rotation, X/Y velocity                                 │
//  │    - Gravity direction (flipped / not)                                │
//  │    - Player size (big / small)                                        │
//  │    - Vehicle size (gamemode: cube/ship/ball/ufo/wave/robot/spider/   │
//  │      swing)                                                           │
//  │    - Dual mode (per-player + global)                                  │
//  │    - Current speed-portal multiplier                                  │
//  │                                                                       │
//  │  Gamemode-specific state captured (best-effort from public Geode     │
//  │  bindings; private fields that aren't exposed are re-derived from     │
//  │  position/velocity on restore):                                       │
//  │    Cube  : (universal covers it)                                      │
//  │    Ship  : isHolding, vertical velocity                               │
//  │    Ball  : isOnGround, rotation direction                             │
//  │    UFO   : isHolding                                                  │
//  │    Wave  : isHolding, trail direction                                 │
//  │    Robot : isDashing, robotJumpCount, isOnGround                      │
//  │    Spider: isOnGround (snap target re-derived)                        │
//  │    Swing : isHolding                                                  │
//  │                                                                       │
//  │  Note: Some private PlayerObject fields are not in Geode's bindings   │
//  │  for 2.2081. For those, we restore via the closest public API or     │
//  │  leave them to GD's own CheckpointObject (which handles them          │
//  │  correctly for the in-scope cases). The combination of our snapshot   │
//  │  and GD's snapshot covers everything needed for deterministic        │
//  │  macro playback after a checkpoint load.                              │
//  │                                                                       │
//  └──────────────────────────────────────────────────────────────────────┘
// ============================================================================

struct PlayerSnap {
    // --- Universal ---
    CCPoint position;
    float   rotation;
    float   yVel;
    float   xVel;
    bool    gravityFlipped;
    float   playerSize;
    int     vehicleSize;   // 0=cube 1=ship 2=ball 3=ufo 4=wave 5=robot 6=spider 7=swing

    // --- Gamemode-specific (best-effort) ---
    bool    isHolding;
    bool    isOnGround;
    bool    isDashing;
    int     robotJumpCount;
    bool    waveTrailVisible;

    PlayerSnap()
        : position(CCPointZero), rotation(0.f), yVel(0.f), xVel(0.f),
          gravityFlipped(false), playerSize(1.f), vehicleSize(0),
          isHolding(false), isOnGround(false), isDashing(false),
          robotJumpCount(0), waveTrailVisible(false) {}
};

struct CheckpointSnap {
    float      gameX;          // PlayLayer world X at checkpoint
    float      levelTime;
    float      currentSpeed;   // active speed-portal multiplier
    bool       isDualMode;
    PlayerSnap p1;
    PlayerSnap p2;
};

// ============================================================================
//  CB Implementation Detection
// ============================================================================
/*
 *  CBF (Syzzi's Click Between Frames mod)
 *  ---------------------------------------
 *  Detection: Loader::get()->isModLoaded("syzzi.click_between_frames").
 *
 *  Syzzi's CBF exposes its internal API via Geode events / setting hooks,
 *  but the public, stable integration surface for third-party bots is
 *  limited to: "the mod is loaded → inputs register between frames".
 *  We do NOT call into CBF internals; we rely on the side effect that
 *  pushButton/releaseButton are invoked at sub-frame X positions, and
 *  we capture those X values exactly. This is the most reliable and
 *  forward-compatible integration point — it survives CBF version
 *  bumps that change internal class names.
 *
 *  CBS (RobTop's built-in Click Between Steps)
 *  --------------------------------------------
 *  Detection: We check GameManager for the CBS toggle. The exact setting
 *  variable name in 2.2081 is "0016" (clickBetweenSteps) per community
 *  references; if not found we fall back to "None". This is a best-effort
 *  detection — see detectCB() implementation in main.cpp.
 *
 *  Hierarchy: CBF > CBS > None. Playback is disabled in the "None" state
 *  because frame-based input resolution is not deterministic enough for
 *  reliable macro playback under varying FPS.
 */
enum class CBImplementation : uint8_t {
    None = 0,   // RED   — playback disabled
    CBS  = 1,   // YELLOW — RobTop Click Between Steps
    CBF  = 2,   // GREEN — Syzzi Click Between Frames (preferred)
};

// ============================================================================
//  Bot Modes
// ============================================================================
enum class BotMode : uint8_t {
    Idle      = 0,
    Recording = 1,
    Playback  = 2,
};

// ============================================================================
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │                      BOT SINGLETON                                   │
//  ├──────────────────────────────────────────────────────────────────────┤
//  │                                                                       │
//  │  One active session per PlayLayer. All hooks in main.cpp delegate    │
//  │  here.                                                                │
//  │                                                                       │
//  │  MEMORY POLICY                                                        │
//  │    - m_inputs: one contiguous std::vector<InputRecord>. We reserve() │
//  │      on recording start to avoid per-input allocation.                │
//  │    - m_checkpointSnaps: parallel vector, same policy.                 │
//  │    - No per-frame allocations during gameplay.                        │
//  │                                                                       │
//  │  THREADING                                                            │
//  │    All access is on the GD main thread (Cocos2d update cycle). No    │
//  │    locking needed.                                                    │
//  │                                                                       │
//  └──────────────────────────────────────────────────────────────────────┘
// ============================================================================
class Bot {
public:
    static Bot& get() {
        static Bot instance;
        return instance;
    }

    // ---------- Mode control ----------
    void startRecording();
    void stopRecording();
    void startPlayback();
    void stopPlayback();
    void toggleRecording();
    void togglePlayback();

    // ---------- Input events (called from PlayerObject hooks) ----------
    void onInputPress(bool player1);
    void onInputRelease(bool player1);

    // ---------- PlayLayer lifecycle ----------
    void onPlayLayerEnter(PlayLayer* pl);
    void onPlayLayerExit();
    void onPlayLayerUpdate(float dt);
    void onPlayerDeath();
    void onLevelReset(bool isCheckpointLoad);
    void onCheckpointCreate();
    void onCheckpointLoad();
    void onCheckpointRemove();

    // ---------- Macro file I/O ----------
    bool saveMacro(const std::string& path);
    bool loadMacro(const std::string& path);
    std::string defaultMacroPath() const;

    // ---------- Speedhack ----------
    void   setSpeedhack(float speed);
    float  getSpeedhack() const { return m_speedhack; }

    // ---------- CB detection ----------
    CBImplementation detectCB() const;
    static const char* cbLabel(CBImplementation cb);
    static ccColor3B   cbColor(CBImplementation cb);

    // ---------- Getters ----------
    BotMode  getMode() const { return m_mode; }
    bool     isRecording() const { return m_mode == BotMode::Recording; }
    bool     isPlayback()  const { return m_mode == BotMode::Playback;  }
    bool     isPlaybackEnabled() const;        // false if CB == None
    size_t   getInputCount() const { return m_inputs.size(); }
    size_t   getPlaybackIndex() const { return m_playbackIndex; }
    CBImplementation getRecordingCB() const { return m_recordingCB; }

    // ---------- Snapshot helpers ----------
    CheckpointSnap captureSnapshot() const;
    void           restoreSnapshot(const CheckpointSnap& snap);

private:
    Bot();

    BotMode  m_mode = BotMode::Idle;

    // ----- Recording: input list, sorted by X by construction -----
    std::vector<InputRecord> m_inputs;
    // Copy of m_inputs used during playback (so recording state isn't mutated)
    std::vector<InputRecord> m_playbackInputs;

    // ----- Per-player hold tracking (avoids duplicate release records) -----
    bool m_p1Holding = false;
    bool m_p2Holding = false;

    // ----- Dead-input cleanup support -----
    // For each checkpoint we store (X at checkpoint, input index at checkpoint).
    // On restart-to-checkpoint we truncate inputs to that index.
    // On full restart we clear everything.
    struct CheckpointMark {
        float  x;
        size_t inputIndex;
    };
    std::vector<CheckpointMark> m_checkpointMarks;

    // Parallel snapshot stack for practice bug fix
    std::vector<CheckpointSnap> m_checkpointSnaps;

    // ----- Playback state -----
    size_t            m_playbackIndex = 0;
    CBImplementation  m_recordingCB   = CBImplementation::None;

    // ----- Speedhack -----
    float m_speedhack = 1.f;

    // ----- CB detection cache (recomputed on level enter) -----
    mutable CBImplementation m_cachedCB   = CBImplementation::None;
    mutable bool             m_cbCacheValid = false;

    // ----- Internal helpers -----
    void pushInput(float x, InputAction a, PlayerSel p);
    void truncateInputsAfterX(float x);
    void truncateInputsAfterIndex(size_t idx);
    void resetPlaybackState();
    void clearCheckpointState();
    void applySpeedhackToScheduler();
};

// ============================================================================
//  Inline helpers (small enough to keep in header)
// ============================================================================

inline const char* Bot::cbLabel(CBImplementation cb) {
    switch (cb) {
        case CBImplementation::CBF:  return "CBF Active (Syzzi)";
        case CBImplementation::CBS:  return "CBS Active (RobTop)";
        case CBImplementation::None: return "No CBF / CBS";
    }
    return "Unknown";
}

inline ccColor3B Bot::cbColor(CBImplementation cb) {
    switch (cb) {
        case CBImplementation::CBF:  return ccc3(0x40, 0xE0, 0x40); // green
        case CBImplementation::CBS:  return ccc3(0xFF, 0xD0, 0x00); // yellow
        case CBImplementation::None: return ccc3(0xE0, 0x30, 0x30); // red
    }
    return ccc3(0xFF, 0xFF, 0xFF);
}

} // namespace macrobot
