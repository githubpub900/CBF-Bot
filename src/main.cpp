// ============================================================================
//  CBF Macro Bot — Geode v5 entry point and GD hooks (GD 2.2081)
// ----------------------------------------------------------------------------
//  All Geometry Dash hooks live here. Hooks are intentionally thin: they
//  extract data from GD's classes and forward it to MacroBot.
//
//  Hooks installed:
//    $execute                           — mod init
//    $on_mod(Loaded)                    — register keybind listeners
//    PlayLayer::init                    — level entered
//    PlayLayer::~PlayLayer              — level exited
//    PlayLayer::update                  — speedhack multi-step + tick counter
//    PlayerObject::pushButton           — capture P1/P2 press
//    PlayerObject::releaseButton        — capture P1/P2 release
//    PlayerObject::update               — per-physics-tick advance
//    PauseLayer::customSetup            — add "Macro Bot" button
//
//  Keybinds are handled via Geode v5's built-in keybind setting system
//  (no more CCKeyboardDispatcher hook needed).
// ============================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/loader/GameEvent.hpp>

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
    log::info("[CBFMacroBot] Mod loaded on Geode {}.",
              Loader::get()->getVersion().toNonVString());
}

// ---------------------------------------------------------------------------
//  Regular setting change listeners (v5 requires explicit template type)
// ---------------------------------------------------------------------------
$on_mod(Loaded) {
    listenForSettingChanges<double>("default-speed", [](double value) {
        Speedhack::get().setSpeed(static_cast<float>(value));
    });

    listenForSettingChanges<int64_t>("max-physics-rate", [](int64_t value) {
        CBFIntegration::get().setMaxPhysicsRate(static_cast<double>(value));
    });

    listenForSettingChanges<bool>("practice-fix-enabled", [](bool value) {
        PracticeFix::get().setEnabled(value);
    });
}

// ---------------------------------------------------------------------------
//  Keybind listeners — Geode v5 unified keybind system.
//  Uses listenForKeybindSettingPresses (NOT listenForSettingChanges).
//  Registered in $on_game(Loaded) so gameplay context is available.
// ---------------------------------------------------------------------------
$on_game(Loaded) {
    listenForKeybindSettingPresses("hotkey-record",
        [](Keybind const& /*kb*/, bool down, bool repeat, double /*ts*/) {
            if (down && !repeat) {
                if (MacroBot::get().isRecording()) MacroBot::get().stopRecording();
                else                                MacroBot::get().startRecording();
            }
        });

    listenForKeybindSettingPresses("hotkey-play",
        [](Keybind const& /*kb*/, bool down, bool repeat, double /*ts*/) {
            if (down && !repeat) {
                if (MacroBot::get().isPlaying()) MacroBot::get().stopPlayback();
                else                              MacroBot::get().playLastRecorded();
            }
        });

    listenForKeybindSettingPresses("hotkey-ui",
        [](Keybind const& /*kb*/, bool down, bool repeat, double /*ts*/) {
            if (down && !repeat) {
                cbf::MacroLayer::toggle();
            }
        });
}

// ---------------------------------------------------------------------------
//  PlayLayer hooks
// ---------------------------------------------------------------------------
class $modify(CBFPlayLayer, PlayLayer) {
    // PlayLayer::init is called when a level is entered (fresh or retry).
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Defer to next main-thread tick so all PlayLayer fields are ready.
        bool isPractice = this->m_isPracticeMode;
        Loader::get()->queueInMainThread([isPractice]() {
            MacroBot::get().onPlayLayerInit(isPractice);
        });
        return true;
    }

    // PlayLayer::update is the per-frame entry point. We:
    //   1. Compute the speedhack plan (dt-per-step + steps-per-frame).
    //   2. Apply the practice-fix dt spike clamp.
    //   3. Call the original update `steps` times, each with dtPerStep.
    //
    // This gives choppy-free slow-motion: at slow speeds we run MORE
    // physics sub-steps per frame (each advancing less game-time) up to
    // the user's max physics rate cap.
    void update(float dt) {
        dt = PracticeFix::get().computeFixedDt(dt);
        auto plan = Speedhack::get().plan(dt);

        for (uint32_t i = 0; i < plan.stepsThisFrame; i++) {
            PlayLayer::update(plan.frameDt);
        }
    }

    ~CBFPlayLayer() {
        MacroBot::get().onPlayLayerDestroy(false);
    }
};

// ---------------------------------------------------------------------------
//  PlayerObject hooks (input capture + tick counting)
// ---------------------------------------------------------------------------
class $modify(CBFPlayerObject, PlayerObject) {
    // pushButton takes PlayerButton enum in GD 2.2081 (not int).
    bool pushButton(PlayerButton button) {
        bool result = PlayerObject::pushButton(button);

        // We only care about the Jump button for macro recording.
        if (button == PlayerButton::Jump) {
            auto pl = PlayLayer::get();
            if (pl) {
                int player = (this == pl->m_player1) ? 1 : 2;
                MacroBot::get().onPlayerButton(player, true);
            }
        }
        return result;
    }

    bool releaseButton(PlayerButton button) {
        bool result = PlayerObject::releaseButton(button);

        if (button == PlayerButton::Jump) {
            auto pl = PlayLayer::get();
            if (pl) {
                int player = (this == pl->m_player1) ? 1 : 2;
                MacroBot::get().onPlayerButton(player, false);
            }
        }
        return result;
    }

    // PlayerObject::update fires once per physics step for each player.
    // We increment the tick counter only for player 1 (so dual-player
    // levels don't double-count). GD's m_tickIndex is also incremented
    // internally, so our counter is a backup/fallback.
    void update(float dt) {
        PlayerObject::update(dt);

        auto pl = PlayLayer::get();
        if (pl && this == pl->m_player1) {
            MacroBot::get().onPhysicsStep();
        }
    }
};

// ---------------------------------------------------------------------------
//  PauseLayer hook — add "Macro Bot" button to the pause menu
// ---------------------------------------------------------------------------
class $modify(CBFPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

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
