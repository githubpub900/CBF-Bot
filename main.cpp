// ============================================================================
//  CBF Macro Bot - Geode mod entry point and GD hooks
// ----------------------------------------------------------------------------
//  All Geometry Dash hooks live here. The hooks are intentionally thin: they
//  extract the relevant data from GD's classes and forward it to MacroBot,
//  which routes events to the appropriate subsystem (recorder, player, etc.).
//
//  Hooks installed:
//    $execute                           - mod init
//    PlayLayer::init                    - level entered (reset state, auto-record)
//    PlayLayer::~PlayLayer              - level exited (stop recording/playback)
//    PlayLayer::update                  - speedhack + multi-step + tick counter
//    PlayerObject::pushButton           - capture P1/P2 press
//    PlayerObject::releaseButton        - capture P1/P2 release
//    PlayerObject::update               - per-physics-tick counter
//    CCKeyboardDispatcher::dispatchKeyMSG - hotkeys (F6 record, F7 play)
// ============================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>

#include "MacroBot.hpp"
#include "CBFIntegration.hpp"
#include "Speedhack.hpp"
#include "PracticeFix.hpp"
#include "MacroRecorder.hpp"
#include "MacroPlayer.hpp"
#include "UI/MacroLayer.hpp"

using namespace geode::prelude;

// ---------------------------------------------------------------------------
//  Mod entry point
// ---------------------------------------------------------------------------
$execute {
    MacroBot::get().initialize();
    log::info("[CBFMacroBot] Mod loaded. Geode version: {}",
              Loader::get()->getVersion().toNonVString());
}

// ---------------------------------------------------------------------------
//  PlayLayer hooks
// ---------------------------------------------------------------------------
class $modify(CBFPlayLayer, PlayLayer) {
    // PlayLayer::init is called when a level is entered (either fresh or
    // via retry). We use it to reset all bot state and possibly auto-start
    // recording in practice mode.
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Defer to next tick so all PlayLayer fields are initialized.
        bool isPractice = this->m_isPracticeMode;
        Loader::get()->queueInMainThread([isPractice]() {
            MacroBot::get().onPlayLayerInit(isPractice);
        });
        return true;
    }

    // PlayLayer::update is the per-frame entry point. We:
    //   1. Compute the speedhack plan (scaled dt + number of sub-steps).
    //   2. Apply the practice-fix spike clamp to the raw dt.
    //   3. Call the original update `steps` times, each with tick-sized dt
    //      (for the no-CBF multi-step path), or once with scaled dt (CBF).
    //
    // The tick counter is incremented inside PlayerObject::update (which
    // PlayLayer::update calls internally), so we don't increment here.
    void update(float dt) {
        if (!MacroBot::get().isRecording() && !MacroBot::get().isPlaying()) {
            // Idle - still apply speedhack/practice-fix so the user can
            // use them independently of the bot.
            dt = PracticeFix::get().computeFixedDt(dt);
            auto plan = Speedhack::get().plan(dt);
            for (uint32_t i = 0; i < plan.steps; i++) {
                PlayLayer::update(plan.scaledDt);
            }
            return;
        }

        dt = PracticeFix::get().computeFixedDt(dt);
        auto plan = Speedhack::get().plan(dt);
        for (uint32_t i = 0; i < plan.steps; i++) {
            PlayLayer::update(plan.scaledDt);
        }
    }

    // Level exit cleanup. Called when the player quits or the PlayLayer is
    // destroyed for any reason.
    void onExit() {
        PlayLayer::onExit();
        // Don't fully tear down here - the destructor will handle final cleanup.
    }

    ~CBFPlayLayer() {
        // The PlayLayer is being destroyed - level is fully over.
        MacroBot::get().onPlayLayerDestroy(false);
    }
};

// ---------------------------------------------------------------------------
//  PlayerObject hooks (input capture + tick counting)
// ---------------------------------------------------------------------------
class $modify(CBFPlayerObject, PlayerObject) {
    // pushButton is called when a player presses jump (mouse, spacebar, or
    // our bot). We capture the event for recording. Playback bypasses this
    // hook's recording path because the recorder checks isRecording().
    void pushButton(int button) {
        PlayerObject::pushButton(button);

        auto pl = PlayLayer::get();
        if (!pl) return;
        // Determine which player this is by comparing pointers.
        int player = (this == pl->m_player1) ? 1 : 2;
        MacroBot::get().onPlayerButton(player, true);
    }

    void releaseButton(int button) {
        PlayerObject::releaseButton(button);

        auto pl = PlayLayer::get();
        if (!pl) return;
        int player = (this == pl->m_player1) ? 1 : 2;
        MacroBot::get().onPlayerButton(player, false);
    }

    // PlayerObject::update is called once per physics step for each player.
    // We count ticks based on player 1's updates only, so dual-player levels
    // don't double-count.
    void update(float dt) {
        PlayerObject::update(dt);

        auto pl = PlayLayer::get();
        if (pl && this == pl->m_player1) {
            MacroBot::get().onPhysicsStep();
        }
    }
};

// ---------------------------------------------------------------------------
//  Keyboard hooks - hotkeys for record/play
// ---------------------------------------------------------------------------
class $modify(CBFKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool idk) {
        // Only act on key press (down=true), not release.
        if (down) {
            auto mod = Mod::get();
            auto recKey = mod->getSettingValue<std::string>("hotkey-record");
            auto playKey = mod->getSettingValue<std::string>("hotkey-play");

            // Convert setting string (e.g. "F6") to enumKeyCodes. Geode
            // exposes key enums with the same names.
            auto keyMatches = [](const std::string& setting, enumKeyCodes k) -> bool {
                // Simple compare: setting is like "F6", enum is KEY_F6.
                // We just check the suffix.
                if (setting.empty()) return false;
                // Handle common keys: F1-F12, digits, letters.
                if (setting.size() >= 2 && setting[0] == 'F') {
                    int n = atoi(setting.c_str() + 1);
                    if (n >= 1 && n <= 12) {
                        enumKeyCodes expected = static_cast<enumKeyCodes>(
                            static_cast<int>(KEY_F1) + (n - 1));
                        return k == expected;
                    }
                }
                return false;
            };

            if (keyMatches(recKey, key)) {
                if (MacroBot::get().isRecording()) {
                    MacroBot::get().stopRecording();
                } else {
                    MacroBot::get().startRecording();
                }
            } else if (keyMatches(playKey, key)) {
                if (MacroBot::get().isPlaying()) {
                    MacroBot::get().stopPlayback();
                } else {
                    MacroBot::get().playLastRecorded();
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, idk);
    }
};

// ---------------------------------------------------------------------------
//  Menu integration - add a button to the pause menu
// ---------------------------------------------------------------------------
// We hook PauseLayer to add a "Macro Bot" button that opens the UI.

class $modify(CBFPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        // Add a "Macro Bot" button at the bottom of the pause menu.
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Macro Bot"),
            this,
            menu_selector(CBFPauseLayer::onMacroBotBtn)
        );
        btn->setPosition(0.f, -120.f);

        auto menu = this->getChildByID("pause-menu");
        if (menu) {
            menu->addChild(btn);
        }
    }

    void onMacroBotBtn(CCObject*) {
        cbf::MacroLayer::toggle();
    }
};
