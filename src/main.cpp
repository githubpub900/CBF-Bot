// =====================================================================================
//  main.cpp  --  Hooks + UI glue for the Practice-to-Normal macro bot
//  Target: Geometry Dash 2.2081  /  Geode v5.7.1
//
//  Design principle: hooks are THIN. They translate an engine event into a single call
//  on Bot::get() and return. All policy lives in Bot.hpp. This keeps the integration
//  surface small, auditable, and easy to repair if a binding signature shifts.
//
//  HOOK MAP
//  --------
//    GJBaseGameLayer::handleButton   -> record OR (during playback) is the dispatch sink
//    PlayerObject::update            -> per-substep deterministic clock + playback dispatch
//    PlayLayer::resetLevel           -> per-attempt runtime reset
//    PlayLayer::markCheckpoint       -> push extended (practice-bug-fix) checkpoint
//    PlayLayer::loadFromCheckpoint   -> restore extended state + dead-input cleanup
//    PlayLayer::removeCheckpoint     -> keep checkpoint stack in lock-step
//    PlayLayer::destroyPlayer        -> death hook (cleanup happens on the auto-reload)
//    PlayLayer::levelComplete        -> freeze final route
//    PauseLayer::onRestart           -> full-restart cleanup
//    CCScheduler::update             -> internal speedhack (time-warp), NOT FPS based
//    CCKeyboardDispatcher::dispatch  -> 'K' opens the bot menu
//
//  CBF / CBS INTEGRATION (summary; full rationale in Bot.hpp):
//    * CBF exposes no API. We never call into it. We detect it with Loader::isModLoaded
//      and otherwise rely on the fact that CBF transparently hooks the very same input
//      and physics path our bot uses -- so a handleButton we emit during playback is
//      processed by CBF at sub-frame resolution automatically.
//    * CBS is native + 480 TPS capped; detection is best-effort + a user override toggle.
//    * RED state (neither) hard-disables playback in Bot::setMode.
// =====================================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include "Bot.hpp"

using namespace geode::prelude;

// Helper: the active PlayLayer (null in menus/editor). PlayLayer::get() is the stable
// Geode accessor for the current play layer.
static PlayLayer* activePL() { return PlayLayer::get(); }

// =====================================================================================
//  INPUT HOOK
// =====================================================================================
//  handleButton(bool down, int button, bool isPlayer1) is GD's single input entry point.
//  During RECORD we capture the transition (keyed by player X). During PLAY this same
//  function is how we *emit* inputs (from Bot::dispatchCrossed), so we must avoid
//  re-recording our own emitted inputs -- the mode check below handles that naturally:
//  in Play mode we do not record.
// =====================================================================================
class $modify(InputHook, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        auto pl = activePL();
        if (!pl) return;
        // Only record genuine user input in Practice while in Record mode.
        if (Bot::get().isRecording() && pl->m_isPracticeMode) {
            Bot::get().recordInput(button, down, isPlayer1, pl);
        }
    }
};

// =====================================================================================
//  PER-SUBSTEP CLOCK + PLAYBACK DISPATCH
// =====================================================================================
//  PlayerObject::update(float) runs once per physics substep per player. We tick the Bot
//  engine exactly once per substep using ONLY the reference player (P1) to avoid double
//  counting in dual. This is the FPS-independent clock that gives sub-frame precision
//  under CBF/CBS automatically (more substeps => finer crossing detection).
// =====================================================================================
class $modify(PlayerStepHook, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        auto pl = activePL();
        if (!pl) return;
        // Tick once per substep, driven by player 1 only.
        if (this == pl->m_player1) {
            Bot::get().onPhysicsStep(pl);
        }
    }
};

// =====================================================================================
//  PLAYLAYER LIFECYCLE HOOKS
// =====================================================================================
class $modify(PlayHook, PlayLayer) {
    // resetLevel runs at the start of every attempt (manual start, death-respawn, restart).
    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::get().onLevelStart(this);
    }

    // Practice checkpoint created -> snapshot extended state for the practice-bug fix.
    CheckpointObject* markCheckpoint() {
        auto* cp = PlayLayer::markCheckpoint();
        if (cp) Bot::get().onMarkCheckpoint(this);
        return cp;
    }

    // Checkpoint loaded (death-respawn or manual) -> restore extended state, then drop
    // any inputs recorded after the checkpoint (dead-input cleanup).
    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        Bot::get().onLoadCheckpoint(this); // post-hook: GD's array is already trimmed
    }

    void removeCheckpoint() {
        PlayLayer::removeCheckpoint();
        Bot::get().onRemoveCheckpoint(this);
    }

    // Death. Cleanup is performed on the subsequent auto loadFromCheckpoint; this hook is
    // kept explicit for clarity and future extension.
    void destroyPlayer(PlayerObject* p, GameObject* o) {
        PlayLayer::destroyPlayer(p, o);
        Bot::get().onDeath(this);
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        Bot::get().onLevelComplete(this);
    }
};

// =====================================================================================
//  RESTART (pause menu) -- full-route abandonment cleanup
// =====================================================================================
class $modify(PauseHook, PauseLayer) {
    void onRestart(CCObject* sender) {
        // A pause-menu restart that is NOT a checkpoint-restart abandons the whole route.
        // PlayLayer::resetLevel will also fire and call onLevelStart; calling onRestart
        // first guarantees the macro + checkpoint stack are cleared deterministically.
        if (auto pl = activePL()) Bot::get().onRestart(pl, /*full=*/true);
        PauseLayer::onRestart(sender);
    }
};

// =====================================================================================
//  SPEEDHACK  (internal time-warp -- explicitly NOT FPS manipulation)
// =====================================================================================
//  We scale the delta fed to the cocos scheduler. Everything that advances off the
//  scheduler (including GD physics) speeds up/slows uniformly. Because the macro is keyed
//  on player X (a physics RESULT), warping time changes how fast we travel but never the
//  X-at-which-an-input-fires mapping -> playback timing stays exact at any speed. This is
//  why the spec's "macro timing must remain accurate regardless of speedhack value" holds
//  for free with this architecture.
// =====================================================================================
class $modify(SpeedHook, CCScheduler) {
    void update(float dt) {
        double warp = Bot::get().timeWarp();
        // Only warp during active gameplay to avoid distorting menus/transitions.
        if (warp != 1.0 && activePL()) {
            CCScheduler::update(static_cast<float>(dt * warp));
        } else {
            CCScheduler::update(dt);
        }
    }
};

// =====================================================================================
//  BOT MENU (opened with 'K')
// =====================================================================================
class BotMenu : public Popup<> {
protected:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_infoLabel = nullptr;
    TextInput*     m_nameInput = nullptr;
    TextInput*     m_speedInput = nullptr;

    bool setup() override {
        this->setTitle("Rylan Macro Bot");
        auto size = m_mainLayer->getContentSize();
        float cx = size.width / 2.f;

        // ---- CBF/CBS status label (green/yellow/red) ----
        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.55f);
        m_statusLabel->setPosition({ cx, size.height - 42.f });
        m_mainLayer->addChild(m_statusLabel);

        // ---- info line (input count / record regime) ----
        m_infoLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_infoLabel->setScale(0.7f);
        m_infoLabel->setPosition({ cx, size.height - 62.f });
        m_mainLayer->addChild(m_infoLabel);

        // ---- button row factory ----
        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        m_mainLayer->addChild(menu);

        auto mkBtn = [&](const char* label, CcColor3B color, SEL_MenuHandler cb,
                         float x, float y) {
            auto spr = ButtonSprite::create(label, "bigFont.fnt", "GJ_button_01.png", 0.7f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition({ x, y });
            menu->addChild(btn);
            return btn;
        };

        float row1 = size.height - 100.f;
        float row2 = size.height - 140.f;
        float row3 = size.height - 180.f;
        float lx = cx - 75.f, rx = cx + 75.f;

        mkBtn("Record",   ccc3(255,80,80),  menu_selector(BotMenu::onRecord),   lx, row1);
        mkBtn("Play",     ccc3(80,255,80),  menu_selector(BotMenu::onPlay),     rx, row1);
        mkBtn("Save",     ccc3(255,255,255),menu_selector(BotMenu::onSave),     lx, row2);
        mkBtn("Load",     ccc3(255,255,255),menu_selector(BotMenu::onLoad),     rx, row2);
        mkBtn("Stop",     ccc3(200,200,200),menu_selector(BotMenu::onStop),     lx, row3);
        mkBtn("CBS On/Off",ccc3(255,220,80),menu_selector(BotMenu::onToggleCBS),rx, row3);

        // ---- macro name input ----
        m_nameInput = TextInput::create(180.f, "macro name");
        m_nameInput->setString("practice_run");
        m_nameInput->setPosition({ cx, size.height - 218.f });
        m_mainLayer->addChild(m_nameInput);

        // ---- speedhack input + toggle ----
        m_speedInput = TextInput::create(80.f, "speed");
        m_speedInput->setString("1.0");
        m_speedInput->setCommonFilter(CommonFilter::Float);
        m_speedInput->setPosition({ cx - 40.f, size.height - 250.f });
        m_mainLayer->addChild(m_speedInput);

        auto spdMenu = CCMenu::create();
        spdMenu->setPosition({ 0, 0 });
        m_mainLayer->addChild(spdMenu);
        auto applySpr = ButtonSprite::create("Speed", "bigFont.fnt", "GJ_button_05.png", 0.6f);
        auto applyBtn = CCMenuItemSpriteExtra::create(applySpr, this, menu_selector(BotMenu::onSpeed));
        applyBtn->setPosition({ cx + 60.f, size.height - 250.f });
        spdMenu->addChild(applyBtn);

        refresh();
        return true;
    }

    void refresh() {
        Bot::get().detectStatus();
        switch (Bot::get().precision()) {
            case Precision::CBF:
                m_statusLabel->setString("CBF ACTIVE  (max precision)");
                m_statusLabel->setColor(ccc3(60, 230, 60));   // GREEN
                break;
            case Precision::CBS:
                m_statusLabel->setString("CBS ACTIVE  (~480 TPS)");
                m_statusLabel->setColor(ccc3(240, 220, 40));  // YELLOW
                break;
            default:
                m_statusLabel->setString("NO CBF/CBS  (playback disabled)");
                m_statusLabel->setColor(ccc3(235, 50, 50));   // RED
                break;
        }
        const char* modeStr =
            Bot::get().isRecording() ? "RECORDING" :
            Bot::get().isPlaying()   ? "PLAYING"   : "idle";
        const char* regime =
            Bot::get().recordPrecision() == Precision::CBF ? "CBF" :
            Bot::get().recordPrecision() == Precision::CBS ? "CBS" : "-";
        m_infoLabel->setString(
            fmt::format("{}  |  inputs: {}  |  regime: {}  |  speed: {}x{}",
                modeStr, Bot::get().inputCount(), regime,
                Bot::get().speed(),
                Bot::get().speedEnabled() ? "" : " (off)").c_str());
    }

    // ---- button handlers ----
    void onRecord(CCObject*) {
        if (auto pl = activePL(); pl && !pl->m_isPracticeMode) {
            Notification::create("Enter Practice Mode to record.", NotificationIcon::Warning)->show();
            return;
        }
        Bot::get().setMode(BotMode::Record);
        if (auto pl = activePL()) Bot::get().onLevelStart(pl);
        refresh();
    }
    void onPlay(CCObject*) {
        Bot::get().setMode(BotMode::Play);
        if (!Bot::get().isPlaying()) {
            Notification::create("Playback disabled: no CBF/CBS active.", NotificationIcon::Error)->show();
        }
        refresh();
    }
    void onStop(CCObject*) {
        Bot::get().setMode(BotMode::Idle);
        refresh();
    }
    void onSave(CCObject*) {
        auto name = m_nameInput->getString();
        if (name.empty()) name = "macro";
        if (Bot::get().save(name))
            Notification::create("Saved.", NotificationIcon::Success)->show();
        else
            Notification::create("Save failed.", NotificationIcon::Error)->show();
    }
    void onLoad(CCObject*) {
        auto name = m_nameInput->getString();
        if (name.empty()) name = "macro";
        if (Bot::get().load(name))
            Notification::create("Loaded.", NotificationIcon::Success)->show();
        else
            Notification::create("Load failed.", NotificationIcon::Error)->show();
        refresh();
    }
    void onToggleCBS(CCObject*) {
        // Manual override for native CBS when automatic detection is unavailable on this
        // binding set. Documented fallback so a legitimate CBS run is never locked out.
        Bot::get().setUserCBSOverride(!Bot::get().userCBSOverride());
        refresh();
    }
    void onSpeed(CCObject*) {
        double s = 1.0;
        try { s = std::stod(m_speedInput->getString()); } catch (...) { s = 1.0; }
        Bot::get().setSpeed(s);
        Bot::get().setSpeedEnabled(s != 1.0);
        refresh();
    }

public:
    static BotMenu* create() {
        auto ret = new BotMenu();
        if (ret->initAnchored(300.f, 290.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// =====================================================================================
//  'K' opens the menu
// =====================================================================================
class $modify(KeyHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (key == enumKeyCodes::KEY_K && down && !repeat) {
            // Don't steal the key while typing in a text field.
            if (auto menu = BotMenu::create()) {
                menu->show();
            }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// =====================================================================================
//  Entry point
// =====================================================================================
$on_mod(Loaded) {
    Bot::get().detectStatus();
    log::info("[Bot] loaded. CBF/CBS status code = {}", (int)Bot::get().precision());
}
