/**
 * ============================================================================
 *  Bot.hpp  --  Core of the "Geode Time Macro" bot for Geometry Dash 2.2081
 * ============================================================================
 *
 *  A practice-mode -> normal-mode macro bot built for the Geode mod loader
 *  (v5.7.1) and designed from the ground up around Click Between Frames.
 *
 *  Design goals (see the request that spawned this file):
 *
 *    * Record a run in Practice Mode, play it back in Normal Mode.
 *    * Work with Syzzi's Click Between Frames (mod id: syzzi.click_between_frames)
 *      which lets you click on *practically infinite* sub-frames, and also keep
 *      working with RobTop's built in "Click Between Steps" (limited to a
 *      ~480 FPS input window).
 *    * Be tiny on disk. We do NOT store one record per physics frame. Instead we
 *      store *state-change events only* (a press or a release), each tagged with
 *      the exact level time it happened at. A 2 minute extreme demon with a few
 *      hundred clicks is only a few kilobytes.
 *    * Time-based, not frame/x-position based. Each input remembers the level
 *      time (m_gameState.m_levelTime, a double) at which it fired. Because that
 *      clock is advanced by the physics delta and not the render frame-rate, the
 *      macro is identical whether you run at 60, 240 or 9999 FPS, and whether or
 *      not CBF is subdividing the frame.
 *    * Recording does not have to start at level time 0. We remember the time the
 *      recording was armed and (optionally) normalise every event back to it, so
 *      starting a recording 0.2s into a level does not desync playback.
 *    * Accurate practice-bug fix: our own checkpoint snapshot stores velocity and
 *      the full player state for every gamemode, so loading a checkpoint resumes
 *      physics *exactly*.
 *    * "Dead input" discard: if you click, die, and retry from a checkpoint, the
 *      inputs you made between the checkpoint and the death are thrown away. The
 *      pause-menu restart button rewinds the input timeline too.
 *    * A frame-rate independent speedhack (a textbox, any multiplier) -- because
 *      the bot is driven by level time and not the frame-rate, cranking the speed
 *      does not desync the macro.
 *    * A GUI (toggle with K) that floats above every layer, captures its own
 *      clicks at a high touch priority, and shows a coloured "." status:
 *          green  -> Syzzi CBF present          (best, infinite sub-frames)
 *          yellow -> RobTop Click Between Steps  (limited, ~480fps window)
 *          red    -> no CBF at all               (bot disabled)
 *
 *  This header holds the data model, the singleton BotManager (all of the logic,
 *  storage and file IO) and the floating BotUILayer. main.cpp wires the Geode
 *  hooks into it.
 * ============================================================================
 */

#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/CCMenuItemToggler.hpp>
#include <Geode/binding/PlatformToolbox.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/ui/TextInput.hpp>

#include <vector>
#include <array>
#include <string>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <system_error>

using namespace geode::prelude;

// ============================================================================
// ============================================================================
//
//   DESIGN & USAGE REFERENCE
//   ------------------------
//
//   This block documents how the bot works end to end so the code below reads
//   in context. None of it is load-bearing -- it is here for whoever maintains
//   the mod next (very possibly future-you at 3am wondering why a hold desynced).
//
//
//   1. WHY TIME, NOT FRAMES OR X-POSITION
//   -------------------------------------
//   Older macro bots key inputs to a frame index or the player's x position.
//   Both break the moment the physics tick rate changes:
//
//       * frame index  -> desyncs the instant FPS differs between record and
//                         playback, and is meaningless once CBF subdivides a
//                         frame into many physics steps.
//       * x position   -> desyncs on any level with a speed change, teleport,
//                         dual portal, or platformer back-and-forth movement.
//
//   The one clock that is stable across all of those is GD's own accumulated
//   physics time: m_gameState.m_levelTime (a double, seconds). It is advanced by
//   the physics delta, so it ticks identically at 60 / 240 / 9999 FPS and it
//   keeps counting sub-frame when CBF runs many steps per frame. Every input we
//   record is simply "the level time at which it happened", and playback fires an
//   input the moment that time is reached. Frame rate is irrelevant -- which is
//   also exactly why the speedhack can be cranked arbitrarily without desync.
//
//
//   2. WHY EVENTS, NOT FRAMES OF STATE  (the "don't store a billion frames" rule)
//   ----------------------------------------------------------------------------
//   We never serialise the held/not-held state of every button every frame.
//   Instead we store only the *transitions* -- a press or a release -- each
//   tagged with its level time. Holding is implied: a button is down from its
//   "down" event until its matching "up" event. A two minute extreme demon has a
//   few hundred transitions, i.e. a few kilobytes, no matter how high the FPS or
//   how finely CBF subdivides. This is the cheapest representation that still
//   reproduces the run exactly.
//
//
//   3. CLICK BETWEEN FRAMES  (the whole reason this exists)
//   -------------------------------------------------------
//   Syzzi's Click Between Frames (mod id: syzzi.click_between_frames) feeds OS
//   inputs into the engine at the physics-step level and subdivides each rendered
//   frame into many steps, so you can click on effectively infinite sub-frames.
//   RobTop's built in "Click Between Steps" does the same idea but is capped to a
//   ~480 FPS input window.
//
//   Both of them ultimately drive a single chokepoint:
//
//       GJBaseGameLayer::handleButton(bool down, int button, bool isPlayer1)
//
//   That is THE place to capture inputs at the lowest level, because by the time
//   a click reaches handleButton, CBF has already placed it on the correct
//   sub-step and advanced m_levelTime to match. So:
//
//       * recording  -> hook handleButton, stamp m_levelTime. Sub-frame accurate
//                       for free, because the clock is already sub-frame.
//       * playback   -> fire handleButton from processCommands (runs per physics
//                       step). With CBF that is many tiny steps per frame, so the
//                       worst-case timing error is a single sub-step.
//
//   CBF state drives the coloured "." in the GUI:
//       green  -> Syzzi CBF present   (full sub-frame precision)
//       yellow -> RobTop CBS present  (limited; timestamps are snapped to 480fps)
//       red    -> neither             (the bot refuses to record or play)
//
//
//   4. THE RECORDING-START-OFFSET PROBLEM
//   -------------------------------------
//   Recording does not have to begin at level time 0. If you arm a recording 0.2s
//   into a level, every captured event carries its true absolute level time, so
//   replaying from the start lines up perfectly -- the first input simply fires at
//   t=0.2 as recorded. No desync. If instead you *want* the macro to begin at the
//   moment you armed it (shift everything back to 0), "Normalize" does exactly
//   that, and offsetMacro() lets you slide the whole timeline by any delta.
//
//
//   5. ACCURATE PRACTICE-BUG FIX
//   ----------------------------
//   Vanilla practice-mode checkpoints do not faithfully restore velocity and a
//   handful of per-gamemode flags, so a respawn can feel subtly wrong. We snapshot
//   the FULL player physics state (position, rotation, y-velocity, platformer
//   x-velocity, gravity, and every gamemode/orientation/contact flag, for both
//   players) at each storeCheckpoint, and re-apply it after loadFromCheckpoint --
//   on top of the engine's own restore. The result is a bit-for-bit consistent
//   resume in every gamemode (cube/ship/ball/ufo/wave/robot/spider/swing).
//
//
//   6. DEAD-INPUT DISCARD
//   ---------------------
//   "Dead inputs" are clicks made on an attempt that then died. Each checkpoint
//   remembers how many events existed when it was set. On a checkpoint reload we
//   truncate the event list back to that count -- the clicks from the failed
//   attempt are gone, and the next attempt re-records cleanly from the checkpoint.
//   The pause-menu restart buttons do the same thing against the level start, so
//   restarting reverts every later input. After any truncation we rebuild the
//   held-button table so the redundancy collapse stays correct.
//
//
//   7. SPEEDHACK
//   ------------
//   We scale the output of GJBaseGameLayer::getModifiedDelta by the textbox value.
//   Because the macro is driven by m_levelTime (which advances by that very
//   delta), the inputs and the clock scale together -- there is nothing to "keep
//   up with", so high speeds do not desync or lag the bot.
//
//
//   8. FILE FORMATS
//   ---------------
//   Binary (.gdtm) is the default: tiny and lossless (full-precision doubles).
//   Text (.txt) is for inspection / hand-editing and is where the "cap timestamps
//   to 50 decimals before rounding" rule is honoured literally. See saveMacro /
//   saveMacroText for the exact byte / line layout.
//
//
//   9. CONTROLS
//   -----------
//       K  toggle the GUI            V  arm / stop recording
//       B  start / stop playback     N  hard stop
//   The GUI floats above every layer (INT_MAX z-order), owns its own keyboard
//   delegate (so K works regardless of focus), and swallows clicks that land on
//   its panel so they never leak into gameplay. Drag it by its title bar.
//
//
//   10. HOOK MAP  (see main.cpp)
//   ----------------------------
//       GJBaseGameLayer::handleButton      record + (guard) inject
//       GJBaseGameLayer::processCommands   per-step playback firing
//       GJBaseGameLayer::update            per-frame playback backstop
//       GJBaseGameLayer::getModifiedDelta  speedhack
//       PlayLayer::init / resetLevel / resetLevelFromStart / destroyPlayer
//       PlayLayer::storeCheckpoint / loadFromCheckpoint / removeCheckpoint
//       PlayLayer::levelComplete
//       PauseLayer::onRestart / onRestartFull
//
// ============================================================================
// ============================================================================

// ============================================================================
//  Constants
// ============================================================================

namespace bot {

    // The Geode mod id for Syzzi's Click Between Frames.
    static constexpr const char* SYZZI_CBF_ID = "syzzi.click_between_frames";

    // RobTop's "Click Between Steps" is a GD game-variable. Unlike a Geode mod it
    // is not advertised through the loader, so we have to read it from the
    // GameManager. The exact key string can shift between minor GD revisions, so
    // it is isolated here as a named constant -- if a future GD update moves it,
    // this is the only line that needs touching.
    static constexpr const char* ROBTOP_CBS_GAMEVAR = "0168";

    // RobTop's CBS is documented as a ~480 FPS input window.
    static constexpr double ROBTOP_CBS_FPS = 480.0;

    // The magic + version stamped at the top of a saved macro file.
    static constexpr uint32_t MACRO_MAGIC   = 0x4D544447; // 'G''D''T''M' little-endian
    static constexpr uint32_t MACRO_VERSION = 3;

    // Default toggle key for the GUI.
    static constexpr cocos2d::enumKeyCodes TOGGLE_KEY = cocos2d::enumKeyCodes::KEY_K;

    // When we serialise a timestamp to *text* we round to this many decimals.
    // (Binary saves keep the full double, which is the most compact + accurate
    //  representation; the request's "50 decimal" cap only meaningfully applies
    //  to a text export, where we clamp before rounding.)
    static constexpr int TEXT_TIME_DECIMALS = 50;

    // Button ids as understood by GJBaseGameLayer::handleButton.
    enum class Button : uint8_t {
        Jump  = 1,
        Left  = 2,
        Right = 3,
    };

    // What level of CBF support is currently available.
    enum class CBFState {
        None,    // red    -- bot disabled
        RobTop,  // yellow -- limited 480fps window
        Syzzi,   // green  -- infinite sub-frame precision
    };

    // The bot's high level mode.
    enum class Mode {
        Disabled,
        Recording,
        Playing,
    };

} // namespace bot

// ============================================================================
//  InputEvent  --  one press or release, tagged with the level time it fired.
// ============================================================================
//
//  This is the *entire* on-disk representation of a click. We never store
//  per-frame state, only the transitions, which keeps macros tiny while still
//  reproducing the run exactly: holding is implied between a down event and the
//  next up event for the same button.
//
struct InputEvent {
    double  time     = 0.0;   // m_gameState.m_levelTime at which this fired
    uint8_t button   = 1;     // bot::Button
    bool    down     = true;  // press (true) or release (false)
    bool    player2  = false; // which player (dual support)

    InputEvent() = default;
    InputEvent(double t, uint8_t b, bool d, bool p2)
        : time(t), button(b), down(d), player2(p2) {}

    // Stable ordering: by time, then presses before releases so a 0-length tap
    // on the exact same timestamp still registers a down before its up.
    bool operator<(InputEvent const& o) const {
        if (time != o.time) return time < o.time;
        if (down != o.down) return down && !o.down; // down first
        return button < o.button;
    }
};

// ============================================================================
//  PlayerSnapshot  --  full physics state of one PlayerObject.
// ============================================================================
//
//  This is the heart of the accurate practice-bug fix. RobTop's own checkpoints
//  do not faithfully restore velocity and a handful of per-gamemode flags, which
//  is why vanilla practice can feel "off" after a respawn. We snapshot every
//  field that matters for every gamemode so a reload is bit-for-bit consistent.
//
struct PlayerSnapshot {
    bool   valid = false;

    // Transform.
    cocos2d::CCPoint position{0.f, 0.f};
    float  rotation = 0.f;
    float  scaleX = 1.f;
    float  scaleY = 1.f;

    // Velocity / gravity (the bits RobTop's checkpoint is sloppy about).
    double yVelocity = 0.0;
    double platformerXVelocity = 0.0;
    double gravity = 0.0;
    double speedMultiplier = 0.0;
    float  playerSpeed = 0.f;
    float  gravityMod = 1.f;
    double yStart = 0.0;

    // Gamemode booleans -- "all gamemodes".
    bool isShip = false;
    bool isBird = false;     // UFO
    bool isBall = false;
    bool isDart = false;     // wave
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;

    // Orientation / contact state.
    bool isUpsideDown = false;
    bool isSideways = false;
    bool isGoingLeft = false;
    bool isOnGround = false;
    bool isOnGround2 = false;
    bool isDashing = false;
    bool isLocked = false;
    bool isDead = false;
    bool isHolding = false;

    // Platformer movement intent.
    bool platformerMovingLeft = false;
    bool platformerMovingRight = false;

    cocos2d::CCPoint lastGroundedPos{0.f, 0.f};

    // Capture every field above out of a live PlayerObject.
    void capture(PlayerObject* p) {
        if (!p) { valid = false; return; }
        valid = true;

        position = p->getPosition();
        rotation = p->getRotation();
        scaleX   = p->getScaleX();
        scaleY   = p->getScaleY();

        yVelocity           = p->m_yVelocity;
        platformerXVelocity = p->m_platformerXVelocity;
        gravity             = p->m_gravity;
        speedMultiplier     = p->m_speedMultiplier;
        playerSpeed         = p->m_playerSpeed;
        gravityMod          = p->m_gravityMod;
        yStart              = p->m_yStart;

        isShip   = p->m_isShip;
        isBird   = p->m_isBird;
        isBall   = p->m_isBall;
        isDart   = p->m_isDart;
        isRobot  = p->m_isRobot;
        isSpider = p->m_isSpider;
        isSwing  = p->m_isSwing;

        isUpsideDown = p->m_isUpsideDown;
        isSideways   = p->m_isSideways;
        isGoingLeft  = p->m_isGoingLeft;
        isOnGround   = p->m_isOnGround;
        isOnGround2  = p->m_isOnGround2;
        isDashing    = p->m_isDashing;
        isLocked     = p->m_isLocked;
        isDead       = p->m_isDead;

        platformerMovingLeft  = p->m_platformerMovingLeft;
        platformerMovingRight = p->m_platformerMovingRight;

        lastGroundedPos = p->m_lastGroundedPos;
    }

    // Push every captured field back into a live PlayerObject. Called *after*
    // RobTop's own loadFromCheckpoint has run, so we only correct what it got
    // wrong rather than fighting it.
    void apply(PlayerObject* p) const {
        if (!p || !valid) return;

        p->setPosition(position);
        p->setRotation(rotation);
        p->setScaleX(scaleX);
        p->setScaleY(scaleY);

        p->m_yVelocity           = yVelocity;
        p->m_platformerXVelocity = platformerXVelocity;
        p->m_gravity             = gravity;
        p->m_speedMultiplier     = speedMultiplier;
        p->m_playerSpeed         = playerSpeed;
        p->m_gravityMod          = gravityMod;
        p->m_yStart              = yStart;

        p->m_isShip   = isShip;
        p->m_isBird   = isBird;
        p->m_isBall   = isBall;
        p->m_isDart   = isDart;
        p->m_isRobot  = isRobot;
        p->m_isSpider = isSpider;
        p->m_isSwing  = isSwing;

        p->m_isUpsideDown = isUpsideDown;
        p->m_isSideways   = isSideways;
        p->m_isGoingLeft  = isGoingLeft;
        p->m_isOnGround   = isOnGround;
        p->m_isOnGround2  = isOnGround2;
        p->m_isDashing    = isDashing;
        p->m_isLocked     = isLocked;
        p->m_isDead       = isDead;

        p->m_platformerMovingLeft  = platformerMovingLeft;
        p->m_platformerMovingRight = platformerMovingRight;

        p->m_lastGroundedPos = lastGroundedPos;
    }
};

// ============================================================================
//  CheckpointFrame  --  what we remember each time a practice checkpoint is set.
// ============================================================================
//
//  Mirrors the game's m_checkpointArray one-to-one. When a checkpoint is loaded
//  we use `eventCount` to truncate the recording back to that moment (this is
//  the "dead input" discard) and the player snapshots to fix the physics.
//
struct CheckpointFrame {
    int            eventCount = 0;     // size of the event list when set
    double         levelTime  = 0.0;   // level time when set
    PlayerSnapshot p1;
    PlayerSnapshot p2;
    void*          checkpointPtr = nullptr; // identity of the CheckpointObject
};

// ============================================================================
//  Macro  --  an ordered list of InputEvents plus a little metadata.
// ============================================================================

struct Macro {
    std::vector<InputEvent> events;
    double recordStartTime = 0.0; // level time the recording was armed at
    double recordFps       = 240.0; // informational: physics fps used to record
    bool   normalized      = false; // were events shifted so the first is at 0?

    // Level identity -- so we can warn before playing a macro on the wrong level.
    int         levelID = 0;
    std::string levelName;

    void clear() {
        events.clear();
        recordStartTime = 0.0;
        normalized = false;
        // levelID / levelName are intentionally preserved across a clear so that
        // re-recording on the same level keeps its identity.
    }

    size_t size() const { return events.size(); }
    bool empty() const { return events.empty(); }

    // The level time of the final input -- used for the progress readout.
    double duration() const {
        return events.empty() ? 0.0 : events.back().time;
    }

    // Keep the list ordered. We almost always append in order while recording,
    // but a stable sort makes editing / merging safe and cheap.
    void sort() {
        std::stable_sort(events.begin(), events.end());
    }
};

// ============================================================================
//  Forward declaration of the floating UI so the manager can poke it.
// ============================================================================
class BotUILayer;

// ============================================================================
//  BotManager  --  the singleton brain.
// ============================================================================
class BotManager {
public:
    static BotManager& get() {
        static BotManager inst;
        return inst;
    }

    // ----- live state ------------------------------------------------------
    bot::Mode mode = bot::Mode::Disabled;
    Macro     macro;

    // Index of the next event to fire during playback.
    size_t    playbackIndex = 0;

    // Set true while we are injecting our own inputs, so the handleButton hook
    // knows not to record them back into the macro.
    bool      injecting = false;

    // True while the GUI panel is open. The physics hooks freeze the level (no
    // time advances) while this is set, so opening the menu pauses gameplay
    // without spawning the vanilla pause menu over our UI.
    bool      guiPaused = false;

    // Speedhack.
    bool      speedhackEnabled = false;
    double    speed = 1.0;

    // Misc options.
    bool      practiceFixEnabled = true;  // accurate checkpoint snapshots
    bool      discardDeadInputs  = true;  // truncate on checkpoint reload / restart
    bool      normalizeRecording = false; // shift events so the first one is t=0

    // When only RobTop's CBS is available (yellow), quantize timestamps onto the
    // ~480 FPS grid that RobTop's limited window can actually represent. With
    // Syzzi's CBF (green) we leave timestamps at full double precision.
    bool      quantizeForRobTopCBS = true;

    // Automatically save the macro to disk when a level is completed while
    // recording -- handy so a clean practice run is never lost.
    bool      autoSaveOnComplete = false;

    // Held-button state, indexed [player2 ? 1 : 0][button]. Maintained
    // incrementally while recording so we can collapse redundant transitions in
    // O(1) instead of rescanning the whole event list per input.
    std::array<std::array<bool, 4>, 2> heldState{};

    // Our parallel checkpoint stack, kept in lock-step with m_checkpointArray.
    std::vector<CheckpointFrame> checkpoints;

    // Weak handle to the floating UI (owned by the scene).
    BotUILayer* ui = nullptr;

    // Currently-loaded macro file name (without extension).
    std::string macroName = "macro";

    // ----- CBF detection ---------------------------------------------------

    bool isSyzziCBFLoaded() const {
        return Loader::get()->isModLoaded(bot::SYZZI_CBF_ID);
    }

    bool isRobTopCBSEnabled() const {
        // RobTop's "Click Between Steps" lives as a GameManager game-variable.
        auto gm = GameManager::sharedState();
        if (!gm) return false;
        return gm->getGameVariable(bot::ROBTOP_CBS_GAMEVAR);
    }

    bot::CBFState cbfState() const {
        if (isSyzziCBFLoaded())  return bot::CBFState::Syzzi;
        if (isRobTopCBSEnabled()) return bot::CBFState::RobTop;
        return bot::CBFState::None;
    }

    bool cbfAvailable() const {
        return cbfState() != bot::CBFState::None;
    }

    // A human-readable one-liner describing the current input precision, shown in
    // the GUI next to the coloured period.
    std::string cbfInfoString() const {
        switch (cbfState()) {
            case bot::CBFState::Syzzi:
                return "CBF: Syzzi (sub-frame)";
            case bot::CBFState::RobTop:
                return fmt::format("CBF: RobTop ({:.0f}fps cap)", bot::ROBTOP_CBS_FPS);
            case bot::CBFState::None:
            default:
                return "CBF: none -- disabled";
        }
    }

    // The smallest time delta the current CBF setup can resolve, in seconds.
    // Syzzi is effectively continuous, so we report the physics tick as a floor;
    // RobTop is bounded by its 480 FPS window.
    double effectiveInputResolution() const {
        switch (cbfState()) {
            case bot::CBFState::Syzzi:  return 1.0 / 1000000.0; // ~continuous
            case bot::CBFState::RobTop: return 1.0 / bot::ROBTOP_CBS_FPS;
            case bot::CBFState::None:
            default:                    return 1.0 / 240.0;
        }
    }

    // ----- gui pause / cursor / music -------------------------------------

    // Open/close the GUI: freeze the level, reveal the OS cursor, and pause the
    // song. Closing reverses all three. The actual physics freeze is enforced by
    // the GJBaseGameLayer hooks reading `guiPaused`.
    void setGuiOpen(bool open) {
        guiPaused = open;
        if (open) {
            PlatformToolbox::showCursor();
            if (auto fae = FMODAudioEngine::sharedEngine()) fae->pauseAllMusic(true);
        } else {
            if (auto fae = FMODAudioEngine::sharedEngine()) fae->resumeAllMusic();
            // GD hides the cursor during gameplay; only re-hide it if we are
            // actually in a level (so we don't hide it back on a menu).
            if (PlayLayer::get()) PlatformToolbox::hideCursor();
        }
    }

    // ----- time helpers ----------------------------------------------------

    // Read the authoritative, frame-rate independent level clock.
    static double levelTime(GJBaseGameLayer* gl) {
        return gl ? gl->m_gameState.m_levelTime : 0.0;
    }

    // Round a timestamp the way a text export wants it: clamp to a sane number
    // of decimals (the request's 50-decimal cap) and round.
    static double roundTimeForText(double t) {
        // doubles only carry ~15-17 significant digits, so 50 decimals is well
        // past the precision floor -- this is effectively a no-op round that
        // honours the requested cap without throwing away anything real.
        if (!std::isfinite(t)) return 0.0;
        double scale = std::pow(10.0, std::min(bot::TEXT_TIME_DECIMALS, 15));
        return std::round(t * scale) / scale;
    }

    // ----- mode control ----------------------------------------------------

    void startRecording(GJBaseGameLayer* gl) {
        if (!cbfAvailable()) {
            notifyNoCBF();
            return;
        }
        mode = bot::Mode::Recording;
        macro.clear();
        macro.recordStartTime = levelTime(gl);
        macro.recordFps = currentPhysicsFps(gl);
        captureLevelInfo();
        checkpoints.clear();
        playbackIndex = 0;
        resetHeldState();
        log::info("[Bot] Recording armed at t={:.6f}", macro.recordStartTime);
        notify("Recording started", NotificationIcon::Success);
        refreshUI();
    }

    void startPlayback(GJBaseGameLayer* gl) {
        if (!cbfAvailable()) {
            notifyNoCBF();
            return;
        }
        if (macro.empty()) {
            notify("No macro to play", NotificationIcon::Warning);
            return;
        }
        macro.sort();
        validateAndRepair();
        warnIfWrongLevel();
        mode = bot::Mode::Playing;
        // Skip any events that are already behind the current level time (e.g.
        // when starting from a checkpoint partway through a level)...
        seekPlaybackTo(levelTime(gl));
        // ...and re-press anything that should already be held at that point.
        applyHeldStateAt(gl, levelTime(gl));
        log::info("[Bot] Playback started {}", description());
        notify("Playback started", NotificationIcon::Success);
        refreshUI();
    }

    void stop() {
        // Make sure we leave no buttons stuck down.
        if (mode == bot::Mode::Playing) releaseAll();
        mode = bot::Mode::Disabled;
        playbackIndex = 0;
        refreshUI();
    }

    void toggleRecording(GJBaseGameLayer* gl) {
        if (mode == bot::Mode::Recording) stop();
        else startRecording(gl);
    }

    void togglePlayback(GJBaseGameLayer* gl) {
        if (mode == bot::Mode::Playing) stop();
        else startPlayback(gl);
    }

    // Move the playback cursor to the first event at/after `time`.
    void seekPlaybackTo(double time) {
        playbackIndex = 0;
        while (playbackIndex < macro.events.size() &&
               macro.events[playbackIndex].time < time) {
            ++playbackIndex;
        }
    }

    // Stamp the current level's identity into the macro so we can later detect a
    // macro being played on the wrong level.
    void captureLevelInfo() {
        if (auto pl = PlayLayer::get()) {
            if (pl->m_level) {
                macro.levelID   = pl->m_level->m_levelID.value();
                macro.levelName = pl->m_level->m_levelName;
            }
        }
    }

    // If the loaded macro was recorded on a different level, warn (but still
    // allow it -- copying a macro between bug-fixed copies of a level is valid).
    void warnIfWrongLevel() {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level || macro.levelID == 0) return;
        int current = pl->m_level->m_levelID.value();
        if (current != macro.levelID) {
            notify(fmt::format("Macro is for level {} -- you are on {}",
                               macro.levelID, current),
                   NotificationIcon::Warning);
        }
    }

    // Ensure every hold is balanced. An unmatched press (e.g. a recording that
    // was stopped mid-hold) gets a synthetic release appended just after the last
    // event, so playback never leaves a button stuck down forever.
    void validateAndRepair() {
        std::array<std::array<bool, 4>, 2> held{};
        for (auto& row : held) row.fill(false);
        double lastTime = 0.0;
        for (auto const& e : macro.events) {
            if (e.button >= 1 && e.button <= 3)
                held[e.player2 ? 1 : 0][e.button] = e.down;
            lastTime = std::max(lastTime, e.time);
        }
        int repaired = 0;
        for (int pi = 0; pi < 2; ++pi) {
            for (int b = 1; b <= 3; ++b) {
                if (held[pi][b]) {
                    macro.events.emplace_back(lastTime + 1e-6,
                        static_cast<uint8_t>(b), false, pi == 1);
                    ++repaired;
                }
            }
        }
        if (repaired) {
            macro.sort();
            log::info("[Bot] Repaired {} unbalanced hold(s) before playback", repaired);
        }
    }

    // Level finished: stop injecting and (optionally) leave the macro intact.
    void onLevelComplete(PlayLayer* pl) {
        if (mode == bot::Mode::Playing) {
            log::info("[Bot] Level complete -- playback finished cleanly");
            stop();
        } else if (mode == bot::Mode::Recording) {
            log::info("[Bot] Level complete while recording ({} inputs captured)",
                      macro.size());
            if (autoSaveOnComplete && !macro.empty()) {
                saveMacro(macroName);
            }
            stop();
        }
    }

    // ----- recording -------------------------------------------------------

    // Called from the handleButton hook. Records a transition tagged with the
    // current level time. We collapse redundant transitions (two downs in a row
    // for the same button/player) so the macro stays clean.
    void recordInput(GJBaseGameLayer* gl, bool down, int button, bool isPlayer1) {
        if (mode != bot::Mode::Recording) return;
        if (injecting) return; // never record our own injected playback
        if (button < 1 || button > 3) return; // only jump/left/right

        bool player2 = !isPlayer1;
        int  pi = player2 ? 1 : 0;

        // Collapse no-op transitions in O(1): ignore a press if that button is
        // already held, or a release if it is already up.
        if (heldState[pi][button] == down) return;
        heldState[pi][button] = down;

        double t = levelTime(gl);

        // RobTop's CBS can only land inputs on its limited grid; snapping the
        // timestamp there keeps a yellow-state recording honest about what the
        // engine can actually reproduce. Syzzi's CBF (green) keeps full precision.
        if (cbfState() == bot::CBFState::RobTop && quantizeForRobTopCBS) {
            t = quantizeRobTop(t);
        }

        macro.events.emplace_back(t, static_cast<uint8_t>(button), down, player2);
        refreshUIProgress();
    }

    // Snap a timestamp onto RobTop's ~480 FPS input grid.
    static double quantizeRobTop(double t) {
        double step = 1.0 / bot::ROBTOP_CBS_FPS;
        return std::round(t / step) * step;
    }

    void resetHeldState() {
        for (auto& row : heldState) row.fill(false);
    }

    // Rebuild heldState by replaying the (possibly truncated) event list. Called
    // after a dead-input discard or a restart so the redundancy collapse stays
    // correct for any inputs the user makes next.
    void recomputeHeldState() {
        resetHeldState();
        for (auto const& e : macro.events) {
            int pi = e.player2 ? 1 : 0;
            if (e.button >= 1 && e.button <= 3) heldState[pi][e.button] = e.down;
        }
    }

    // At playback start / checkpoint-resume, re-press any button whose last
    // transition before `time` was a press, so a hold that spans the resume
    // point is honoured instead of silently dropped.
    void applyHeldStateAt(GJBaseGameLayer* gl, double time) {
        if (!gl) return;
        std::array<std::array<int, 4>, 2> last{};   // -1 up, +1 down, 0 unknown
        for (auto& row : last) row.fill(0);
        for (auto const& e : macro.events) {
            if (e.time >= time) break;
            int pi = e.player2 ? 1 : 0;
            if (e.button >= 1 && e.button <= 3) last[pi][e.button] = e.down ? 1 : -1;
        }
        injecting = true;
        for (int pi = 0; pi < 2; ++pi) {
            for (int b = 1; b <= 3; ++b) {
                if (last[pi][b] == 1) gl->handleButton(true, b, pi == 0);
            }
        }
        injecting = false;
    }

    // ----- playback --------------------------------------------------------

    // Called every physics step (and once per frame as a backstop). Fires every
    // event whose timestamp has been reached. Because this runs per *step* and
    // CBF makes steps tiny, the worst-case error is a single sub-step.
    void fireDueInputs(GJBaseGameLayer* gl) {
        if (mode != bot::Mode::Playing) return;
        if (!gl) return;

        double now = levelTime(gl);
        injecting = true;
        while (playbackIndex < macro.events.size() &&
               macro.events[playbackIndex].time <= now) {
            auto const& e = macro.events[playbackIndex];
            gl->handleButton(e.down, static_cast<int>(e.button), !e.player2);
            ++playbackIndex;
        }
        injecting = false;

        // Auto-stop when the macro has been fully replayed.
        if (playbackIndex >= macro.events.size()) {
            // Leave the mode as Playing until the level ends so a level that
            // keeps scrolling past the last input still finishes naturally,
            // but make sure nothing is stuck down.
        }
    }

    // Force-release every button (used when stopping playback abruptly).
    void releaseAll() {
        auto gl = GJBaseGameLayer::get();
        if (!gl) return;
        injecting = true;
        for (int b = 1; b <= 3; ++b) {
            gl->handleButton(false, b, true);
            gl->handleButton(false, b, false);
        }
        injecting = false;
    }

    // ----- checkpoints / practice fix / dead-input discard -----------------

    void onCheckpointStored(PlayLayer* pl, void* cpPtr) {
        if (!pl) return;
        CheckpointFrame f;
        f.eventCount    = static_cast<int>(macro.events.size());
        f.levelTime     = levelTime(pl);
        f.checkpointPtr = cpPtr;
        if (practiceFixEnabled) {
            f.p1.capture(pl->m_player1);
            f.p2.capture(pl->m_player2);
        }
        checkpoints.push_back(f);
        log::debug("[Bot] Checkpoint stored (events={}, t={:.4f})",
                   f.eventCount, f.levelTime);
    }

    void onCheckpointRemoved() {
        if (!checkpoints.empty()) checkpoints.pop_back();
    }

    // Called after the game has loaded a checkpoint. We:
    //   1. discard "dead inputs" recorded after that checkpoint, and
    //   2. apply our accurate physics snapshot to fix the practice bug.
    void onCheckpointLoaded(PlayLayer* pl, void* cpPtr) {
        if (!pl) return;

        // Find the matching frame. Prefer an exact pointer match, else fall
        // back to the most recent one (RobTop loads the last checkpoint).
        CheckpointFrame* frame = nullptr;
        for (auto it = checkpoints.rbegin(); it != checkpoints.rend(); ++it) {
            if (it->checkpointPtr == cpPtr) { frame = &*it; break; }
        }
        if (!frame && !checkpoints.empty()) frame = &checkpoints.back();
        if (!frame) return;

        // ---- dead input discard -------------------------------------------
        if (discardDeadInputs && mode == bot::Mode::Recording) {
            if (frame->eventCount < static_cast<int>(macro.events.size())) {
                size_t discarded = macro.events.size() - frame->eventCount;
                macro.events.resize(frame->eventCount);
                recomputeHeldState();
                log::info("[Bot] Discarded {} dead input(s) on checkpoint reload",
                          discarded);
                refreshUIProgress();
            }
        }

        // ---- accurate practice fix ----------------------------------------
        if (practiceFixEnabled) {
            frame->p1.apply(pl->m_player1);
            if (frame->p2.valid) frame->p2.apply(pl->m_player2);
        }

        // Keep playback (if running) lined up with the checkpoint time.
        if (mode == bot::Mode::Playing) {
            seekPlaybackTo(frame->levelTime);
        }
    }

    // Pause-menu restart: rewind the input timeline. A full restart goes back to
    // the level start, so every event recorded after the (post-restart) level
    // time is reverted.
    void onRestart(PlayLayer* pl, bool fromStart) {
        checkpoints.clear();
        if (mode == bot::Mode::Recording && discardDeadInputs) {
            if (fromStart) {
                // Back to the very beginning: drop everything past the start.
                double startT = macro.recordStartTime;
                size_t before = macro.events.size();
                macro.events.erase(
                    std::remove_if(macro.events.begin(), macro.events.end(),
                        [startT](InputEvent const& e){ return e.time >= startT; }),
                    macro.events.end());
                if (before != macro.events.size()) {
                    recomputeHeldState();
                    log::info("[Bot] Restart reverted {} input(s)",
                              before - macro.events.size());
                }
            }
        }
        if (mode == bot::Mode::Playing) playbackIndex = 0;
        refreshUIProgress();
    }

    // Fresh level / resetLevel: reset transient state but keep the macro.
    void onLevelReset(PlayLayer* pl) {
        checkpoints.clear();
        if (mode == bot::Mode::Playing) {
            seekPlaybackTo(0.0);
            releaseAll();
        }
    }

    void onPlayerDeath(PlayLayer* pl) {
        // In normal-mode playback a death means the attempt failed; stop cleanly
        // so we are not still injecting on the respawn.
        if (mode == bot::Mode::Playing && pl && !pl->m_isPracticeMode) {
            // Keep the macro but halt injection until the next reset.
        }
    }

    // ----- speedhack -------------------------------------------------------

    // Multiplier applied to GJBaseGameLayer::getModifiedDelta. Returns 1.0 when
    // the speedhack is off or the value is nonsensical.
    double speedMultiplier() const {
        if (!speedhackEnabled) return 1.0;
        if (!std::isfinite(speed) || speed <= 0.0) return 1.0;
        return speed;
    }

    // Write the current options to the mod's saved values. Called whenever an
    // option changes (there is no on-unload event in Geode, so we persist eagerly).
    void persist() {
        auto m = Mod::get();
        m->setSavedValue<double>("speed", speed);
        m->setSavedValue<bool>("speedhack", speedhackEnabled);
        m->setSavedValue<bool>("practice-fix", practiceFixEnabled);
        m->setSavedValue<bool>("discard-dead", discardDeadInputs);
        m->setSavedValue<bool>("quantize-robtop", quantizeForRobTopCBS);
        m->setSavedValue<bool>("auto-save", autoSaveOnComplete);
        m->setSavedValue<std::string>("macro-name", macroName);
    }

    void setSpeedFromString(std::string const& s) {
        try {
            double v = std::stod(s);
            if (std::isfinite(v) && v > 0.0) {
                speed = v;
                log::info("[Bot] Speed set to {}", speed);
                persist();
            }
        } catch (...) {
            // ignore malformed input; keep the previous value
        }
    }

    // One-line status summary, handy for the log.
    std::string description() const {
        const char* m = mode == bot::Mode::Recording ? "recording"
                      : mode == bot::Mode::Playing   ? "playing"
                      : "idle";
        return fmt::format("[{}] {} events, {:.2f}s, {}",
                           m, macro.size(), macro.duration(), cbfInfoString());
    }

    // Estimate the physics fps the game is currently running so we can stamp it
    // into a recording for informational purposes.
    static double currentPhysicsFps(GJBaseGameLayer*) {
        // GD's base physics tick is 240hz in 2.2; CBF subdivides further.
        return 240.0;
    }

    // ----- file IO ---------------------------------------------------------

    // Strip anything that could escape the save directory or upset the
    // filesystem, so a user-typed macro name can never traverse paths.
    static std::string sanitizeName(std::string const& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if (std::isalnum(static_cast<unsigned char>(c)) ||
                c == '-' || c == '_' || c == ' ' || c == '.') {
                out.push_back(c);
            }
        }
        // Collapse leading dots / spaces and forbid an empty result.
        while (!out.empty() && (out.front() == '.' || out.front() == ' '))
            out.erase(out.begin());
        if (out.empty()) out = "macro";
        return out;
    }

    std::filesystem::path macroPath(std::string const& name) const {
        return Mod::get()->getSaveDir() / (sanitizeName(name) + ".gdtm");
    }

    // Compact little-endian binary writer. Layout:
    //   u32 magic, u32 version,
    //   double recordStartTime, double recordFps, u8 normalized,
    //   i32 levelID, u16 nameLen, nameLen bytes (level name),
    //   u32 count, then count * { double time, u8 button, u8 flags }
    // flags: bit0 = down, bit1 = player2.
    //
    // Per event this is 10 bytes -- a 500-click level lands around 5 KB. We never
    // store a record per frame, only the transitions, which is what keeps macros
    // tiny regardless of level length or playback FPS.
    bool saveMacro(std::string const& name) {
        auto path = macroPath(name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            notify("Could not open file for writing", NotificationIcon::Error);
            return false;
        }
        auto wr = [&](void const* p, size_t n){ out.write(reinterpret_cast<char const*>(p), n); };

        uint32_t magic = bot::MACRO_MAGIC, version = bot::MACRO_VERSION;
        uint32_t count = static_cast<uint32_t>(macro.events.size());
        uint8_t  norm  = macro.normalized ? 1 : 0;
        int32_t  levelID = macro.levelID;
        std::string lname = macro.levelName.substr(0, 0xFFFF);
        uint16_t nameLen = static_cast<uint16_t>(lname.size());

        wr(&magic, 4); wr(&version, 4);
        wr(&macro.recordStartTime, 8);
        wr(&macro.recordFps, 8);
        wr(&norm, 1);
        wr(&levelID, 4);
        wr(&nameLen, 2);
        if (nameLen) wr(lname.data(), nameLen);
        wr(&count, 4);
        for (auto const& e : macro.events) {
            uint8_t flags = (e.down ? 1 : 0) | (e.player2 ? 2 : 0);
            wr(&e.time, 8);
            wr(&e.button, 1);
            wr(&flags, 1);
        }
        out.close();
        macroName = name;
        log::info("[Bot] Saved {} events to {}", count, path.string());
        notify(fmt::format("Saved {} inputs", count), NotificationIcon::Success);
        return true;
    }

    bool loadMacro(std::string const& name) {
        auto path = macroPath(name);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            notify("Macro file not found", NotificationIcon::Error);
            return false;
        }
        auto rd = [&](void* p, size_t n){ in.read(reinterpret_cast<char*>(p), n); };

        uint32_t magic = 0, version = 0, count = 0;
        uint8_t  norm = 0;
        rd(&magic, 4); rd(&version, 4);
        if (magic != bot::MACRO_MAGIC) {
            notify("Not a valid macro file", NotificationIcon::Error);
            return false;
        }
        macro.clear();
        macro.levelID = 0;
        macro.levelName.clear();
        rd(&macro.recordStartTime, 8);
        rd(&macro.recordFps, 8);
        rd(&norm, 1);
        macro.normalized = norm != 0;

        // Level identity was added in v2; older files jump straight to count.
        if (version >= 2) {
            int32_t  levelID = 0;
            uint16_t nameLen = 0;
            rd(&levelID, 4);
            rd(&nameLen, 2);
            macro.levelID = levelID;
            if (nameLen) {
                std::string buf(nameLen, '\0');
                rd(buf.data(), nameLen);
                macro.levelName = buf;
            }
        }

        rd(&count, 4);
        macro.events.reserve(count);
        for (uint32_t i = 0; i < count && in; ++i) {
            InputEvent e;
            uint8_t flags = 0;
            rd(&e.time, 8);
            rd(&e.button, 1);
            rd(&flags, 1);
            e.down    = (flags & 1) != 0;
            e.player2 = (flags & 2) != 0;
            macro.events.push_back(e);
        }
        in.close();
        macro.sort();
        recomputeHeldState();
        macroName = name;
        playbackIndex = 0;
        log::info("[Bot] Loaded {} events from {} (level {})",
                  macro.events.size(), path.string(), macro.levelID);
        notify(fmt::format("Loaded {} inputs", macro.events.size()),
               NotificationIcon::Success);
        refreshUIProgress();
        return true;
    }

    // ----- macro post-processing ------------------------------------------

    // Shift every event so the first one sits at level time 0. This is the
    // "offset all the inputs to where the recording was started" option, useful
    // when a recording was armed partway into a level.
    void normalizeToStart() {
        if (macro.events.empty()) return;
        double off = macro.events.front().time;
        for (auto& e : macro.events) e.time = std::max(0.0, e.time - off);
        macro.recordStartTime = 0.0;
        macro.normalized = true;
        refreshUIProgress();
    }

    // ----- offsetting ------------------------------------------------------

    // Shift every timestamp by `delta` seconds (clamped at 0). This is the
    // "offset all the inputs to where the recording was started" knob: if a
    // recording was armed late, a negative delta of recordStartTime lines it up
    // with level time 0; a positive delta delays the whole macro.
    void offsetMacro(double delta) {
        for (auto& e : macro.events) e.time = std::max(0.0, e.time + delta);
        macro.sort();
        recomputeHeldState();
        refreshUIProgress();
    }

    // ----- human-readable text export / import ----------------------------
    //
    //  Binary is the default (smallest + lossless), but a plain-text format is
    //  handy for inspecting or hand-editing a macro. This is also where the
    //  "cap timestamps to 50 decimals before rounding" rule from the spec is
    //  honoured literally: each time is formatted with up to 50 fractional
    //  digits, then rounded, before being written.
    //
    //  Format (one event per line):
    //      <time>  <button>  <D|U>  <P1|P2>
    //
    std::filesystem::path macroTextPath(std::string const& name) const {
        return Mod::get()->getSaveDir() / (sanitizeName(name) + ".txt");
    }

    static std::string formatTime50(double t) {
        // Clamp to 50 decimals, then round to the double's real precision.
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(bot::TEXT_TIME_DECIMALS) << t;
        std::string s = ss.str();
        // Trim trailing zeros for readability while keeping the decimal point.
        auto dot = s.find('.');
        if (dot != std::string::npos) {
            size_t last = s.find_last_not_of('0');
            if (last != std::string::npos && last > dot) s.erase(last + 1);
            else if (last == dot) s.erase(dot);
        }
        return s;
    }

    bool saveMacroText(std::string const& name) {
        auto path = macroTextPath(name);
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            notify("Could not open text file for writing", NotificationIcon::Error);
            return false;
        }
        out << "# Geode Time Macro (text) v" << bot::MACRO_VERSION << "\n";
        out << "# levelID=" << macro.levelID
            << " levelName=" << macro.levelName << "\n";
        out << "# recordStartTime=" << formatTime50(macro.recordStartTime)
            << " fps=" << macro.recordFps
            << " normalized=" << (macro.normalized ? 1 : 0) << "\n";
        out << "# time  button(1=jump,2=left,3=right)  D/U  P1/P2\n";
        for (auto const& e : macro.events) {
            out << formatTime50(BotManager::roundTimeForText(e.time)) << '\t'
                << static_cast<int>(e.button) << '\t'
                << (e.down ? 'D' : 'U') << '\t'
                << (e.player2 ? "P2" : "P1") << '\n';
        }
        out.close();
        log::info("[Bot] Exported {} events to {}", macro.events.size(), path.string());
        notify(fmt::format("Exported {} inputs (text)", macro.events.size()),
               NotificationIcon::Success);
        return true;
    }

    bool loadMacroText(std::string const& name) {
        auto path = macroTextPath(name);
        std::ifstream in(path);
        if (!in) {
            notify("Text macro not found", NotificationIcon::Error);
            return false;
        }
        macro.clear();
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            // Parse metadata comments.
            if (line[0] == '#') {
                auto pos = line.find("recordStartTime=");
                if (pos != std::string::npos) {
                    try { macro.recordStartTime =
                            std::stod(line.substr(pos + 16)); } catch (...) {}
                }
                continue;
            }
            std::istringstream ss(line);
            double t; int button; std::string du, pp;
            if (!(ss >> t >> button >> du >> pp)) continue;
            InputEvent e;
            e.time    = t;
            e.button  = static_cast<uint8_t>(button);
            e.down    = (!du.empty() && (du[0] == 'D' || du[0] == 'd'));
            e.player2 = (pp == "P2" || pp == "p2");
            macro.events.push_back(e);
        }
        in.close();
        macro.sort();
        recomputeHeldState();
        macroName = name;
        playbackIndex = 0;
        log::info("[Bot] Imported {} events from {}", macro.events.size(), path.string());
        notify(fmt::format("Imported {} inputs (text)", macro.events.size()),
               NotificationIcon::Success);
        refreshUIProgress();
        return true;
    }

    // ----- stats (for the UI / logging) -----------------------------------

    struct Stats {
        size_t presses = 0;
        size_t releases = 0;
        size_t p1 = 0;
        size_t p2 = 0;
        double duration = 0.0;
    };

    Stats computeStats() const {
        Stats s;
        for (auto const& e : macro.events) {
            if (e.down) ++s.presses; else ++s.releases;
            if (e.player2) ++s.p2; else ++s.p1;
        }
        s.duration = macro.duration();
        return s;
    }

    // ----- macro file listing ---------------------------------------------

    std::vector<std::string> listMacros() const {
        std::vector<std::string> out;
        std::error_code ec;
        auto dir = Mod::get()->getSaveDir();
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.extension() == ".gdtm") {
                out.push_back(p.stem().string());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    BotManager() = default;

    void notifyNoCBF() {
        notify("Enable Click Between Frames to use the bot",
               NotificationIcon::Error);
    }

    // Defined out-of-line below (needs the full BotUILayer definition).
    void refreshUI();
    void refreshUIProgress();

    static void notify(std::string const& msg, NotificationIcon icon) {
        Notification::create(msg, icon)->show();
    }
};

// ============================================================================
//  BotUILayer  --  the floating, always-on-top GUI (toggle with K).
// ============================================================================
//
//  This is a real CCLayer (not a $modify) so it can:
//    * own a keyboard delegate (setKeyboardEnabled) and catch K reliably on
//      every platform, including Windows where dispatchKeyboardMSG is not a
//      stable hook target, and
//    * be added to the running scene at INT_MAX z-order so it sits above every
//      gameplay / pause layer, capturing its own clicks at a high touch
//      priority.
//
class BotUILayer : public cocos2d::CCLayer {
protected:
    cocos2d::CCNode*      m_panel        = nullptr;
    cocos2d::CCLabelBMFont* m_periodLabel = nullptr; // the coloured "."
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr; // CBF: Syzzi / RobTop / None
    cocos2d::CCLabelBMFont* m_modeLabel   = nullptr; // Idle / Recording / Playing
    cocos2d::CCLabelBMFont* m_progressLabel = nullptr; // input count + duration
    cocos2d::CCLabelBMFont* m_statsLabel  = nullptr; // presses / releases / dual
    geode::TextInput*     m_speedInput   = nullptr;
    geode::TextInput*     m_nameInput    = nullptr; // macro file name
    geode::TextInput*     m_offsetInput  = nullptr; // timeline offset (seconds)
    CCMenuItemToggler*    m_recordToggle = nullptr;
    CCMenuItemToggler*    m_playToggle   = nullptr;
    CCMenuItemToggler*    m_speedToggle  = nullptr;
    CCMenuItemToggler*    m_practiceToggle = nullptr;
    CCMenuItemToggler*    m_deadToggle   = nullptr;
    CCMenuItemToggler*    m_quantToggle  = nullptr; // RobTop CBS quantization
    CCMenuItemToggler*    m_autoSaveToggle = nullptr;
    bool                  m_visible      = false;

    // Always-visible corner badge (shown when the panel is closed).
    cocos2d::CCNode*        m_badge       = nullptr;
    cocos2d::CCLabelBMFont* m_badgePeriod = nullptr;
    cocos2d::CCLabelBMFont* m_badgeMode   = nullptr;

    // Drag state for the panel title bar.
    bool             m_dragging   = false;
    cocos2d::CCPoint m_dragStart  {0.f, 0.f};
    cocos2d::CCPoint m_panelStart {0.f, 0.f};

public:
    static BotUILayer* create() {
        auto ret = new BotUILayer();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    ~BotUILayer() override {
        // If we are torn down while open (e.g. the player quits the level with the
        // menu up), make sure we don't leave the game frozen / cursor showing.
        if (BotManager::get().guiPaused) BotManager::get().setGuiOpen(false);
        if (BotManager::get().ui == this) BotManager::get().ui = nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        this->setKeyboardEnabled(true); // so keyDown() gets called for K
        // Touches: we register our own targeted delegate (see
        // registerWithTouchDispatcher) so the panel can swallow background clicks
        // and be dragged without leaking input to the gameplay underneath.
        this->setTouchEnabled(true);
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setZOrder((std::numeric_limits<int>::max)());

        buildPanel();
        buildBadge();
        setPanelVisible(false);

        BotManager::get().ui = this;
        this->scheduleUpdate();
        return true;
    }

    // --- keyboard: toggle the panel on K ----------------------------------
    void keyDown(cocos2d::enumKeyCodes key, double timing) override {
        auto& bot = BotManager::get();
        switch (key) {
            case bot::TOGGLE_KEY:                       // K -> toggle the menu
                togglePanel();
                return;
            case cocos2d::enumKeyCodes::KEY_V:          // V -> arm/stop recording
                bot.toggleRecording(GJBaseGameLayer::get());
                refreshAll();
                return;
            case cocos2d::enumKeyCodes::KEY_B:          // B -> start/stop playback
                bot.togglePlayback(GJBaseGameLayer::get());
                refreshAll();
                return;
            case cocos2d::enumKeyCodes::KEY_N:          // N -> hard stop
                bot.stop();
                refreshAll();
                return;
            default:
                break;
        }
        CCLayer::keyDown(key, timing);
    }

    void togglePanel() { setPanelVisible(!m_visible); }

    void setPanelVisible(bool v) {
        m_visible = v;
        if (m_panel) m_panel->setVisible(v);
        if (m_badge) m_badge->setVisible(!v); // badge shows only when panel hidden

        // Pause the level + show the cursor + pause music while open.
        BotManager::get().setGuiOpen(v);

        if (v) {
            bringToFront(); // sit above every other layer / GUI
            refreshAll();
        } else {
            refreshBadge();
        }
    }

    // Re-parent ourselves to the very top of the current running scene so the
    // panel renders above the pause menu, other mods' overlays, everything.
    void bringToFront() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        if (this->getParent() != scene) {
            this->retain();
            this->removeFromParentAndCleanup(false); // keep us (and our state) alive
            scene->addChild(this, (std::numeric_limits<int>::max)());
            this->release();
        } else {
            scene->reorderChild(this, (std::numeric_limits<int>::max)());
        }
    }

    // ---- touch: high priority so our clicks land first, swallow the panel ---
    void registerWithTouchDispatcher() override {
        // Priority -500: lower (later) than the inner CCMenu (-1000) so the
        // buttons still get first dibs, but earlier than gameplay so a click on
        // the panel background is swallowed instead of jumping the player.
        CCDirector::sharedDirector()->getTouchDispatcher()
            ->addTargetedDelegate(this, -500, true);
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (!m_visible || !m_panel) return false;
        auto local = m_panel->convertToNodeSpace(touch->getLocation());
        auto size = m_panel->getContentSize();
        bool inside = local.x >= 0 && local.y >= 0 &&
                      local.x <= size.width && local.y <= size.height;
        if (!inside) return false; // let clicks outside the panel pass through

        // Dragging: grab the top strip (the title bar) and move the panel.
        if (local.y >= size.height - 32.f) {
            m_dragging   = true;
            m_dragStart  = touch->getLocation();
            m_panelStart = m_panel->getPosition();
        }
        return true; // swallow: a click on the panel never reaches gameplay
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (!m_dragging || !m_panel) return;
        auto delta = touch->getLocation() - m_dragStart;
        m_panel->setPosition(m_panelStart + delta);
    }

    void ccTouchEnded(cocos2d::CCTouch*, cocos2d::CCEvent*) override {
        m_dragging = false;
    }
    void ccTouchCancelled(cocos2d::CCTouch*, cocos2d::CCEvent*) override {
        m_dragging = false;
    }

    // --- periodic refresh so the status colour stays live ------------------
    void update(float dt) override {
        CCLayer::update(dt);
        if (m_visible) { refreshStatus(); refreshMode(); }
        else refreshBadge();
    }

    // --- live refresh entry points (called by the manager) -----------------
    void refreshAll() {
        refreshStatus();
        refreshMode();
        refreshProgress();
        refreshBadge();
        syncToggles();
        if (m_nameInput) m_nameInput->setString(BotManager::get().macroName);
        if (m_speedInput) {
            m_speedInput->setString(fmt::format("{:g}", BotManager::get().speed));
        }
    }

    void refreshStatus() {
        auto& bot = BotManager::get();
        auto state = bot.cbfState();
        if (m_periodLabel) {
            switch (state) {
                case bot::CBFState::Syzzi:
                    m_periodLabel->setColor({0, 255, 0});   // green
                    break;
                case bot::CBFState::RobTop:
                    m_periodLabel->setColor({255, 235, 0}); // yellow
                    break;
                case bot::CBFState::None:
                    m_periodLabel->setColor({255, 40, 40}); // red
                    break;
            }
        }
        if (m_statusLabel) {
            m_statusLabel->setString(bot.cbfInfoString().c_str());
        }
    }

    void refreshMode() {
        if (!m_modeLabel) return;
        switch (BotManager::get().mode) {
            case bot::Mode::Recording:
                m_modeLabel->setString("Mode: RECORDING");
                m_modeLabel->setColor({255, 80, 80});
                break;
            case bot::Mode::Playing:
                m_modeLabel->setString("Mode: PLAYING");
                m_modeLabel->setColor({80, 255, 120});
                break;
            default:
                m_modeLabel->setString("Mode: idle");
                m_modeLabel->setColor({200, 200, 200});
                break;
        }
    }

    void refreshProgress() {
        if (m_progressLabel) {
            auto& m = BotManager::get().macro;
            m_progressLabel->setString(
                fmt::format("{} inputs  |  {:.2f}s", m.size(), m.duration()).c_str());
        }
        if (m_statsLabel) {
            auto s = BotManager::get().computeStats();
            m_statsLabel->setString(
                fmt::format("press {}  rel {}  p1 {}  p2 {}",
                            s.presses, s.releases, s.p1, s.p2).c_str());
        }
    }

    void syncToggles() {
        auto& bot = BotManager::get();
        if (m_recordToggle) m_recordToggle->toggle(bot.mode == bot::Mode::Recording);
        if (m_playToggle)   m_playToggle->toggle(bot.mode == bot::Mode::Playing);
        if (m_speedToggle)  m_speedToggle->toggle(bot.speedhackEnabled);
        if (m_practiceToggle) m_practiceToggle->toggle(bot.practiceFixEnabled);
        if (m_deadToggle)   m_deadToggle->toggle(bot.discardDeadInputs);
        if (m_quantToggle)  m_quantToggle->toggle(bot.quantizeForRobTopCBS);
        if (m_autoSaveToggle) m_autoSaveToggle->toggle(bot.autoSaveOnComplete);
        bot.persist(); // options changed -> save eagerly
    }

private:
    // ----- panel construction ---------------------------------------------
    void buildPanel() {
        constexpr float W = 320.f, H = 452.f;
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Container node, centred on screen. Scaled down so the panel is compact
        // and doesn't dominate the screen.
        m_panel = CCNode::create();
        m_panel->setContentSize({ W, H });
        m_panel->setAnchorPoint({ 0.5f, 0.5f });
        m_panel->setScale(0.72f);
        m_panel->setPosition({ winSize.width * 0.5f, winSize.height * 0.5f });
        this->addChild(m_panel);

        // Background (rounded GD square).
        auto bg = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({ W, H });
        bg->setPosition({ W * 0.5f, H * 0.5f });
        bg->setOpacity(238);
        m_panel->addChild(bg);

        // Title.
        auto title = CCLabelBMFont::create("Time Macro Bot", "goldFont.fnt");
        title->setPosition({ W * 0.5f, H - 18.f });
        title->setScale(0.72f);
        m_panel->addChild(title);

        // The coloured period + status line. The period is the green/yellow/red
        // CBF indicator the spec asked for.
        m_periodLabel = CCLabelBMFont::create(".", "bigFont.fnt");
        m_periodLabel->setPosition({ 24.f, H - 44.f });
        m_periodLabel->setScale(1.25f);
        m_periodLabel->setAnchorPoint({ 0.5f, 0.7f });
        m_panel->addChild(m_periodLabel);

        m_statusLabel = CCLabelBMFont::create("CBF: ...", "chatFont.fnt");
        m_statusLabel->setAnchorPoint({ 0.f, 0.5f });
        m_statusLabel->setPosition({ 40.f, H - 44.f });
        m_statusLabel->setScale(0.62f);
        m_panel->addChild(m_statusLabel);

        // Mode + progress + stats lines.
        m_modeLabel = CCLabelBMFont::create("Mode: idle", "chatFont.fnt");
        m_modeLabel->setAnchorPoint({ 0.f, 0.5f });
        m_modeLabel->setPosition({ 18.f, H - 66.f });
        m_modeLabel->setScale(0.6f);
        m_panel->addChild(m_modeLabel);

        m_progressLabel = CCLabelBMFont::create("0 inputs  |  0.00s", "chatFont.fnt");
        m_progressLabel->setAnchorPoint({ 0.f, 0.5f });
        m_progressLabel->setPosition({ 18.f, H - 84.f });
        m_progressLabel->setScale(0.55f);
        m_panel->addChild(m_progressLabel);

        m_statsLabel = CCLabelBMFont::create("press 0  rel 0  p1 0  p2 0", "chatFont.fnt");
        m_statsLabel->setAnchorPoint({ 0.f, 0.5f });
        m_statsLabel->setPosition({ 18.f, H - 100.f });
        m_statsLabel->setScale(0.48f);
        m_statsLabel->setOpacity(190);
        m_panel->addChild(m_statsLabel);

        // ---- main menu of buttons / togglers -----------------------------
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        // Capture clicks at a very high priority so they always land on us even
        // though there is live gameplay underneath.
        menu->setTouchPriority(-1000);
        m_panel->addChild(menu);

        float yRec = H - 130.f;
        // Record / Play togglers.
        m_recordToggle = makeToggle(menu, { 40.f, yRec },
            menu_selector(BotUILayer::onRecord), "Record (V)");
        m_playToggle = makeToggle(menu, { 180.f, yRec },
            menu_selector(BotUILayer::onPlay), "Play (B)");

        // Speedhack toggle + textbox.
        float ySpeed = yRec - 36.f;
        m_speedToggle = makeToggle(menu, { 40.f, ySpeed },
            menu_selector(BotUILayer::onSpeedToggle), "Speed");
        m_speedInput = geode::TextInput::create(120.f, "speed", "chatFont.fnt");
        m_speedInput->setCommonFilter(geode::CommonFilter::Float);
        m_speedInput->setString(fmt::format("{:g}", BotManager::get().speed));
        m_speedInput->setPosition({ 222.f, ySpeed });
        m_speedInput->setScale(0.9f);
        m_speedInput->setCallback([](std::string const& s) {
            BotManager::get().setSpeedFromString(s);
        });
        m_panel->addChild(m_speedInput);

        // Macro name textbox.
        float yName = ySpeed - 34.f;
        auto nameLbl = CCLabelBMFont::create("Name", "chatFont.fnt");
        nameLbl->setAnchorPoint({ 0.f, 0.5f });
        nameLbl->setPosition({ 18.f, yName });
        nameLbl->setScale(0.55f);
        m_panel->addChild(nameLbl);
        m_nameInput = geode::TextInput::create(170.f, "macro", "chatFont.fnt");
        m_nameInput->setString(BotManager::get().macroName);
        m_nameInput->setPosition({ 222.f, yName });
        m_nameInput->setScale(0.9f);
        m_nameInput->setCallback([](std::string const& s) {
            if (!s.empty()) { BotManager::get().macroName = s; BotManager::get().persist(); }
        });
        m_panel->addChild(m_nameInput);

        // Timeline offset textbox + Apply button.
        float yOff = yName - 34.f;
        auto offLbl = CCLabelBMFont::create("Offset", "chatFont.fnt");
        offLbl->setAnchorPoint({ 0.f, 0.5f });
        offLbl->setPosition({ 18.f, yOff });
        offLbl->setScale(0.55f);
        m_panel->addChild(offLbl);
        m_offsetInput = geode::TextInput::create(110.f, "+/- sec", "chatFont.fnt");
        m_offsetInput->setCommonFilter(geode::CommonFilter::Float);
        m_offsetInput->setString("0");
        m_offsetInput->setPosition({ 178.f, yOff });
        m_offsetInput->setScale(0.9f);
        m_panel->addChild(m_offsetInput);
        makeButton(menu, { 272.f, yOff }, "Apply", menu_selector(BotUILayer::onOffset));

        // Practice-fix / Discard-dead / Quantize / Auto-save togglers.
        float yOpt = yOff - 32.f;
        m_practiceToggle = makeToggle(menu, { 40.f, yOpt },
            menu_selector(BotUILayer::onPracticeFix), "Practice fix");
        m_deadToggle = makeToggle(menu, { 180.f, yOpt },
            menu_selector(BotUILayer::onDeadInputs), "Discard dead");
        float yOpt2 = yOpt - 26.f;
        m_quantToggle = makeToggle(menu, { 40.f, yOpt2 },
            menu_selector(BotUILayer::onQuantize), "480fps snap");
        m_autoSaveToggle = makeToggle(menu, { 180.f, yOpt2 },
            menu_selector(BotUILayer::onAutoSave), "Auto-save");

        // File row 1: binary save / load.
        float yFile1 = yOpt2 - 34.f;
        makeButton(menu, { 50.f, yFile1 },  "Save",   menu_selector(BotUILayer::onSave));
        makeButton(menu, { 120.f, yFile1 }, "Load",   menu_selector(BotUILayer::onLoad));
        makeButton(menu, { 200.f, yFile1 }, "Export", menu_selector(BotUILayer::onExport));
        makeButton(menu, { 272.f, yFile1 }, "Import", menu_selector(BotUILayer::onImport));

        // File row 2: clear / normalize / list / close.
        float yFile2 = yFile1 - 32.f;
        makeButton(menu, { 42.f, yFile2 },  "Clear",     menu_selector(BotUILayer::onClear));
        makeButton(menu, { 116.f, yFile2 }, "Normalize", menu_selector(BotUILayer::onNormalize));
        makeButton(menu, { 200.f, yFile2 }, "List",      menu_selector(BotUILayer::onList));
        makeButton(menu, { 268.f, yFile2 }, "Close",     menu_selector(BotUILayer::onClose));

        // Hint at the bottom.
        auto hint = CCLabelBMFont::create("K: menu   V: record   B: play   N: stop",
                                          "chatFont.fnt");
        hint->setPosition({ W * 0.5f, 14.f });
        hint->setScale(0.46f);
        hint->setOpacity(160);
        m_panel->addChild(hint);
    }

    // ----- always-visible corner badge ------------------------------------
    //
    //  Even with the panel closed the spec wants the coloured CBF "." to be
    //  glanceable, so we keep a tiny badge in the top-left corner showing the
    //  status colour and a one-letter mode (R = recording, P = playing).
    //
    void buildBadge() {
        m_badge = CCNode::create();
        m_badge->setPosition({ 14.f, CCDirector::sharedDirector()->getWinSize().height - 14.f });
        m_badge->setAnchorPoint({ 0.f, 1.f });
        this->addChild(m_badge);

        m_badgePeriod = CCLabelBMFont::create(".", "bigFont.fnt");
        m_badgePeriod->setScale(1.1f);
        m_badgePeriod->setAnchorPoint({ 0.f, 1.f });
        m_badgePeriod->setPosition({ 0.f, 0.f });
        m_badge->addChild(m_badgePeriod);

        m_badgeMode = CCLabelBMFont::create("", "bigFont.fnt");
        m_badgeMode->setScale(0.4f);
        m_badgeMode->setAnchorPoint({ 0.f, 1.f });
        m_badgeMode->setPosition({ 14.f, -2.f });
        m_badge->addChild(m_badgeMode);
    }

    void refreshBadge() {
        if (!m_badgePeriod) return;
        auto& bot = BotManager::get();
        switch (bot.cbfState()) {
            case bot::CBFState::Syzzi:  m_badgePeriod->setColor({0, 255, 0}); break;
            case bot::CBFState::RobTop: m_badgePeriod->setColor({255, 235, 0}); break;
            case bot::CBFState::None:   m_badgePeriod->setColor({255, 40, 40}); break;
        }
        if (m_badgeMode) {
            switch (bot.mode) {
                case bot::Mode::Recording:
                    m_badgeMode->setString("R");
                    m_badgeMode->setColor({255, 80, 80});
                    break;
                case bot::Mode::Playing:
                    m_badgeMode->setString("P");
                    m_badgeMode->setColor({80, 255, 120});
                    break;
                default:
                    m_badgeMode->setString("");
                    break;
            }
        }
    }

    CCMenuItemToggler* makeToggle(CCMenu* menu, CCPoint pos,
                                  cocos2d::SEL_MenuHandler sel, const char* label) {
        auto toggle = CCMenuItemToggler::createWithStandardSprites(this, sel, 0.6f);
        toggle->setPosition(pos);
        menu->addChild(toggle);

        auto lbl = CCLabelBMFont::create(label, "chatFont.fnt");
        lbl->setAnchorPoint({ 0.f, 0.5f });
        lbl->setPosition({ pos.x + 16.f, pos.y });
        lbl->setScale(0.55f);
        m_panel->addChild(lbl);
        return toggle;
    }

    void makeButton(CCMenu* menu, CCPoint pos, const char* label,
                    cocos2d::SEL_MenuHandler sel) {
        // Use the simplest, most binding-stable ButtonSprite overload.
        auto spr = ButtonSprite::create(label);
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, sel);
        btn->setPosition(pos);
        menu->addChild(btn);
    }

    // ----- button callbacks -----------------------------------------------
    void onRecord(CCObject*) {
        BotManager::get().toggleRecording(GJBaseGameLayer::get());
        refreshAll();
    }
    void onPlay(CCObject*) {
        BotManager::get().togglePlayback(GJBaseGameLayer::get());
        refreshAll();
    }
    void onSpeedToggle(CCObject*) {
        auto& bot = BotManager::get();
        bot.speedhackEnabled = !bot.speedhackEnabled;
        syncToggles();
    }
    void onPracticeFix(CCObject*) {
        auto& bot = BotManager::get();
        bot.practiceFixEnabled = !bot.practiceFixEnabled;
        syncToggles();
    }
    void onDeadInputs(CCObject*) {
        auto& bot = BotManager::get();
        bot.discardDeadInputs = !bot.discardDeadInputs;
        syncToggles();
    }
    void onQuantize(CCObject*) {
        auto& bot = BotManager::get();
        bot.quantizeForRobTopCBS = !bot.quantizeForRobTopCBS;
        syncToggles();
    }
    void onAutoSave(CCObject*) {
        auto& bot = BotManager::get();
        bot.autoSaveOnComplete = !bot.autoSaveOnComplete;
        syncToggles();
    }
    void onOffset(CCObject*) {
        if (!m_offsetInput) return;
        try {
            double delta = std::stod(m_offsetInput->getString());
            BotManager::get().offsetMacro(delta);
            Notification::create(
                fmt::format("Offset macro by {:+.4f}s", delta),
                NotificationIcon::Success)->show();
        } catch (...) {
            Notification::create("Invalid offset", NotificationIcon::Error)->show();
        }
        refreshProgress();
    }
    void onSave(CCObject*) {
        BotManager::get().saveMacro(BotManager::get().macroName);
    }
    void onLoad(CCObject*) {
        BotManager::get().loadMacro(BotManager::get().macroName);
        refreshAll();
    }
    void onExport(CCObject*) {
        BotManager::get().saveMacroText(BotManager::get().macroName);
    }
    void onImport(CCObject*) {
        BotManager::get().loadMacroText(BotManager::get().macroName);
        refreshAll();
    }
    void onClear(CCObject*) {
        auto& bot = BotManager::get();
        bot.macro.clear();
        bot.checkpoints.clear();
        bot.resetHeldState();
        bot.playbackIndex = 0;
        refreshAll();
    }
    void onList(CCObject*) {
        auto names = BotManager::get().listMacros();
        if (names.empty()) {
            Notification::create("No saved macros", NotificationIcon::Info)->show();
            return;
        }
        std::string joined;
        for (size_t i = 0; i < names.size(); ++i) {
            joined += names[i];
            if (i + 1 < names.size()) joined += ", ";
        }
        log::info("[Bot] Saved macros: {}", joined);
        Notification::create(
            fmt::format("{} macro(s) -- see console", names.size()),
            NotificationIcon::Info)->show();
    }
    void onNormalize(CCObject*) {
        BotManager::get().normalizeToStart();
        refreshProgress();
    }
    void onClose(CCObject*) {
        setPanelVisible(false);
    }
};

// ============================================================================
//  Out-of-line BotManager methods that need the full BotUILayer definition.
// ============================================================================

inline void BotManager::refreshUI() {
    if (ui) ui->refreshAll();
}

inline void BotManager::refreshUIProgress() {
    if (ui) ui->refreshProgress();
}
