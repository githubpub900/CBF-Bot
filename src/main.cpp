/**
 * ============================================================================
 * main.cpp  --  Geode hook wiring for the "Geode Time Macro" bot.
 * ============================================================================
 *
 * Target : Geometry Dash 2.2081
 * Loader : Geode v5.7.1
 *
 * This translation unit contains nothing but the thin glue between Geometry
 * Dash's runtime and the brain that lives in Bot.hpp. Every hook here is a
 * small, well-commented shim that forwards into BotManager so the actual logic
 * stays in one testable place.
 *
 * The hooks, at a glance:
 *
 * GJBaseGameLayer::handleButton    -> capture inputs at the lowest level
 * (this is where CBF feeds clicks in)
 * GJBaseGameLayer::processCommands -> per-physics-step playback firing
 * GJBaseGameLayer::update          -> per-frame backstop for playback
 * GJBaseGameLayer::getModifiedDelta-> frame-rate independent speedhack
 *
 * CCScene::onEnter                 -> ensure the floating UI is spawned globally
 * and moved to the active scene
 *
 * PlayLayer::init                  -> reset macro state on level enter
 * PlayLayer::resetLevel            -> rewind playback cursor
 * PlayLayer::resetLevelFromStart   -> full restart -> revert later inputs
 * PlayLayer::destroyPlayer         -> death handling
 * PlayLayer::storeCheckpoint       -> snapshot velocity + player state
 * PlayLayer::loadFromCheckpoint    -> restore snapshot + discard dead inputs
 * PlayLayer::removeCheckpoint      -> keep our checkpoint stack in sync
 *
 * PauseLayer::onRestart            -> revert inputs on a pause-menu restart
 * PauseLayer::onRestartFull        -> ditto
 * ============================================================================
 */

#include "Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScene.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PauseLayer.hpp>

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
//  Global UI Hook
// ============================================================================
//
//  Hooking CCScene::onEnter guarantees our Bot UI stays globally active regardless
//  of whether the user is in a level, the main menu, or anywhere else. Every time
//  the game changes scenes, we gently re-parent our persistent UI node to the top
//  of the new scene so it follows the user everywhere.
//
class $modify(GlobalBotUIHook, CCScene) {
    void onEnter() {
        CCScene::onEnter();
        
        auto& bot = BotManager::get();
        if (!bot.ui) {
            bot.ui = BotUILayer::create();
            if (bot.ui) {
                bot.ui->retain(); // Keep alive across scene transitions
            }
        }
        
        // Ensure it's gracefully sitting on the very top of the newly active scene
        if (bot.ui && bot.ui->getParent() != this) {
            if (bot.ui->getParent()) {
                bot.ui->removeFromParentAndCleanup(false);
            }
            this->addChild(bot.ui, (std::numeric_limits<int>::max)());
        }
    }
};

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
    //
    // processCommands runs once per physics step. With Syzzi's CBF active there
    // are many tiny steps per rendered frame, so firing our due inputs here gives
    // sub-frame accuracy: the worst-case error between the recorded timestamp and
    // the moment we replay it is a single physics sub-step.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (isPlay(this)) {
            BotManager::get().fireDueInputs(this);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    // ---- playback (backstop, per frame) ----------------------------------
    //
    // In the rare case a CBF build does not route through processCommands every
    // step, this per-frame backstop guarantees forward progress. The playback
    // cursor is monotonic, so an input can never be fired twice.
    void update(float dt) {
        // Freeze gameplay while the GUI is open (our own pause -- no menu).
        if (isPlay(this) && BotManager::get().guiPaused) {
            return;
        }
        GJBaseGameLayer::update(dt);
        if (isPlay(this)) {
            // Detect restarts / respawns (level clock jumping backwards) so a
            // re-recorded attempt overwrites the superseded inputs instead of
            // appending them.
            BotManager::get().syncRecordingToTime(this);
            BotManager::get().fireDueInputs(this);
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
        // While the GUI is open the level is frozen: hand back a zero delta so no
        // physics steps run and m_levelTime does not advance (keeps the bot in sync).
        if (isPlay(this) && bot.guiPaused) {
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
//  PlayLayer hooks
// ============================================================================

class $modify(BotPlayLayer, PlayLayer) {

    // ---- level init: reset transient tracking state ----------------------
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        BotManager::get().onLevelReset(this);
        return true;
    }

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
    //
    // storeCheckpoint is the funnel every practice checkpoint passes through
    // (manual or automatic). We snapshot here, *after* the engine has registered
    // the checkpoint, so our parallel stack stays one-to-one with
    // m_checkpointArray and we capture the exact physics state at that instant.
    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        BotManager::get().onCheckpointStored(this, static_cast<void*>(checkpoint));
    }

    // ---- checkpoint loaded: restore snapshot + discard dead inputs -------
    //
    // We let the engine do its own (imperfect) restore first, then apply our
    // accurate snapshot on top -- correcting the velocity / per-gamemode flags
    // that vanilla practice mode gets wrong. We also truncate the recording back
    // to the checkpoint, throwing away the "dead inputs" made on the failed
    // attempt.
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
//  Mod entry point
// ============================================================================

$on_mod(Loaded) {
    auto& bot = BotManager::get();

    // Pull any persisted options back in.
    bot.speed                = Mod::get()->getSavedValue<double>("speed", 1.0);
    bot.speedhackEnabled     = Mod::get()->getSavedValue<bool>("speedhack", false);
    bot.practiceFixEnabled   = Mod::get()->getSavedValue<bool>("practice-fix", true);
    bot.discardDeadInputs    = Mod::get()->getSavedValue<bool>("discard-dead", true);
    bot.autoSaveOnComplete   = Mod::get()->getSavedValue<bool>("auto-save", false);
    bot.macroName            = Mod::get()->getSavedValue<std::string>("macro-name", "macro");

    // Report the CBF situation so the log makes the green/yellow/red state obvious.
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

    log::info("[Bot] Geode Time Macro loaded. Press K to open the menu.");
}

// Note: Geode has no "unloaded" mod event, so options are persisted eagerly by
// BotManager::persist() whenever they change (see Bot.hpp).