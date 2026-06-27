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
 *    GJBaseGameLayer::processCommands -> per-physics-step playback firing
 *    GJBaseGameLayer::update          -> per-frame backstop for playback
 *    GJBaseGameLayer::getModifiedDelta-> frame-rate independent speedhack
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

        // Record the *transition* (press/release) tagged with the current level
        // time. We deliberately do this before calling the original so the
        // timestamp matches the exact moment the engine is about to react to the
        // input -- and we skip anything we injected ourselves during playback.
        if (!bot.injecting && isPlay(this)) {
            bot.recordInput(this, down, button, isPlayer1);
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // ---- playback (primary, per physics step) ----------------------------
    // processCommands runs once per physics step. With Syzzi's CBF active there
    // are many tiny steps per rendered frame, so firing our due inputs here gives
    // sub-frame accuracy: the worst-case error between the recorded timestamp and
    // the moment we replay it is a single physics sub-step.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& bot = BotManager::get();
        if (isPlay(this) && bot.mode == bot::Mode::Playing) {
            if (bot.physicsMode) {
                // Apply physics frame BEFORE processCommands
                bot.applyPhysicsFrame(BotManager::levelTime(this));
                // Also fire any due inputs (for CPS counters, rings/pads)
                bot.fireDueInputs(this, 0.0f);
            } else {
                bot.onPhysicsStepStart(BotManager::levelTime(this), dt);
                bot.fireDueInputs(this, dt);
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (isPlay(this) && bot.mode == bot::Mode::Recording && bot.physicsMode) {
            bot.recordPhysicsFrame(BotManager::levelTime(this));
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
        double modified = GJBaseGameLayer::getModifiedDelta(dt);
        if (bot.speedhackEnabled && isPlay(this)) {
            modified *= bot.speedMultiplier();
        }
        return modified;
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
        BotManager::get().resetAudioPitch();
    }
};

// ============================================================================
//  PlayerObject::update hook  --  fire at exact sub-step level time
// ============================================================================
//
//  CBF splits each physics step into sub-steps inside PlayerObject::update.
//  By hooking here with VeryEarly priority, we run BEFORE CBF's splitting,
//  once per sub-step. We interpolate within the step using the sub-step
//  delta to compute the exact level time, then fire inputs due at that time.
//
//  This gives:
//  - Sub-step accuracy (~1ms at 240Hz)
//  - No hold bug (fires directly via handleButton, no queue deferral)
//  - Determinism (level time, not wall-clock)
//  - Speed-independence (CBF maintains 240Hz regardless of speedhack)
//
class $modify(BotPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void) self.setHookPriority("PlayerObject::update", 1000000);
    }

    void update(float dt) {
        auto& bot = BotManager::get();
        // Only use sub-step firing for input bot mode (not physics mode)
        if (!bot.physicsMode && bot.mode == bot::Mode::Playing &&
            this->m_gameLayer &&
            this == this->m_gameLayer->m_player1) {
            bot.fireDueInputsSubStep(this->m_gameLayer, dt);
        }
        PlayerObject::update(dt);
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
    bot.stateAlignEnabled  = Mod::get()->getSavedValue<bool>("state-align", false);
    bot.physicsMode = Mod::get()->getSavedValue<bool>("physics-mode", true);

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