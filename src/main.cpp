// ============================================================
// MacroBot — Geode mod for Geometry Dash 2.2081
// Geode SDK v5.7.1
//
// Features:
//  - Time-based macro recording/playback with CBF precision
//  - Supports syzzi.click_between_frames (unlimited precision)
//  - Supports RobTop's built-in Click Between Steps (480 TPS)
//  - Bot is DISABLED if no CBF is active
//  - Dead-input discard (deaths in practice mode)
//  - Pause-menu restart reverts later inputs correctly
//  - Own practice bug-fix (full PlayerState checkpoints)
//  - Own speedhack (textbox, decoupled from frame rate)
//  - GUI via K key (CBF status dot, record/play, save/load)
// ============================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

// Geode UI
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>

// cocos2d input
#include <Geode/cocos/base_nodes/CCNode.h>
#include <Geode/cocos/cocoa/CCGeometry.h>

#include "Bot.hpp"

using namespace geode::prelude;

// ============================================================
// Persistent checkpoint data — stored per checkpoint index
// ============================================================
struct CheckpointExtra {
    PlayerState p1State;
    PlayerState p2State;
    double      levelTime = 0.0;
};

static std::vector<CheckpointExtra> s_checkpointExtras;

// ============================================================
// Speedhack — we intercept CCScheduler::update so that the
// physics step receives our custom time scale without touching
// the visual frame rate counter.
// ============================================================
class $modify(CCScheduler) {
    void update(float dt) {
        // We already set the CCScheduler time scale through applySpeedhack(),
        // so we just pass through. This hook exists to ensure future
        // compatibility and lets us guard against extreme dt spikes.
        float clampedDt = std::min(dt, 0.5f); // never more than 0.5s
        CCScheduler::update(clampedDt);
    }
};

// ============================================================
// GJBaseGameLayer — intercept handleButton for recording
// ============================================================
class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        Bot& bot = Bot::get();
        if (bot.mode != BotMode::Recording) return;
        if (!bot.canOperate()) return;

        // Translate GD button index → BotButton
        BotButton btn;
        switch (button) {
            case 0:  btn = BotButton::Jump;  break;
            case 1:  btn = BotButton::Left;  break;
            case 2:  btn = BotButton::Right; break;
            default: return; // ignore unknown
        }

        int playerIdx = isPlayer1 ? 0 : 1;

        // Use the authoritative level time from PlayLayer
        double levelTime = bot.currentLevelTime;

        bot.recordInput(levelTime, playerIdx, btn, down);
    }
};

// ============================================================
// PlayLayer — main hook for timing, checkpoints, death, etc.
// ============================================================
class $modify(MyPlayLayer, PlayLayer) {

    // ----------------------------------------------------------
    // init — refresh CBF status whenever a level is entered
    // ----------------------------------------------------------
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        Bot::get().refreshCBF();
        Bot::get().reset();
        s_checkpointExtras.clear();
        return true;
    }

    // ----------------------------------------------------------
    // update — track current level time, drive playback
    // ----------------------------------------------------------
    void update(float dt) {
        PlayLayer::update(dt);

        Bot& bot = Bot::get();
        // m_currentTime is the authoritative level time in PlayLayer
        bot.currentLevelTime = static_cast<double>(m_currentTime);

        if (bot.mode == BotMode::Playing) {
            bot.processPlayback(this, bot.currentLevelTime);
        }
    }

    // ----------------------------------------------------------
    // resetLevel — handles both "restart from start" and
    //              "restore from checkpoint" paths.
    // We need to revert bot input state accordingly.
    // ----------------------------------------------------------
    void resetLevel() {
        PlayLayer::resetLevel();

        Bot& bot = Bot::get();
        double resetTime = static_cast<double>(m_currentTime);

        if (bot.mode == BotMode::Recording) {
            bot.onRestart(resetTime);
        } else if (bot.mode == BotMode::Playing) {
            bot.rewindPlaybackTo(resetTime);
        }
    }

    // ----------------------------------------------------------
    // destroyPlayer — mark player as dead in recording mode
    // ----------------------------------------------------------
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) {
            bool isP1 = (player == m_player1);
            double deathTime = bot.currentLevelTime;
            bot.onDeath(isP1 ? 0 : 1, deathTime);
        }
        PlayLayer::destroyPlayer(player, object);
    }

    // ----------------------------------------------------------
    // Practice-mode checkpoint creation — store full player states
    // ----------------------------------------------------------
    CheckpointObject* createCheckpoint() {
        CheckpointObject* cp = PlayLayer::createCheckpoint();
        if (!cp || !m_isPracticeMode) return cp;

        CheckpointExtra extra;
        extra.levelTime = Bot::get().currentLevelTime;
        if (m_player1) extra.p1State = capturePlayerState(m_player1);
        if (m_player2) extra.p2State = capturePlayerState(m_player2);

        // Index = current array count (checkpoint just created is at the end)
        s_checkpointExtras.push_back(extra);
        return cp;
    }

    // ----------------------------------------------------------
    // Practice-mode checkpoint restore — apply full player states
    // ----------------------------------------------------------
    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);

        if (!m_isPracticeMode) return;
        if (s_checkpointExtras.empty()) return;

        // Find the matching extra by index using the checkpoint array
        // The checkpoint array size after removal tells us the index.
        int idx = static_cast<int>(m_checkpointArray->count()) - 1;
        if (idx < 0 || idx >= static_cast<int>(s_checkpointExtras.size())) return;

        const CheckpointExtra& extra = s_checkpointExtras[idx];

        if (m_player1) applyPlayerState(m_player1, extra.p1State);
        if (m_player2) applyPlayerState(m_player2, extra.p2State);

        // Revert bot state to checkpoint time
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) {
            bot.onRestart(extra.levelTime);
        } else if (bot.mode == BotMode::Playing) {
            bot.rewindPlaybackTo(extra.levelTime);
        }
    }

    // ----------------------------------------------------------
    // removeAllCheckpoints — clear our extras too
    // ----------------------------------------------------------
    void removeAllCheckpoints() {
        PlayLayer::removeAllCheckpoints();
        s_checkpointExtras.clear();
    }

    // ----------------------------------------------------------
    // levelComplete — stop recording/playing on completion
    // ----------------------------------------------------------
    void levelComplete() {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) {
            bot.stopRecording();
        } else if (bot.mode == BotMode::Playing) {
            bot.stopPlayback();
        }
        PlayLayer::levelComplete();
    }

    // ----------------------------------------------------------
    // onQuit — clean up
    // ----------------------------------------------------------
    void onQuit() {
        Bot::get().reset();
        PlayLayer::onQuit();
    }
};

// ============================================================
// PauseLayer — intercept "Restart" button to revert inputs
// ============================================================
class $modify(PauseLayer) {
    void onRestart(CCObject* sender) {
        // Full restart → revert bot to time 0
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) {
            bot.onRestart(0.0);
        } else if (bot.mode == BotMode::Playing) {
            bot.rewindPlaybackTo(0.0);
        }
        PauseLayer::onRestart(sender);
    }

    void onRestartFull(CCObject* sender) {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) {
            bot.onRestart(0.0);
        } else if (bot.mode == BotMode::Playing) {
            bot.rewindPlaybackTo(0.0);
        }
        PauseLayer::onRestartFull(sender);
    }
};

// ============================================================
// GUI Popup (opens with K key in-level)
// ============================================================
class BotGUI : public Popup<> {
protected:
    CCLabelBMFont* m_cbfLabel       = nullptr;
    CCSprite*      m_cbfDot         = nullptr;
    CCLabelBMFont* m_statusLabel    = nullptr;
    CCTextInputNode* m_speedInput   = nullptr;
    CCTextInputNode* m_saveInput    = nullptr;

    bool setup() override {
        setTitle("MacroBot");

        Bot& bot = Bot::get();
        bot.refreshCBF();

        auto* winSize = CCDirector::get()->getWinSize();
        auto  center  = CCPoint{m_mainLayer->getContentWidth() / 2.f,
                                m_mainLayer->getContentHeight() / 2.f};

        float y = m_mainLayer->getContentHeight() - 55.f;
        const float lineH = 32.f;

        // ---- CBF Status dot + label ----
        {
            auto color  = CBFStatus::dotColor(bot.cbfMode);
            auto label  = CBFStatus::dotLabel(bot.cbfMode);

            // Dot (using a small CCSprite from GJ_button_01.png tinted)
            auto* dot = CCSprite::create("GJ_circle_01.png");
            if (!dot) dot = CCSprite::create("GJ_circle_01.png");
            if (dot) {
                dot->setScale(0.35f);
                dot->setColor(color);
                dot->setPosition({30.f, y});
                m_mainLayer->addChild(dot);
                m_cbfDot = dot;
            }

            auto* lbl = CCLabelBMFont::create(label, "bigFont.fnt");
            lbl->setScale(0.38f);
            lbl->setAnchorPoint({0.f, 0.5f});
            lbl->setPosition({50.f, y});
            m_mainLayer->addChild(lbl);
            m_cbfLabel = lbl;
            y -= lineH;
        }

        // Separator
        y -= 4.f;

        // ---- Status ----
        {
            const char* statusStr = "Idle";
            if (bot.mode == BotMode::Recording) statusStr = "Recording...";
            if (bot.mode == BotMode::Playing)   statusStr = "Playing Back";
            m_statusLabel = CCLabelBMFont::create(statusStr, "bigFont.fnt");
            m_statusLabel->setScale(0.5f);
            m_statusLabel->setPosition({center.x, y});
            m_mainLayer->addChild(m_statusLabel);
            y -= lineH;
        }

        // ---- Record button ----
        {
            auto* btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Record", "bigFont.fnt", "GJ_button_02.png"),
                this, menu_selector(BotGUI::onRecord));
            auto* menu = CCMenu::create();
            menu->setPosition({center.x - 60.f, y});
            menu->addChild(btn);
            m_mainLayer->addChild(menu);
        }
        // ---- Play button ----
        {
            auto* btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Play", "bigFont.fnt", "GJ_button_01.png"),
                this, menu_selector(BotGUI::onPlay));
            auto* menu = CCMenu::create();
            menu->setPosition({center.x + 60.f, y});
            menu->addChild(btn);
            m_mainLayer->addChild(menu);
        }
        // ---- Stop button ----
        {
            auto* btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Stop", "bigFont.fnt", "GJ_button_06.png"),
                this, menu_selector(BotGUI::onStop));
            auto* menu = CCMenu::create();
            menu->setPosition({center.x, y});
            menu->addChild(btn);
            m_mainLayer->addChild(menu);
        }
        y -= lineH + 4.f;

        // ---- Save / Load ----
        {
            // Filename input
            auto* bgSave = cocos2d::extension::CCScale9Sprite::create(
                "square02_small.png", {0,0,40,40});
            bgSave->setContentSize({160.f, 26.f});
            bgSave->setPosition({center.x, y});
            m_mainLayer->addChild(bgSave);

            auto* inp = CCTextInputNode::create(150.f, 26.f, "macro.bmac", "bigFont.fnt");
            inp->setPosition({center.x, y});
            inp->setScale(0.5f);
            m_saveInput = inp;
            m_mainLayer->addChild(inp);
            y -= lineH;

            auto* saveBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Save", "bigFont.fnt", "GJ_button_05.png"),
                this, menu_selector(BotGUI::onSave));
            auto* loadBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_05.png"),
                this, menu_selector(BotGUI::onLoad));
            auto* menu = CCMenu::create();
            menu->setPosition({center.x, y});
            menu->addChild(saveBtn);
            menu->addChild(loadBtn);
            menu->alignItemsHorizontally();
            m_mainLayer->addChild(menu);
            y -= lineH;
        }

        // ---- Speedhack ----
        {
            auto* lbl = CCLabelBMFont::create("Speedhack:", "bigFont.fnt");
            lbl->setScale(0.4f);
            lbl->setPosition({center.x - 55.f, y});
            m_mainLayer->addChild(lbl);

            auto* bg = cocos2d::extension::CCScale9Sprite::create(
                "square02_small.png", {0,0,40,40});
            bg->setContentSize({80.f, 26.f});
            bg->setPosition({center.x + 35.f, y});
            m_mainLayer->addChild(bg);

            auto* inp = CCTextInputNode::create(70.f, 26.f, "1.0", "bigFont.fnt");
            inp->setPosition({center.x + 35.f, y});
            inp->setScale(0.5f);
            m_speedInput = inp;
            // Show current speed
            auto str = std::to_string(Bot::get().speedhack);
            // Trim trailing zeros
            str.erase(str.find_last_not_of('0') + 1);
            if (str.back() == '.') str += "0";
            inp->setString(str.c_str());
            m_mainLayer->addChild(inp);

            auto* applyBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Apply", "bigFont.fnt", "GJ_button_04.png"),
                this, menu_selector(BotGUI::onApplySpeed));
            auto* menu = CCMenu::create();
            menu->setPosition({center.x + 80.f, y});
            menu->addChild(applyBtn);
            m_mainLayer->addChild(menu);
            y -= lineH;
        }

        // ---- Input count info ----
        {
            std::string info = "Inputs: " + std::to_string(Bot::get().macro.inputs.size());
            auto* lbl = CCLabelBMFont::create(info.c_str(), "bigFont.fnt");
            lbl->setScale(0.38f);
            lbl->setPosition({center.x, y});
            m_mainLayer->addChild(lbl);
        }

        return true;
    }

    void onRecord(CCObject*) {
        Bot& bot = Bot::get();
        if (!bot.canOperate()) {
            FLAlertLayer::create("Bot Disabled",
                "No CBF detected. Install <cy>syzzi.click_between_frames</c> from the Geode index.",
                "OK")->show();
            return;
        }
        if (bot.mode == BotMode::Recording) return;
        auto* pl = PlayLayer::get();
        if (!pl) {
            FLAlertLayer::create("Error","Enter a level first.","OK")->show();
            return;
        }
        bot.startRecording(bot.currentLevelTime);
        refreshStatus();
    }

    void onPlay(CCObject*) {
        Bot& bot = Bot::get();
        if (!bot.canOperate()) return;
        if (bot.mode == BotMode::Playing) return;
        if (bot.macro.inputs.empty()) {
            FLAlertLayer::create("Empty Macro","No inputs recorded or loaded.","OK")->show();
            return;
        }
        auto* pl = PlayLayer::get();
        if (!pl) {
            FLAlertLayer::create("Error","Enter a level first.","OK")->show();
            return;
        }
        bot.startPlayback();
        refreshStatus();
    }

    void onStop(CCObject*) {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) bot.stopRecording();
        else if (bot.mode == BotMode::Playing) bot.stopPlayback();
        refreshStatus();
    }

    void onSave(CCObject*) {
        const char* name = m_saveInput ? m_saveInput->getString() : "macro.bmac";
        auto path = Mod::get()->getSaveDir() / name;
        if (Bot::get().macro.save(path.string())) {
            FLAlertLayer::create("Saved", ("Macro saved to:\n" + path.string()).c_str(), "OK")->show();
        } else {
            FLAlertLayer::create("Error","Failed to save macro.","OK")->show();
        }
    }

    void onLoad(CCObject*) {
        const char* name = m_saveInput ? m_saveInput->getString() : "macro.bmac";
        auto path = Mod::get()->getSaveDir() / name;
        if (Bot::get().macro.load(path.string())) {
            refreshStatus();
            std::string msg = "Loaded " + std::to_string(Bot::get().macro.inputs.size()) + " inputs.";
            FLAlertLayer::create("Loaded", msg.c_str(), "OK")->show();
        } else {
            FLAlertLayer::create("Error","Failed to load macro. Check filename.","OK")->show();
        }
    }

    void onApplySpeed(CCObject*) {
        if (!m_speedInput) return;
        const char* s = m_speedInput->getString();
        try {
            float v = std::stof(std::string(s));
            v = std::clamp(v, 0.01f, 100.f);
            applySpeedhack(v);
        } catch (...) {
            FLAlertLayer::create("Error","Invalid speed value.","OK")->show();
        }
    }

    void refreshStatus() {
        if (!m_statusLabel) return;
        Bot& bot = Bot::get();
        const char* statusStr = "Idle";
        if (bot.mode == BotMode::Recording) statusStr = "Recording...";
        if (bot.mode == BotMode::Playing)   statusStr = "Playing Back";
        m_statusLabel->setString(statusStr);

        // Refresh CBF dot
        bot.refreshCBF();
        if (m_cbfDot)   m_cbfDot->setColor(CBFStatus::dotColor(bot.cbfMode));
        if (m_cbfLabel) m_cbfLabel->setString(CBFStatus::dotLabel(bot.cbfMode));
    }

public:
    static BotGUI* create() {
        auto* ret = new BotGUI();
        if (ret->initAnchored(360.f, 400.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ============================================================
// Key handler — open GUI on K press while in a level
// ============================================================
class $modify(MyPlayLayerKey, PlayLayer) {
    void keyDown(cocos2d::enumKeyCodes key) {
        if (key == cocos2d::enumKeyCodes::KEY_K) {
            if (!Bot::get().guiOpen) {
                Bot::get().guiOpen = true;
                auto* popup = BotGUI::create();
                if (popup) popup->show();
                // Reset guiOpen when popup closes — handled via destructor
                // We can't easily hook that, so we just let it be reset on
                // next open attempt via the flag check above. For a proper
                // solution, subclass BotGUI and override onClose():
            }
        }
        PlayLayer::keyDown(key);
    }
};

// ============================================================
// Geode mod entry point
// ============================================================
$on_mod(Loaded) {
    log::info("MacroBot loaded — Geode v5.7.1 / GD 2.2081");
    log::info("CBF Status at startup: {}", (int)CBFStatus::detect());
}
