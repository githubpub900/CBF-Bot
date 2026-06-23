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
struct InputAction {
    double    time    = 0.0;
    bool      isP2    = false;
    InputType type    = InputType::Press;
    uint16_t  attempt = 0;
};

// ═══ Full player physics snapshot ═════════════════════════
// Only fields confirmed available in GD 2.2081 bindings.
// 'gravity' stores the raw value including sign (negative =
// flipped), so it encodes both strength and direction.
struct PlayerSnap {
    float x = 0.f, y = 0.f;
    float rot = 0.f, rotSpd = 0.f;
    float speed     = 0.9f;   // m_playerSpeed
    float speedMult = 1.f;    // m_speedMultiplier
    float gravity   = 1.f;    // m_gravity  (negative = flipped)
    float yVel      = 0.f;    // m_yVelocity
    float xVel      = 0.f;    // m_platformerXVelocity (truncated)
    float vehicleSize = 1.f;  // m_vehicleSize
    bool  onGround  = false;  // m_isOnGround
    bool  dashing   = false;  // m_isDashing
};

struct CheckpointSnap {
    double     gameTime = 0.0;
    PlayerSnap p1{}, p2{};
};

// ═══ Macro ════════════════════════════════════════════════
class Macro {
public:
    std::vector<InputAction> actions;
    int    levelID     = 0;
    double startOffset = 0.0;
    double duration    = 0.0;

    void   clear()  { actions.clear(); startOffset = duration = 0; }
    bool   empty()  const { return actions.empty(); }
    size_t size()   const { return actions.size(); }

    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void truncateAfter(double t);
    void removeAttempt(uint16_t att);
};

// ═══ Practice Bug Fix ═════════════════════════════════════
class PracticeFix {
public:
    std::deque<CheckpointSnap> snaps;
    size_t syncedCount = 0;

    void sync(PlayLayer* layer, double gt);
    void apply(PlayLayer* layer);
    void reset() { snaps.clear(); syncedCount = 0; }
private:
    PlayerSnap capture(PlayerObject* p);
    void       restore(PlayerObject* p, const PlayerSnap& s);
};

// ═══ Speedhack ════════════════════════════════════════════
class Speedhack {
public:
    float       speed  = 1.f;
    bool        active = false;
    std::string buf    = "1.0";
    void apply(float& dt) const { if (active) dt *= speed; }
    bool setText(const std::string& s);
};

// ═══ MacroBot ═════════════════════════════════════════════
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