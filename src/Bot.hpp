#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <deque>
#include <string>
#include <cstdint>

using namespace geode::prelude;

namespace mb {

enum class CBFStatus : uint8_t { None = 0, RobTop = 1, Syzzi = 2 };
enum class BotState  : uint8_t { Idle = 0, Recording = 1, Playing = 2 };
enum class InputType : uint8_t { Press = 0, Release = 1 };

// ═══ Single timed input ═══════════════════════════════════
// 9 bytes in binary — extremely compact.
// 'time' is seconds-into-level with full double precision,
// which gives sub-nanosecond resolution even for hour-long levels.
struct InputAction {
    double    time    = 0.0;
    bool      isP2    = false;
    InputType type    = InputType::Press;
    uint16_t  attempt = 0;
};

// ═══ Full player physics snapshot ═════════════════════════
// Captures every state variable that vanilla practice mode
// fails to restore (velocity, gravity, gamemode flags, etc.)
struct PlayerSnap {
    float x = 0.f, y = 0.f;
    float rot = 0.f, rotSpd = 0.f;
    float speed = 0.9f, speedMult = 1.f;
    float gravity = 1.f;
    float yVel = 0.f, xVel = 0.f;
    float vehicleSize = 1.f;
    bool  gravFlip   = false;
    bool  onGround   = false;
    bool  holding    = false;
    bool  canJump    = false;
    bool  justHeld   = false;
    bool  dashing    = false;
    int   gameMode   = 0;
};

struct CheckpointSnap {
    double     gameTime = 0.0;
    PlayerSnap p1{}, p2{};
};

// ═══ Macro — stored input sequence ════════════════════════
class Macro {
public:
    std::vector<InputAction> actions;
    int    levelID     = 0;
    double startOffset = 0.0;   // gameTime of first recorded input
    double duration    = 0.0;

    void   clear()  { actions.clear(); startOffset = duration = 0; }
    bool   empty()  const { return actions.empty(); }
    size_t size()   const { return actions.size(); }

    // Binary format: "MBOT" | ver(4) | levelID(4) | offset(8) | count(8)
    //               then per-action: time(8) + flags(1) = 9 bytes each
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void truncateAfter(double t);
    void removeAttempt(uint16_t att);
};

// ═══ Practice Bug Fix ═════════════════════════════════════
// Keeps a parallel snapshot stack synced to the game's
// checkpoint array. On respawn we overwrite the game's
// incomplete restoration with our full physics state.
class PracticeFix {
public:
    std::deque<CheckpointSnap> snaps;
    size_t syncedCount = 0;

    void sync(PlayLayer* layer, double gt);   // call every frame
    void apply(PlayLayer* layer);              // call after resetLevel
    void reset() { snaps.clear(); syncedCount = 0; }
private:
    PlayerSnap capture(PlayerObject* p);
    void       restore(PlayerObject* p, const PlayerSnap& s);
};

// ═══ Speedhack ════════════════════════════════════════════
// Multiplies dt instead of changing frame-rate, so the bot's
// time-based tracking stays accurate regardless of speed.
class Speedhack {
public:
    float       speed  = 1.f;
    bool        active = false;
    std::string buf    = "1.0";
    void apply(float& dt) const { if (active) dt *= speed; }
    bool setText(const std::string& s);
};

// ═══ MacroBot — central controller (singleton) ════════════
class MacroBot {
public:
    static MacroBot& get();

    Macro       macro;
    PracticeFix fix;
    Speedhack   hack;

    BotState    state     = BotState::Idle;
    CBFStatus   cbf       = CBFStatus::None;
    double      gameTime  = 0.0;
    double      chkTime   = 0.0;
    uint16_t    attempt   = 0;
    bool        inLevel   = false;
    bool        practice  = false;
    bool        deadLast  = false;
    int         levelID   = 0;
    PlayLayer*  layer     = nullptr;

    size_t playIdx   = 0;
    bool   playReady = false;
    bool   guiOpen   = false;
    bool   pauseFlag = false;

    void init();
    void detectCBF();
    void enterLevel(PlayLayer* l);
    void exitLevel();
    void onDeath();
    void onPauseRestart();

    void startRec();
    void stopRec();
    void recInput(bool isP2, InputType t);

    void startPlay();
    void stopPlay();
    void tickPlay();

    void saveMacro();
    void loadMacro();
};

} // namespace mb