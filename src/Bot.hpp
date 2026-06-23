#pragma once

// ============================================================
// Bot.hpp — MacroBot core: data structures + singleton state
// Geode v5.7.1 / GD 2.2081
// ============================================================

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/Loader.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

// ============================================================
// CBF detection
// ============================================================
namespace CBFStatus {
    enum class Mode { None, RobTop, Syzzi };

    inline Mode detect() {
        // Highest priority: syzzi's mod
        if (auto* m = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
            // Honour the in-mod "Disable CBF" soft toggle
            bool softOff = m->template getSettingValue<bool>("soft-toggle");
            if (!softOff) return Mode::Syzzi;
        }
        // RobTop's built-in Click Between Steps — game variable 0218
        if (auto* gm = GameManager::sharedState()) {
            if (gm->getGameVariable("0218")) return Mode::RobTop;
        }
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
            case Mode::Syzzi:  return "CBF: Syzzi (unlimited TPS)";
            case Mode::RobTop: return "CBF: RobTop CBS (480 TPS)";
            default:           return "No CBF — bot locked";
        }
    }
}

// ============================================================
// Input record — 11 bytes on disk, time-keyed
// ============================================================
// Binary layout (little-endian):
//   u8     player  (0 = P1, 1 = P2)
//   u8     button  (0 = Jump, 1 = Left, 2 = Right)
//   u8     down    (1 = press, 0 = release)
//   f64    time    (absolute level time in seconds)
// Header: 4-char magic "BMAC" + u8 version + f64 offset + u32 count
// ============================================================

enum class BotButton : uint8_t { Jump = 0, Left = 1, Right = 2 };

struct BotInput {
    double    time   = 0.0;
    uint8_t   player = 0;
    BotButton btn    = BotButton::Jump;
    bool      down   = true;

    bool operator<(const BotInput& o) const { return time < o.time; }
};

// ============================================================
// Full PlayerObject state snapshot for practice-mode fix
// ============================================================
struct PlayerState {
    cocos2d::CCPoint pos            = {};
    double           yVel           = 0.0;
    double           yVelBS         = 0.0;
    double           gravity        = 0.0;
    double           speedMult      = 0.0;
    double           totalTime      = 0.0;
    double           fallSpeed      = 0.0;

    bool isShip     = false;
    bool isBird     = false;
    bool isBall     = false;
    bool isDart     = false;
    bool isRobot    = false;
    bool isSpider   = false;
    bool isSwing    = false;

    bool isUpsideDown    = false;
    bool isOnGround      = false;
    bool isOnGround2     = false;
    bool isSideways      = false;
    bool isGoingLeft     = false;
    bool isDashing       = false;
    bool isHidden        = false;
    bool isLocked        = false;
    bool controlsDisab   = false;

    float  rotation      = 0.f;
    double slopeRotation = 0.0;

    int stateBoostX    = 0;
    int stateBoostY    = 0;
    int stateOnGround  = 0;
    int stateFlipGrav  = 0;
    int stateDartSlide = 0;
    int stateHitHead   = 0;
    int stateNoAutoJmp = 0;
    int stateForce     = 0;

    float gravMod     = 1.f;
    float playerSpeed = 0.f;
    float vehicleSize = 1.f;

    bool jumpBuffered     = false;
    bool touchedGravPort  = false;
    bool touchedRing      = false;
    bool stateRingJump    = false;
    bool stateRingJump2   = false;

    double platformerXVel = 0.0;
    bool   holdingLeft    = false;
    bool   holdingRight   = false;
    bool   isPlatformer   = false;

    float fallStartY     = 0.f;
    bool  isOutOfBounds  = false;
};

inline PlayerState capturePlayer(PlayerObject* p) {
    PlayerState s;
    if (!p) return s;
    s.pos            = p->getPosition();
    s.yVel           = p->m_yVelocity;
    s.yVelBS         = p->m_yVelocityBeforeSlope;
    s.gravity        = p->m_gravity;
    s.speedMult      = p->m_speedMultiplier;
    s.totalTime      = p->m_totalTime;
    s.fallSpeed      = p->m_fallSpeed;
    s.isShip         = p->m_isShip;
    s.isBird         = p->m_isBird;
    s.isBall         = p->m_isBall;
    s.isDart         = p->m_isDart;
    s.isRobot        = p->m_isRobot;
    s.isSpider       = p->m_isSpider;
    s.isSwing        = p->m_isSwing;
    s.isUpsideDown   = p->m_isUpsideDown;
    s.isOnGround     = p->m_isOnGround;
    s.isOnGround2    = p->m_isOnGround2;
    s.isSideways     = p->m_isSideways;
    s.isGoingLeft    = p->m_isGoingLeft;
    s.isDashing      = p->m_isDashing;
    s.isHidden       = p->m_isHidden;
    s.isLocked       = p->m_isLocked;
    s.controlsDisab  = p->m_controlsDisabled;
    s.rotation       = p->getRotation();
    s.slopeRotation  = p->m_slopeRotation;
    s.stateBoostX    = p->m_stateBoostX;
    s.stateBoostY    = p->m_stateBoostY;
    s.stateOnGround  = p->m_stateOnGround;
    s.stateFlipGrav  = p->m_stateFlipGravity;
    s.stateDartSlide = p->m_stateDartSlide;
    s.stateHitHead   = p->m_stateHitHead;
    s.stateNoAutoJmp = p->m_stateNoAutoJump;
    s.stateForce     = p->m_stateForce;
    s.gravMod        = p->m_gravityMod;
    s.playerSpeed    = p->m_playerSpeed;
    s.vehicleSize    = p->m_vehicleSize;
    s.jumpBuffered   = p->m_jumpBuffered;
    s.touchedGravPort= p->m_touchedGravityPortal;
    s.touchedRing    = p->m_touchedRing;
    s.stateRingJump  = p->m_stateRingJump;
    s.stateRingJump2 = p->m_stateRingJump2;
    s.platformerXVel = p->m_platformerXVelocity;
    s.holdingLeft    = p->m_holdingLeft;
    s.holdingRight   = p->m_holdingRight;
    s.isPlatformer   = p->m_isPlatformer;
    s.fallStartY     = p->m_fallStartY;
    s.isOutOfBounds  = p->m_isOutOfBounds;
    return s;
}

inline void applyPlayer(PlayerObject* p, const PlayerState& s) {
    if (!p) return;
    p->setPosition(s.pos);
    p->m_yVelocity            = s.yVel;
    p->m_yVelocityBeforeSlope = s.yVelBS;
    p->m_gravity              = s.gravity;
    p->m_speedMultiplier      = s.speedMult;
    p->m_totalTime            = s.totalTime;
    p->m_fallSpeed            = s.fallSpeed;
    p->m_isShip               = s.isShip;
    p->m_isBird               = s.isBird;
    p->m_isBall               = s.isBall;
    p->m_isDart               = s.isDart;
    p->m_isRobot              = s.isRobot;
    p->m_isSpider             = s.isSpider;
    p->m_isSwing              = s.isSwing;
    p->m_isUpsideDown         = s.isUpsideDown;
    p->m_isOnGround           = s.isOnGround;
    p->m_isOnGround2          = s.isOnGround2;
    p->m_isSideways           = s.isSideways;
    p->m_isGoingLeft          = s.isGoingLeft;
    p->m_isDashing            = s.isDashing;
    p->m_isHidden             = s.isHidden;
    p->m_isLocked             = s.isLocked;
    p->m_controlsDisabled     = s.controlsDisab;
    p->setRotation(s.rotation);
    p->m_slopeRotation        = s.slopeRotation;
    p->m_stateBoostX          = s.stateBoostX;
    p->m_stateBoostY          = s.stateBoostY;
    p->m_stateOnGround        = s.stateOnGround;
    p->m_stateFlipGravity     = s.stateFlipGrav;
    p->m_stateDartSlide       = s.stateDartSlide;
    p->m_stateHitHead         = s.stateHitHead;
    p->m_stateNoAutoJump      = s.stateNoAutoJmp;
    p->m_stateForce           = s.stateForce;
    p->m_gravityMod           = s.gravMod;
    p->m_playerSpeed          = s.playerSpeed;
    p->m_vehicleSize          = s.vehicleSize;
    p->m_jumpBuffered         = s.jumpBuffered;
    p->m_touchedGravityPortal = s.touchedGravPort;
    p->m_touchedRing          = s.touchedRing;
    p->m_stateRingJump        = s.stateRingJump;
    p->m_stateRingJump2       = s.stateRingJump2;
    p->m_platformerXVelocity  = s.platformerXVel;
    p->m_holdingLeft          = s.holdingLeft;
    p->m_holdingRight         = s.holdingRight;
    p->m_isPlatformer         = s.isPlatformer;
    p->m_fallStartY           = s.fallStartY;
    p->m_isOutOfBounds        = s.isOutOfBounds;
}

// ============================================================
// Per-checkpoint extra data (parallel to m_checkpointArray)
// ============================================================
struct CheckpointExtra {
    PlayerState p1, p2;
    double      levelTime = 0.0;
};

// ============================================================
// Macro container
// ============================================================
struct Macro {
    std::vector<BotInput> inputs;
    double recordingStartOffset = 0.0; // informational only

    void clear() { inputs.clear(); recordingStartOffset = 0.0; }

    void sortInputs() { std::sort(inputs.begin(), inputs.end()); }

    // Erase all inputs at or after `t`
    void truncateFrom(double t) {
        BotInput dummy; dummy.time = t;
        auto it = std::lower_bound(inputs.begin(), inputs.end(), dummy);
        inputs.erase(it, inputs.end());
    }

    void addInput(double levelTime, uint8_t player, BotButton btn, bool down) {
        inputs.push_back({levelTime, player, btn, down});
    }

    // ---- Binary I/O ----
    bool save(const std::string& path) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;

        const char magic[4] = {'B','M','A','C'};
        f.write(magic, 4);
        const uint8_t ver = 1;
        f.write(reinterpret_cast<const char*>(&ver), 1);
        const double off = recordingStartOffset;
        f.write(reinterpret_cast<const char*>(&off), 8);
        const uint32_t cnt = static_cast<uint32_t>(inputs.size());
        f.write(reinterpret_cast<const char*>(&cnt), 4);

        for (const auto& inp : inputs) {
            f.write(reinterpret_cast<const char*>(&inp.player), 1);
            const uint8_t b = static_cast<uint8_t>(inp.btn);
            f.write(reinterpret_cast<const char*>(&b), 1);
            const uint8_t d = inp.down ? 1u : 0u;
            f.write(reinterpret_cast<const char*>(&d), 1);
            f.write(reinterpret_cast<const char*>(&inp.time), 8);
        }
        return f.good();
    }

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        char magic[4];
        f.read(magic, 4);
        if (std::memcmp(magic, "BMAC", 4) != 0) return false;

        uint8_t ver;
        f.read(reinterpret_cast<char*>(&ver), 1);
        if (ver != 1) return false;

        f.read(reinterpret_cast<char*>(&recordingStartOffset), 8);

        uint32_t cnt;
        f.read(reinterpret_cast<char*>(&cnt), 4);

        inputs.resize(cnt);
        for (auto& inp : inputs) {
            f.read(reinterpret_cast<char*>(&inp.player), 1);
            uint8_t b; f.read(reinterpret_cast<char*>(&b), 1);
            inp.btn = static_cast<BotButton>(b);
            uint8_t d; f.read(reinterpret_cast<char*>(&d), 1);
            inp.down = (d != 0);
            f.read(reinterpret_cast<char*>(&inp.time), 8);
        }
        return f.good();
    }
};

// ============================================================
// Bot singleton
// ============================================================
enum class BotMode { Idle, Recording, Playing };

class Bot {
public:
    static Bot& get() {
        static Bot s_instance;
        return s_instance;
    }

    // --- State ---
    BotMode mode = BotMode::Idle;
    Macro   macro;
    size_t  playIdx = 0;

    CBFStatus::Mode cbfMode = CBFStatus::Mode::None;

    // Dead-input tracking
    bool   p1Dead      = false;
    bool   p2Dead      = false;
    double deathTime1  = -1.0;
    double deathTime2  = -1.0;

    // Current level time (written by PlayLayer::update hook)
    double currentTime = 0.0;

    // Speedhack multiplier
    float  speedhack   = 1.0f;

    // --- Operations ---
    void refreshCBF() { cbfMode = CBFStatus::detect(); }
    bool canRun() const { return cbfMode != CBFStatus::Mode::None; }

    void startRecording(double levelTime) {
        if (!canRun()) return;
        macro.clear();
        macro.recordingStartOffset = levelTime;
        p1Dead = false; p2Dead = false;
        deathTime1 = -1.0; deathTime2 = -1.0;
        mode = BotMode::Recording;
    }

    void stopRecording() {
        if (mode == BotMode::Recording) {
            macro.sortInputs();
            mode = BotMode::Idle;
        }
    }

    void startPlayback() {
        if (!canRun() || macro.inputs.empty()) return;
        playIdx = 0;
        mode = BotMode::Playing;
    }

    void stopPlayback() {
        if (mode == BotMode::Playing) mode = BotMode::Idle;
    }

    void fullReset() {
        mode = BotMode::Idle;
        playIdx = 0;
        p1Dead = false; p2Dead = false;
        deathTime1 = -1.0; deathTime2 = -1.0;
        currentTime = 0.0;
    }

    // Called when a player dies during recording
    void onDeath(int playerIdx, double t) {
        if (mode != BotMode::Recording) return;
        if (playerIdx == 0) { p1Dead = true; deathTime1 = t; }
        else                { p2Dead = true; deathTime2 = t; }
    }

    // Called on checkpoint restore or restart — trims dead inputs
    void onRevert(double levelTime) {
        if (mode == BotMode::Recording) {
            macro.truncateFrom(levelTime);
            p1Dead = false; p2Dead = false;
            deathTime1 = -1.0; deathTime2 = -1.0;
        } else if (mode == BotMode::Playing) {
            rewindTo(levelTime);
        }
    }

    // Record a single input during recording mode
    void recordInput(double levelTime, int playerIdx, BotButton btn, bool down) {
        if (mode != BotMode::Recording) return;
        if (playerIdx == 0 && p1Dead) return;
        if (playerIdx == 1 && p2Dead) return;
        macro.addInput(levelTime, static_cast<uint8_t>(playerIdx), btn, down);
    }

    // Fire any pending playback inputs up to `levelTime`
    void tickPlayback(PlayLayer* pl, double levelTime) {
        if (mode != BotMode::Playing || !pl) return;
        while (playIdx < macro.inputs.size()) {
            const BotInput& inp = macro.inputs[playIdx];
            if (inp.time > levelTime) break;
            // handleButton(bool down, int button, bool isPlayer1)
            pl->handleButton(inp.down, static_cast<int>(inp.btn), inp.player == 0);
            ++playIdx;
        }
    }

    // Seek playback index to the first input at or after `t`
    void rewindTo(double t) {
        BotInput dummy; dummy.time = t;
        auto it = std::lower_bound(macro.inputs.begin(), macro.inputs.end(), dummy);
        playIdx = static_cast<size_t>(std::distance(macro.inputs.begin(), it));
    }

private:
    Bot() = default;
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;
};

// ============================================================
// Speedhack helper
// ============================================================
inline void applySpeedhack(float mult) {
    mult = std::clamp(mult, 0.01f, 100.f);
    Bot::get().speedhack = mult;
    if (auto* sched = cocos2d::CCDirector::sharedDirector()->getScheduler()) {
        sched->setTimeScale(static_cast<float>(mult));
    }
}