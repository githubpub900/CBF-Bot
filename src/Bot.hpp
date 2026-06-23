#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>

#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <optional>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// CBF detection helpers
// ---------------------------------------------------------------------------

namespace CBFStatus {
    enum class Mode {
        None,       // No CBF at all — bot is LOCKED (red dot)
        RobTop,     // GD's built-in Click Between Steps (yellow dot)
        Syzzi       // syzzi.click_between_frames (green dot)
    };

    inline Mode detect() {
        // Check Syzzi's mod first (highest priority)
        if (auto* m = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
            // Honour the "soft-toggle" (Disable CBF) setting
            bool softOff = m->getSettingValue<bool>("soft-toggle");
            if (!softOff) return Mode::Syzzi;
        }
        // Check RobTop's built-in Click Between Steps
        // GameManager::getGameVariable("0218") is the CBS toggle
        auto* gm = GameManager::get();
        if (gm && gm->getGameVariable("0218")) return Mode::RobTop;
        return Mode::None;
    }

    inline cocos2d::ccColor3B dotColor(Mode m) {
        switch (m) {
            case Mode::Syzzi:  return {0, 220, 0};
            case Mode::RobTop: return {230, 200, 0};
            default:           return {220, 0, 0};
        }
    }
    inline const char* dotLabel(Mode m) {
        switch (m) {
            case Mode::Syzzi:  return "CBF: Syzzi (unlimited precision)";
            case Mode::RobTop: return "CBF: RobTop CBS (480 TPS)";
            default:           return "No CBF — bot disabled";
        }
    }
}

// ---------------------------------------------------------------------------
// Input record (time-based, very compact)
// ---------------------------------------------------------------------------
// We store time as a 50-decimal precision double, but internally we use
// long double during accumulation and round to double on write (gives ~15-17
// significant digits which is more than enough for any realistic level length).
//
// Format on disk (binary, little-endian):
//   [uint8]  player  — 0 = player 1, 1 = player 2
//   [uint8]  btn     — 0 = jump, 1 = left, 2 = right
//   [uint8]  down    — 1 = press, 0 = release
//   [double] time    — seconds into the level (from recording start offset)
// Total: 11 bytes per input — extremely compact.
// ---------------------------------------------------------------------------

enum class BotButton : uint8_t { Jump = 0, Left = 1, Right = 2 };

struct BotInput {
    double   time    = 0.0;   // seconds since level time 0.0
    uint8_t  player  = 0;     // 0 or 1
    BotButton btn    = BotButton::Jump;
    bool     down    = true;

    bool operator<(const BotInput& o) const { return time < o.time; }
};

// ---------------------------------------------------------------------------
// Full player state snapshot (for practice mode checkpoints)
// ---------------------------------------------------------------------------
struct PlayerState {
    // Position / motion
    cocos2d::CCPoint pos           = {};
    double           yVelocity     = 0.0;
    double           yVelocityBS   = 0.0;  // yVelocityBeforeSlope
    double           gravity       = 0.0;
    double           speedMult     = 0.0;
    double           totalTime     = 0.0;
    double           fallSpeed     = 0.0;

    // Flags — game-modes (one-hot)
    bool isShip    = false;
    bool isBird    = false;
    bool isBall    = false;
    bool isDart    = false;
    bool isRobot   = false;
    bool isSpider  = false;
    bool isSwing   = false;

    // Orientation / misc
    bool isUpsideDown   = false;
    bool isOnGround     = false;
    bool isOnGround2    = false;
    bool isSideways     = false;
    bool isGoingLeft    = false;
    bool isDashing      = false;
    bool isHidden       = false;
    bool isLocked       = false;
    bool controlsDisab  = false;

    // Rotation
    float  rotation        = 0.f;
    double slopeRotation   = 0.f;

    // Boost states
    int stateBoostX     = 0;
    int stateBoostY     = 0;
    int stateOnGround   = 0;
    int stateFlipGrav   = 0;
    int stateDartSlide  = 0;
    int stateHitHead    = 0;
    int stateNoAutoJump = 0;
    int stateForce      = 0;

    float gravMod       = 1.f;
    float playerSpeed   = 0.f;
    float vehicleSize   = 1.f;

    // Jump / ring tracking
    bool jumpBuffered    = false;
    bool touchedGravPort = false;
    bool touchedRing     = false;
    bool stateRingJump   = false;
    bool stateRingJump2  = false;

    // Platformer
    double platformerXVel = 0.0;
    bool   holdingLeft    = false;
    bool   holdingRight   = false;
    bool   isPlatformer   = false;

    float  fallStartY     = 0.f;
    bool   isOutOfBounds  = false;
};

inline PlayerState capturePlayerState(PlayerObject* p) {
    if (!p) return {};
    PlayerState s;
    s.pos           = p->getPosition();
    s.yVelocity     = p->m_yVelocity;
    s.yVelocityBS   = p->m_yVelocityBeforeSlope;
    s.gravity       = p->m_gravity;
    s.speedMult     = p->m_speedMultiplier;
    s.totalTime     = p->m_totalTime;
    s.fallSpeed     = p->m_fallSpeed;
    s.isShip        = p->m_isShip;
    s.isBird        = p->m_isBird;
    s.isBall        = p->m_isBall;
    s.isDart        = p->m_isDart;
    s.isRobot       = p->m_isRobot;
    s.isSpider      = p->m_isSpider;
    s.isSwing       = p->m_isSwing;
    s.isUpsideDown  = p->m_isUpsideDown;
    s.isOnGround    = p->m_isOnGround;
    s.isOnGround2   = p->m_isOnGround2;
    s.isSideways    = p->m_isSideways;
    s.isGoingLeft   = p->m_isGoingLeft;
    s.isDashing     = p->m_isDashing;
    s.isHidden      = p->m_isHidden;
    s.isLocked      = p->m_isLocked;
    s.controlsDisab = p->m_controlsDisabled;
    s.rotation      = p->getRotation();
    s.slopeRotation = p->m_slopeRotation;
    s.stateBoostX   = p->m_stateBoostX;
    s.stateBoostY   = p->m_stateBoostY;
    s.stateOnGround = p->m_stateOnGround;
    s.stateFlipGrav = p->m_stateFlipGravity;
    s.stateDartSlide= p->m_stateDartSlide;
    s.stateHitHead  = p->m_stateHitHead;
    s.stateNoAutoJump= p->m_stateNoAutoJump;
    s.stateForce    = p->m_stateForce;
    s.gravMod       = p->m_gravityMod;
    s.playerSpeed   = p->m_playerSpeed;
    s.vehicleSize   = p->m_vehicleSize;
    s.jumpBuffered  = p->m_jumpBuffered;
    s.touchedGravPort= p->m_touchedGravityPortal;
    s.touchedRing   = p->m_touchedRing;
    s.stateRingJump = p->m_stateRingJump;
    s.stateRingJump2= p->m_stateRingJump2;
    s.platformerXVel= p->m_platformerXVelocity;
    s.holdingLeft   = p->m_holdingLeft;
    s.holdingRight  = p->m_holdingRight;
    s.isPlatformer  = p->m_isPlatformer;
    s.fallStartY    = p->m_fallStartY;
    s.isOutOfBounds = p->m_isOutOfBounds;
    return s;
}

inline void applyPlayerState(PlayerObject* p, const PlayerState& s) {
    if (!p) return;
    p->setPosition(s.pos);
    p->m_yVelocity           = s.yVelocity;
    p->m_yVelocityBeforeSlope= s.yVelocityBS;
    p->m_gravity             = s.gravity;
    p->m_speedMultiplier     = s.speedMult;
    p->m_totalTime           = s.totalTime;
    p->m_fallSpeed           = s.fallSpeed;
    p->m_isShip              = s.isShip;
    p->m_isBird              = s.isBird;
    p->m_isBall              = s.isBall;
    p->m_isDart              = s.isDart;
    p->m_isRobot             = s.isRobot;
    p->m_isSpider            = s.isSpider;
    p->m_isSwing             = s.isSwing;
    p->m_isUpsideDown        = s.isUpsideDown;
    p->m_isOnGround          = s.isOnGround;
    p->m_isOnGround2         = s.isOnGround2;
    p->m_isSideways          = s.isSideways;
    p->m_isGoingLeft         = s.isGoingLeft;
    p->m_isDashing           = s.isDashing;
    p->m_isHidden            = s.isHidden;
    p->m_isLocked            = s.isLocked;
    p->m_controlsDisabled    = s.controlsDisab;
    p->setRotation(s.rotation);
    p->m_slopeRotation       = s.slopeRotation;
    p->m_stateBoostX         = s.stateBoostX;
    p->m_stateBoostY         = s.stateBoostY;
    p->m_stateOnGround       = s.stateOnGround;
    p->m_stateFlipGravity    = s.stateFlipGrav;
    p->m_stateDartSlide      = s.stateDartSlide;
    p->m_stateHitHead        = s.stateHitHead;
    p->m_stateNoAutoJump     = s.stateNoAutoJump;
    p->m_stateForce          = s.stateForce;
    p->m_gravityMod          = s.gravMod;
    p->m_playerSpeed         = s.playerSpeed;
    p->m_vehicleSize         = s.vehicleSize;
    p->m_jumpBuffered        = s.jumpBuffered;
    p->m_touchedGravityPortal= s.touchedGravPort;
    p->m_touchedRing         = s.touchedRing;
    p->m_stateRingJump       = s.stateRingJump;
    p->m_stateRingJump2      = s.stateRingJump2;
    p->m_platformerXVelocity = s.platformerXVel;
    p->m_holdingLeft         = s.holdingLeft;
    p->m_holdingRight        = s.holdingRight;
    p->m_isPlatformer        = s.isPlatformer;
    p->m_fallStartY          = s.fallStartY;
    p->m_isOutOfBounds       = s.isOutOfBounds;
}

// ---------------------------------------------------------------------------
// Macro (collection of inputs + metadata)
// ---------------------------------------------------------------------------
struct Macro {
    std::vector<BotInput> inputs;
    // Offset: the level-time at which recording was started.
    // All stored input times are ABSOLUTE (offset has already been subtracted
    // so that time 0 = start of level). We store the original offset purely
    // for informational purposes.
    double recordingStartOffset = 0.0;

    void clear() {
        inputs.clear();
        recordingStartOffset = 0.0;
    }

    // Sort by time (they should already be ordered, but keep it safe)
    void sort() { std::sort(inputs.begin(), inputs.end()); }

    // Remove all inputs at or after `timeSeconds` (used when restarting or
    // dying during recording to discard dead inputs)
    void truncateFrom(double timeSeconds) {
        auto it = std::lower_bound(inputs.begin(), inputs.end(), BotInput{timeSeconds});
        inputs.erase(it, inputs.end());
    }

    // Add an input; called during recording.
    // `levelTime` is the current PlayLayer time at which the input occurred.
    // `startOffset` is stored in recordingStartOffset.
    void addInput(double levelTime, uint8_t player, BotButton btn, bool down) {
        inputs.push_back({levelTime, player, btn, down});
    }

    // -----------------------------------------------------------------------
    // Binary serialisation — compact 11-byte-per-input format
    // -----------------------------------------------------------------------
    bool save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        // Header: magic, version, offset, count
        const char magic[4] = {'B','M','A','C'};
        f.write(magic, 4);
        uint8_t ver = 1;
        f.write(reinterpret_cast<const char*>(&ver), 1);
        double off = recordingStartOffset;
        f.write(reinterpret_cast<const char*>(&off), sizeof(double));
        uint32_t cnt = static_cast<uint32_t>(inputs.size());
        f.write(reinterpret_cast<const char*>(&cnt), sizeof(uint32_t));
        for (const auto& inp : inputs) {
            f.write(reinterpret_cast<const char*>(&inp.player), 1);
            uint8_t b = static_cast<uint8_t>(inp.btn);
            f.write(reinterpret_cast<const char*>(&b), 1);
            uint8_t d = inp.down ? 1 : 0;
            f.write(reinterpret_cast<const char*>(&d), 1);
            f.write(reinterpret_cast<const char*>(&inp.time), sizeof(double));
        }
        return f.good();
    }

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        char magic[4];
        f.read(magic, 4);
        if (std::strncmp(magic, "BMAC", 4) != 0) return false;
        uint8_t ver;
        f.read(reinterpret_cast<char*>(&ver), 1);
        if (ver != 1) return false;
        f.read(reinterpret_cast<char*>(&recordingStartOffset), sizeof(double));
        uint32_t cnt;
        f.read(reinterpret_cast<char*>(&cnt), sizeof(uint32_t));
        inputs.resize(cnt);
        for (auto& inp : inputs) {
            f.read(reinterpret_cast<char*>(&inp.player), 1);
            uint8_t b; f.read(reinterpret_cast<char*>(&b), 1);
            inp.btn = static_cast<BotButton>(b);
            uint8_t d; f.read(reinterpret_cast<char*>(&d), 1);
            inp.down = d != 0;
            f.read(reinterpret_cast<char*>(&inp.time), sizeof(double));
        }
        return f.good();
    }
};

// ---------------------------------------------------------------------------
// Bot singleton state machine
// ---------------------------------------------------------------------------
enum class BotMode { Idle, Recording, Playing };

class Bot {
public:
    static Bot& get() {
        static Bot instance;
        return instance;
    }

    BotMode mode = BotMode::Idle;
    Macro   macro;

    // During recording — current playback index (unused in recording mode
    // but kept for clarity)
    size_t  playbackIdx = 0;

    // Cached CBF mode (refreshed when the level loads)
    CBFStatus::Mode cbfMode = CBFStatus::Mode::None;

    // True while the player is "dead" in practice (between death and
    // checkpoint restore / full restart). Inputs during this window are dead.
    bool    p1Dead = false;
    bool    p2Dead = false;

    // The level time at which the LAST death occurred; we truncate inputs
    // up to this point on the next restart/checkpoint.
    double  deathTime1 = -1.0;
    double  deathTime2 = -1.0;

    // Last known level time — updated every physics step.
    double  currentLevelTime = 0.0;

    // Speedhack multiplier (1.0 = normal).
    float   speedhack = 1.0f;

    // Whether the GUI is open.
    bool    guiOpen = false;

    // Refresh CBF status
    void refreshCBF() { cbfMode = CBFStatus::detect(); }

    bool canOperate() const { return cbfMode != CBFStatus::Mode::None; }

    void startRecording(double levelTime) {
        if (!canOperate()) return;
        macro.clear();
        macro.recordingStartOffset = levelTime;
        mode = BotMode::Recording;
        p1Dead = false; p2Dead = false;
        deathTime1 = -1.0; deathTime2 = -1.0;
    }

    void stopRecording() {
        if (mode == BotMode::Recording) {
            macro.sort();
            mode = BotMode::Idle;
        }
    }

    void startPlayback() {
        if (!canOperate() || macro.inputs.empty()) return;
        playbackIdx = 0;
        mode = BotMode::Playing;
    }

    void stopPlayback() {
        if (mode == BotMode::Playing) mode = BotMode::Idle;
    }

    void reset() {
        mode = BotMode::Idle;
        playbackIdx = 0;
        p1Dead = false; p2Dead = false;
        deathTime1 = -1.0; deathTime2 = -1.0;
        currentLevelTime = 0.0;
    }

    // Called when the player dies during recording. Marks a dead-input window.
    void onDeath(int player, double levelTime) {
        if (mode != BotMode::Recording) return;
        if (player == 0) { p1Dead = true; deathTime1 = levelTime; }
        else             { p2Dead = true; deathTime2 = levelTime; }
    }

    // Called on checkpoint restore or level restart during recording.
    // Discards all inputs recorded after `levelTime`.
    void onRestart(double levelTime) {
        if (mode != BotMode::Recording) return;
        macro.truncateFrom(levelTime);
        // Reset death state
        p1Dead = false; p2Dead = false;
        deathTime1 = -1.0; deathTime2 = -1.0;
    }

    // Called when an input is registered by the game (CBF or regular).
    // `levelTime` is the ABSOLUTE level time at which the input happened.
    void recordInput(double levelTime, int player, BotButton btn, bool down) {
        if (mode != BotMode::Recording) return;
        // Drop dead inputs
        if (player == 0 && p1Dead) return;
        if (player == 1 && p2Dead) return;
        macro.addInput(levelTime, static_cast<uint8_t>(player), btn, down);
    }

    // Process playback: fire any inputs whose time has arrived.
    // `pl`         — PlayLayer pointer for handleButton calls.
    // `levelTime`  — current level time.
    void processPlayback(PlayLayer* pl, double levelTime) {
        if (mode != BotMode::Playing || !pl) return;
        while (playbackIdx < macro.inputs.size()) {
            const auto& inp = macro.inputs[playbackIdx];
            if (inp.time > levelTime) break;

            bool isP2 = inp.player == 1;
            int  btn  = static_cast<int>(inp.btn);
            pl->handleButton(inp.down, btn, !isP2);
            ++playbackIdx;
        }
    }

    // Rewind playback index to the first input at or after `levelTime`.
    void rewindPlaybackTo(double levelTime) {
        if (mode != BotMode::Playing) return;
        // Binary search
        BotInput dummy; dummy.time = levelTime;
        auto it = std::lower_bound(macro.inputs.begin(), macro.inputs.end(), dummy);
        playbackIdx = static_cast<size_t>(std::distance(macro.inputs.begin(), it));
    }

private:
    Bot() = default;
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;
};

// ---------------------------------------------------------------------------
// Speedhack helper — applied to CCScheduler time scale
// ---------------------------------------------------------------------------
inline void applySpeedhack(float mult) {
    Bot::get().speedhack = mult;
    auto* sched = cocos2d::CCDirector::get()->getScheduler();
    if (sched) sched->setTimeScale(mult);
}
