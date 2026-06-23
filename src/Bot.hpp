#pragma once

// =====================================================================================
//  Bot.hpp  --  Core engine for the Practice-to-Normal macro bot
//  Target: Geometry Dash 2.2081  /  Geode v5.7.1
//
//  This header contains everything that is NOT a hook or a UI widget:
//    * the recording data model
//    * the serializer / deserializer
//    * the playback cursor
//    * the practice-checkpoint state machine (the "practice bug fix")
//    * the dead-input cleanup logic
//    * the speedhack time-warp state
//    * CBF / CBS detection
//
//  The hooks in main.cpp are deliberately *thin*: they only forward engine events
//  (handleButton, per-step update, checkpoint mark/load, reset, death) into the
//  Bot singleton below. All decision making lives here so the logic is testable and
//  centralised.
//
//  -------------------------------------------------------------------------------
//  WHY X-POSITION-BASED RECORDING (and not frame/step/polling based)
//  -------------------------------------------------------------------------------
//  The single most important design choice in a 2.2081 / CBF / CBS macro is the
//  *key* that an input is stored against. The requirement is determinism that is
//  independent of FPS, refresh rate, render performance, substep count and
//  speedhack value. The candidates:
//
//    (a) Frame index            -- REJECTED. A frame is a *render* event. At 60 FPS
//                                  you get a different number of samples than at
//                                  240 FPS, so a recording made at one FPS desyncs
//                                  at another. This is exactly the failure mode the
//                                  spec forbids ("never fall back to frame-based").
//
//    (b) Physics-step index     -- Deterministic, but the *number of steps* changes
//                                  between regimes: vanilla, CBS (480 TPS cap) and
//                                  CBF (its Physics Bypass can change substepping).
//                                  A step index recorded under CBF is not the same
//                                  clock as one recorded under CBS, so it needs a
//                                  TPS rescale on load and is fragile.
//
//    (c) Polling-rate timestamp -- REJECTED for storage. Ties the file to the
//                                  recorder's poll rate and bloats size with
//                                  duration, not clicks.
//
//    (d) Player X position      -- CHOSEN. In GD the physics integrator is a fixed
//                                  timestep simulation; the *trajectory* (the set of
//                                  positions the player passes through) is identical
//                                  regardless of FPS or substep count -- that is the
//                                  entire reason CBS/CBF are "legal": they change
//                                  *when within a step* an input lands, not where the
//                                  player ends up for a given input. Therefore the
//                                  X coordinate at which a click happened is a
//                                  regime-independent, FPS-independent, speedhack-
//                                  independent anchor. It is also a single float, so
//                                  one input == one record: file size scales with
//                                  CLICK COUNT, not run duration (a 6-minute hold and
//                                  a 6-second hold cost the same one byte-record).
//
//  CBF PRECISION: because CBF/CBS sample the player position at sub-frame resolution,
//  the X we capture at click time is itself sub-frame precise. Storing the raw float
//  preserves "the maximum timing precision available" (spec) with no quantisation to
//  frame boundaries. On CBS we additionally expose a 480 TPS-aware note (see header
//  flags) but we still store the full-precision X so a CBF replay of a CBS recording
//  loses nothing.
//
//  NON-MONOTONIC X (dual is fine; reverse triggers and teleport portals are not):
//  X is monotonic in the overwhelming majority of gameplay, but "reverse" gameplay
//  and teleport portals break it. We solve this with a *segment* counter that both
//  the recorder and the player advance using identical, deterministic motion
//  detection (direction flip or large X discontinuity => new segment). Each event
//  stores its segment and a "forward" bit, so playback only ever fires an event
//  inside the matching segment and in the correct travel direction. See
//  Bot::updateSegment().
// =====================================================================================

#include <Geode/Geode.hpp>
#include <cstdint>
#include <vector>
#include <string>
#include <cmath>

using namespace geode::prelude;

// -------------------------------------------------------------------------------------
//  Constants / IDs
// -------------------------------------------------------------------------------------

// Verified against the published mod.json (id = "syzzi.click_between_frames", v1.5.0).
// CBF exposes NO api section, so this string + Loader::isModLoaded is the ONLY supported
// integration surface. We do not (and cannot) call any CBF function directly.
static constexpr const char* CBF_MOD_ID = "syzzi.click_between_frames";

// RobTop's native "Click Between Steps" is a game setting, not a mod, and is capped at
// ~480 TPS input resolution. There is no Geode-stable, publicly documented accessor that
// is guaranteed across binding revisions for this exact toggle, so we treat its detection
// as "best effort + user override" (see Bot::detectStatus / setUserCBSOverride). We never
// invent a GameVariable key we cannot verify.
static constexpr double CBS_INPUT_TPS = 480.0;

// -------------------------------------------------------------------------------------
//  Enums
// -------------------------------------------------------------------------------------

enum class BotMode : uint8_t {
    Idle    = 0,
    Record  = 1,
    Play    = 2,
};

// Precision regime currently detected. Drives the green/yellow/red status label and
// whether playback is permitted at all.
enum class Precision : uint8_t {
    None = 0,   // RED   -- neither CBF nor CBS. Playback DISABLED (per spec).
    CBS  = 1,   // YELLOW -- RobTop native Click Between Steps (~480 TPS cap).
    CBF  = 2,   // GREEN  -- Syzzi Click Between Frames (preferred, ~unbounded precision).
};

// GD's PlayerButton enum is {Jump=1, Left=2, Right=3}. We pack into 2 bits as 0..3 so
// `button == 0` is unused / "none". We store GD's raw value (1..3).
enum class HeldButton : uint8_t {
    None  = 0,
    Jump  = 1,
    Left  = 2,
    Right = 3,
};

// -------------------------------------------------------------------------------------
//  InputEvent -- one recorded state transition (a press OR a release)
// -------------------------------------------------------------------------------------
//  We store TRANSITIONS, never the held state per frame. A 5-second hold is two events
//  (down, then up), not 300 frames. This is what makes file size scale with click count.
//
//  Serialized layout (6 bytes, little-endian):
//      float   x        // player X at the instant of the input (sub-frame precise)
//      uint8   packed   // bit0-1: button(1..3)  bit2: down  bit3: player2  bit4: forward
//      uint8   segment  // motion segment index (handles reverse / teleport)
// -------------------------------------------------------------------------------------
struct InputEvent {
    float    x        = 0.f;     // anchor: where, in level space, the input fired
    uint8_t  button   = 0;       // HeldButton raw (1=Jump,2=Left,3=Right)
    bool     down     = false;   // true = press, false = release
    bool     player2  = false;   // which player (dual). false = P1
    bool     forward  = true;    // travel direction in this segment at capture time
    uint8_t  segment  = 0;       // monotonic motion segment id
};

// -------------------------------------------------------------------------------------
//  PlayerSnapshot -- complete physics/gamemode state for ONE PlayerObject
// -------------------------------------------------------------------------------------
//  This is the heart of the "complete practice bug fix". GD's built-in PlayerCheckpoint
//  restores position + a subset of state, which historically leaves residual velocity /
//  gravity / dash / size / speed-portal desyncs (the "practice bug"). We snapshot the
//  full set ourselves at checkpoint time and re-apply it AFTER GD's own restore, so our
//  values win and the restored attempt matches the original deterministically.
//
//  !!! BINDING NOTE !!!
//  The PlayerObject member NAMES below are the conventional Geode 2.2 names. Field names
//  occasionally shift between binding releases. If a field fails to compile under your
//  installed bindings, open `Geode/binding/PlayerObject.hpp` and adjust the name -- the
//  *semantics* (what we capture) are what matter and are documented per-field. We keep
//  the capture/apply isolated to capture()/applyTo() so there is exactly one place to fix.
// -------------------------------------------------------------------------------------
struct PlayerSnapshot {
    bool   valid = false;

    // --- Universal state -------------------------------------------------------------
    double posX = 0.0, posY = 0.0;   // position
    float  rotation = 0.f;           // visual + physics rotation
    double yVelocity = 0.0;          // vertical velocity (primary physics carrier)
    bool   upsideDown = false;       // gravity direction (true = flipped)
    float  gravityMod = 1.f;         // gravity multiplier (gravity portals)
    bool   isMini = false;           // size state (mini portal)
    float  scaleX = 1.f, scaleY = 1.f; // residual visual scale (size lerp)
    double playerSpeed = 1.0;        // current speed-portal multiplier

    // --- Gamemode flags (cube/ship/ball/ufo/wave/robot/spider/swing) -----------------
    bool isShip = false;   // m_isShip
    bool isBird = false;   // m_isBird   (UFO)
    bool isBall = false;   // m_isBall
    bool isDart = false;   // m_isDart   (Wave)
    bool isRobot = false;  // m_isRobot
    bool isSpider = false; // m_isSpider
    bool isSwing = false;  // m_isSwing

    // --- Gamemode-specific carriers --------------------------------------------------
    bool   onGround = false;     // ground contact (ball/robot/cube landing logic)
    bool   onGround2 = false;    // secondary ground flag (ceiling for flipped grav)
    bool   isDashing = false;    // dash-orb / spider dash latch
    bool   isSideways = false;   // sideways gravity corridors
    bool   isHolding = false;    // input latch (ship/ufo/wave hold state)
    int    ufoClicked = 0;       // UFO single-jump consumed latch (jump availability)
    double snapDiff = 0.0;       // robot/ground micro-offset carrier

    // Capture every field we can read from a live PlayerObject.
    void capture(PlayerObject* p);
    // Re-apply onto a live PlayerObject (called after GD's loadFromCheckpoint).
    void applyTo(PlayerObject* p) const;
};

// -------------------------------------------------------------------------------------
//  Checkpoint -- our extended checkpoint, kept in lock-step with PlayLayer's array
// -------------------------------------------------------------------------------------
struct Checkpoint {
    size_t        eventCount = 0;   // events.size() at the moment this checkpoint was made
    uint8_t       segment    = 0;   // motion segment at checkpoint
    bool          forward    = true;
    PlayerSnapshot p1;
    PlayerSnapshot p2;
};

// -------------------------------------------------------------------------------------
//  Bot -- the singleton engine
// -------------------------------------------------------------------------------------
class Bot {
public:
    static Bot& get() {
        static Bot inst;
        return inst;
    }

    // ---------------- status / detection ----------------
    Precision precision() const { return m_precision; }
    void detectStatus();                       // recompute CBF/CBS/None
    bool playbackAllowed() const { return m_precision != Precision::None; }
    // Manual override for native CBS when automatic detection is not reliable on a given
    // binding set (see header note). The UI exposes this so the user is never silently
    // locked out of a legitimate CBS run.
    void setUserCBSOverride(bool on) { m_userSaysCBS = on; detectStatus(); }
    bool userCBSOverride() const { return m_userSaysCBS; }

    // ---------------- mode ----------------
    BotMode mode() const { return m_mode; }
    void setMode(BotMode m);
    bool isRecording() const { return m_mode == BotMode::Record; }
    bool isPlaying()   const { return m_mode == BotMode::Play; }

    // ---------------- recording lifecycle ----------------
    void onLevelStart(PlayLayer* pl);          // reset per-attempt runtime state
    void recordInput(int button, bool down, bool isPlayer1, PlayLayer* pl);
    void onPhysicsStep(PlayLayer* pl);         // per-substep clock + segment tracking + playback dispatch
    void onMarkCheckpoint(PlayLayer* pl);      // push extended checkpoint
    void onLoadCheckpoint(PlayLayer* pl);      // restore extended state + drop dead inputs
    void onRemoveCheckpoint(PlayLayer* pl);
    void onDeath(PlayLayer* pl);               // death in practice -> GD will load last cp
    void onRestart(PlayLayer* pl, bool full);  // pause-menu restart cleanup
    void onLevelComplete(PlayLayer* pl);       // freeze recording as final route

    // ---------------- speedhack ----------------
    double timeWarp() const { return m_enableSpeed ? m_speed : 1.0; }
    void   setSpeed(double s) { m_speed = std::max(0.01, s); }
    double speed() const { return m_speed; }
    void   setSpeedEnabled(bool e) { m_enableSpeed = e; }
    bool   speedEnabled() const { return m_enableSpeed; }

    // ---------------- serialization ----------------
    // Returns true on success. Path defaults to Mod save dir.
    bool save(const std::string& name);
    bool load(const std::string& name);
    std::string defaultDir() const;            // Mod::get()->getSaveDir() / "macros"

    // ---------------- introspection (for UI) ----------------
    size_t inputCount() const { return m_events.size(); }
    bool   recordedUnderCBF() const { return m_recordPrecision == Precision::CBF; }
    Precision recordPrecision() const { return m_recordPrecision; }

private:
    Bot() = default;

    // Deterministic motion-segment update. Called once per physics substep for the
    // *reference* player (P1). Detects direction reversals and teleport discontinuities
    // identically during record and play so segment numbering is reproducible.
    void updateSegment(PlayLayer* pl);

    // Playback: dispatch any events whose anchor X we have just crossed in the current
    // segment / direction.
    void dispatchCrossed(PlayLayer* pl);

    // ----- detected environment -----
    Precision m_precision = Precision::None;
    bool      m_userSaysCBS = false;

    // ----- mode -----
    BotMode m_mode = BotMode::Idle;

    // ----- the macro -----
    std::vector<InputEvent> m_events;        // the recording (the saved/loaded artifact)
    Precision m_recordPrecision = Precision::None; // regime the macro was captured under

    // ----- checkpoints (extended) -----
    std::vector<Checkpoint> m_checkpoints;

    // ----- per-attempt runtime (NOT serialized) -----
    float   m_lastX = 0.f;          // previous-substep X for direction/teleport detection
    bool    m_haveLastX = false;
    bool    m_forward = true;       // current travel direction
    uint8_t m_segment = 0;          // current motion segment
    size_t  m_playIndex = 0;        // playback cursor into m_events

    // ----- speedhack -----
    bool   m_enableSpeed = false;
    double m_speed = 1.0;
};

// =====================================================================================
//  INLINE IMPLEMENTATION
//  (kept in the header so the project only needs main.cpp + Bot.hpp, as requested)
// =====================================================================================

// -------------------------------------------------------------------------------------
//  PlayerSnapshot::capture / applyTo
// -------------------------------------------------------------------------------------
inline void PlayerSnapshot::capture(PlayerObject* p) {
    if (!p) { valid = false; return; }
    valid = true;

    // Universal
    auto pos = p->getPosition();
    posX = pos.x;
    posY = pos.y;
    rotation   = p->getRotation();
    scaleX     = p->getScaleX();
    scaleY     = p->getScaleY();

    // --- direct members (see BINDING NOTE above) ---
    yVelocity   = p->m_yVelocity;
    upsideDown  = p->m_isUpsideDown;
    gravityMod  = p->m_gravityMod;
    playerSpeed = p->m_playerSpeed;
    isMini      = p->m_isMini;

    isShip   = p->m_isShip;
    isBird   = p->m_isBird;
    isBall   = p->m_isBall;
    isDart   = p->m_isDart;
    isRobot  = p->m_isRobot;
    isSpider = p->m_isSpider;
    isSwing  = p->m_isSwing;

    onGround  = p->m_isOnGround;
    onGround2 = p->m_isOnGround2;
    isDashing = p->m_isDashing;
    isSideways= p->m_isSideways;
    isHolding = p->m_isHolding;
}

inline void PlayerSnapshot::applyTo(PlayerObject* p) const {
    if (!p || !valid) return;

    // Apply position/rotation/scale first.
    p->setPosition({ static_cast<float>(posX), static_cast<float>(posY) });
    p->setRotation(rotation);
    p->setScaleX(scaleX);
    p->setScaleY(scaleY);

    // Physics carriers -- these are the values GD's own checkpoint frequently gets
    // subtly wrong, producing the practice bug. We overwrite them last so we win.
    p->m_yVelocity   = yVelocity;
    p->m_isUpsideDown= upsideDown;
    p->m_gravityMod  = gravityMod;
    p->m_playerSpeed = playerSpeed;
    p->m_isMini      = isMini;

    p->m_isShip   = isShip;
    p->m_isBird   = isBird;
    p->m_isBall   = isBall;
    p->m_isDart   = isDart;
    p->m_isRobot  = isRobot;
    p->m_isSpider = isSpider;
    p->m_isSwing  = isSwing;

    p->m_isOnGround  = onGround;
    p->m_isOnGround2 = onGround2;
    p->m_isDashing   = isDashing;
    p->m_isSideways  = isSideways;
    p->m_isHolding   = isHolding;
}

// -------------------------------------------------------------------------------------
//  Bot::detectStatus
// -------------------------------------------------------------------------------------
//  GREEN  : CBF mod present (it auto-overrides vanilla CBS, so it always wins).
//  YELLOW : CBF absent, but native CBS active (best-effort detect OR user override).
//  RED    : neither -> playback disabled.
// -------------------------------------------------------------------------------------
inline void Bot::detectStatus() {
    // 1) CBF -- the only fully reliable signal, via the documented mod id. CBF exposes no
    //    API, but if it is loaded it transparently intercepts the same input/physics path
    //    our bot uses, so detection == "loaded" is sufficient and correct.
    if (Loader::get()->isModLoaded(CBF_MOD_ID)) {
        m_precision = Precision::CBF;
        return;
    }

    // 2) Native CBS. There is no binding-stable public accessor we can guarantee across
    //    Geode 5.7.1 revisions for RobTop's exact CBS toggle, so we do not fabricate a
    //    GameVariable key. We honour an explicit user override (set from the UI) which is
    //    the reliable fallback. If you DO confirm the GameVariable key for your bindings,
    //    OR it in here:
    //        bool nativeCBS = GameManager::sharedState()->getGameVariable("<verified-key>");
    bool nativeCBS = m_userSaysCBS /* || nativeCBSFromVerifiedSetting() */;
    if (nativeCBS) {
        m_precision = Precision::CBS;
        return;
    }

    // 3) Nothing.
    m_precision = Precision::None;
}

// -------------------------------------------------------------------------------------
//  Bot::setMode
// -------------------------------------------------------------------------------------
inline void Bot::setMode(BotMode m) {
    detectStatus();
    // Hard rule from the spec: in RED, playback is disabled.
    if (m == BotMode::Play && !playbackAllowed()) {
        log::warn("[Bot] Playback blocked: no CBF/CBS implementation active (RED state).");
        return;
    }
    m_mode = m;
    m_playIndex = 0;
    if (m == BotMode::Record) {
        // Record regime is locked to whatever is active at record start so playback can
        // honour it (CBF preserves full precision; CBS is the 480 TPS-capped fallback).
        m_recordPrecision = m_precision;
    }
}

// -------------------------------------------------------------------------------------
//  Bot::onLevelStart -- reset per-attempt runtime (NOT the macro itself)
// -------------------------------------------------------------------------------------
inline void Bot::onLevelStart(PlayLayer* pl) {
    detectStatus();
    m_haveLastX = false;
    m_forward = true;
    m_segment = 0;
    m_playIndex = 0;

    if (m_mode == BotMode::Record) {
        // Fresh recording from the top of the level.
        m_events.clear();
        m_checkpoints.clear();
    }
}

// -------------------------------------------------------------------------------------
//  Bot::updateSegment -- deterministic, identical in record & play
// -------------------------------------------------------------------------------------
inline void Bot::updateSegment(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return;
    float x = pl->m_player1->getPositionX();

    if (!m_haveLastX) {
        m_lastX = x;
        m_haveLastX = true;
        return;
    }

    float dx = x - m_lastX;

    // Teleport portal / hard discontinuity: a single-substep jump far larger than any
    // physically plausible per-substep delta starts a new segment.
    constexpr float TELEPORT_THRESHOLD = 60.f; // ~2 blocks in one substep == teleport
    if (std::fabs(dx) > TELEPORT_THRESHOLD) {
        m_segment++;
        m_lastX = x;
        return;
    }

    // Direction reversal (reverse gameplay / reverse triggers). Use a small deadzone so
    // momentary near-zero dx at apexes does not spuriously flip the segment.
    constexpr float DIR_DEADZONE = 0.001f;
    if (dx > DIR_DEADZONE && !m_forward) {
        m_forward = true;
        m_segment++;
    } else if (dx < -DIR_DEADZONE && m_forward) {
        m_forward = false;
        m_segment++;
    }

    m_lastX = x;
}

// -------------------------------------------------------------------------------------
//  Bot::recordInput
// -------------------------------------------------------------------------------------
inline void Bot::recordInput(int button, bool down, bool isPlayer1, PlayLayer* pl) {
    if (m_mode != BotMode::Record || !pl || !pl->m_player1) return;
    if (button < 1 || button > 3) return; // only Jump/Left/Right

    // The reference player for the X anchor is whichever player produced the input, so a
    // dual recording stores each click against its own player's trajectory.
    PlayerObject* ref = isPlayer1 ? pl->m_player1 : pl->m_player2;
    if (!ref) ref = pl->m_player1;

    InputEvent e;
    e.x       = ref->getPositionX();
    e.button  = static_cast<uint8_t>(button);
    e.down    = down;
    e.player2 = !isPlayer1;
    e.forward = m_forward;
    e.segment = m_segment;
    m_events.push_back(e);
}

// -------------------------------------------------------------------------------------
//  Bot::onPhysicsStep -- called once per physics substep (from PlayerObject::update hook)
// -------------------------------------------------------------------------------------
//  This is the deterministic, FPS-independent clock. Under CBF/CBS it runs at the substep
//  rate, giving us sub-frame resolution for crossing detection (and therefore sub-frame
//  accurate playback dispatch) for free.
// -------------------------------------------------------------------------------------
inline void Bot::onPhysicsStep(PlayLayer* pl) {
    updateSegment(pl);
    if (m_mode == BotMode::Play) {
        dispatchCrossed(pl);
    }
}

// -------------------------------------------------------------------------------------
//  Bot::dispatchCrossed -- the playback core
// -------------------------------------------------------------------------------------
inline void Bot::dispatchCrossed(PlayLayer* pl) {
    if (!pl || !pl->m_player1 || !playbackAllowed()) return;

    // Advance the cursor through every event we have reached this substep. Because events
    // are stored in capture order and segments increase monotonically, a simple forward
    // cursor is correct and O(events) total over the whole run.
    while (m_playIndex < m_events.size()) {
        const InputEvent& e = m_events[m_playIndex];
        PlayerObject* ref = e.player2 ? pl->m_player2 : pl->m_player1;
        if (!ref) ref = pl->m_player1;
        float x = ref->getPositionX();

        bool reached;
        if (e.segment < m_segment) {
            // We are already past this event's segment (e.g. after a teleport we may have
            // skipped its exact X). Fire immediately so we never drop an input.
            reached = true;
        } else if (e.segment > m_segment) {
            // Haven't entered this event's segment yet.
            break;
        } else {
            // Same segment: fire when we cross the anchor in the recorded direction.
            reached = e.forward ? (x >= e.x) : (x <= e.x);
        }

        if (!reached) break;

        // Dispatch the real input through GD's own handler. Under CBF/CBS this lands at
        // the current substep, i.e. sub-frame accurate. PlayerButton: 1=Jump,2=Left,3=Right.
        pl->handleButton(e.down, static_cast<int>(e.button), !e.player2);
        m_playIndex++;
    }
}

// -------------------------------------------------------------------------------------
//  Checkpoint state machine  (practice bug fix + dead-input cleanup)
// -------------------------------------------------------------------------------------
inline void Bot::onMarkCheckpoint(PlayLayer* pl) {
    if (!pl) return;
    Checkpoint cp;
    cp.eventCount = m_events.size();   // <-- the cut line for dead-input cleanup
    cp.segment    = m_segment;
    cp.forward    = m_forward;
    if (pl->m_player1) cp.p1.capture(pl->m_player1);
    if (pl->m_player2) cp.p2.capture(pl->m_player2);
    m_checkpoints.push_back(cp);
}

inline void Bot::onLoadCheckpoint(PlayLayer* pl) {
    if (!pl) return;

    // Keep our checkpoint stack in lock-step with GD's own m_checkpointArray. GD has
    // already done its (partial) restore by the time this post-hook runs and has trimmed
    // its array to the loaded checkpoint, so we mirror that count.
    size_t gdCount = 0;
    if (pl->m_checkpointArray) gdCount = static_cast<size_t>(pl->m_checkpointArray->count());
    while (m_checkpoints.size() > gdCount && !m_checkpoints.empty()) {
        m_checkpoints.pop_back();
    }
    if (m_checkpoints.empty()) {
        // Loaded back to the very start.
        if (m_mode == BotMode::Record) m_events.clear();
        m_segment = 0; m_forward = true; m_haveLastX = false; m_playIndex = 0;
        return;
    }

    const Checkpoint& cp = m_checkpoints.back();

    // (1) COMPLETE PRACTICE BUG FIX: re-apply our full snapshot over GD's restore.
    cp.p1.applyTo(pl->m_player1);
    cp.p2.applyTo(pl->m_player2);

    // (2) DEAD-INPUT CLEANUP (death case): every input recorded AFTER this checkpoint
    //     belongs to the attempt that just failed. Drop it so the macro only ever
    //     contains the surviving route.
    if (m_mode == BotMode::Record && m_events.size() > cp.eventCount) {
        m_events.resize(cp.eventCount);
    }

    // (3) Restore the deterministic runtime cursors so record/play stay in sync after
    //     repeated checkpoint loads.
    m_segment   = cp.segment;
    m_forward   = cp.forward;
    m_haveLastX = false;            // re-seed lastX from the restored position next step
    if (m_mode == BotMode::Play) {
        // Rewind the playback cursor to the first event at/after the checkpoint so a
        // checkpoint reload during playback replays the route from the checkpoint.
        m_playIndex = cp.eventCount;
    }
}

inline void Bot::onRemoveCheckpoint(PlayLayer* pl) {
    if (!m_checkpoints.empty()) m_checkpoints.pop_back();
}

inline void Bot::onDeath(PlayLayer* pl) {
    // In practice mode, death is followed by GD auto-loading the last checkpoint, which
    // routes through onLoadCheckpoint above (where the actual truncation happens). If
    // there are no checkpoints, GD restarts the level -> onLevelStart clears the route.
    // Nothing else to do here; kept as an explicit hook point for clarity/extension.
    (void)pl;
}

inline void Bot::onRestart(PlayLayer* pl, bool full) {
    if (full) {
        // Full restart from the pause menu: abandon the entire route.
        if (m_mode == BotMode::Record) {
            m_events.clear();
            m_checkpoints.clear();
        }
        m_segment = 0; m_forward = true; m_haveLastX = false; m_playIndex = 0;
    } else {
        // Restart-from-checkpoint: identical cleanup to a checkpoint load.
        onLoadCheckpoint(pl);
    }
}

inline void Bot::onLevelComplete(PlayLayer* pl) {
    // The recording now represents the final, successful practice route. Stop recording
    // so nothing post-completion pollutes the macro; the user can then Save.
    if (m_mode == BotMode::Record) {
        m_mode = BotMode::Idle;
        log::info("[Bot] Level complete -- final route locked: {} inputs.", m_events.size());
    }
}

// -------------------------------------------------------------------------------------
//  Serialization
// -------------------------------------------------------------------------------------
//  Binary, little-endian, fixed 6-byte records. Header carries the precision regime so a
//  loader knows whether the macro is CBF-precise or CBS-capped. Reading/writing is a
//  single bulk file op (no per-frame I/O), satisfying the performance requirements.
//
//  Layout:
//      [0..3]   magic   'R','G','B','1'
//      [4..5]   uint16  version
//      [6]      uint8   recordPrecision (0 none / 1 CBS / 2 CBF)
//      [7]      uint8   flags (reserved)
//      [8..11]  uint32  eventCount
//      [12..]   eventCount * { float x; uint8 packed; uint8 segment }
// -------------------------------------------------------------------------------------
namespace botserial {
    inline void put_u16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
    }
    inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
        b.push_back((v >> 16) & 0xFF); b.push_back((v >> 24) & 0xFF);
    }
    inline void put_f32(std::vector<uint8_t>& b, float f) {
        uint32_t v; std::memcpy(&v, &f, 4); put_u32(b, v);
    }
    inline uint16_t get_u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
    inline uint32_t get_u32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    inline float get_f32(const uint8_t* p) {
        uint32_t v = get_u32(p); float f; std::memcpy(&f, &v, 4); return f;
    }
}

inline std::string Bot::defaultDir() const {
    auto dir = Mod::get()->getSaveDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

inline bool Bot::save(const std::string& name) {
    using namespace botserial;
    std::vector<uint8_t> buf;
    buf.reserve(12 + m_events.size() * 6);

    buf.push_back('R'); buf.push_back('G'); buf.push_back('B'); buf.push_back('1');
    put_u16(buf, 1); // version
    buf.push_back(static_cast<uint8_t>(m_recordPrecision));
    buf.push_back(0); // flags reserved
    put_u32(buf, static_cast<uint32_t>(m_events.size()));

    for (const auto& e : m_events) {
        put_f32(buf, e.x);
        uint8_t packed = (e.button & 0x03)
                       | (e.down    ? 0x04 : 0)
                       | (e.player2 ? 0x08 : 0)
                       | (e.forward ? 0x10 : 0);
        buf.push_back(packed);
        buf.push_back(e.segment);
    }

    auto path = std::filesystem::path(defaultDir()) / (name + ".rgb");
    auto res = file::writeBinary(path, buf);
    if (res.isErr()) {
        log::error("[Bot] save failed: {}", res.unwrapErr());
        return false;
    }
    log::info("[Bot] saved {} inputs ({} bytes) -> {}", m_events.size(), buf.size(), path.string());
    return true;
}

inline bool Bot::load(const std::string& name) {
    using namespace botserial;
    auto path = std::filesystem::path(defaultDir()) / (name + ".rgb");
    auto res = file::readBinary(path);
    if (res.isErr()) {
        log::error("[Bot] load failed: {}", res.unwrapErr());
        return false;
    }
    auto buf = res.unwrap();
    if (buf.size() < 12 || buf[0] != 'R' || buf[1] != 'G' || buf[2] != 'B' || buf[3] != '1') {
        log::error("[Bot] bad macro header");
        return false;
    }
    m_recordPrecision = static_cast<Precision>(buf[6]);
    uint32_t count = get_u32(buf.data() + 8);
    if (buf.size() < 12 + (size_t)count * 6) {
        log::error("[Bot] macro truncated");
        return false;
    }

    m_events.clear();
    m_events.reserve(count);
    const uint8_t* p = buf.data() + 12;
    for (uint32_t i = 0; i < count; ++i, p += 6) {
        InputEvent e;
        e.x       = get_f32(p);
        uint8_t packed = p[4];
        e.button  = packed & 0x03;
        e.down    = (packed & 0x04) != 0;
        e.player2 = (packed & 0x08) != 0;
        e.forward = (packed & 0x10) != 0;
        e.segment = p[5];
        m_events.push_back(e);
    }
    m_playIndex = 0;
    log::info("[Bot] loaded {} inputs (regime={})", count, (int)m_recordPrecision);
    return true;
}
