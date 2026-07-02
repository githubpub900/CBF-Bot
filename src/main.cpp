/**
 * ============================================================================
 *  main.cpp  --  Geode hook wiring for the "Geode Time Macro" bot.
 * ============================================================================
 *
 *  Target : Geometry Dash 2.2081
 *  Loader : Geode v5.7.1
 *
 *  This translation unit contains nothing but the thin glue between Geometry
 *  Dash's runtime and the brain that lives in Bot.hpp. Every hook here is a
 *  small, well-commented shim that forwards into BotManager so the actual logic
 *  stays in one testable place.
 *
 *  The hooks, at a glance:
 *
 *    GJBaseGameLayer::handleButton    -> capture inputs at the lowest level
 *                                        (this is where CBF feeds clicks in)
 *    PlayerObject::update             -> per-SUBSTEP heartbeat: macro playback
 *                                        firing, sub-step clock, autoclicker,
 *                                        wave gravity correction
 *    GJBaseGameLayer::processCommands -> per-frame clock drift guard, physics
 *                                        frame recording, debug position replay
 *    GJBaseGameLayer::update          -> per-frame backstop for playback
 *    GJBaseGameLayer::getModifiedDelta-> speedhack + frame stepping
 *
 *    PlayLayer::init                  -> spawn the floating UI, reset state
 *    PlayLayer::resetLevel            -> rewind playback cursor
 *    PlayLayer::resetLevelFromStart   -> full restart -> revert later inputs
 *    PlayLayer::destroyPlayer         -> death handling
 *    PlayLayer::storeCheckpoint       -> snapshot velocity + player state
 *    PlayLayer::loadFromCheckpoint    -> restore snapshot + discard dead inputs
 *    PlayLayer::removeCheckpoint      -> keep our checkpoint stack in sync
 *
 *    PauseLayer::onRestart            -> revert inputs on a pause-menu restart
 *    PauseLayer::onRestartFull        -> ditto
 * ============================================================================
 */

#include "Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;


// ============================================================================
//  Small helpers
// ============================================================================

// Is this base-game-layer actually the PlayLayer? Both the editor preview and
// the real level run through GJBaseGameLayer, and we only ever want to touch the
// real one, so every hook funnels through this guard.
static inline bool isPlay(GJBaseGameLayer* self) {
    return typeinfo_cast<PlayLayer*>(self) != nullptr;
}

// ============================================================================
//  GJBaseGameLayer hooks
// ============================================================================
//
//  GJBaseGameLayer is the shared base of PlayLayer and LevelEditorLayer, and --
//  importantly -- it is the layer that Syzzi's Click Between Frames drives its
//  sub-frame inputs through. handleButton is the single chokepoint for *every*
//  jump / left / right, whether it came from a real key press, a touch, RobTop's
//  CBS, or Syzzi's CBF queue. That makes it the perfect place to both record and
//  inject, which is exactly why the whole bot is built around it.
//
class $modify(BotBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        // CBF uses "VeryEarly" priority. We need to run BEFORE CBF so our
        // inputs are in m_queuedButtons before CBF's buildStepQueue() reads them.
        // In Geode, LOWER priority number = runs EARLIER in the hook chain.
        (void) self.setHookPriority("GJBaseGameLayer::getModifiedDelta", -1000000);
        (void) self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
    }

    // ---- input capture (recording) ---------------------------------------
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& bot = BotManager::get();
        bool play = isPlay(this);

        // Wave "maintain gravity": may rewrite a live jump transition (so the
        // hold direction survives gravity portals) or swallow it entirely if
        // the rewritten transition is redundant. Only for raw user input --
        // playback (injecting) and our own re-dispatches (selfDispatch) have
        // already been through this.
        if (play && !bot.injecting && !bot.selfDispatch && button == 1) {
            if (!bot.filterLiveJump(down, isPlayer1)) return;
        }

        // Record every non-injecting handleButton transition. Full fidelity:
        // NO dedup, no death-time filter -- letting our own selfDispatched
        // corrections (mirror, wave gravity, autoclicker) update a shared
        // "last-seen state" table was silently swallowing the user's later
        // matching release as a duplicate. The result was uneven press /
        // release counts, holds that lasted way too long, and desynced
        // playback. Truncation on death / checkpoint reload handles the
        // "dropped an input on the ground while dying" case cleanly.
        if (play && !bot.injecting) {
            bot.recordInput(this, down, button, isPlayer1);
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // Input mirroring: forward a copy of every live input to the other
        // player (inverted if that option is on). The copy is dispatched with
        // selfDispatch set so it isn't mirrored back, but it IS recorded.
        if (play && !bot.injecting && !bot.selfDispatch && bot.mirrorEnabled) {
            bot.dispatchMirrored(this, down, button, isPlayer1);
        }
    }
    
    void update(float dt) {
        if (isPlay(this) && BotManager::get().guiPaused) {
            return;
        }
        GJBaseGameLayer::update(dt);
        if (isPlay(this)) {
            BotManager::get().syncRecordingToTime(this);
        }
        BotManager::get().applyMusicSpeed();
    }

// ---- playback (primary, per physics step) ----------------------------
    // processCommands runs once per physics step. With Syzzi's CBF active there
    // are many tiny steps per rendered frame, so firing our due inputs here gives
    // sub-frame accuracy: the worst-case error between the recorded timestamp and
    // the moment we replay it is a single physics sub-step, because m_levelTime
    // has already been advanced to this sub-step's value by the time we read it.
    //
    // We fire inputs BEFORE the original so CBF/the engine sees them during this
    // sub-step. We do NOT teleport the player — position teleportation fights
    // GD's own continuous collision detection and causes the engine to register
    // deaths whenever consecutive recorded positions straddle a hazard. Pure
    // input replay lets physics (and collision) run naturally from replayed
    // button state, keyed on the same level-time clock inputs were recorded with.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& bot = BotManager::get();
        // Input firing / autoclick / wave correction all live in the
        // PlayerObject::update hook below -- per CBF's own source, that's
        // where the physics substeps and CBF's input dispatch happen, one
        // call per substep with the substep dt. Nothing to do here.
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        // NOTE: applyPhysicsPosition() below is DEBUG-ONLY (settings
        // checkbox, off by default). Teleporting the player to a recorded
        // position every step fights GD's own continuous collision detection
        // (a forced position jump can skip clean past -- or shove the player
        // into -- a hazard edge that natural, input-driven movement would
        // have handled correctly), and it can cause the engine to re-evaluate
        // ground/landing state in a way that makes a queued input appear to
        // fire twice. Pure input replay against an identical starting state
        // and identical held-button timeline is the real playback mechanism.
        if (isPlay(this) && bot.mode == bot::Mode::Recording) {
            bot.recordPhysicsFrame(bot.playClock());
        }
        if (isPlay(this) && bot.mode == bot::Mode::Playing &&
            bot.physicsDebugEnabled) {
            bot.applyPhysicsPosition(bot.playClock());
        }
    }
    // ---- frame-rate independent speedhack --------------------------------
    //
    // getModifiedDelta returns the delta the physics engine actually steps with.
    // Scaling its output scales game time directly. Because the macro is driven
    // by m_gameState.m_levelTime (which advances by this very delta) and not by
    // the render frame-rate, you can crank the speed arbitrarily high without the
    // bot desyncing or "lagging behind" -- the clock and the inputs scale together.
    double getModifiedDelta(float dt) {
        auto& bot = BotManager::get();
        if (isPlay(this) && bot.guiPaused) return 0.0;
        // Frame stepping: freeze the level and hand out exactly one
        // 1/frameStepFps slice per queued step (M key). We MUST call
        // GJBaseGameLayer::getModifiedDelta so CBF's own hook further down
        // the chain runs its step-count math on the fake dt -- returning
        // a raw 1/fps here bypassed CBF entirely (my hook's priority is
        // earlier than CBF's), which is why "something stepped but the
        // player didn't move": no CBF hook run == no substep processing
        // == no PlayerObject::update calls == no physics.
        if (isPlay(this) && bot.frameStepEnabled) {
            if (bot.pendingSteps > 0) {
                --bot.pendingSteps;
                double fps = (std::isfinite(bot.frameStepFps) &&
                              bot.frameStepFps >= 1.0)
                             ? bot.frameStepFps : 240.0;
                float fakeDt = 1.0f / static_cast<float>(fps);
                return GJBaseGameLayer::getModifiedDelta(fakeDt);
            }
            return 0.0;
        }
        double modified = GJBaseGameLayer::getModifiedDelta(dt);
        if (bot.speedhackEnabled && isPlay(this)) {
            modified *= bot.speedMultiplier();
        }
        return modified;
    }
};

// ============================================================================
//  PlayerObject hook  --  the per-SUBSTEP heartbeat of the bot
// ============================================================================
//
//  Per CBF's own source (theyareonit/Click-Between-Frames), the physics
//  substeps -- and CBF's dispatch of queued OS inputs via handleButton --
//  happen inside the per-substep player update, NOT in processCommands
//  (which runs once per rendered frame). So this is the only hook granular
//  enough to (a) replay macro inputs on the exact substep they were recorded
//  on, and (b) advance a clock with true substep resolution. Player 1 drives
//  the clock and the input firing; the wave gravity correction runs for each
//  player right after their own substep so a mid-frame portal flip is
//  corrected before the next substep.
//
class $modify(BotPlayerObject, PlayerObject) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!pl) {  // menus, editor preview, icon kit -- not our players
            PlayerObject::update(dt);
            return;
        }
        // ONLY the real gameplay players count -- anything else (ghost
        // trail, dual portal preview, spider secondary etc.) that shares
        // this class must not double-drive our per-substep hooks. The
        // reentrancy guard inside onSubStepBegin is a second belt.
        bool isP1 = (this == pl->m_player1);
        bool isP2 = (this == pl->m_player2);
        if (!isP1 && !isP2) {
            PlayerObject::update(dt);
            return;
        }
        auto& bot = BotManager::get();
        bot.onSubStepBegin(pl, this, isP1);
        PlayerObject::update(dt);
        bot.onSubStepEnd(pl, this, isP1);
    }
};

// ============================================================================
//  PlayLayer hooks
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {

    // ---- level init: reset state (the UI is spawned globally now) --------
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        BotManager::get().onLevelReset(this);
        // The floating GUI is created once in $on_mod(Loaded) and kept across
        // every scene by geode::SceneManager, so it works on the menu, in the
        // editor, AND inside levels. No per-PlayLayer spawn needed.
        return true;
    }

    // (spawnUI() removed -- no longer used)

    // ---- resetLevel: rewind the playback cursor --------------------------
    void resetLevel() {
        PlayLayer::resetLevel();
        BotManager::get().onLevelReset(this);
    }

    // ---- full restart from the start: revert later inputs ----------------
    void resetLevelFromStart() {
        PlayLayer::resetLevelFromStart();
        BotManager::get().onRestart(this, /*fromStart=*/true);
    }

    // ---- death handling --------------------------------------------------
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        BotManager::get().onPlayerDeath(this);
    }

    // ---- checkpoint stored: snapshot velocity + full player state --------
    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        BotManager::get().onCheckpointStored(this, static_cast<void*>(checkpoint));
    }

    // ---- checkpoint loaded: restore snapshot + discard dead inputs -------
    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        BotManager::get().onCheckpointLoaded(this, static_cast<void*>(checkpoint));
    }

    // ---- checkpoint removed: keep our stack in sync ----------------------
    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        BotManager::get().onCheckpointRemoved();
    }

    // ---- level complete: finish playback cleanly -------------------------
    void levelComplete() {
        BotManager::get().onLevelComplete(this);
        PlayLayer::levelComplete();
    }

    // ---- leaving the level: reset audio pitch so menu music is normal ----
    void onExit() {
        PlayLayer::onExit();
        auto& bot = BotManager::get();
        // Reset m_timeWarp and audio pitch when leaving the level
        this->m_gameState.m_timeWarp = 1.0f;
        bot.resetAudioPitch();
    }
};

// ============================================================================
//  CCKeyboardDispatcher hook  --  catch K / V / B / N on EVERY screen
// ============================================================================
//
//  The floating BotUILayer relies on scheduleUpdate() + onEnter() to register
//  its keyboard delegate, which is fragile across scene transitions. Hooking
//  the dispatcher directly is bulletproof: K opens the menu on the main menu,
//  in the editor, inside a level, and while paused.
//
class $modify(BotKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown,
                             bool isKeyRepeat, double timestamp) {
        // M -> advance one frame-step slice. Handled BEFORE the repeat gate
        // on purpose: holding M scrubs forward continuously.
        if (isKeyDown && key == cocos2d::enumKeyCodes::KEY_M) {
            auto& bot = BotManager::get();
            if (bot.frameStepEnabled && PlayLayer::get()) {
                bot.queueFrameStep();
                return true;
            }
        }
        if (isKeyDown && !isKeyRepeat) {  // ignore auto-repeat key holds
            auto& bot = BotManager::get();

            // K -> toggle the GUI (works on every screen)
            if (key == bot::TOGGLE_KEY) {
                if (bot.ui) {
                    bot.ui->ensureInScene();   // make sure we're parented to the current scene
                    bot.ui->togglePanel();
                }
                return true; // swallow K so it never reaches gameplay
            }

            // V / B / N only make sense during gameplay
            if (PlayLayer::get()) {
                switch (key) {
                    case cocos2d::enumKeyCodes::KEY_V:
                        bot.toggleRecording(GJBaseGameLayer::get());
                        if (bot.ui) bot.ui->refreshAll();
                        return true;
                    case cocos2d::enumKeyCodes::KEY_B:
                        bot.togglePlayback(GJBaseGameLayer::get());
                        if (bot.ui) bot.ui->refreshAll();
                        return true;
                    case cocos2d::enumKeyCodes::KEY_N:
                        bot.stop();
                        if (bot.ui) bot.ui->refreshAll();
                        return true;
                    default: break;
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown,
                                                         isKeyRepeat, timestamp);
    }
};

// ============================================================================
//  PauseLayer hooks
// ============================================================================
//
//  The pause-menu restart buttons rewind the run to the start, so the macro
//  should rewind with them: every input recorded after the restart point is
//  reverted. We notify the manager *before* the original transitions away.
//
class $modify(BotPauseLayer, PauseLayer) {

    void onRestart(CCObject* sender) {
        BotManager::get().onRestart(PlayLayer::get(), /*fromStart=*/true);
        PauseLayer::onRestart(sender);
    }

    void onRestartFull(CCObject* sender) {
        BotManager::get().onRestart(PlayLayer::get(), /*fromStart=*/true);
        PauseLayer::onRestartFull(sender);
    }
};

// ============================================================================
//  MenuLayer hook  --  spawn the floating UI once GD is fully loaded
// ============================================================================
//
//  $on_mod(Loaded) fires very early, before GD has loaded its sprite sheets.
//  If we build the panel there, CCMenuItemToggler::createWithStandardSprites
//  looks up "GJ_checkOff.png" / "GJ_checkOn.png" in an empty sprite frame
//  cache and falls back to the missing-texture grid (purple/black squares).
//
//  MenuLayer::init fires when the main menu first appears, by which point
//  every sprite sheet GD uses is loaded. Spawning the UI here guarantees
//  the checkbox sprites resolve correctly.
//
class $modify(BotMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto& bot = BotManager::get();
        if (!bot.ui) {
            if (auto ui = BotUILayer::create()) {
                ui->retain();  // keep alive across scene transitions
                bot.ui = ui;
                // Parent to the running scene; update() / ensureInScene()
                // will re-parent us on every subsequent scene transition.
                if (auto scene = CCDirector::sharedDirector()->getRunningScene()) {
                    scene->addChild(ui, (std::numeric_limits<int>::max)());
                }
                ui->refreshAll();
            }
        }
        return true;
    }
};

// ============================================================================
//  Mod entry point
// ============================================================================

 $on_mod(Loaded) {
    auto& bot = BotManager::get();

    // Pull any persisted options back in.
    bot.speed                = Mod::get()->getSavedValue<double>("speed", 1.0);
    bot.speedhackEnabled     = Mod::get()->getSavedValue<bool>("speedhack", false);
    bot.practiceFixEnabled   = Mod::get()->getSavedValue<bool>("practice-fix", true);
    bot.discardDeadInputs    = Mod::get()->getSavedValue<bool>("discard-dead", true);
    bot.autoSaveOnComplete   = Mod::get()->getSavedValue<bool>("auto-save", true);
    bot.macroName            = Mod::get()->getSavedValue<std::string>("macro-name", "macro");
    bot.mirrorEnabled        = Mod::get()->getSavedValue<bool>("mirror", false);
    bot.mirrorInvert         = Mod::get()->getSavedValue<bool>("mirror-invert", false);
    bot.seedLockEnabled      = Mod::get()->getSavedValue<bool>("seed-lock", false);
    bot.lockedSeed           = static_cast<int>(
                                   Mod::get()->getSavedValue<int64_t>("seed-value", 1337));
    bot.waveMaintainEnabled  = Mod::get()->getSavedValue<bool>("wave-maintain", false);
    bot.autoClickEnabled     = Mod::get()->getSavedValue<bool>("auto-click", false);
    bot.autoClickHz          = Mod::get()->getSavedValue<double>("auto-hz", 10.0);
    bot.autoClickHoldRatio   = Mod::get()->getSavedValue<double>("auto-hold", 0.5);
    bot.frameStepEnabled     = Mod::get()->getSavedValue<bool>("frame-step", false);
    bot.frameStepFps         = Mod::get()->getSavedValue<double>("step-fps", 240.0);
    bot.physicsDebugEnabled  = Mod::get()->getSavedValue<bool>("physics-debug", false);

    switch (bot.cbfState()) {
        case bot::CBFState::Syzzi:
            log::info("[Bot] Syzzi Click Between Frames detected -- full "
                      "sub-frame precision available (green).");
            break;
        case bot::CBFState::RobTop:
            log::info("[Bot] RobTop Click Between Steps detected -- limited "
                      "~480fps input window (yellow).");
            break;
        case bot::CBFState::None:
            log::warn("[Bot] No Click Between Frames found -- the bot will be "
                      "disabled until CBF is enabled (red). Install '{}'.",
                      bot::SYZZI_CBF_ID);
            break;
    }

    log::info("[Bot] Geode Time Macro loaded. Press K anywhere to open the menu.");
}

// Note: Geode has no "unloaded" mod event, so options are persisted eagerly by
// BotManager::persist() whenever they change (see Bot.hpp).

// ============================================================================
//  BUILD, INSTALL & TROUBLESHOOTING NOTES
// ============================================================================
//
//  Since the CMake is already set up, the only things to get right outside this
//  file are the Geode metadata and the runtime expectations:
//
//  -- mod.json -------------------------------------------------------------
//  Target Geometry Dash 2.2081 and Geode v5.7.1, and declare Syzzi's Click
//  Between Frames as a RECOMMENDED (not required) dependency so the loader can
//  surface it but the mod still loads without it (we degrade to the red state):
//
//      {
//          "geode": "5.7.1",
//          "gd": { "win": "2.2081" },
//          "id": "you.time-macro-bot",
//          "name": "Time Macro Bot",
//          "version": "1.0.0",
//          "developer": "you",
//          "dependencies": [
//              {
//                  "id": "syzzi.click_between_frames",
//                  "version": ">=1.0.0",
//                  "importance": "recommended"
//              }
//          ]
//      }
//
//  We intentionally do NOT mark CBF "required": the mod should still load so the
//  GUI can show the red "." and explain that CBF is needed, rather than refusing
//  to load at all.
//
//  -- Why the bot is gated on CBF -----------------------------------------
//  Without CBF the engine only samples input once per rendered frame, so a macro
//  recorded with sub-frame timing cannot be reproduced faithfully (and a macro
//  recorded without it would itself be frame-locked). BotManager::cbfAvailable()
//  therefore blocks record/playback in the red state. Syzzi (green) is preferred
//  because its input window is effectively unbounded; RobTop's CBS (yellow) works
//  but is capped at ~480 FPS, which is why we snap timestamps to that grid.
//
//  -- Hook ordering with CBF ----------------------------------------------
//  CBF also hooks getModifiedDelta / the stepping path. Geode resolves hook order
//  by priority and runs every mod's hook, so our speedhack multiply and our
//  per-step playback both compose correctly with CBF's own work. We deliberately
//  read m_gameState.m_levelTime (which CBF keeps correct per sub-step) rather than
//  trying to reimplement CBF's timing, so we stay compatible by construction.
//
//  -- If K does nothing ----------------------------------------------------
//  The GUI catches K via a CCLayer keyboard delegate (setKeyboardEnabled), which
//  is reliable across platforms -- unlike hooking CCKeyboardDispatcher, which has
//  no Windows address in the current bindings snapshot. If another mod consumes
//  the key first, change bot::TOGGLE_KEY in Bot.hpp.
//
//  -- If checkpoints feel off ---------------------------------------------
//  The accurate practice fix re-applies our snapshot AFTER the engine's own
//  loadFromCheckpoint. If a future GD update reorders that, snapshots may need to
//  be applied on the next frame instead; see BotManager::onCheckpointLoaded.
//
// ============================================================================