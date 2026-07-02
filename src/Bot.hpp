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
 *    * Work with Syzzi's Click Between Frames
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
#include <Geode/binding/DashRingObject.hpp>

#include <vector>
#include <array>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <limits>
#include <system_error>

#include <chrono>
#if defined(GEODE_IS_WINDOWS)
#include <windows.h>
#elif defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
#include <time.h>
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

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
//   4. THE RECORDING-START-OFFSET PROBLEM (handled automatically)
//   -------------------------------------------------------------
//   Recording does not have to begin at level time 0. If you arm a recording 0.2s
//   into a level, every captured event carries its true ABSOLUTE level time, so
//   replaying from the start lines up perfectly -- the first input simply fires at
//   t=0.2 as recorded, the bot waiting out that 0.2s delay on its own. There is no
//   manual offset control on purpose: the start delay is baked into the data, so
//   nothing the user could set can desync it.
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
    static constexpr uint32_t MACRO_VERSION = 6;  // v6: level-time based inputs (accurate)

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
    double  time     = 0.0;   // m_gameState.m_levelTime when the input fired
    uint8_t button   = 1;
    bool    down     = true;
    bool    player2  = false;

    InputEvent() = default;
    InputEvent(double t, uint8_t b, bool d, bool p2)
        : time(t), button(b), down(d), player2(p2) {}

    bool operator<(InputEvent const& o) const {
        if (time != o.time) return time < o.time;
        if (down != o.down) return down && !o.down;
        return button < o.button;
    }
};

// ============================================================================
//  PhysicsFrame  --  one snapshot of player position/velocity per physics step.
// ============================================================================
//
//  Used by the physics bot mode. Records the exact player state at each
//  physics step (240Hz with CBF). During playback, the player is teleported
//  to the recorded position/velocity, giving perfect accuracy regardless of
//  speed or timing.
//
struct PhysicsFrame {
    double time = 0.0;
    float  p1x = 0.f, p1y = 0.f;
    double p1yVel = 0.0;
    float  p2x = 0.f, p2y = 0.f;
    double p2yVel = 0.0;
    // Held button state at this frame (for physics-driven input playback)
    bool p1Jump = false, p1Left = false, p1Right = false;
    bool p2Jump = false, p2Left = false, p2Right = false;
};

// Directly set m_toggled AND button visibility. No reliance on toggle(bool),
// which doesn't work reliably on some Geode v5 builds. The binding shows
// the field is `m_toggled` and the buttons are `m_onButton`/`m_offButton`.
static inline void forceTogglerState(CCMenuItemToggler* t, bool on) {
    if (!t) return;
    t->m_toggled = on;
    // In GD's CCMenuItemToggler, m_onButton holds the UNCHECKED sprite and
    // m_offButton holds the CHECKED sprite — the opposite of what the names
    // imply. GD's own toggle(bool) uses setVisible(!toggled) / setVisible(toggled)
    // respectively. We mirror that exactly.
    if (t->m_onButton)  t->m_onButton->setVisible(on);
    if (t->m_offButton) t->m_offButton->setVisible(!on);
}
// ============================================================================
//  PlayerSnapshot  --  full physics state of one PlayerObject.
// ============================================================================
//
//  This is the heart of the accurate practice-bug fix. RobTop's own checkpoints
//  do not faithfully restore velocity and a handful of per-gamemode flags, which
//  is why vanilla practice can feel "off" after a respawn. We snapshot every
//  field that matters for every gamemode so a reload is bit-for-bit consistent.
// thanks silicate
struct PlayerSnapshot {
    bool valid = false;

    // Transform
    cocos2d::CCPoint m_position{0.f, 0.f};
    float m_ccRotation = 0.f;

    // Teleport / gravity / reverse
    bool m_wasTeleported = false;
    bool m_fixGravityBug = false;
    bool m_reverseSync = false;
    double m_yVelocityBeforeSlope = 0.0;

    // Dash
    double m_dashX = 0.0;
    double m_dashY = 0.0;
    double m_dashAngle = 0.0;
    double m_dashStartTime = 0.0;
    DashRingObject* m_dashRing = nullptr;

    // Slope
    double m_slopeStartTime = 0.0;
    bool m_justPlacedStreak = false;
    GameObject* m_maybeLastGroundObject = nullptr;
    int m_lastCollisionBottom = -1;
    int m_lastCollisionTop = -1;
    int m_lastCollisionLeft = -1;
    int m_lastCollisionRight = -1;
    int m_unk50C = -1;
    int m_unk510 = -1;
    GameObject* m_currentSlope2 = nullptr;
    GameObject* m_preLastGroundObject = nullptr;
    float m_slopeAngle = 0.f;
    bool m_slopeSlidingMaybeRotated = false;
    bool m_quickCheckpointMode = false;
    GameObject* m_collidedObject = nullptr;
    GameObject* m_lastGroundObject = nullptr;
    GameObject* m_collidingWithLeft = nullptr;
    GameObject* m_collidingWithRight = nullptr;
    int m_maybeSavedPlayerFrame = 0;
    double m_scaleXRelated2 = 0.0;
    double m_groundYVelocity = 0.0;
    double m_yVelocityRelated = 0.0;
    double m_scaleXRelated3 = 0.0;
    double m_scaleXRelated4 = 0.0;
    double m_scaleXRelated5 = 0.0;
    bool m_isCollidingWithSlope = false;
    bool m_isBallRotating = false;
    bool m_unk669 = false;
    GameObject* m_currentPotentialSlope = nullptr;
    GameObject* m_currentSlope = nullptr;
    double unk_584 = 0.0;
    int m_collidingWithSlopeId = 0;
    bool m_slopeFlipGravityRelated = false;
    cocos2d::CCArray* m_particleSystems = nullptr;
    float m_slopeAngleRadians = 0.f;
    float m_rotationSpeed = 0.f;
    float m_rotateSpeed = 1.0f;
    bool m_isRotating = false;
    bool m_isBallRotating2 = false;
    bool m_hasGlow = false;
    bool m_isHidden = false;

    // Physics
    double m_speedMultiplier = 0.0;
    double m_yStart = 0.0;
    double m_gravity = 0.0;
    float m_trailingParticleLife = 0.f;
    float m_unk648 = 0.f;
    double m_gameModeChangedTime = 0.0;
    bool m_padRingRelated = false;
    bool m_maybeReducedEffects = false;
    bool m_maybeIsFalling = false;
    bool m_shouldTryPlacingCheckpoint = false;
    bool m_playEffects = false;
    bool m_maybeCanRunIntoBlocks = false;
    bool m_hasGroundParticles = false;
    bool m_hasShipParticles = false;
    bool m_isOnGround3 = false;
    bool m_checkpointTimeout = false;
    double m_lastCheckpointTime = 0.0;
    double m_lastJumpTime = 0.0;
    double m_lastFlipTime = 0.0;
    double m_flashTime = 0.0;
    float m_flashDuration = 0.f;
    float m_flashDelay = 0.f;
    double m_lastSpiderFlipTime = 0.0;
    bool m_unkBool5 = false;
    bool m_maybeIsVehicleGlowing = false;
    bool m_switchWaveTrailColor = false;
    bool m_practiceDeathEffect = false;
    double m_accelerationOrSpeed = 0.0;
    double m_snapDistance = 0.0;
    bool m_ringJumpRelated = false;
    GameObject* m_objectSnappedTo = nullptr;
    CheckpointObject* m_pendingCheckpoint = nullptr;
    int m_onFlyCheckpointTries = 0;
    bool m_maybeSpriteRelated = false;
    bool m_useLandParticles0 = false;
    float m_landParticlesAngle = 0.f;
    float m_landParticleRelatedY = 0.f;
    int m_playerStreak = 0;
    float m_streakStrokeWidth = 0.f;
    bool m_disableStreakTint = false;
    bool m_alwaysShowStreak = false;
    int m_shipStreakType = 0;
    double m_slopeRotation = 0.0;
    double m_currentSlopeYVelocity = 0.0;
    double m_unk3d0 = 0.0;
    double m_blackOrbRelated = 0.0;
    bool m_unk3e0 = false;
    bool m_unk3e1 = false;
    bool m_isAccelerating = false;
    bool m_isCurrentSlopeTop = false;
    double m_collidedTopMinY = 0.0;
    double m_collidedBottomMaxY = 0.0;
    double m_collidedLeftMaxX = 0.0;
    double m_collidedRightMinX = 0.0;
    bool m_fadeOutStreak = false;
    bool m_canPlaceCheckpoint = false;
    bool m_hasCustomGlowColor = false;
    bool m_maybeIsColliding = false;

    // Jump buffer
    bool m_jumpBuffered = false;
    bool m_stateRingJump = false;
    bool m_wasJumpBuffered = false;
    bool m_wasRobotJump = false;
    unsigned char m_stateJumpBuffered = 0;
    bool m_stateRingJump2 = false;
    bool m_touchedRing = false;
    bool m_touchedCustomRing = false;
    bool m_touchedGravityPortal = false;
    bool m_maybeTouchedBreakableBlock = false;
    bool m_touchedPad = false;

    // Velocity
    double m_yVelocity = 0.0;
    double m_fallSpeed = 0.0;
    bool m_isOnSlope = false;
    bool m_wasOnSlope = false;
    float m_slopeVelocity = 0.f;
    bool m_maybeUpsideDownSlope = false;

    // Gamemode
    bool m_isShip = false;
    bool m_isBird = false;
    bool m_isBall = false;
    bool m_isDart = false;
    bool m_isRobot = false;
    bool m_isSpider = false;
    bool m_isUpsideDown = false;
    bool m_isDead = false;
    bool m_isOnGround = false;
    bool m_isGoingLeft = false;
    bool m_isSideways = false;
    bool m_isSwing = false;
    int m_reverseRelated = 0;
    double m_maybeReverseSpeed = 0.0;
    double m_maybeReverseAcceleration = 0.0;
    float m_xVelocityRelated2 = 0.f;
    bool m_isDashing = false;
    int m_dashFireFrame = 0;
    int m_groundObjectMaterial = 0;
    float m_vehicleSize = 1.f;
    float m_playerSpeed = 0.f;
    cocos2d::CCPoint m_shipRotation{0.f, 0.f};
    cocos2d::CCPoint m_lastPortalPos{0.f, 0.f};
    float m_unkUnused3 = 0.f;
    bool m_isOnGround2 = false;
    double m_lastLandTime = 0.0;
    float m_platformerVelocityRelated = 0.f;
    bool m_maybeIsBoosted = false;
    double m_scaleXRelatedTime = 0.0;
    bool m_decreaseBoostSlide = false;
    bool m_unkA29 = false;
    bool m_isLocked = false;
    bool m_controlsDisabled = false;
    cocos2d::CCPoint m_lastGroundedPos{0.f, 0.f};

    // Rings
    std::vector<cocos2d::CCObject*> m_touchingRingsList;
    GameObject* m_lastActivatedPortal = nullptr;
    bool m_hasEverJumped = false;
    bool m_hasEverHitRing = false;

    // Identity / time
    bool m_isSecondPlayer = false;
    bool m_unkA99 = false;
    double m_totalTime = 0.0;
    bool m_isBeingSpawnedByDualPortal = false;
    float m_audioScale = 0.f;
    float m_unkAngle1 = 0.f;
    float m_yVelocityRelated3 = 0.f;
    bool m_defaultMiniIcon = false;
    bool m_swapColors = false;
    bool m_switchDashFireColor = false;
    int m_followRelated = 0;
    std::vector<float> m_playerFollowFloats;
    float m_unk838 = 0.f;

    // State variables
    int m_stateOnGround = 0;
    unsigned char m_stateUnk = 0;
    unsigned char m_stateNoStickX = 0;
    unsigned char m_stateNoStickY = 0;
    unsigned char m_stateUnk2 = 0;
    int m_stateBoostX = 0;
    int m_stateBoostY = 0;
    int m_maybeStateForce2 = 0;
    int m_stateScale = 0;
    double m_platformerXVelocity = 0.0;
    bool m_holdingRight = false;
    bool m_holdingLeft = false;
    bool m_leftPressedFirst = false;
    double m_scaleXRelated = 0.0;
    bool m_maybeHasStopped = false;
    float m_xVelocityRelated = 0.f;
    bool m_maybeGoingCorrectSlopeDirection = false;
    bool m_isSliding = false;
    double m_maybeSlopeForce = 0.0;
    bool m_isOnIce = false;
    double m_physDeltaRelated = 0.0;
    bool m_isOnGround4 = false;
    int m_maybeSlidingTime = 0;
    double m_maybeSlidingStartTime = 0.0;
    double m_changedDirectionsTime = 0.0;
    double m_slopeEndTime = 0.0;
    bool m_isMoving = false;
    bool m_platformerMovingLeft = false;
    bool m_platformerMovingRight = false;
    bool m_isSlidingRight = false;
    double m_maybeChangedDirectionAngle = 0.0;
    double m_unkUnused2 = 0.0;
    bool m_isPlatformer = false;
    int m_stateNoAutoJump = 0;
    int m_stateDartSlide = 0;
    int m_stateHitHead = 0;
    int m_stateFlipGravity = 0;
    float m_gravityMod = 1.f;
    int m_stateForce = 0;
    cocos2d::CCPoint m_stateForceVector{0.f, 0.f};
    bool m_affectedByForces = false;
    float m_somethingPlayerSpeedTime = 0.f;
    float m_playerSpeedAC = 0.f;
    bool m_fixRobotJump = false;
    bool m_inputsLocked = false;
    bool m_gv0123 = false;
    int m_iconRequestID = 0;
    int m_unkUnused = 0;
    bool m_isOutOfBounds = false;
    float m_fallStartY = 0.f;
    bool m_disablePlayerSqueeze = false;
    bool m_robotAnimation1Enabled = false;
    bool m_robotAnimation2Enabled = false;
    bool m_spiderAnimationEnabled = false;
    bool m_ignoreDamage = false;
    bool m_enable22Changes = false;

    void capture(PlayerObject* p) {
        if (!p) { valid = false; return; }
        valid = true;

        m_ccRotation = p->getRotation();
        m_wasTeleported = p->m_wasTeleported;
        m_fixGravityBug = p->m_fixGravityBug;
        m_reverseSync = p->m_reverseSync;
        m_yVelocityBeforeSlope = p->m_yVelocityBeforeSlope;
        m_dashX = p->m_dashX;
        m_dashY = p->m_dashY;
        m_dashAngle = p->m_dashAngle;
        m_dashStartTime = p->m_dashStartTime;
        m_dashRing = p->m_dashRing;
        m_slopeStartTime = p->m_slopeStartTime;
        m_justPlacedStreak = p->m_justPlacedStreak;
        m_maybeLastGroundObject = p->m_maybeLastGroundObject;
        m_lastCollisionBottom = p->m_lastCollisionBottom;
        m_lastCollisionTop = p->m_lastCollisionTop;
        m_lastCollisionLeft = p->m_lastCollisionLeft;
        m_lastCollisionRight = p->m_lastCollisionRight;
        m_unk50C = p->m_unk50C;
        m_unk510 = p->m_unk510;
        m_currentSlope2 = p->m_currentSlope2;
        m_preLastGroundObject = p->m_preLastGroundObject;
        m_slopeAngle = p->m_slopeAngle;
        m_slopeSlidingMaybeRotated = p->m_slopeSlidingMaybeRotated;
        m_quickCheckpointMode = p->m_quickCheckpointMode;
        m_collidedObject = p->m_collidedObject;
        m_lastGroundObject = p->m_lastGroundObject;
        m_collidingWithLeft = p->m_collidingWithLeft;
        m_collidingWithRight = p->m_collidingWithRight;
        m_maybeSavedPlayerFrame = p->m_maybeSavedPlayerFrame;
        m_scaleXRelated2 = p->m_scaleXRelated2;
        m_groundYVelocity = p->m_groundYVelocity;
        m_yVelocityRelated = p->m_yVelocityRelated;
        m_scaleXRelated3 = p->m_scaleXRelated3;
        m_scaleXRelated4 = p->m_scaleXRelated4;
        m_scaleXRelated5 = p->m_scaleXRelated5;
        m_isCollidingWithSlope = p->m_isCollidingWithSlope;
        m_isBallRotating = p->m_isBallRotating;
        m_unk669 = p->m_unk669;
        m_currentPotentialSlope = p->m_currentPotentialSlope;
        m_currentSlope = p->m_currentSlope;
        unk_584 = p->unk_584;
        m_collidingWithSlopeId = p->m_collidingWithSlopeId;
        m_slopeFlipGravityRelated = p->m_slopeFlipGravityRelated;
        m_particleSystems = p->m_particleSystems;
        m_slopeAngleRadians = p->m_slopeAngleRadians;
        m_rotationSpeed = p->m_rotationSpeed;
        m_rotateSpeed = p->m_rotateSpeed;
        m_isRotating = p->m_isRotating;
        m_isBallRotating2 = p->m_isBallRotating2;
        m_hasGlow = p->m_hasGlow;
        m_isHidden = p->m_isHidden;
        m_speedMultiplier = p->m_speedMultiplier;
        m_yStart = p->m_yStart;
        m_gravity = p->m_gravity;
        m_trailingParticleLife = p->m_trailingParticleLife;
        m_unk648 = p->m_unk648;
        m_gameModeChangedTime = p->m_gameModeChangedTime;
        m_padRingRelated = p->m_padRingRelated;
        m_maybeReducedEffects = p->m_maybeReducedEffects;
        m_maybeIsFalling = p->m_maybeIsFalling;
        m_shouldTryPlacingCheckpoint = p->m_shouldTryPlacingCheckpoint;
        m_playEffects = p->m_playEffects;
        m_maybeCanRunIntoBlocks = p->m_maybeCanRunIntoBlocks;
        m_hasGroundParticles = p->m_hasGroundParticles;
        m_hasShipParticles = p->m_hasShipParticles;
        m_isOnGround3 = p->m_isOnGround3;
        m_checkpointTimeout = p->m_checkpointTimeout;
        m_lastCheckpointTime = p->m_lastCheckpointTime;
        m_lastJumpTime = p->m_lastJumpTime;
        m_lastFlipTime = p->m_lastFlipTime;
        m_flashTime = p->m_flashTime;
        m_flashDuration = p->m_flashDuration;
        m_flashDelay = p->m_flashDelay;
        m_lastSpiderFlipTime = p->m_lastSpiderFlipTime;
        m_unkBool5 = p->m_unkBool5;
        m_maybeIsVehicleGlowing = p->m_maybeIsVehicleGlowing;
        m_switchWaveTrailColor = p->m_switchWaveTrailColor;
        m_practiceDeathEffect = p->m_practiceDeathEffect;
        m_accelerationOrSpeed = p->m_accelerationOrSpeed;
        m_snapDistance = p->m_snapDistance;
        m_ringJumpRelated = p->m_ringJumpRelated;
        m_objectSnappedTo = p->m_objectSnappedTo;
        m_pendingCheckpoint = p->m_pendingCheckpoint;
        m_onFlyCheckpointTries = p->m_onFlyCheckpointTries;
        m_maybeSpriteRelated = p->m_maybeSpriteRelated;
        m_useLandParticles0 = p->m_useLandParticles0;
        m_landParticlesAngle = p->m_landParticlesAngle;
        m_landParticleRelatedY = p->m_landParticleRelatedY;
        m_playerStreak = p->m_playerStreak;
        m_streakStrokeWidth = p->m_streakStrokeWidth;
        m_disableStreakTint = p->m_disableStreakTint;
        m_alwaysShowStreak = p->m_alwaysShowStreak;
        m_shipStreakType = static_cast<int>(p->m_shipStreakType);
        m_slopeRotation = p->m_slopeRotation;
        m_currentSlopeYVelocity = p->m_currentSlopeYVelocity;
        m_unk3d0 = p->m_unk3d0;
        m_blackOrbRelated = p->m_blackOrbRelated;
        m_unk3e0 = p->m_unk3e0;
        m_unk3e1 = p->m_unk3e1;
        m_isAccelerating = p->m_isAccelerating;
        m_isCurrentSlopeTop = p->m_isCurrentSlopeTop;
        m_collidedTopMinY = p->m_collidedTopMinY;
        m_collidedBottomMaxY = p->m_collidedBottomMaxY;
        m_collidedLeftMaxX = p->m_collidedLeftMaxX;
        m_collidedRightMinX = p->m_collidedRightMinX;
        m_fadeOutStreak = p->m_fadeOutStreak;
        m_canPlaceCheckpoint = p->m_canPlaceCheckpoint;
        m_hasCustomGlowColor = p->m_hasCustomGlowColor;
        m_maybeIsColliding = p->m_maybeIsColliding;
        m_jumpBuffered = p->m_jumpBuffered;
        m_stateRingJump = p->m_stateRingJump;
        m_wasJumpBuffered = p->m_wasJumpBuffered;
        m_wasRobotJump = p->m_wasRobotJump;
        m_stateJumpBuffered = p->m_stateJumpBuffered;
        m_stateRingJump2 = p->m_stateRingJump2;
        m_touchedRing = p->m_touchedRing;
        m_touchedCustomRing = p->m_touchedCustomRing;
        m_touchedGravityPortal = p->m_touchedGravityPortal;
        m_maybeTouchedBreakableBlock = p->m_maybeTouchedBreakableBlock;
        m_touchedPad = p->m_touchedPad;
        m_yVelocity = p->m_yVelocity;
        m_fallSpeed = p->m_fallSpeed;
        m_isOnSlope = p->m_isOnSlope;
        m_wasOnSlope = p->m_wasOnSlope;
        m_slopeVelocity = p->m_slopeVelocity;
        m_maybeUpsideDownSlope = p->m_maybeUpsideDownSlope;
        m_isShip = p->m_isShip;
        m_isBird = p->m_isBird;
        m_isBall = p->m_isBall;
        m_isDart = p->m_isDart;
        m_isRobot = p->m_isRobot;
        m_isSpider = p->m_isSpider;
        m_isUpsideDown = p->m_isUpsideDown;
        m_isDead = p->m_isDead;
        m_isOnGround = p->m_isOnGround;
        m_isGoingLeft = p->m_isGoingLeft;
        m_isSideways = p->m_isSideways;
        m_isSwing = p->m_isSwing;
        m_reverseRelated = p->m_reverseRelated;
        m_maybeReverseSpeed = p->m_maybeReverseSpeed;
        m_maybeReverseAcceleration = p->m_maybeReverseAcceleration;
        m_xVelocityRelated2 = p->m_xVelocityRelated2;
        m_isDashing = p->m_isDashing;
        m_dashFireFrame = p->m_dashFireFrame;
        m_groundObjectMaterial = p->m_groundObjectMaterial;
        m_vehicleSize = p->m_vehicleSize;
        m_playerSpeed = p->m_playerSpeed;
        m_shipRotation = p->m_shipRotation;
        m_lastPortalPos = p->m_lastPortalPos;
        m_unkUnused3 = p->m_unkUnused3;
        m_isOnGround2 = p->m_isOnGround2;
        m_lastLandTime = p->m_lastLandTime;
        m_platformerVelocityRelated = p->m_platformerVelocityRelated;
        m_maybeIsBoosted = p->m_maybeIsBoosted;
        m_scaleXRelatedTime = p->m_scaleXRelatedTime;
        m_decreaseBoostSlide = p->m_decreaseBoostSlide;
        m_unkA29 = p->m_unkA29;
        m_isLocked = p->m_isLocked;
        m_controlsDisabled = p->m_controlsDisabled;
        m_lastGroundedPos = p->m_lastGroundedPos;

        // Touching rings (CCArray -> vector)
        m_touchingRingsList.clear();
        if (p->m_touchingRings) {
            for (unsigned int i = 0; i < p->m_touchingRings->count(); i++) {
                m_touchingRingsList.push_back(p->m_touchingRings->objectAtIndex(i));
            }
        }

        m_lastActivatedPortal = p->m_lastActivatedPortal;
        m_hasEverJumped = p->m_hasEverJumped;
        m_hasEverHitRing = p->m_hasEverHitRing;
        m_position = p->m_position;
        m_isSecondPlayer = p->m_isSecondPlayer;
        m_unkA99 = p->m_unkA99;
        m_totalTime = p->m_totalTime;
        m_isBeingSpawnedByDualPortal = p->m_isBeingSpawnedByDualPortal;
        m_audioScale = p->m_audioScale;
        m_unkAngle1 = p->m_unkAngle1;
        m_yVelocityRelated3 = p->m_yVelocityRelated3;
        m_defaultMiniIcon = p->m_defaultMiniIcon;
        m_swapColors = p->m_swapColors;
        m_switchDashFireColor = p->m_switchDashFireColor;
        m_followRelated = p->m_followRelated;
        m_playerFollowFloats = p->m_playerFollowFloats;
        m_unk838 = p->m_unk838;
        m_stateOnGround = p->m_stateOnGround;
        m_stateUnk = p->m_stateUnk;
        m_stateNoStickX = p->m_stateNoStickX;
        m_stateNoStickY = p->m_stateNoStickY;
        m_stateUnk2 = p->m_stateUnk2;
        m_stateBoostX = p->m_stateBoostX;
        m_stateBoostY = p->m_stateBoostY;
        m_maybeStateForce2 = p->m_maybeStateForce2;
        m_stateScale = p->m_stateScale;
        m_platformerXVelocity = p->m_platformerXVelocity;
        m_holdingRight = p->m_holdingRight;
        m_holdingLeft = p->m_holdingLeft;
        m_leftPressedFirst = p->m_leftPressedFirst;
        m_scaleXRelated = p->m_scaleXRelated;
        m_maybeHasStopped = p->m_maybeHasStopped;
        m_xVelocityRelated = p->m_xVelocityRelated;
        m_maybeGoingCorrectSlopeDirection = p->m_maybeGoingCorrectSlopeDirection;
        m_isSliding = p->m_isSliding;
        m_maybeSlopeForce = p->m_maybeSlopeForce;
        m_isOnIce = p->m_isOnIce;
        m_physDeltaRelated = p->m_physDeltaRelated;
        m_isOnGround4 = p->m_isOnGround4;
        m_maybeSlidingTime = p->m_maybeSlidingTime;
        m_maybeSlidingStartTime = p->m_maybeSlidingStartTime;
        m_changedDirectionsTime = p->m_changedDirectionsTime;
        m_slopeEndTime = p->m_slopeEndTime;
        m_isMoving = p->m_isMoving;
        m_platformerMovingLeft = p->m_platformerMovingLeft;
        m_platformerMovingRight = p->m_platformerMovingRight;
        m_isSlidingRight = p->m_isSlidingRight;
        m_maybeChangedDirectionAngle = p->m_maybeChangedDirectionAngle;
        m_unkUnused2 = p->m_unkUnused2;
        m_isPlatformer = p->m_isPlatformer;
        m_stateNoAutoJump = p->m_stateNoAutoJump;
        m_stateDartSlide = p->m_stateDartSlide;
        m_stateHitHead = p->m_stateHitHead;
        m_stateFlipGravity = p->m_stateFlipGravity;
        m_gravityMod = p->m_gravityMod;
        m_stateForce = p->m_stateForce;
        m_stateForceVector = p->m_stateForceVector;
        m_affectedByForces = p->m_affectedByForces;
        m_somethingPlayerSpeedTime = p->m_somethingPlayerSpeedTime;
        m_playerSpeedAC = p->m_playerSpeedAC;
        m_fixRobotJump = p->m_fixRobotJump;
        m_inputsLocked = p->m_inputsLocked;
        m_gv0123 = p->m_gv0123;
        m_iconRequestID = p->m_iconRequestID;
        m_unkUnused = p->m_unkUnused;
        m_isOutOfBounds = p->m_isOutOfBounds;
        m_fallStartY = p->m_fallStartY;
        m_disablePlayerSqueeze = p->m_disablePlayerSqueeze;
        m_robotAnimation1Enabled = p->m_robotAnimation1Enabled;
        m_robotAnimation2Enabled = p->m_robotAnimation2Enabled;
        m_spiderAnimationEnabled = p->m_spiderAnimationEnabled;
        m_ignoreDamage = p->m_ignoreDamage;
        m_enable22Changes = p->m_enable22Changes;
    }

    void apply(PlayerObject* p) const {
        if (!p || !valid) return;

        p->setPosition(m_position);
        p->setRotation(m_ccRotation);
        p->m_wasTeleported = m_wasTeleported;
        p->m_fixGravityBug = m_fixGravityBug;
        p->m_reverseSync = m_reverseSync;
        p->m_yVelocityBeforeSlope = m_yVelocityBeforeSlope;
        p->m_dashX = m_dashX;
        p->m_dashY = m_dashY;
        p->m_dashAngle = m_dashAngle;
        p->m_dashStartTime = m_dashStartTime;
        p->m_dashRing = m_dashRing;
        p->m_slopeStartTime = m_slopeStartTime;
        p->m_justPlacedStreak = m_justPlacedStreak;
        p->m_maybeLastGroundObject = m_maybeLastGroundObject;
        p->m_lastCollisionBottom = m_lastCollisionBottom;
        p->m_lastCollisionTop = m_lastCollisionTop;
        p->m_lastCollisionLeft = m_lastCollisionLeft;
        p->m_lastCollisionRight = m_lastCollisionRight;
        p->m_unk50C = m_unk50C;
        p->m_unk510 = m_unk510;
        p->m_currentSlope2 = m_currentSlope2;
        p->m_preLastGroundObject = m_preLastGroundObject;
        p->m_slopeAngle = m_slopeAngle;
        p->m_slopeSlidingMaybeRotated = m_slopeSlidingMaybeRotated;
        p->m_quickCheckpointMode = m_quickCheckpointMode;
        p->m_collidedObject = m_collidedObject;
        p->m_lastGroundObject = m_lastGroundObject;
        p->m_collidingWithLeft = m_collidingWithLeft;
        p->m_collidingWithRight = m_collidingWithRight;
        p->m_maybeSavedPlayerFrame = m_maybeSavedPlayerFrame;
        p->m_scaleXRelated2 = m_scaleXRelated2;
        p->m_groundYVelocity = m_groundYVelocity;
        p->m_yVelocityRelated = m_yVelocityRelated;
        p->m_scaleXRelated3 = m_scaleXRelated3;
        p->m_scaleXRelated4 = m_scaleXRelated4;
        p->m_scaleXRelated5 = m_scaleXRelated5;
        p->m_isCollidingWithSlope = m_isCollidingWithSlope;
        p->m_isBallRotating = m_isBallRotating;
        p->m_unk669 = m_unk669;
        p->m_currentPotentialSlope = m_currentPotentialSlope;
        p->m_currentSlope = m_currentSlope;
        p->unk_584 = unk_584;
        p->m_collidingWithSlopeId = m_collidingWithSlopeId;
        p->m_slopeFlipGravityRelated = m_slopeFlipGravityRelated;
        p->m_particleSystems = m_particleSystems;
        p->m_slopeAngleRadians = m_slopeAngleRadians;
        p->m_rotationSpeed = m_rotationSpeed;
        p->m_rotateSpeed = m_rotateSpeed;
        p->m_isRotating = m_isRotating;
        p->m_isBallRotating2 = m_isBallRotating2;
        p->m_hasGlow = m_hasGlow;
        p->m_isHidden = m_isHidden;
        p->m_speedMultiplier = m_speedMultiplier;
        p->m_yStart = m_yStart;
        p->m_gravity = m_gravity;
        p->m_trailingParticleLife = m_trailingParticleLife;
        p->m_unk648 = m_unk648;
        p->m_gameModeChangedTime = m_gameModeChangedTime;
        p->m_padRingRelated = m_padRingRelated;
        p->m_maybeReducedEffects = m_maybeReducedEffects;
        p->m_maybeIsFalling = m_maybeIsFalling;
        p->m_shouldTryPlacingCheckpoint = m_shouldTryPlacingCheckpoint;
        p->m_playEffects = m_playEffects;
        p->m_maybeCanRunIntoBlocks = m_maybeCanRunIntoBlocks;
        p->m_hasGroundParticles = m_hasGroundParticles;
        p->m_hasShipParticles = m_hasShipParticles;
        p->m_isOnGround3 = m_isOnGround3;
        p->m_checkpointTimeout = m_checkpointTimeout;
        p->m_lastCheckpointTime = m_lastCheckpointTime;
        p->m_lastJumpTime = m_lastJumpTime;
        p->m_lastFlipTime = m_lastFlipTime;
        p->m_flashTime = m_flashTime;
        p->m_flashDuration = m_flashDuration;
        p->m_flashDelay = m_flashDelay;
        p->m_lastSpiderFlipTime = m_lastSpiderFlipTime;
        p->m_unkBool5 = m_unkBool5;
        p->m_maybeIsVehicleGlowing = m_maybeIsVehicleGlowing;
        p->m_switchWaveTrailColor = m_switchWaveTrailColor;
        p->m_practiceDeathEffect = m_practiceDeathEffect;
        p->m_accelerationOrSpeed = m_accelerationOrSpeed;
        p->m_snapDistance = m_snapDistance;
        p->m_ringJumpRelated = m_ringJumpRelated;
        p->m_objectSnappedTo = m_objectSnappedTo;
        p->m_pendingCheckpoint = m_pendingCheckpoint;
        p->m_onFlyCheckpointTries = m_onFlyCheckpointTries;
        p->m_maybeSpriteRelated = m_maybeSpriteRelated;
        p->m_useLandParticles0 = m_useLandParticles0;
        p->m_landParticlesAngle = m_landParticlesAngle;
        p->m_landParticleRelatedY = m_landParticleRelatedY;
        p->m_playerStreak = m_playerStreak;
        p->m_streakStrokeWidth = m_streakStrokeWidth;
        p->m_disableStreakTint = m_disableStreakTint;
        p->m_alwaysShowStreak = m_alwaysShowStreak;
        p->m_shipStreakType = static_cast<ShipStreak>(m_shipStreakType);
        p->m_slopeRotation = m_slopeRotation;
        p->m_currentSlopeYVelocity = m_currentSlopeYVelocity;
        p->m_unk3d0 = m_unk3d0;
        p->m_blackOrbRelated = m_blackOrbRelated;
        p->m_unk3e0 = m_unk3e0;
        p->m_unk3e1 = m_unk3e1;
        p->m_isAccelerating = m_isAccelerating;
        p->m_isCurrentSlopeTop = m_isCurrentSlopeTop;
        p->m_collidedTopMinY = m_collidedTopMinY;
        p->m_collidedBottomMaxY = m_collidedBottomMaxY;
        p->m_collidedLeftMaxX = m_collidedLeftMaxX;
        p->m_collidedRightMinX = m_collidedRightMinX;
        p->m_fadeOutStreak = m_fadeOutStreak;
        p->m_canPlaceCheckpoint = m_canPlaceCheckpoint;
        p->m_hasCustomGlowColor = m_hasCustomGlowColor;
        p->m_maybeIsColliding = m_maybeIsColliding;
        p->m_jumpBuffered = m_jumpBuffered;
        p->m_stateRingJump = m_stateRingJump;
        p->m_wasJumpBuffered = m_wasJumpBuffered;
        p->m_wasRobotJump = m_wasRobotJump;
        p->m_stateJumpBuffered = m_stateJumpBuffered;
        p->m_stateRingJump2 = m_stateRingJump2;
        p->m_touchedRing = m_touchedRing;
        p->m_touchedCustomRing = m_touchedCustomRing;
        p->m_touchedGravityPortal = m_touchedGravityPortal;
        p->m_maybeTouchedBreakableBlock = m_maybeTouchedBreakableBlock;
        p->m_touchedPad = m_touchedPad;
        p->m_yVelocity = m_yVelocity;
        p->m_fallSpeed = m_fallSpeed;
        p->m_isOnSlope = m_isOnSlope;
        p->m_wasOnSlope = m_wasOnSlope;
        p->m_slopeVelocity = m_slopeVelocity;
        p->m_maybeUpsideDownSlope = m_maybeUpsideDownSlope;
        p->m_isShip = m_isShip;
        p->m_isBird = m_isBird;
        p->m_isBall = m_isBall;
        p->m_isDart = m_isDart;
        p->m_isRobot = m_isRobot;
        p->m_isSpider = m_isSpider;
        p->m_isUpsideDown = m_isUpsideDown;
        p->m_isDead = m_isDead;
        p->m_isOnGround = m_isOnGround;
        p->m_isGoingLeft = m_isGoingLeft;
        p->m_isSideways = m_isSideways;
        p->m_isSwing = m_isSwing;
        p->m_reverseRelated = m_reverseRelated;
        p->m_maybeReverseSpeed = m_maybeReverseSpeed;
        p->m_maybeReverseAcceleration = m_maybeReverseAcceleration;
        p->m_xVelocityRelated2 = m_xVelocityRelated2;
        p->m_isDashing = m_isDashing;
        p->m_dashFireFrame = m_dashFireFrame;
        p->m_groundObjectMaterial = m_groundObjectMaterial;
        p->m_vehicleSize = m_vehicleSize;
        p->m_playerSpeed = m_playerSpeed;
        p->m_shipRotation = m_shipRotation;
        p->m_lastPortalPos = m_lastPortalPos;
        p->m_unkUnused3 = m_unkUnused3;
        p->m_isOnGround2 = m_isOnGround2;
        p->m_lastLandTime = m_lastLandTime;
        p->m_platformerVelocityRelated = m_platformerVelocityRelated;
        p->m_maybeIsBoosted = m_maybeIsBoosted;
        p->m_scaleXRelatedTime = m_scaleXRelatedTime;
        p->m_decreaseBoostSlide = m_decreaseBoostSlide;
        p->m_unkA29 = m_unkA29;
        p->m_isLocked = m_isLocked;
        p->m_controlsDisabled = m_controlsDisabled;
        p->m_lastGroundedPos = m_lastGroundedPos;

        // Touching rings
        p->m_touchingRings->removeAllObjects();
        for (auto& ring : m_touchingRingsList) {
            p->m_touchingRings->addObject(ring);
        }

        p->m_lastActivatedPortal = m_lastActivatedPortal;
        p->m_hasEverJumped = m_hasEverJumped;
        p->m_hasEverHitRing = m_hasEverHitRing;
        p->m_position = m_position;
        p->m_isSecondPlayer = m_isSecondPlayer;
        p->m_unkA99 = m_unkA99;
        p->m_totalTime = m_totalTime;
        p->m_isBeingSpawnedByDualPortal = m_isBeingSpawnedByDualPortal;
        p->m_audioScale = m_audioScale;
        p->m_unkAngle1 = m_unkAngle1;
        p->m_yVelocityRelated3 = m_yVelocityRelated3;
        p->m_defaultMiniIcon = m_defaultMiniIcon;
        p->m_swapColors = m_swapColors;
        p->m_switchDashFireColor = m_switchDashFireColor;
        p->m_followRelated = m_followRelated;
        p->m_playerFollowFloats = m_playerFollowFloats;
        p->m_unk838 = m_unk838;
        p->m_stateOnGround = m_stateOnGround;
        p->m_stateUnk = m_stateUnk;
        p->m_stateNoStickX = m_stateNoStickX;
        p->m_stateNoStickY = m_stateNoStickY;
        p->m_stateUnk2 = m_stateUnk2;
        p->m_stateBoostX = m_stateBoostX;
        p->m_stateBoostY = m_stateBoostY;
        p->m_maybeStateForce2 = m_maybeStateForce2;
        p->m_stateScale = m_stateScale;
        p->m_platformerXVelocity = m_platformerXVelocity;
        p->m_holdingRight = m_holdingRight;
        p->m_holdingLeft = m_holdingLeft;
        p->m_leftPressedFirst = m_leftPressedFirst;
        p->m_scaleXRelated = m_scaleXRelated;
        p->m_maybeHasStopped = m_maybeHasStopped;
        p->m_xVelocityRelated = m_xVelocityRelated;
        p->m_maybeGoingCorrectSlopeDirection = m_maybeGoingCorrectSlopeDirection;
        p->m_isSliding = m_isSliding;
        p->m_maybeSlopeForce = m_maybeSlopeForce;
        p->m_isOnIce = m_isOnIce;
        p->m_physDeltaRelated = m_physDeltaRelated;
        p->m_isOnGround4 = m_isOnGround4;
        p->m_maybeSlidingTime = m_maybeSlidingTime;
        p->m_maybeSlidingStartTime = m_maybeSlidingStartTime;
        p->m_changedDirectionsTime = m_changedDirectionsTime;
        p->m_slopeEndTime = m_slopeEndTime;
        p->m_isMoving = m_isMoving;
        p->m_platformerMovingLeft = m_platformerMovingLeft;
        p->m_platformerMovingRight = m_platformerMovingRight;
        p->m_isSlidingRight = m_isSlidingRight;
        p->m_maybeChangedDirectionAngle = m_maybeChangedDirectionAngle;
        p->m_unkUnused2 = m_unkUnused2;
        p->m_isPlatformer = m_isPlatformer;
        p->m_stateNoAutoJump = m_stateNoAutoJump;
        p->m_stateDartSlide = m_stateDartSlide;
        p->m_stateHitHead = m_stateHitHead;
        p->m_stateFlipGravity = m_stateFlipGravity;
        p->m_gravityMod = m_gravityMod;
        p->m_stateForce = m_stateForce;
        p->m_stateForceVector = m_stateForceVector;
        p->m_affectedByForces = m_affectedByForces;
        p->m_somethingPlayerSpeedTime = m_somethingPlayerSpeedTime;
        p->m_playerSpeedAC = m_playerSpeedAC;
        p->m_fixRobotJump = m_fixRobotJump;
        p->m_inputsLocked = m_inputsLocked;
        p->m_gv0123 = m_gv0123;
        p->m_iconRequestID = m_iconRequestID;
        p->m_unkUnused = m_unkUnused;
        p->m_isOutOfBounds = m_isOutOfBounds;
        p->m_fallStartY = m_fallStartY;
        p->m_disablePlayerSqueeze = m_disablePlayerSqueeze;
        p->m_robotAnimation1Enabled = m_robotAnimation1Enabled;
        p->m_robotAnimation2Enabled = m_robotAnimation2Enabled;
        p->m_spiderAnimationEnabled = m_spiderAnimationEnabled;
        p->m_ignoreDamage = m_ignoreDamage;
        p->m_enable22Changes = m_enable22Changes;
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
    int            eventCount = 0;
    double         levelTime  = 0.0;   // used for both physics frames AND inputs
    PlayerSnapshot p1;
    PlayerSnapshot p2;
    void*          checkpointPtr = nullptr;
};

// ============================================================================
//  Macro  --  an ordered list of InputEvents plus a little metadata.
// ============================================================================

struct Macro {
    std::vector<InputEvent> events;
    std::vector<PhysicsFrame> physicsFrames;  // for physics bot mode
    double recordStartTime = 0.0; // level time the recording was armed at
    double recordFps       = 240.0; // informational: physics fps used to record
    bool   normalized      = false; // were events shifted so the first is at 0?

    // Level identity -- so we can warn before playing a macro on the wrong level.
    int         levelID = 0;
    std::string levelName;

    void clear() {
        events.clear();
        recordStartTime = 0.0;
        physicsFrames.clear();
        normalized = false;
        // levelID / levelName are intentionally preserved across a clear so that
        // re-recording on the same level keeps its identity.
    }

    size_t size() const { return events.size(); }
    bool empty() const { return events.empty(); }

    // The level time of the final input/frame -- used for the progress readout.
    double duration() const {
        double d = events.empty() ? 0.0 : events.back().time;
        if (!physicsFrames.empty()) d = std::max(d, physicsFrames.back().time);
        return d;
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
    double m_lastRecordTime = 0.0;


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

    // Automatically save the macro to disk when a level is completed while
    // recording -- handy so a clean practice run is never lost.
    bool      autoSaveOnComplete = true;

    // ----- settings (gear menu) --------------------------------------------

    // Input mirroring: every live P1 input is also dispatched to P2 (and vice
    // versa). With `mirrorInvert` the mirrored copy behaves like an
    // upside-down player two: jump state flipped, left/right swapped.
    bool      mirrorEnabled = false;
    bool      mirrorInvert  = false;

    // Random-seed lock: reseed the RNG with `lockedSeed` at the start of
    // every attempt so seeded effects roll identically on every macro run.
    bool      seedLockEnabled = false;
    int       lockedSeed      = 1337;

    // Wave-only "maintain gravity": when a gravity portal flips the wave, the
    // effective hold state is inverted automatically so the wave keeps
    // travelling the direction the player is asking for.
    bool      waveMaintainEnabled = false;

    // Autoclicker: click train on P1 jump at this many clicks per second,
    // phase-locked to the level clock (see tickAutoClicker).
    bool      autoClickEnabled = false;
    double    autoClickCPS     = 10.0;

    // Set while WE are re-dispatching a transformed input (mirror copy, wave
    // gravity fix, autoclick edge). Unlike `injecting`, these still get
    // RECORDED into the macro -- the flag only stops the handleButton hook
    // from transforming them a second time (mirror ping-pong, double flips).
    bool      selfDispatch = false;

    // Internal feature state.
    bool                autoClickHeld = false; // current autoclick edge
    std::array<bool, 2> waveRawHeld{};         // physical jump state  [p1, p2]
    std::array<bool, 2> waveEffectiveHeld{};   // dispatched jump state [p1, p2]

    // ----- Physics bot mode -----
    // Physics bot mode is always on — we always record physics frames
    // AND inputs. Physics frames correct drift from input timing errors.
    bool physicsMode = true;
    size_t    physicsPlaybackIndex = 0;

    // Held-button state, indexed [player2 ? 1 : 0][button]. Maintained
    // incrementally while recording so we can collapse redundant transitions in
    // O(1) instead of rescanning the whole event list per input.
    std::array<std::array<bool, 4>, 2> heldState{};
    std::array<bool, 6> m_currentHeld{}; // [p1Jump, p1Left, p1Right, p2Jump, p2Left, p2Right]

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
        // Gameplay keeps running while the GUI is open -- no freeze, no music
        // pause. We only toggle the OS cursor so the user can click the panel.
        guiPaused = false;
        if (open) {
            PlatformToolbox::showCursor();
        } else {
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
        physicsPlaybackIndex = 0;
        resetHeldState();
        m_lastRecordTime = macro.recordStartTime;
        log::info("[Bot] Recording armed at t={:.6f}", macro.recordStartTime);
        notify("Recording started", NotificationIcon::Success);
        refreshUI();
    }

    void startPlayback(GJBaseGameLayer* gl) {
        if (!cbfAvailable()) {
            notifyNoCBF();
            return;
        }
        if (macro.events.empty()) {
            notify("No inputs to play back", NotificationIcon::Warning);
            return;
        }
        mode = bot::Mode::Playing;
        validateAndRepair();
        double startT = levelTime(gl);
        seekPlaybackTo(startT);
        seekPhysicsPlayback(startT);
        applyHeldStateAt(gl, startT);
        m_currentHeld.fill(false);
        log::info("[Bot] Playback started ({} events, {} frames)",
                  macro.events.size(), macro.physicsFrames.size());
        notify("Playback started", NotificationIcon::Success);
        refreshUI();
    }

    void stop() {
        if (mode == bot::Mode::Playing) releaseAll();
        mode = bot::Mode::Disabled;
        playbackIndex = 0;
        physicsPlaybackIndex = 0;
        m_currentHeld.fill(false);
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
                    // Release just after the last real event so playback never
                    // leaves a button stuck down, without shifting any real timing.
                    macro.events.emplace_back(lastTime + 0.001,
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

    // Snap a timestamp onto the grid the ACTIVE CBF tier can actually address.
    // Under RobTop's CBS the engine can only place a click on one of 480 slots
    // per second; recording the exact raw m_levelTime (which can fall between
    // two of those slots) stores a timestamp CBS itself could never reproduce,
    // so on playback the click lands on whichever neighbouring slot the engine
    // happens to round to -- which can be either early or late depending on
    // which side of the slot boundary m_levelTime is on. Snapping at record
    // time removes that ambiguity entirely. Under Syzzi CBF the resolution is
    // effectively continuous, so this is a no-op there.
    double quantizeToResolution(double t) const {
        double step = effectiveInputResolution();
        if (step <= 0.0 || !std::isfinite(step)) return t;
        return std::round(t / step) * step;
    }
    static double quantizeRobTop(double t) {
        double step = 1.0 / bot::ROBTOP_CBS_FPS;
        return std::round(t / step) * step;
    }

    // ----- recording -------------------------------------------------------

    // Called from the handleButton hook. Records a transition tagged with the
    // current level time. We collapse redundant transitions (two downs in a row
    // for the same button/player) so the macro stays clean.
       void recordInput(GJBaseGameLayer* gl, bool down, int button, bool isPlayer1) {
        if (mode != bot::Mode::Recording) return;
        if (injecting) return;
        if (button < 1 || button > 3) return;

        auto pl = PlayLayer::get();
        if (pl) {
            if (pl->m_player1 && pl->m_player1->m_isDead) return;
            if (pl->m_player2 && pl->m_player2->m_isDead) return;
        }

        bool player2 = !isPlayer1;
        int  pi = player2 ? 1 : 0;

        // Collapse redundant transitions: GD/CBF can legitimately call
        // handleButton twice in a row for what is really one logical press
        // (e.g. a click landing exactly on a step boundary, or certain input
        // paths re-asserting an already-held button). If the button is
        // already in the state we're about to record, this is a duplicate,
        // not a new transition -- recording it would bake an extra
        // press/release pair into the macro that plays back as a real
        // double-click. heldState is the source of truth for "is this button
        // currently down in the recording", so compare against it, not
        // against the last emitted event (which could be for a different
        // button/player and isn't a valid comparison).
        if (heldState[pi][button] == down) return;
        heldState[pi][button] = down;

        // Capture the authoritative level clock. This is what CBF has already
        // advanced to the correct sub-step by the time handleButton fires, so
        // it is sub-frame accurate for free (see the design notes at the top
        // of this file). It is also monotonic, unlike X position.
        //
        // Under Syzzi CBF we store the RAW double -- effectively infinite
        // resolution, any sub-step addressable -- because rounding would throw
        // away exactly the timing CBF exists to provide, and a double costs
        // the same 8 bytes on disk either way. Only under RobTop's CBS do we
        // snap to its 480fps grid, since that is the only grid the engine
        // itself can reproduce a click on (see quantizeRobTop).
        double t = levelTime(gl);
        if (cbfState() == bot::CBFState::RobTop) t = quantizeRobTop(t);
        if (!macro.events.empty() && t <= macro.events.back().time) {
            double step = effectiveInputResolution();
            t = macro.events.back().time + (step > 0.0 ? step : 1e-6);
        }

        InputEvent e(t, static_cast<uint8_t>(button), down, player2);
        macro.events.emplace_back(e);
        refreshUIProgress();
    }

    void syncRecordingToTime(GJBaseGameLayer* gl) {
        if (mode != bot::Mode::Recording) return;
        m_lastRecordTime = levelTime(gl);
    }

    // Drop every recorded event whose timestamp is strictly after `t`.
    void truncateAfter(double t) {
        size_t n = macro.events.size();
        while (n > 0 && macro.events[n - 1].time > t) --n;
        if (n < macro.events.size()) {
            size_t dropped = macro.events.size() - n;
            macro.events.resize(n);
            refreshUIProgress();
            log::info("[Bot] Re-record: discarded {} superseded input(s) after t={:.6f}",
                      dropped, t);
        }
    }

    // Drop every physics frame whose timestamp is strictly after `t`.
    // Called when the level clock jumps backwards (death, checkpoint load,
    // restart) so stale frames don't interfere with re-recording.
    void truncatePhysicsAfter(double t) {
        if (macro.physicsFrames.empty()) return;
        size_t n = macro.physicsFrames.size();
        while (n > 0 && macro.physicsFrames[n - 1].time > t) --n;
        if (n < macro.physicsFrames.size()) {
            macro.physicsFrames.resize(n);
        }
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
        std::array<std::array<int, 4>, 2> last{};
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

    // ----- settings features: mirror / wave gravity / autoclick / seed -----

    // Wave "maintain gravity", live-input half. Rewrites a LIVE jump
    // transition so the wave keeps travelling the direction the user asks
    // for even after a gravity portal flips the meaning of "hold":
    // effective = raw XOR upside-down. Returns false when the rewritten
    // transition is redundant (the engine is already in that state) and the
    // hook should swallow it. Wave-only by design -- every other gamemode
    // passes through untouched. The raw state is tracked even while the
    // option is off so enabling it mid-attempt behaves correctly.
    bool filterLiveJump(bool& down, bool isPlayer1) {
        int pi = isPlayer1 ? 0 : 1;
        waveRawHeld[pi] = down;
        if (!waveMaintainEnabled) { waveEffectiveHeld[pi] = down; return true; }
        auto pl = PlayLayer::get();
        PlayerObject* p = pl ? (isPlayer1 ? pl->m_player1 : pl->m_player2)
                             : nullptr;
        if (!p || !p->m_isDart) { waveEffectiveHeld[pi] = down; return true; }
        bool effective = (down != p->m_isUpsideDown);
        if (effective == waveEffectiveHeld[pi]) return false;
        waveEffectiveHeld[pi] = effective;
        down = effective;
        return true;
    }

    // Wave "maintain gravity", per-physics-step half. When a gravity portal
    // flips m_isUpsideDown while the raw button state is unchanged, this
    // re-dispatches the corrected hold on the very step the flip happened.
    // The corrected transitions go through the normal recording path, so a
    // macro recorded with the option on plays back exactly as flown -- which
    // is also why this never runs during playback: the macro already
    // contains the corrected inputs.
    void tickWaveGravity(GJBaseGameLayer* gl) {
        if (!waveMaintainEnabled || mode == bot::Mode::Playing || !gl) return;
        auto pl = PlayLayer::get();
        if (!pl) return;
        for (int pi = 0; pi < 2; ++pi) {
            PlayerObject* p = (pi == 0) ? pl->m_player1 : pl->m_player2;
            if (!p || p->m_isDead) continue;
            // Outside the wave the correction is a straight pass-through --
            // which also releases a phantom hold the moment a portal takes
            // the player out of wave mode.
            bool effective = p->m_isDart
                             ? (waveRawHeld[pi] != p->m_isUpsideDown)
                             : waveRawHeld[pi];
            if (effective == waveEffectiveHeld[pi]) continue;
            waveEffectiveHeld[pi] = effective;
            selfDispatch = true;
            gl->handleButton(effective, 1, pi == 0);
            selfDispatch = false;
        }
    }

    void setWaveMaintain(GJBaseGameLayer* gl, bool on) {
        waveMaintainEnabled = on;
        if (!on && gl) {
            // Hand control back to the raw button state so a phantom hold
            // (or a suppressed one) doesn't outlive the option.
            selfDispatch = true;
            for (int pi = 0; pi < 2; ++pi) {
                if (waveEffectiveHeld[pi] != waveRawHeld[pi]) {
                    waveEffectiveHeld[pi] = waveRawHeld[pi];
                    gl->handleButton(waveRawHeld[pi], 1, pi == 0);
                }
            }
            selfDispatch = false;
        }
        persist();
    }

    // Mirror a LIVE input onto the other player. With `mirrorInvert` the
    // mirrored copy behaves like an upside-down player two: jump state
    // flipped, left/right swapped. Dispatched with selfDispatch set so the
    // copy is not re-mirrored back (no ping-pong) -- but it IS recorded, so
    // a macro captured with mirroring on replays identically even with the
    // option later turned off.
    void dispatchMirrored(GJBaseGameLayer* gl, bool down, int button, bool isPlayer1) {
        if (!gl) return;
        bool d = down;
        int  b = button;
        if (mirrorInvert) {
            if (b == 1)      d = !d;
            else if (b == 2) b = 3;
            else if (b == 3) b = 2;
        }
        bool prev = selfDispatch;
        selfDispatch = true;
        gl->handleButton(d, b, !isPlayer1);
        selfDispatch = prev;
    }

    // Autoclicker: a 50% duty-cycle click train on P1 jump at `autoClickCPS`
    // clicks per second, phase-locked to the level clock -- so the edges are
    // exactly reproducible, frame-rate independent, and speedhack-safe. Runs
    // once per physics step; with CBF that means the click edges land on
    // sub-frame step boundaries, the same precision as everything else here.
    // Recording captures the clicks as normal transitions. Disabled during
    // playback: the macro owns the buttons then.
    void tickAutoClicker(GJBaseGameLayer* gl, double now) {
        bool want = autoClickEnabled &&
                    mode != bot::Mode::Playing &&
                    !guiPaused &&
                    std::isfinite(autoClickCPS) && autoClickCPS > 0.0 &&
                    std::fmod(now * autoClickCPS, 1.0) < 0.5;
        if (want == autoClickHeld || !gl) return;
        autoClickHeld = want;
        selfDispatch = true;
        gl->handleButton(want, 1, true);
        if (mirrorEnabled) dispatchMirrored(gl, want, 1, true);
        selfDispatch = false;
    }

    // Random-seed lock: reseed the CRT RNG at the start of every attempt so
    // anything driven by rand() rolls the same numbers on every run of the
    // macro. (On Windows GD links its own CRT, so randomness living entirely
    // inside the game's copy of rand() may be out of reach -- but everything
    // in mod-land, and the common sources of run-to-run drift, are pinned.)
    void applySeedLock() {
        if (seedLockEnabled) std::srand(static_cast<unsigned int>(lockedSeed));
    }

    // ----- Physics bot: record and apply -----

    // Record a physics frame. Called from processCommands AFTER the original
    // runs (so position reflects the physics step that just happened).
    void recordPhysicsFrame(double time) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (pl->m_player1 && pl->m_player1->m_isDead) return;
        if (pl->m_player2 && pl->m_player2->m_isDead) return;
        
        if (!macro.physicsFrames.empty() && 
            time < macro.physicsFrames.back().time - 0.001) {
            return;
        }
        
        PhysicsFrame f;
        f.time = time;
        if (pl->m_player1) {
            f.p1x = pl->m_player1->getPositionX();
            f.p1y = pl->m_player1->getPositionY();
            f.p1yVel = pl->m_player1->m_yVelocity;
            // Capture held state from heldState tracker
            f.p1Jump = heldState[0][1];
            f.p1Left = heldState[0][2];
            f.p1Right = heldState[0][3];
        }
        if (pl->m_player2) {
            f.p2x = pl->m_player2->getPositionX();
            f.p2y = pl->m_player2->getPositionY();
            f.p2yVel = pl->m_player2->m_yVelocity;
            f.p2Jump = heldState[1][1];
            f.p2Left = heldState[1][2];
            f.p2Right = heldState[1][3];
        }
        macro.physicsFrames.push_back(f);
    }
    
    // Fire every recorded event whose timestamp falls at or before `time`.
    //
    // `time` is the level clock the UPCOMING physics step will have advanced
    // to (current m_levelTime + this step's dt), passed in by the
    // processCommands hook BEFORE the original step runs. That lookahead is
    // the heart of the alignment fix: an input recorded mid-step at t=T
    // influenced the physics step that *covered* T, so on playback it must be
    // re-applied before that same step -- not one step later, which is what
    // comparing against the pre-step clock used to do. With the lookahead,
    // every event lands on exactly the physics step it was recorded on, at
    // any FPS, any speedhack multiplier, and any CBF subdivision.
    void fireDueInputs(GJBaseGameLayer* gl, double time) {
        if (mode != bot::Mode::Playing) return;
        if (!gl) return;

        injecting = true;
        while (playbackIndex < macro.events.size() &&
               macro.events[playbackIndex].time <= time) {
            auto const& e = macro.events[playbackIndex];
            gl->handleButton(e.down, static_cast<int>(e.button), !e.player2);
            ++playbackIndex;
        }
        injecting = false;
    }

    // NOT CALLED DURING PLAYBACK -- kept only as a diagnostic/opt-in utility.
    // Do not wire this back into processCommands: forcing the player's
    // position to a recorded snapshot every step fights GD's own continuous
    // collision detection (see the note above processCommands in main.cpp)
    // and was the direct cause of spurious deaths-on-snap and effective
    // double-clicks. Pure input replay (fireDueInputs) is the sole playback
    // mechanism; this function is retained only in case a future opt-in
    // "assisted / rewind" feature explicitly wants it.
    void applyPhysicsPosition(double time) {
        auto pl = PlayLayer::get();
        if (!pl || macro.physicsFrames.empty()) return;

        double firstFrameTime = macro.physicsFrames.front().time;
        if (time < firstFrameTime) return;

        double lastFrameTime = macro.physicsFrames.back().time;
        if (time > lastFrameTime) {
            physicsPlaybackIndex = macro.physicsFrames.size();
            return;
        }

        if (physicsPlaybackIndex >= macro.physicsFrames.size()) return;

        while (physicsPlaybackIndex + 1 < macro.physicsFrames.size() &&
               macro.physicsFrames[physicsPlaybackIndex + 1].time <= time) {
            ++physicsPlaybackIndex;
        }

        if (physicsPlaybackIndex >= macro.physicsFrames.size()) return;

        auto const& frame = macro.physicsFrames[physicsPlaybackIndex];
        // ALWAYS apply position (no range check). Do NOT set velocity —
        // setting velocity fights the inputs.
        if (pl->m_player1) {
            pl->m_player1->setPosition({frame.p1x, frame.p1y});
        }
        if (pl->m_player2) {
            pl->m_player2->setPosition({frame.p2x, frame.p2y});
        }
    }

    // Fire inputs from the current physics frame. Called AFTER processCommands
    // so inputs are set for the NEXT step, not the current one. This prevents
    // the input from desyncing with CBF's sub-step processing.
    void firePhysicsInputs() {
        auto pl = PlayLayer::get();
        if (!pl || macro.physicsFrames.empty()) return;
        if (physicsPlaybackIndex >= macro.physicsFrames.size()) return;

        auto const& frame = macro.physicsFrames[physicsPlaybackIndex];
        
        injecting = true;
        // P1
        if (frame.p1Jump != m_currentHeld[0]) {
            pl->handleButton(frame.p1Jump, 1, true);
            m_currentHeld[0] = frame.p1Jump;
        }
        if (frame.p1Left != m_currentHeld[1]) {
            pl->handleButton(frame.p1Left, 2, true);
            m_currentHeld[1] = frame.p1Left;
        }
        if (frame.p1Right != m_currentHeld[2]) {
            pl->handleButton(frame.p1Right, 3, true);
            m_currentHeld[2] = frame.p1Right;
        }
        // P2
        if (frame.p2Jump != m_currentHeld[3]) {
            pl->handleButton(frame.p2Jump, 1, false);
            m_currentHeld[3] = frame.p2Jump;
        }
        if (frame.p2Left != m_currentHeld[4]) {
            pl->handleButton(frame.p2Left, 2, false);
            m_currentHeld[4] = frame.p2Left;
        }
        if (frame.p2Right != m_currentHeld[5]) {
            pl->handleButton(frame.p2Right, 3, false);
            m_currentHeld[5] = frame.p2Right;
        }
        injecting = false;
    }

    // Seek the physics playback cursor to the first frame at or after `time`.
    void seekPhysicsPlayback(double time) {
        physicsPlaybackIndex = 0;
        while (physicsPlaybackIndex < macro.physicsFrames.size() &&
               macro.physicsFrames[physicsPlaybackIndex].time < time) {
            ++physicsPlaybackIndex;
        }
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
    }

    void onCheckpointRemoved() {
        if (!checkpoints.empty()) checkpoints.pop_back();
    }

    // Called after the game has loaded a checkpoint. The actual discarding of
    // "dead inputs" is handled automatically by syncRecordingToTime (the level
    // clock jumps backwards on a checkpoint load); here we just apply our accurate
    // physics snapshot to fix the practice bug, and re-align playback.
      void onCheckpointLoaded(PlayLayer* pl, void* cpPtr) {
        if (!pl) return;

        CheckpointFrame* frame = nullptr;
        for (auto it = checkpoints.rbegin(); it != checkpoints.rend(); ++it) {
            if (it->checkpointPtr == cpPtr) { frame = &*it; break; }
        }
        if (!frame && !checkpoints.empty()) frame = &checkpoints.back();
        if (!frame) return;

        if (practiceFixEnabled) {
            frame->p1.apply(pl->m_player1);
            if (frame->p2.valid) frame->p2.apply(pl->m_player2);
        }

        if (mode == bot::Mode::Playing) {
            seekPhysicsPlayback(frame->levelTime);
            seekPlaybackTo(frame->levelTime);
            applyHeldStateAt(pl, frame->levelTime);
        } else if (mode == bot::Mode::Recording) {
            truncateAfter(frame->levelTime);
            truncatePhysicsAfter(frame->levelTime);
            recomputeHeldState();
        }
    }
    // Pause-menu restart. The level clock resets, so syncRecordingToTime will
    // overwrite the superseded inputs on the next frame; we just clear the
    // checkpoint stack and rewind the playback cursor here.
    void onRestart(PlayLayer* pl, bool fromStart) {
        checkpoints.clear();
        if (mode == bot::Mode::Playing) {
            releaseAll();
            playbackIndex = 0;
            physicsPlaybackIndex = 0;
            if (pl) {
                seekPlaybackTo(0.0);
                seekPhysicsPlayback(0.0);
                applyHeldStateAt(pl, 0.0);
            }
        } else if (mode == bot::Mode::Recording) {
            truncateAfter(0.0);
            truncatePhysicsAfter(0.0);
            recomputeHeldState();
            resetHeldState();
        }
        refreshUIProgress();
    }

    // Fresh level / resetLevel: reset transient state but keep the macro.
    void onLevelReset(PlayLayer* pl) {
        checkpoints.clear();
        // New attempt: feature state from the previous one is stale.
        autoClickHeld = false;
        waveRawHeld.fill(false);
        waveEffectiveHeld.fill(false);
        applySeedLock();
        if (mode == bot::Mode::Playing) {
            releaseAll();
            playbackIndex = 0;
            physicsPlaybackIndex = 0;
            seekPlaybackTo(0.0);
            seekPhysicsPlayback(0.0);
            if (pl) applyHeldStateAt(pl, 0.0);
        }
    }

    void onPlayerDeath(PlayLayer* pl) {
        if (mode == bot::Mode::Playing) {
            releaseAll();
            playbackIndex = 0;
            physicsPlaybackIndex = 0;
            seekPlaybackTo(0.0);
            seekPhysicsPlayback(0.0);
        } else if (mode == bot::Mode::Recording) {
            double t = levelTime(pl);
            truncateAfter(t);
            truncatePhysicsAfter(t);
            recomputeHeldState();
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

    // ----- speedhack audio ------------------------------------------------
    // Push the current speedhack multiplier onto FMOD's master channel group
    // so the entire audio mix (music + SFX) plays at the same speed as the
    // physics. We talk to FMOD directly through FMODAudioEngine::m_system --
    // which is present in every Geode binding -- rather than relying on any
    // GD-named convenience method (setSpeedEffect / m_musicChannel / etc),
    // which shift between SDK checkouts.
    //
    // Note: pitching the MASTER group pitches both music AND SFX. That is
    // intentional here -- it keeps the whole game mix in lockstep with the
    // speedhack, which is what most users expect when they crank the speed.
    void applyMusicSpeed() {
        auto fae = FMODAudioEngine::sharedEngine();
        if (!fae || !fae->m_system) return;
        bool inLevel = (PlayLayer::get() != nullptr);
        float pitch = (speedhackEnabled && inLevel &&
                       std::isfinite(speed) && speed > 0.0)
                      ? static_cast<float>(speedMultiplier()) : 1.0f;
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (fae->m_system->getMasterChannelGroup(&masterGroup) == FMOD_OK &&
            masterGroup) {
            masterGroup->setPitch(pitch);
        }
    }
    // Reset the master channel group pitch to 1.0. Called from
    // PlayLayer::onExit so menu music plays at normal speed after leaving
    // a level where the speedhack was active.
    void resetAudioPitch() {
        auto fae = FMODAudioEngine::sharedEngine();
        if (!fae || !fae->m_system) return;
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (fae->m_system->getMasterChannelGroup(&masterGroup) == FMOD_OK &&
            masterGroup) {
            masterGroup->setPitch(1.0f);
        }
    }

    // Write the current options to the mod's saved values. Called whenever an
    // option changes (there is no on-unload event in Geode, so we persist eagerly).
    void persist() {
        auto m = Mod::get();
        m->setSavedValue<double>("speed", speed);
        m->setSavedValue<bool>("speedhack", speedhackEnabled);
        m->setSavedValue<bool>("practice-fix", practiceFixEnabled);
        m->setSavedValue<bool>("discard-dead", discardDeadInputs);
        m->setSavedValue<bool>("auto-save", autoSaveOnComplete);
        m->setSavedValue<std::string>("macro-name", macroName);
        m->setSavedValue<bool>("mirror", mirrorEnabled);
        m->setSavedValue<bool>("mirror-invert", mirrorInvert);
        m->setSavedValue<bool>("seed-lock", seedLockEnabled);
        m->setSavedValue<int64_t>("seed-value", lockedSeed);
        m->setSavedValue<bool>("wave-maintain", waveMaintainEnabled);
        m->setSavedValue<bool>("auto-click", autoClickEnabled);
        m->setSavedValue<double>("auto-cps", autoClickCPS);
    }

    void setSpeedFromString(std::string const& s) {
        try {
            double v = std::stod(s);
            if (std::isfinite(v) && v > 0.0) {
                speed = v;
                log::info("[Bot] Speed set to {}", speed);
                persist();
                applyMusicSpeed();   // <-- add this line
            }
        } catch (...) {
            // ignore malformed input; keep the previous value
        }
    }

    void setSeedFromString(std::string const& s) {
        try {
            lockedSeed = static_cast<int>(std::stol(s));
            persist();
        } catch (...) {
            // ignore malformed input; keep the previous value
        }
    }

    void setAutoClickCPSFromString(std::string const& s) {
        try {
            double v = std::stod(s);
            if (std::isfinite(v) && v >= 0.0) {
                autoClickCPS = v;
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
        validateAndRepair();
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
            wr(&e.time, 8);       // double, level-time seconds
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
            if (version >= 6) {
                // v6+: level-time based (double) -- accurate.
                rd(&e.time, 8);
            } else if (version == 5) {
                // v5: X-position based (float). There is no way to recover a
                // true timestamp from a stored X position, so these macros
                // cannot be losslessly upgraded. Skip the corrupted field and
                // warn loudly rather than silently mis-playing the run.
                float oldX = 0.f;
                rd(&oldX, 4);
                e.time = 0.0;
            } else {
                // v4 and below: time-based (double) -- same format as v6+.
                rd(&e.time, 8);
            }
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
        if (version == 5) {
            notify("This macro was saved by the old X-position build and "
                   "cannot be recovered accurately -- please re-record it.",
                   NotificationIcon::Warning);
            log::warn("[Bot] Loaded v5 (X-position) macro '{}' -- timestamps "
                      "are unrecoverable, re-record recommended", name);
        }
        log::info("[Bot] Loaded {} events from {} (level {})",
                  macro.events.size(), path.string(), macro.levelID);
        notify(fmt::format("Loaded {} inputs", macro.events.size()),
               NotificationIcon::Success);
        refreshUIProgress();
        return true;
    }

        // ----- Physics macro file IO -----

    std::filesystem::path physicsMacroPath(std::string const& name) const {
        return Mod::get()->getSaveDir() / (sanitizeName(name) + ".gdpm");
    }

       bool savePhysicsMacro(std::string const& name) {
        auto path = physicsMacroPath(name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            notify("Could not open file for writing", NotificationIcon::Error);
            return false;
        }
        auto wr = [&](void const* p, size_t n){
            out.write(reinterpret_cast<char const*>(p), n);
        };

        uint32_t magic = 0x4D504447; // 'GDPM'
        uint32_t version = 5;  // v5: level-time based inputs (accurate)
        uint32_t frameCount = static_cast<uint32_t>(macro.physicsFrames.size());
        uint32_t inputCount = static_cast<uint32_t>(macro.events.size());
        int32_t levelID = macro.levelID;
        std::string lname = macro.levelName.substr(0, 0xFFFF);
        uint16_t nameLen = static_cast<uint16_t>(lname.size());

        wr(&magic, 4); wr(&version, 4);
        wr(&levelID, 4);
        wr(&nameLen, 2);
        if (nameLen) wr(lname.data(), nameLen);
        wr(&frameCount, 4);
        wr(&inputCount, 4);

        for (auto const& f : macro.physicsFrames) {
            wr(&f.time, 8);
            wr(&f.p1x, 4); wr(&f.p1y, 4); wr(&f.p1yVel, 8);
            wr(&f.p2x, 4); wr(&f.p2y, 4); wr(&f.p2yVel, 8);
            uint8_t held = 0;
            if (f.p1Jump)  held |= 1;
            if (f.p1Left)  held |= 2;
            if (f.p1Right) held |= 4;
            if (f.p2Jump)  held |= 8;
            if (f.p2Left)  held |= 16;
            if (f.p2Right) held |= 32;
            wr(&held, 1);
        }

        // Write input events (level-time based)
        for (auto const& e : macro.events) {
            uint8_t flags = (e.down ? 1 : 0) | (e.player2 ? 2 : 0);
            wr(&e.time, 8);       // double, level-time seconds
            wr(&e.button, 1);
            wr(&flags, 1);
        }

        out.close();
        macroName = name;
        log::info("[Bot] Saved {} frames + {} inputs to {}",
                  frameCount, inputCount, path.string());
        notify(fmt::format("Saved {} frames + {} inputs", frameCount, inputCount),
               NotificationIcon::Success);
        return true;
    }

     bool loadPhysicsMacro(std::string const& name) {
        auto path = physicsMacroPath(name);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            notify("Physics macro file not found", NotificationIcon::Error);
            return false;
        }
        auto rd = [&](void* p, size_t n){
            in.read(reinterpret_cast<char*>(p), n);
        };

        uint32_t magic = 0, version = 0;
        rd(&magic, 4); rd(&version, 4);
        if (magic != 0x4D504447) {
            notify("Not a valid physics macro file", NotificationIcon::Error);
            return false;
        }

        macro.clear();
        int32_t levelID = 0;
        uint16_t nameLen = 0;
        rd(&levelID, 4);
        rd(&nameLen, 2);
        macro.levelID = levelID;
        if (nameLen) {
            std::string buf(nameLen, '\0');
            rd(buf.data(), nameLen);
            macro.levelName = buf;
        }

        uint32_t frameCount = 0, inputCount = 0;
        rd(&frameCount, 4);
        if (version >= 2) rd(&inputCount, 4);
        else inputCount = 0;

        // Read physics frames
        macro.physicsFrames.reserve(frameCount);
        for (uint32_t i = 0; i < frameCount && in; ++i) {
            PhysicsFrame f;
            rd(&f.time, 8);
            rd(&f.p1x, 4); rd(&f.p1y, 4); rd(&f.p1yVel, 8);
            rd(&f.p2x, 4); rd(&f.p2y, 4); rd(&f.p2yVel, 8);
            uint8_t held = 0;
            rd(&held, 1);
            f.p1Jump  = (held & 1)  != 0;
            f.p1Left  = (held & 2)  != 0;
            f.p1Right = (held & 4)  != 0;
            f.p2Jump  = (held & 8)  != 0;
            f.p2Left  = (held & 16) != 0;
            f.p2Right = (held & 32) != 0;
            macro.physicsFrames.push_back(f);
        }

        // Read input events (v2+)
        if (version >= 2) {
            macro.events.reserve(inputCount);
            bool warnV4 = false;
            for (uint32_t i = 0; i < inputCount && in; ++i) {
                InputEvent e;
                uint8_t flags = 0;
                if (version >= 5) {
                    // v5+: level-time based (double) -- accurate.
                    rd(&e.time, 8);
                } else if (version == 4) {
                    // v4: X-position based (float). Not recoverable as a
                    // timestamp -- skip the field and flag for a warning.
                    float oldX = 0.f;
                    rd(&oldX, 4);
                    e.time = 0.0;
                    warnV4 = true;
                } else {
                    // v2/v3: time-based (double) -- same format as v5+.
                    rd(&e.time, 8);
                }
                rd(&e.button, 1);
                rd(&flags, 1);
                e.down    = (flags & 1) != 0;
                e.player2 = (flags & 2) != 0;
                macro.events.push_back(e);
            }
            if (warnV4) {
                notify("This physics macro was saved by the old X-position "
                       "build and cannot be recovered accurately -- please "
                       "re-record it.", NotificationIcon::Warning);
                log::warn("[Bot] Loaded v4 (X-position) physics macro '{}' -- "
                          "input timestamps are unrecoverable", name);
            }
        }

        in.close();
        macroName = name;
        physicsPlaybackIndex = 0;
        playbackIndex = 0;
        macro.sort();
        recomputeHeldState();
        log::info("[Bot] Loaded {} frames + {} inputs from {}",
                  macro.physicsFrames.size(), macro.events.size(), path.string());
        notify(fmt::format("Loaded {} frames + {} inputs",
                          macro.physicsFrames.size(), macro.events.size()),
               NotificationIcon::Success);
        return true;
    }

    // ----- timing model ----------------------------------------------------
    //
    //  There is intentionally NO manual offset control. Every input is stored at
    //  its ABSOLUTE level time, so the start delay is handled automatically: if a
    //  recording was armed 0.2s into the level, the first input simply carries a
    //  timestamp of ~0.2, and on playback from the start the bot waits until the
    //  level clock reaches 0.2 before firing it. The recording start time is thus
    //  baked into the data -- no user-set offset can desync it.

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
            out << formatTime50(e.time) << '\t'
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
    CCMenuItemToggler*    m_recordToggle = nullptr;
    CCMenuItemToggler*    m_playToggle   = nullptr;
    CCMenuItemToggler*    m_speedToggle  = nullptr;
    CCMenuItemToggler*    m_practiceToggle = nullptr;
    CCMenuItemToggler*    m_deadToggle   = nullptr;
    CCMenuItemToggler*    m_autoSaveToggle = nullptr;
    bool                  m_visible      = false;

    // Settings page (opened via the gear button in the panel corner).
    cocos2d::CCNode*      m_settingsPanel = nullptr;
    cocos2d::CCMenu*      m_mainMenu      = nullptr;
    cocos2d::CCMenu*      m_settingsMenu  = nullptr;
    CCMenuItemToggler*    m_mirrorToggle   = nullptr;
    CCMenuItemToggler*    m_invertToggle   = nullptr;
    CCMenuItemToggler*    m_seedToggle     = nullptr;
    CCMenuItemToggler*    m_waveToggle     = nullptr;
    CCMenuItemToggler*    m_autoClickToggle = nullptr;
    geode::TextInput*     m_seedInput = nullptr;
    geode::TextInput*     m_cpsInput  = nullptr;
    bool                  m_settingsOpen = false;

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
        buildSettingsPanel();
        setPanelVisible(false);

        BotManager::get().ui = this;
        this->scheduleUpdate();
        return true;
    }

    // --- survive scene transitions ---------------------------------------
    // When the scene the UI is parented to gets destroyed (menu -> level,
    // level -> pause, etc.), cocos2d recursively calls cleanup() on every
    // child. The base CCLayer::cleanup() would:
    //   * unschedule all our selectors  -> update() stops firing
    //   * remove the keyboard delegate  -> K stops working
    //   * remove the touch delegate     -> panel can't be clicked/dragged
    //
    // We intentionally skip the base class so all of those survive. We also
    // null out m_pParent: the old scene's destructor will still release us
    // (which is fine, we have a manual retain), but m_pParent would otherwise
    // be left dangling -- and update() comparing getParent() to the new
    // scene would dereference that dangling pointer. Setting it to null here
    // makes getParent() safely return null after the old scene is gone, so
    // update() can detect "I'm orphaned, re-parent me" without UB.
    void cleanup() override {
        m_pParent = nullptr;
        // Still recursively clean up our own children so their scheduled
        // callbacks / delegates don't leak. Geode v5 removed CCARRAY_FOREACH
        // in favor of CCArrayExt, which works as a range-based-for wrapper.
        if (m_pChildren && m_pChildren->count() > 0) {
            for (auto* child : geode::cocos::CCArrayExt<cocos2d::CCNode*>(m_pChildren)) {
                if (child) child->cleanup();
            }
        }
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

    // Explicitly parent ourselves to the current running scene. Called from
    // the CCKeyboardDispatcher hook when K is pressed, so the UI is guaranteed
    // to be in the scene (and therefore rendered + receiving onEnter) before
    // we toggle it visible. This replaces the reparenting logic that used to
    // live in update() -- which was unreliable because scheduleUpdate() on a
    // parentless node doesn't always fire.
    void ensureInScene() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        if (this->getParent() != scene) {
            this->retain();
            this->removeFromParentAndCleanup(false); // keep children + state
            scene->addChild(this, (std::numeric_limits<int>::max)());
            this->release();
        }
    }

    void setPanelVisible(bool v) {
        m_visible = v;
        // Always come back up on the main page; a hidden menu is also
        // disabled so its (invisible) buttons can never swallow touches.
        m_settingsOpen = false;
        if (m_panel)         m_panel->setVisible(v);
        if (m_settingsPanel) m_settingsPanel->setVisible(false);
        if (m_mainMenu)      m_mainMenu->setEnabled(v);
        if (m_settingsMenu)  m_settingsMenu->setEnabled(false);

        // Pause the level + show the cursor + pause music while open.
        BotManager::get().setGuiOpen(v);

        if (v) {
            bringToFront(); // sit above every other layer / GUI
            refreshAll();
        }
    }

    // Swap between the main page and the settings page. Both pages share the
    // same footprint; positions are synced so a dragged panel doesn't jump.
    void showSettings(bool open) {
        m_settingsOpen = open;
        if (m_panel && m_settingsPanel) {
            if (open) m_settingsPanel->setPosition(m_panel->getPosition());
            else      m_panel->setPosition(m_settingsPanel->getPosition());
        }
        if (m_panel)         m_panel->setVisible(m_visible && !open);
        if (m_settingsPanel) m_settingsPanel->setVisible(m_visible && open);
        if (m_mainMenu)      m_mainMenu->setEnabled(m_visible && !open);
        if (m_settingsMenu)  m_settingsMenu->setEnabled(m_visible && open);
        if (open) refreshAll();
    }

    // Whichever page is currently shown -- touch hit-testing and dragging
    // operate on this.
    cocos2d::CCNode* activePanel() const {
        return (m_settingsOpen && m_settingsPanel) ? m_settingsPanel : m_panel;
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
        auto panel = activePanel();
        if (!m_visible || !panel) return false;
        auto local = panel->convertToNodeSpace(touch->getLocation());
        auto size = panel->getContentSize();
        bool inside = local.x >= 0 && local.y >= 0 &&
                      local.x <= size.width && local.y <= size.height;
        if (!inside) return false; // let clicks outside the panel pass through

        // Dragging: grab the top strip (the title bar) and move the panel.
        if (local.y >= size.height - 32.f) {
            m_dragging   = true;
            m_dragStart  = touch->getLocation();
            m_panelStart = panel->getPosition();
        }
        return true; // swallow: a click on the panel never reaches gameplay
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        auto panel = activePanel();
        if (!m_dragging || !panel) return;
        auto delta = touch->getLocation() - m_dragStart;
        panel->setPosition(m_panelStart + delta);
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

        // --- self-adoption: re-parent to the current running scene ---------
        // If our parent isn't the active scene (e.g. after a scene transition
        // like menu -> level -> pause), re-attach ourselves to the new scene
        // so the GUI follows the player everywhere. This replaces the
        // SceneManager::keepNodeAcrossScenes call so we don't depend on that
        // header existing in every Geode SDK checkout.
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (scene && this->getParent() != scene) {
            this->retain();
            this->removeFromParentAndCleanup(false); // keep children + state
            scene->addChild(this, (std::numeric_limits<int>::max)());
            this->release();
        }

        if (m_visible) { refreshStatus(); refreshMode(); }
        // Keep the music pitch aligned with the speedhack multiplier, even when
        // the panel is closed (so toggling it via the K menu still works).
        BotManager::get().applyMusicSpeed();
    }

    // --- live refresh entry points (called by the manager) -----------------
    void refreshAll() {
        refreshStatus();
        refreshMode();
        refreshProgress();
        syncToggles();
        if (m_nameInput) m_nameInput->setString(BotManager::get().macroName);
        if (m_speedInput) {
            m_speedInput->setString(fmt::format("{:g}", BotManager::get().speed));
        }
        if (m_seedInput) {
            m_seedInput->setString(fmt::format("{}", BotManager::get().lockedSeed));
        }
        if (m_cpsInput) {
            m_cpsInput->setString(fmt::format("{:g}", BotManager::get().autoClickCPS));
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
            m_progressLabel->setString(
                fmt::format("{} inputs  |  {:.2f}s",
                    BotManager::get().macro.events.size(),
                    BotManager::get().macro.duration()).c_str());
        }
        if (m_statsLabel) {
            auto s = BotManager::get().computeStats();
            m_statsLabel->setString(
                fmt::format("press {}  rel {}  p1 {}  p2 {}",
                            s.presses, s.releases, s.p1, s.p2).c_str());
        }
    }

    // Push every toggler's visual to match our bools. Only called on panel
    // open / refreshAll -- never from a toggler's own callback. toggle(bool)
    // SETS the state (not flips), so this is idempotent.
    void syncToggles() {
        auto& bot = BotManager::get();
        syncModeToggles();
        forceTogglerState(m_speedToggle,    bot.speedhackEnabled);
        forceTogglerState(m_practiceToggle, bot.practiceFixEnabled);
        forceTogglerState(m_deadToggle,     bot.discardDeadInputs);
        forceTogglerState(m_autoSaveToggle, bot.autoSaveOnComplete);
        forceTogglerState(m_mirrorToggle,    bot.mirrorEnabled);
        forceTogglerState(m_invertToggle,    bot.mirrorInvert);
        forceTogglerState(m_seedToggle,      bot.seedLockEnabled);
        forceTogglerState(m_waveToggle,      bot.waveMaintainEnabled);
        forceTogglerState(m_autoClickToggle, bot.autoClickEnabled);
    }

    void syncModeToggles() {
        auto& bot = BotManager::get();
        forceTogglerState(m_recordToggle, bot.mode == bot::Mode::Recording);
        forceTogglerState(m_playToggle,   bot.mode == bot::Mode::Playing);
    }

private:
    // ----- panel construction ---------------------------------------------
    void buildPanel() {
        constexpr float W = 320.f, H = 396.f;
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
        bg->setOpacity(255);
        m_panel->addChild(bg);

        // Title.
        auto title = CCLabelBMFont::create("Time Macro Bot", "goldFont.fnt");
        title->setPosition({ W * 0.5f, H - 18.f });
        title->setScale(0.72f);
        m_panel->addChild(title);

        // The coloured period + status line. The period is the green/yellow/red
        // CBF indicator the spec asked for.
        // Status indicator moved to the top-RIGHT corner of the panel:
        // [status text] [coloured .]   both right-anchored so they stay
        // inside the panel regardless of how long the status string is.
        m_periodLabel = CCLabelBMFont::create(".", "bigFont.fnt");
        m_periodLabel->setAnchorPoint({ 1.f, 0.5f });
        m_periodLabel->setPosition({ W - 14.f, H - 16.f });
        m_periodLabel->setScale(1.25f);
        m_panel->addChild(m_periodLabel);

        m_statusLabel = CCLabelBMFont::create("CBF: ...", "chatFont.fnt");
        m_statusLabel->setAnchorPoint({ 1.f, 0.5f });
        m_statusLabel->setPosition({ W - 30.f, H - 16.f });
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
        m_mainMenu = menu;

        // Settings gear in the top-left corner -> opens the settings page.
        cocos2d::CCNode* gearSpr =
            CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (gearSpr) {
            gearSpr->setScale(0.42f);
        } else {
            // Sprite sheet not loaded yet for some reason -- fall back to a
            // plain labelled button rather than crashing.
            auto bs = ButtonSprite::create("Cfg");
            bs->setScale(0.5f);
            gearSpr = bs;
        }
        auto gearBtn = CCMenuItemSpriteExtra::create(
            gearSpr, this, menu_selector(BotUILayer::onSettings));
        gearBtn->setPosition({ 24.f, H - 20.f });
        menu->addChild(gearBtn);

        auto& bot = BotManager::get();

        float yRec = H - 130.f;
        // Record / Play togglers (driven by mode, not a saved bool).
        m_recordToggle = makeToggle(menu, m_panel, { 40.f, yRec },
            menu_selector(BotUILayer::onRecord), "Record (V)",
            bot.mode == bot::Mode::Recording);
        m_playToggle = makeToggle(menu, m_panel, { 180.f, yRec },
            menu_selector(BotUILayer::onPlay), "Play (B)",
            bot.mode == bot::Mode::Playing);

        // Speedhack toggle + textbox.
        float ySpeed = yRec - 36.f;
        m_speedToggle = makeToggle(menu, m_panel, { 40.f, ySpeed },
            menu_selector(BotUILayer::onSpeedToggle), "Speed", bot.speedhackEnabled);
        m_speedInput = geode::TextInput::create(120.f, "speed", "chatFont.fnt");
        m_speedInput->setCommonFilter(geode::CommonFilter::Float);
        m_speedInput->setString(fmt::format("{:g}", bot.speed));
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
        m_nameInput->setString(bot.macroName);
        m_nameInput->setPosition({ 222.f, yName });
        m_nameInput->setScale(0.9f);
        m_nameInput->setCallback([](std::string const& s) {
            if (!s.empty()) { BotManager::get().macroName = s; BotManager::get().persist(); }
        });
        m_panel->addChild(m_nameInput);

        // Option togglers. Initial visual state is set from the saved bools so the
        // checkmarks always start in sync (no manual offset / no 480fps snap -- the
        // snap is automatic and only applies under RobTop's CBS).
        float yOpt = yName - 36.f;
        m_practiceToggle = makeToggle(menu, m_panel, { 40.f, yOpt },
            menu_selector(BotUILayer::onPracticeFix), "Practice fix",
            bot.practiceFixEnabled);
        m_deadToggle = makeToggle(menu, m_panel, { 180.f, yOpt },
            menu_selector(BotUILayer::onDeadInputs), "Discard dead",
            bot.discardDeadInputs);
        float yOpt2 = yOpt - 28.f;
        m_autoSaveToggle = makeToggle(menu, m_panel, { 40.f, yOpt2 },
            menu_selector(BotUILayer::onAutoSave), "Auto-save on finish",
            bot.autoSaveOnComplete);

        // File row 1: binary save / load + text export / import.
        float yFile1 = yOpt2 - 34.f;
        makeButton(menu, { 50.f, yFile1 },  "Save",   menu_selector(BotUILayer::onSave));
        makeButton(menu, { 120.f, yFile1 }, "Load",   menu_selector(BotUILayer::onLoad));
        makeButton(menu, { 200.f, yFile1 }, "Export", menu_selector(BotUILayer::onExport));
        makeButton(menu, { 272.f, yFile1 }, "Import", menu_selector(BotUILayer::onImport));

        // File row 2: clear / list / close.
        float yFile2 = yFile1 - 32.f;
        makeButton(menu, { 60.f, yFile2 },  "Clear", menu_selector(BotUILayer::onClear));
        makeButton(menu, { 160.f, yFile2 }, "List",  menu_selector(BotUILayer::onList));
        makeButton(menu, { 255.f, yFile2 }, "Close", menu_selector(BotUILayer::onClose));

        // Hint at the bottom.
        auto hint = CCLabelBMFont::create("K: menu   V: record   B: play   N: stop",
                                          "chatFont.fnt");
        hint->setPosition({ W * 0.5f, 14.f });
        hint->setScale(0.46f);
        hint->setOpacity(160);
        m_panel->addChild(hint);
    }

    // ----- settings page (gear button) -------------------------------------
    void buildSettingsPanel() {
        constexpr float W = 320.f, H = 396.f;
        auto winSize = CCDirector::sharedDirector()->getWinSize();

        m_settingsPanel = CCNode::create();
        m_settingsPanel->setContentSize({ W, H });
        m_settingsPanel->setAnchorPoint({ 0.5f, 0.5f });
        m_settingsPanel->setScale(0.72f);
        m_settingsPanel->setPosition({ winSize.width * 0.5f, winSize.height * 0.5f });
        this->addChild(m_settingsPanel);

        auto bg = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({ W, H });
        bg->setPosition({ W * 0.5f, H * 0.5f });
        m_settingsPanel->addChild(bg);

        auto title = CCLabelBMFont::create("Settings", "goldFont.fnt");
        title->setPosition({ W * 0.5f, H - 18.f });
        title->setScale(0.72f);
        m_settingsPanel->addChild(title);

        m_settingsMenu = CCMenu::create();
        m_settingsMenu->setPosition({ 0.f, 0.f });
        m_settingsMenu->setTouchPriority(-1000);
        m_settingsMenu->setEnabled(false); // main page is up first
        m_settingsPanel->addChild(m_settingsMenu);

        auto& bot = BotManager::get();

        // Row 1: input mirroring + the upside-down "invert" variant.
        float y = H - 70.f;
        m_mirrorToggle = makeToggle(m_settingsMenu, m_settingsPanel, { 40.f, y },
            menu_selector(BotUILayer::onMirror), "Mirror inputs",
            bot.mirrorEnabled);
        m_invertToggle = makeToggle(m_settingsMenu, m_settingsPanel, { 180.f, y },
            menu_selector(BotUILayer::onInvert), "Invert P2",
            bot.mirrorInvert);

        // Row 2: random-seed lock + the seed value.
        y -= 44.f;
        m_seedToggle = makeToggle(m_settingsMenu, m_settingsPanel, { 40.f, y },
            menu_selector(BotUILayer::onSeedLock), "Lock seed",
            bot.seedLockEnabled);
        m_seedInput = geode::TextInput::create(110.f, "seed", "chatFont.fnt");
        m_seedInput->setCommonFilter(geode::CommonFilter::Int);
        m_seedInput->setString(fmt::format("{}", bot.lockedSeed));
        m_seedInput->setPosition({ 240.f, y });
        m_seedInput->setScale(0.9f);
        m_seedInput->setCallback([](std::string const& s) {
            BotManager::get().setSeedFromString(s);
        });
        m_settingsPanel->addChild(m_seedInput);

        // Row 3: wave gravity maintain.
        y -= 44.f;
        m_waveToggle = makeToggle(m_settingsMenu, m_settingsPanel, { 40.f, y },
            menu_selector(BotUILayer::onWaveMaintain), "Maintain gravity (wave)",
            bot.waveMaintainEnabled);

        // Row 4: autoclicker + clicks-per-second.
        y -= 44.f;
        m_autoClickToggle = makeToggle(m_settingsMenu, m_settingsPanel, { 40.f, y },
            menu_selector(BotUILayer::onAutoClick), "Autoclick",
            bot.autoClickEnabled);
        auto cpsLbl = CCLabelBMFont::create("CPS", "chatFont.fnt");
        cpsLbl->setAnchorPoint({ 1.f, 0.5f });
        cpsLbl->setPosition({ 182.f, y });
        cpsLbl->setScale(0.55f);
        m_settingsPanel->addChild(cpsLbl);
        m_cpsInput = geode::TextInput::create(110.f, "cps", "chatFont.fnt");
        m_cpsInput->setCommonFilter(geode::CommonFilter::Float);
        m_cpsInput->setString(fmt::format("{:g}", bot.autoClickCPS));
        m_cpsInput->setPosition({ 240.f, y });
        m_cpsInput->setScale(0.9f);
        m_cpsInput->setCallback([](std::string const& s) {
            BotManager::get().setAutoClickCPSFromString(s);
        });
        m_settingsPanel->addChild(m_cpsInput);

        // One-line explanations so nobody has to guess.
        const char* hints[] = {
            "Mirror: live P1 inputs also drive P2 (and vice versa).",
            "Invert P2: mirrored copy acts upside-down (jump flipped, L/R swapped).",
            "Lock seed: reseeds the RNG with this value every attempt.",
            "Maintain gravity: wave hold direction survives gravity portals.",
            "Autoclick: clicks P1 jump at the given clicks per second.",
        };
        float hy = y - 38.f;
        for (auto* h : hints) {
            auto lbl = CCLabelBMFont::create(h, "chatFont.fnt");
            lbl->setAnchorPoint({ 0.f, 0.5f });
            lbl->setPosition({ 18.f, hy });
            lbl->setScale(0.42f);
            lbl->setOpacity(160);
            m_settingsPanel->addChild(lbl);
            hy -= 15.f;
        }

        makeButton(m_settingsMenu, { W * 0.5f, 26.f }, "Back",
                   menu_selector(BotUILayer::onSettingsBack));

        m_settingsPanel->setVisible(false);
    }

    CCMenuItemToggler* makeToggle(CCMenu* menu, cocos2d::CCNode* panel, CCPoint pos,
                                  cocos2d::SEL_MenuHandler sel, const char* label,
                                  bool initialState) {
        auto toggle = CCMenuItemToggler::createWithStandardSprites(this, sel, 0.6f);
        toggle->setPosition(pos);
        // Set the initial checkmark state to match the saved option so the
        // checkmarks never start out of sync (toggle(bool) just sets state and
        // does NOT fire the selector, so this is safe).
        forceTogglerState(toggle, initialState);
        menu->addChild(toggle);

        auto lbl = CCLabelBMFont::create(label, "chatFont.fnt");
        lbl->setAnchorPoint({ 0.f, 0.5f });
        lbl->setPosition({ pos.x + 16.f, pos.y });
        lbl->setScale(0.55f);
        panel->addChild(lbl);
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
        refreshProgress();
        geode::queueInMainThread([this]() {
        syncModeToggles();
        });
    }
    void onPlay(CCObject*) {
        BotManager::get().togglePlayback(GJBaseGameLayer::get());
        refreshProgress();
        geode::queueInMainThread([this]() {
         syncModeToggles();
        });
    }
       // Each callback: activate() has ALREADY flipped the visual. We just flip
    // our bool to match and persist. We do NOT call toggle() here -- doing so
    // can double-flip on some Geode versions where toggle() doesn't have an
    // early-return guard. syncToggles() on next panel open will fix any drift.
   void onSpeedToggle(CCObject*) {
        auto& bot = BotManager::get();
        bot.speedhackEnabled = !bot.speedhackEnabled;
        bot.persist();
        bot.applyMusicSpeed();
        // No refreshAll() — activate() flips the visual correctly after this
        // callback returns. Calling syncToggles() here would be undone by
        // activate()'s own flip the moment our callback exits.
    }
    void onPracticeFix(CCObject*) {
        auto& bot = BotManager::get();
        bot.practiceFixEnabled = !bot.practiceFixEnabled;
        bot.persist();
    }
    void onDeadInputs(CCObject*) {
        auto& bot = BotManager::get();
        bot.discardDeadInputs = !bot.discardDeadInputs;
        bot.persist();
    }
    void onAutoSave(CCObject*) {
        auto& bot = BotManager::get();
        bot.autoSaveOnComplete = !bot.autoSaveOnComplete;
        bot.persist();
    }

    // ----- settings-page callbacks ------------------------------------------
    void onSettings(CCObject*)     { showSettings(true); }
    void onSettingsBack(CCObject*) { showSettings(false); }
    void onMirror(CCObject*) {
        auto& bot = BotManager::get();
        bot.mirrorEnabled = !bot.mirrorEnabled;
        bot.persist();
    }
    void onInvert(CCObject*) {
        auto& bot = BotManager::get();
        bot.mirrorInvert = !bot.mirrorInvert;
        bot.persist();
    }
    void onSeedLock(CCObject*) {
        auto& bot = BotManager::get();
        bot.seedLockEnabled = !bot.seedLockEnabled;
        bot.persist();
    }
    void onWaveMaintain(CCObject*) {
        auto& bot = BotManager::get();
        // setWaveMaintain also re-syncs any phantom hold when turning off.
        bot.setWaveMaintain(GJBaseGameLayer::get(), !bot.waveMaintainEnabled);
    }
    void onAutoClick(CCObject*) {
        auto& bot = BotManager::get();
        bot.autoClickEnabled = !bot.autoClickEnabled;
        bot.persist();
    }
    void onSave(CCObject*) {
        auto& bot = BotManager::get();
        if (bot.physicsMode) bot.savePhysicsMacro(bot.macroName);
        else bot.saveMacro(bot.macroName);
    }
    void onLoad(CCObject*) {
        auto& bot = BotManager::get();
        if (bot.physicsMode) bot.loadPhysicsMacro(bot.macroName);
        else bot.loadMacro(bot.macroName);
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