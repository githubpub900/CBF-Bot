// ============================================================
// MacroBot — main.cpp
// Geode v5.7.1 / GD 2.2081
//
// - Time-keyed macro recording + playback (CBF-precise)
// - Supports syzzi.click_between_frames + RobTop CBS
// - Locked (red dot) if no CBF present
// - Dead-input discard, restart-revert
// - Full-state practice checkpoints (velocity, gamemode, etc.)
// - Speedhack via CCScheduler::setTimeScale (textbox)
// - GUI opens on K key inside a level
// ============================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

// ---- Geode UI ----
// In Geode v5, geode::Popup<> lives in:
#include <Geode/ui/Popup.hpp>

// ---- Cocos2d extensions (CCScale9Sprite, CCTextInputNode) ----
#include <Geode/cocos/extensions/GUI/CCControlExtension/CCControl.h>
#include <Geode/cocos/extensions/GUI/CCScrollView/CCScrollView.h>

#include "Bot.hpp"

using namespace geode::prelude;

// ============================================================
// Checkpoint extra data — parallel vector to m_checkpointArray
// ============================================================
static std::vector<CheckpointExtra> s_extras;

// ============================================================
// Helper: alert shorthand
// ============================================================
static void showAlert(const char* title, const char* msg) {
    FLAlertLayer::create(title, msg, "OK")->show();
}

// ============================================================
// GJBaseGameLayer hook — capture inputs for recording
// Every input path (CBF sub-frame, RobTop CBS, normal) calls
// handleButton, so one hook covers everything.
// ============================================================
class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        Bot& bot = Bot::get();
        if (bot.mode != BotMode::Recording) return;
        if (!bot.canRun()) return;

        BotButton btn;
        switch (button) {
            case 0:  btn = BotButton::Jump;  break;
            case 1:  btn = BotButton::Left;  break;
            case 2:  btn = BotButton::Right; break;
            default: return;
        }

        bot.recordInput(bot.currentTime, isPlayer1 ? 0 : 1, btn, down);
    }
};

// ============================================================
// PlayLayer hooks
// ============================================================
class $modify(MyPlayLayer, PlayLayer) {

    // init — refresh CBF, reset bot state
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        Bot::get().refreshCBF();
        Bot::get().fullReset();
        s_extras.clear();
        return true;
    }

    // update — track time, drive playback
    void update(float dt) {
        PlayLayer::update(dt);
        Bot& bot = Bot::get();
        bot.currentTime = static_cast<double>(m_currentTime);
        if (bot.mode == BotMode::Playing) {
            bot.tickPlayback(this, bot.currentTime);
        }
    }

    // resetLevel — handles checkpoint restore AND full restart
    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::get().onRevert(static_cast<double>(m_currentTime));
    }

    // destroyPlayer — mark dead in recording mode
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) {
            int pidx = (player == m_player1) ? 0 : 1;
            bot.onDeath(pidx, bot.currentTime);
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    // createCheckpoint — save full player state alongside GD's checkpoint
    CheckpointObject* createCheckpoint() {
        CheckpointObject* cp = PlayLayer::createCheckpoint();
        if (!cp || !m_isPracticeMode) return cp;

        CheckpointExtra ex;
        ex.levelTime = Bot::get().currentTime;
        if (m_player1) ex.p1 = capturePlayer(m_player1);
        if (m_player2) ex.p2 = capturePlayer(m_player2);
        s_extras.push_back(ex);
        return cp;
    }

    // loadFromCheckpoint — restore full player state
    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        if (!m_isPracticeMode || s_extras.empty()) return;

        // After loadFromCheckpoint, m_checkpointArray has already had the
        // "future" checkpoints removed. Its current count maps to our index.
        int idx = static_cast<int>(m_checkpointArray->count()) - 1;
        if (idx < 0 || idx >= static_cast<int>(s_extras.size())) return;

        const CheckpointExtra& ex = s_extras[idx];
        if (m_player1) applyPlayer(m_player1, ex.p1);
        if (m_player2) applyPlayer(m_player2, ex.p2);

        // Trim s_extras so it stays in sync
        s_extras.resize(static_cast<size_t>(idx + 1));

        Bot::get().onRevert(ex.levelTime);
    }

    // removeAllCheckpoints — keep extras in sync
    void removeAllCheckpoints() {
        PlayLayer::removeAllCheckpoints();
        s_extras.clear();
    }

    // levelComplete — stop bot
    void levelComplete() {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) bot.stopRecording();
        else if (bot.mode == BotMode::Playing) bot.stopPlayback();
        PlayLayer::levelComplete();
    }

    // onQuit — clean up
    void onQuit() {
        Bot::get().fullReset();
        PlayLayer::onQuit();
    }

    // K key → open GUI
    void keyDown(cocos2d::enumKeyCodes key) {
        PlayLayer::keyDown(key);
        if (key == cocos2d::enumKeyCodes::KEY_K) {
            // Forward declaration; defined below
            openBotGUI();
        }
    }

    // Forward declaration (defined after BotGUI)
    void openBotGUI();
};

// ============================================================
// PauseLayer — Restart buttons revert bot
// ============================================================
class $modify(PauseLayer) {
    void onRestart(CCObject* sender) {
        Bot::get().onRevert(0.0);
        PauseLayer::onRestart(sender);
    }
    void onRestartFull(CCObject* sender) {
        Bot::get().onRevert(0.0);
        PauseLayer::onRestartFull(sender);
    }
};

// ============================================================
// BotGUI — subclasses geode::Popup<> correctly
//
// Geode v5 Popup<Args...> API:
//   - Inherit from geode::Popup<>
//   - bool setup() override — return true on success
//   - m_mainLayer is the content CCLayer (defined in Popup base)
//   - Use static create() calling initAnchored(w, h)
//   - Callbacks: this must be cast to CCObject* via
//     static_cast<CCObject*>(this) for CCMenuItemSpriteExtra
// ============================================================

class BotGUI : public geode::Popup<> {
public:
    // ---- Member widgets ----
    CCLabelBMFont*   m_cbfLabel    = nullptr;
    CCSprite*        m_cbfDot      = nullptr;
    CCLabelBMFont*   m_statusLbl   = nullptr;
    CCTextInputNode* m_fileInput   = nullptr;
    CCTextInputNode* m_speedInput  = nullptr;

    // ---- Popup<> virtual ----
    bool setup() override {
        // Title
        this->setTitle("MacroBot v1.0");

        Bot& bot = Bot::get();
        bot.refreshCBF();

        const float W = m_mainLayer->getContentWidth();
        const float H = m_mainLayer->getContentHeight();
        const float cx = W * 0.5f;
        float y = H - 52.f;
        const float rowH = 34.f;

        // =============================================
        // CBF status row (dot + label)
        // =============================================
        {
            // Coloured circle dot using CCLayerColor (always available)
            auto* dot = CCLayerColor::create(
                ccc4(0, 0, 0, 255),   // colour set below
                12.f, 12.f
            );
            ccColor3B c = CBFStatus::dotColor(bot.cbfMode);
            dot->setColor(c);
            dot->setPosition({18.f, y - 5.f});
            m_mainLayer->addChild(dot, 1);
            // Store as sprite (we'll just re-create the dot on refresh via label)

            auto* lbl = CCLabelBMFont::create(
                CBFStatus::dotLabel(bot.cbfMode), "bigFont.fnt"
            );
            lbl->setScale(0.32f);
            lbl->setAnchorPoint({0.f, 0.5f});
            lbl->setColor(c);
            lbl->setPosition({34.f, y});
            m_mainLayer->addChild(lbl, 1);
            m_cbfLabel = lbl;
            y -= rowH;
        }

        // =============================================
        // Status label
        // =============================================
        {
            m_statusLbl = CCLabelBMFont::create(modeStr(), "bigFont.fnt");
            m_statusLbl->setScale(0.45f);
            m_statusLbl->setPosition({cx, y});
            m_mainLayer->addChild(m_statusLbl, 1);
            y -= rowH;
        }

        // =============================================
        // Record / Stop / Play buttons — using a single CCMenu
        // =============================================
        {
            auto* recSprite  = ButtonSprite::create("Record",  "bigFont.fnt", "GJ_button_02.png");
            auto* stopSprite = ButtonSprite::create("Stop",    "bigFont.fnt", "GJ_button_06.png");
            auto* playSprite = ButtonSprite::create("Play",    "bigFont.fnt", "GJ_button_01.png");

            // CCMenuItemSpriteExtra::create needs target as CCObject*
            auto* self = static_cast<CCObject*>(this);

            auto* recBtn  = CCMenuItemSpriteExtra::create(recSprite,  self, menu_selector(BotGUI::onRecord));
            auto* stopBtn = CCMenuItemSpriteExtra::create(stopSprite, self, menu_selector(BotGUI::onStop));
            auto* playBtn = CCMenuItemSpriteExtra::create(playSprite, self, menu_selector(BotGUI::onPlay));

            auto* menu = CCMenu::create();
            menu->setPosition({cx, y});
            menu->addChild(recBtn);
            menu->addChild(stopBtn);
            menu->addChild(playBtn);
            menu->alignItemsHorizontallyWithPadding(6.f);
            m_mainLayer->addChild(menu, 1);
            y -= rowH + 4.f;
        }

        // =============================================
        // File name input + Save / Load
        // =============================================
        {
            // Background box for text input
            auto* bg = cocos2d::extension::CCScale9Sprite::create(
                "square02_small.png", {0, 0, 40, 40}
            );
            bg->setContentSize({170.f, 28.f});
            bg->setPosition({cx, y});
            m_mainLayer->addChild(bg, 1);

            // Text input (Geode wraps CCTextInputNode)
            m_fileInput = CCTextInputNode::create(160.f, 28.f, "macro.bmac", "bigFont.fnt");
            m_fileInput->setPosition({cx, y});
            m_fileInput->setScale(0.48f);
            m_mainLayer->addChild(m_fileInput, 2);
            y -= 30.f;

            auto* self = static_cast<CCObject*>(this);
            auto* saveSpr = ButtonSprite::create("Save", "bigFont.fnt", "GJ_button_05.png");
            auto* loadSpr = ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_05.png");

            auto* saveBtn = CCMenuItemSpriteExtra::create(saveSpr, self, menu_selector(BotGUI::onSave));
            auto* loadBtn = CCMenuItemSpriteExtra::create(loadSpr, self, menu_selector(BotGUI::onLoad));

            auto* ioMenu = CCMenu::create();
            ioMenu->setPosition({cx, y});
            ioMenu->addChild(saveBtn);
            ioMenu->addChild(loadBtn);
            ioMenu->alignItemsHorizontallyWithPadding(6.f);
            m_mainLayer->addChild(ioMenu, 1);
            y -= rowH;
        }

        // =============================================
        // Speedhack row
        // =============================================
        {
            auto* lbl = CCLabelBMFont::create("Speed:", "bigFont.fnt");
            lbl->setScale(0.38f);
            lbl->setAnchorPoint({1.f, 0.5f});
            lbl->setPosition({cx - 10.f, y});
            m_mainLayer->addChild(lbl, 1);

            auto* bg = cocos2d::extension::CCScale9Sprite::create(
                "square02_small.png", {0, 0, 40, 40}
            );
            bg->setContentSize({70.f, 26.f});
            bg->setPosition({cx + 30.f, y});
            m_mainLayer->addChild(bg, 1);

            m_speedInput = CCTextInputNode::create(62.f, 26.f, "1.0", "bigFont.fnt");
            m_speedInput->setPosition({cx + 30.f, y});
            m_speedInput->setScale(0.48f);
            // Fill with current speed value
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.2f", bot.speedhack);
                m_speedInput->setString(buf);
            }
            m_mainLayer->addChild(m_speedInput, 2);

            auto* self    = static_cast<CCObject*>(this);
            auto* applSpr = ButtonSprite::create("Set", "bigFont.fnt", "GJ_button_04.png");
            auto* applBtn = CCMenuItemSpriteExtra::create(applSpr, self, menu_selector(BotGUI::onApplySpeed));

            auto* spMenu = CCMenu::create();
            spMenu->setPosition({cx + 85.f, y});
            spMenu->addChild(applBtn);
            m_mainLayer->addChild(spMenu, 1);
            y -= rowH;
        }

        // =============================================
        // Input count
        // =============================================
        {
            std::string info = "Inputs stored: " + std::to_string(bot.macro.inputs.size());
            auto* cntLbl = CCLabelBMFont::create(info.c_str(), "bigFont.fnt");
            cntLbl->setScale(0.34f);
            cntLbl->setColor({180, 180, 180});
            cntLbl->setPosition({cx, y});
            m_mainLayer->addChild(cntLbl, 1);
        }

        return true;
    }

    // ---- Callbacks ----

    void onRecord(CCObject*) {
        Bot& bot = Bot::get();
        if (!bot.canRun()) {
            showAlert("Bot Locked",
                "No CBF active.\nInstall <cy>syzzi.click_between_frames</c> "
                "from the Geode index, or enable RobTop's Click Between Steps in settings.");
            return;
        }
        if (bot.mode == BotMode::Recording) return;
        if (!PlayLayer::get()) {
            showAlert("Error", "You must be inside a level to record."); return;
        }
        bot.startRecording(bot.currentTime);
        refreshStatus();
    }

    void onStop(CCObject*) {
        Bot& bot = Bot::get();
        if (bot.mode == BotMode::Recording) bot.stopRecording();
        else if (bot.mode == BotMode::Playing) bot.stopPlayback();
        refreshStatus();
    }

    void onPlay(CCObject*) {
        Bot& bot = Bot::get();
        if (!bot.canRun()) {
            showAlert("Bot Locked", "No CBF active."); return;
        }
        if (bot.mode == BotMode::Playing) return;
        if (bot.macro.inputs.empty()) {
            showAlert("Empty Macro", "No inputs recorded or loaded."); return;
        }
        if (!PlayLayer::get()) {
            showAlert("Error", "You must be inside a level to play back."); return;
        }
        bot.startPlayback();
        refreshStatus();
    }

    void onSave(CCObject*) {
        const char* fname = m_fileInput ? m_fileInput->getString() : "macro.bmac";
        if (!fname || fname[0] == '\0') fname = "macro.bmac";
        auto path = Mod::get()->getSaveDir() / fname;
        if (Bot::get().macro.save(path.string())) {
            std::string msg = "Saved to:\n" + path.string();
            showAlert("Saved", msg.c_str());
        } else {
            showAlert("Save Failed", "Could not write file. Check the path/permissions.");
        }
    }

    void onLoad(CCObject*) {
        const char* fname = m_fileInput ? m_fileInput->getString() : "macro.bmac";
        if (!fname || fname[0] == '\0') fname = "macro.bmac";
        auto path = Mod::get()->getSaveDir() / fname;
        if (Bot::get().macro.load(path.string())) {
            std::string msg = "Loaded " +
                std::to_string(Bot::get().macro.inputs.size()) + " inputs.";
            showAlert("Loaded", msg.c_str());
            refreshStatus();
        } else {
            showAlert("Load Failed",
                "Could not read file. Check filename and ensure it's a valid .bmac file.");
        }
    }

    void onApplySpeed(CCObject*) {
        if (!m_speedInput) return;
        const char* s = m_speedInput->getString();
        if (!s || s[0] == '\0') return;
        float v = 1.0f;
        try { v = std::stof(std::string(s)); }
        catch (...) { showAlert("Error", "Invalid speed value. Enter a number like 1.5"); return; }
        applySpeedhack(v);
        // Update display to show clamped value
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", Bot::get().speedhack);
        m_speedInput->setString(buf);
    }

    // ---- Helpers ----

    const char* modeStr() {
        switch (Bot::get().mode) {
            case BotMode::Recording: return "Status: Recording";
            case BotMode::Playing:   return "Status: Playing";
            default:                 return "Status: Idle";
        }
    }

    void refreshStatus() {
        if (m_statusLbl) m_statusLbl->setString(modeStr());
        if (m_cbfLabel) {
            Bot::get().refreshCBF();
            m_cbfLabel->setString(CBFStatus::dotLabel(Bot::get().cbfMode));
            m_cbfLabel->setColor(CBFStatus::dotColor(Bot::get().cbfMode));
        }
    }

    // ---- Static factory ----
    static BotGUI* create() {
        auto* ret = new BotGUI();
        // initAnchored(width, height) — sets up the Popup frame and calls setup()
        if (ret->initAnchored(370.f, 420.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// ============================================================
// Definition of openBotGUI (forward-declared in MyPlayLayer)
// Must come after BotGUI is fully defined.
// ============================================================
void MyPlayLayer::openBotGUI() {
    // Only open one instance at a time
    if (auto* existing = CCDirector::sharedDirector()
                             ->getRunningScene()
                             ->getChildByID("bot-gui-popup")) {
        return;
    }
    auto* gui = BotGUI::create();
    if (gui) {
        gui->setID("bot-gui-popup");
        gui->show();
    }
}

// ============================================================
// Mod entry point
// ============================================================
$on_mod(Loaded) {
    log::info("[MacroBot] Loaded — GD 2.2081, Geode v5.7.1");
    log::info("[MacroBot] CBF at startup: {}",
              static_cast<int>(CBFStatus::detect()));
}