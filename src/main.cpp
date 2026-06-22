#include "Bot.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <fstream>

// --- Singleton & Engine Detection ---

Bot& Bot::get() {
    static Bot instance;
    return instance;
}

void Bot::detectEngine() {
    if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
        engine = EngineType::SyzziCBF;
    } else {
        auto gm = GameManager::sharedState();
        if (gm->getGameVariable("0179") || gm->getGameVariable("0178")) {
            engine = EngineType::CBS;
        } else {
            engine = EngineType::None;
        }
    }
}

// --- Recording & Playback Systems ---

void Bot::recordAction(float xPos, int button, bool player2, bool push) {
    actions.push_back({xPos, button, player2, push});
}

void Bot::updatePlayback(PlayLayer* pl) {
    if (!isPlaying) return;
    
    while (playbackIndex < actions.size()) {
        auto& action = actions[playbackIndex];
        PlayerObject* p = action.player2 ? pl->m_player2 : pl->m_player1;
        
        if (!p) break;
        
        if (p->m_position.x >= action.xPos) {
            pl->handleButton(action.push, action.button, action.player2);
            playbackIndex++;
        } else {
            break;
        }
    }
}

// --- Practice Mode Bug Fix & Dead Input Handling ---

PlayerState Bot::captureState(PlayerObject* p) {
    PlayerState s;
    s.position = p->m_position;
    s.rotation = p->getRotation();
    s.yVelocity = p->m_yVelocity;
    s.isUpsideDown = p->m_isUpsideDown;
    s.isOnGround = p->m_isOnGround;
    s.isDashing = p->m_isDashing;
    s.isSliding = p->m_isSliding;
    s.vehicleSize = p->m_vehicleSize;
    s.speed = p->m_playerSpeed;
    s.isShip = p->m_isShip;
    s.isBird = p->m_isBird;
    s.isBall = p->m_isBall;
    s.isDart = p->m_isDart;
    s.isRobot = p->m_isRobot;
    s.isSpider = p->m_isSpider;
    s.isSwing = p->m_isSwing;
    return s;
}

void Bot::applyState(PlayerObject* p, const PlayerState& s) {
    p->m_position = s.position;
    p->setRotation(s.rotation);
    p->m_yVelocity = s.yVelocity;
    p->m_isUpsideDown = s.isUpsideDown;
    p->m_isOnGround = s.isOnGround;
    p->m_isDashing = s.isDashing;
    p->m_isSliding = s.isSliding;
    p->m_vehicleSize = s.vehicleSize;
    p->m_playerSpeed = s.speed;
    p->m_isShip = s.isShip;
    p->m_isBird = s.isBird;
    p->m_isBall = s.isBall;
    p->m_isDart = s.isDart;
    p->m_isRobot = s.isRobot;
    p->m_isSpider = s.isSpider;
    p->m_isSwing = s.isSwing;
}

void Bot::saveCheckpoint(PlayLayer* pl) {
    CheckpointData cp;
    cp.actionIndex = actions.size(); 
    cp.p1 = captureState(pl->m_player1);
    if (pl->m_player2) cp.p2 = captureState(pl->m_player2);
    checkpoints.push_back(cp);
}

void Bot::removeLastCheckpoint() {
    if (!checkpoints.empty()) checkpoints.pop_back();
}

void Bot::restoreCheckpoint(PlayLayer* pl) {
    if (!checkpoints.empty()) {
        auto& cp = checkpoints.back();
        actions.resize(cp.actionIndex); 
        
        applyState(pl->m_player1, cp.p1);
        if (pl->m_player2) applyState(pl->m_player2, cp.p2);
    } else {
        clearMacro();
    }
}

void Bot::clearCheckpoints() { checkpoints.clear(); }
void Bot::clearMacro() { actions.clear(); playbackIndex = 0; }

// --- Efficient Serialization ---

void Bot::saveMacro(const std::string& filename) {
    auto path = geode::Mod::get()->getSaveDir() / filename;
    std::ofstream file(path, std::ios::binary);
    if (!file) return;
    
    size_t count = actions.size();
    file.write(reinterpret_cast<char*>(&count), sizeof(count));
    file.write(reinterpret_cast<char*>(actions.data()), count * sizeof(MacroAction));
}

void Bot::loadMacro(const std::string& filename) {
    auto path = geode::Mod::get()->getSaveDir() / filename;
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    
    size_t count = 0;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    
    actions.resize(count);
    file.read(reinterpret_cast<char*>(actions.data()), count * sizeof(MacroAction));
}

// --- Hooks ---

class $modify(BotPlayLayer, PlayLayer) {
    void update(float dt) {
        if (Bot::get().isPlaying) {
            Bot::get().updatePlayback(this);
        }
        PlayLayer::update(dt);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::get().playbackIndex = 0;
        
        if (Bot::get().isRecording) {
            if (this->m_isPracticeMode) {
                Bot::get().restoreCheckpoint(this);
            } else {
                Bot::get().clearCheckpoints();
                Bot::get().clearMacro();
            }
        }
    }

    void createCheckpoint() {
        PlayLayer::createCheckpoint();
        if (Bot::get().isRecording) Bot::get().saveCheckpoint(this);
    }

    void removeCheckpoint(bool p0) {
        PlayLayer::removeCheckpoint(p0);
        if (Bot::get().isRecording) Bot::get().removeLastCheckpoint();
    }
    
    void handleButton(bool push, int button, bool player2) {
        if (Bot::get().isRecording && !Bot::get().isPlaying) {
            auto p = player2 ? this->m_player2 : this->m_player1;
            if (p) {
                Bot::get().recordAction(p->m_position.x, button, player2, push);
            }
        }
        PlayLayer::handleButton(push, button, player2);
    }
};

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        float scale = Bot::get().speedHackValue;
        CCScheduler::update(dt * scale);
    }
};

// --- User Interface Layer ---

class BotUI : public FLAlertLayer {
public:
    static BotUI* create() {
        auto ret = new BotUI();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() {
        if (!FLAlertLayer::init(150)) return false;

        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        
        auto bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({400.f, 250.f});
        bg->setPosition(winSize / 2);
        this->m_mainLayer->addChild(bg);

        auto title = cocos2d::CCLabelBMFont::create("Macro Bot 2.2081", "goldFont.fnt");
        title->setPosition(winSize.width / 2, winSize.height / 2 + 100);
        title->setScale(0.8f);
        this->m_mainLayer->addChild(title);

        auto statusLabel = cocos2d::CCLabelBMFont::create("", "bigFont.fnt");
        statusLabel->setPosition(winSize.width / 2, winSize.height / 2 + 50);
        statusLabel->setScale(0.5f);
        
        Bot::get().detectEngine();
        switch (Bot::get().engine) {
            case EngineType::SyzziCBF:
                statusLabel->setString("Status: Syzzi CBF Active");
                statusLabel->setColor({0, 255, 0});
                break;
            case EngineType::CBS:
                statusLabel->setString("Status: RobTop CBS Active");
                statusLabel->setColor({255, 255, 0});
                break;
            case EngineType::None:
            default:
                statusLabel->setString("Status: Disabled (No CBF/CBS)");
                statusLabel->setColor({255, 0, 0});
                break;
        }
        this->m_mainLayer->addChild(statusLabel);

        auto menu = cocos2d::CCMenu::create();
        menu->setPosition(winSize.width / 2, winSize.height / 2 - 20);
        
        auto btnRecord = CCMenuItemSpriteExtra::create(ButtonSprite::create("Record"), this, menu_selector(BotUI::onRecord));
        auto btnPlay = CCMenuItemSpriteExtra::create(ButtonSprite::create("Play"), this, menu_selector(BotUI::onPlay));
        auto btnSave = CCMenuItemSpriteExtra::create(ButtonSprite::create("Save"), this, menu_selector(BotUI::onSave));
        auto btnLoad = CCMenuItemSpriteExtra::create(ButtonSprite::create("Load"), this, menu_selector(BotUI::onLoad));
        auto btnSpeed = CCMenuItemSpriteExtra::create(ButtonSprite::create("Speed"), this, menu_selector(BotUI::onSpeedHack));
        
        menu->addChild(btnRecord);
        menu->addChild(btnPlay);
        menu->addChild(btnSave);
        menu->addChild(btnLoad);
        menu->addChild(btnSpeed);
        menu->alignItemsHorizontallyWithPadding(10.f);
        
        this->m_mainLayer->addChild(menu);

        auto closeMenu = cocos2d::CCMenu::create();
        closeMenu->setPosition(winSize.width / 2, winSize.height / 2 - 90);
        auto closeBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Close"), this, menu_selector(BotUI::onClose));
        closeMenu->addChild(closeBtn);
        this->m_mainLayer->addChild(closeMenu);

        this->setKeypadEnabled(true);
        this->setTouchEnabled(true);

        return true;
    }
    
    void onRecord(cocos2d::CCObject*) {
        Bot::get().isRecording = !Bot::get().isRecording;
        Bot::get().isPlaying = false;
        geode::Notification::create(Bot::get().isRecording ? "Recording Started" : "Recording Stopped", geode::NotificationIcon::Info)->show();
    }
    
    void onPlay(cocos2d::CCObject*) {
        if (Bot::get().engine == EngineType::None) {
            geode::Notification::create("Playback Locked: No CBF/CBS", geode::NotificationIcon::Error)->show();
            return;
        }
        Bot::get().isPlaying = !Bot::get().isPlaying;
        Bot::get().isRecording = false;
        Bot::get().playbackIndex = 0;
        geode::Notification::create(Bot::get().isPlaying ? "Playback Started" : "Playback Stopped", geode::NotificationIcon::Info)->show();
    }
    
    void onSave(cocos2d::CCObject*) {
        Bot::get().saveMacro("macro.dat");
        geode::Notification::create("Macro Saved", geode::NotificationIcon::Success)->show();
    }
    
    void onLoad(cocos2d::CCObject*) {
        Bot::get().loadMacro("macro.dat");
        geode::Notification::create("Macro Loaded", geode::NotificationIcon::Success)->show();
    }

    void onSpeedHack(cocos2d::CCObject*) {
        auto& bot = Bot::get();
        if (bot.speedHackValue == 1.0f) bot.speedHackValue = 0.5f;
        else if (bot.speedHackValue == 0.5f) bot.speedHackValue = 2.0f;
        else bot.speedHackValue = 1.0f;
        
        geode::Notification::create("Speedhack: " + std::to_string(bot.speedHackValue).substr(0,3) + "x", geode::NotificationIcon::Info)->show();
    }

    void onClose(cocos2d::CCObject*) {
        this->removeFromParentAndCleanup(true);
    }
};

void Bot::toggleUI() { 
    auto ui = BotUI::create();
    if (ui) {
        cocos2d::CCDirector::sharedDirector()->getRunningScene()->addChild(ui, 100);
    }
}

class $modify(BotKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && key == cocos2d::KEY_F8) {
            Bot::get().toggleUI();
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};