#include "Bot.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <fstream>

class BotUI;
static BotUI* s_activeMenuInstance = nullptr;

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

void Bot::updateAudioPitch() {
    auto fmod = FMODAudioEngine::sharedEngine();
    if (fmod && fmod->m_backgroundMusicChannel) {
        if (isPlaying || isRecording) {
            fmod->m_backgroundMusicChannel->setPitch(speedHackValue);
        } else {
            fmod->m_backgroundMusicChannel->setPitch(1.0f);
        }
    }
}

// --- Recording & Playback Systems ---

void Bot::recordAction(float xPos, int button, bool player2, bool push) {
    actions.push_back({xPos, button, player2, push});
}

void Bot::updatePlayback(PlayLayer* pl) {
    if (!isPlaying || actions.empty()) return;

    while (playbackIndex < actions.size()) {
        auto& action = actions[playbackIndex];
        
        // Target player dynamically by pulling dual-mode status directly from player 1
        bool actualPlayer2 = action.player2 && pl->m_player1 && pl->m_player1->m_isDualMode;
        PlayerObject* p = actualPlayer2 ? pl->m_player2 : pl->m_player1;
        if (!p) p = pl->m_player1; 

        if (p && p->m_position.x >= action.xPos) {
            pl->handleButton(action.push, action.button, action.player2);
            playbackIndex++;
        } else {
            break; 
        }
    }
}

// --- Practice Mode Operations ---

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
    if (pl->m_player1) cp.p1 = captureState(pl->m_player1);
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
        if (pl->m_player1) applyState(pl->m_player1, cp.p1);
        if (pl->m_player2) applyState(pl->m_player2, cp.p2);
    } else {
        clearMacro();
    }
}

void Bot::clearCheckpoints() { checkpoints.clear(); }
void Bot::clearMacro() { actions.clear(); playbackIndex = 0; }

// --- Serialization Framework ---

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
            bool actualPlayer2 = player2 && this->m_player1 && this->m_player1->m_isDualMode;
            auto p = actualPlayer2 ? this->m_player2 : this->m_player1;
            if (p) {
                Bot::get().recordAction(p->m_position.x, button, player2, push);
            }
        }
        PlayLayer::handleButton(push, button, player2);
    }
};

class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt * Bot::get().speedHackValue);
    }
};

class $modify(BotAudioEngine, FMODAudioEngine) {
    void playMusic(gd::string path, bool loop, float fadeTime, int channel) {
        FMODAudioEngine::playMusic(path, loop, fadeTime, channel);
        Bot::get().updateAudioPitch();
    }
};

// --- User Interface Layer ---

class BotUI : public FLAlertLayer, public TextInputDelegate {
public:
    cocos2d::CCLabelBMFont* m_counterLabel = nullptr;
    CCMenuItemToggler* m_recordToggle = nullptr;
    CCMenuItemToggler* m_playToggle = nullptr;

    static BotUI* create() {
        auto ret = new BotUI();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() override {
        if (!FLAlertLayer::init(150)) return false;

        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        
        auto bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({420.f, 280.f});
        bg->setPosition(winSize / 2);
        this->m_mainLayer->addChild(bg);

        auto title = cocos2d::CCLabelBMFont::create("Macro Bot Engine", "goldFont.fnt");
        title->setPosition(winSize.width / 2, winSize.height / 2 + 115);
        title->setScale(0.8f);
        this->m_mainLayer->addChild(title);

        m_counterLabel = cocos2d::CCLabelBMFont::create("Captured Inputs: 0", "bigFont.fnt");
        m_counterLabel->setPosition(winSize.width / 2, winSize.height / 2 + 80);
        m_counterLabel->setScale(0.4f);
        m_counterLabel->setColor({180, 220, 255});
        this->m_mainLayer->addChild(m_counterLabel);

        auto menu = cocos2d::CCMenu::create();
        menu->setPosition({0, 0});
        this->m_mainLayer->addChild(menu);

        auto onLabel = cocos2d::CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto offLabel = cocos2d::CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        
        m_recordToggle = CCMenuItemToggler::create(offLabel, onLabel, this, menu_selector(BotUI::onToggleRecord));
        m_recordToggle->setPosition(winSize.width / 2 - 110, winSize.height / 2 + 20);
        m_recordToggle->toggle(Bot::get().isRecording);
        menu->addChild(m_recordToggle);

        auto recText = cocos2d::CCLabelBMFont::create("Record Mode", "bigFont.fnt");
        recText->setPosition(winSize.width / 2 - 40, winSize.height / 2 + 20);
        recText->setScale(0.45f);
        this->m_mainLayer->addChild(recText);

        auto onLabel2 = cocos2d::CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto offLabel2 = cocos2d::CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");

        m_playToggle = CCMenuItemToggler::create(offLabel2, onLabel2, this, menu_selector(BotUI::onTogglePlay));
        m_playToggle->setPosition(winSize.width / 2 + 40, winSize.height / 2 + 20);
        m_playToggle->toggle(Bot::get().isPlaying);
        menu->addChild(m_playToggle);

        auto playText = cocos2d::CCLabelBMFont::create("Playback", "bigFont.fnt");
        playText->setPosition(winSize.width / 2 + 110, winSize.height / 2 + 20);
        playText->setScale(0.45f);
        this->m_mainLayer->addChild(playText);

        auto speedLabel = cocos2d::CCLabelBMFont::create("Speedhack Value:", "bigFont.fnt");
        speedLabel->setPosition(winSize.width / 2 - 60, winSize.height / 2 - 35);
        speedLabel->setScale(0.4f);
        this->m_mainLayer->addChild(speedLabel);

        auto speedInput = TextInput::create(110.f, "1.0", "bigFont.fnt");
        speedInput->setPosition(winSize.width / 2 + 70, winSize.height / 2 - 35);
        speedInput->setDelegate(this);
        speedInput->setString(std::to_string(Bot::get().speedHackValue).substr(0, 4));
        this->m_mainLayer->addChild(speedInput);

        auto btnSave = CCMenuItemSpriteExtra::create(ButtonSprite::create("Save IO"), this, menu_selector(BotUI::onSaveFile));
        btnSave->setPosition(winSize.width / 2 - 70, winSize.height / 2 - 85);
        menu->addChild(btnSave);

        auto btnLoad = CCMenuItemSpriteExtra::create(ButtonSprite::create("Load IO"), this, menu_selector(BotUI::onLoadFile));
        btnLoad->setPosition(winSize.width / 2 + 70, winSize.height / 2 - 85);
        menu->addChild(btnLoad);

        auto closeBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Close Window"), this, menu_selector(BotUI::onClose));
        closeBtn->setPosition(winSize.width / 2, winSize.height / 2 - 125);
        menu->addChild(closeBtn);

        this->setKeypadEnabled(true);
        this->setTouchEnabled(true);
        this->scheduleUpdate();

        return true;
    }

    void update(float dt) override {
        FLAlertLayer::update(dt);
        if (m_counterLabel) {
            std::string updatedStr = "Captured Inputs: " + std::to_string(Bot::get().actions.size());
            m_counterLabel->setString(updatedStr.c_str());
        }
    }
    
    void textChanged(CCTextInputNode* node) override {
        std::string rawVal = node->getString();
        try {
            if (!rawVal.empty()) {
                float val = std::stof(rawVal);
                if (val > 0.001f && val <= 10.f) {
                    Bot::get().speedHackValue = val;
                    Bot::get().updateAudioPitch();
                }
            }
        } catch (...) {}
    }

    void onToggleRecord(cocos2d::CCObject* pSender) {
        auto toggle = static_cast<CCMenuItemToggler*>(pSender);
        Bot::get().isRecording = toggle->isOn();
        if (Bot::get().isRecording) {
            Bot::get().isPlaying = false;
            if (m_playToggle) m_playToggle->toggle(false);
        }
        Bot::get().updateAudioPitch();
    }

    void onTogglePlay(cocos2d::CCObject* pSender) {
        auto toggle = static_cast<CCMenuItemToggler*>(pSender);
        Bot::get().isPlaying = toggle->isOn();
        if (Bot::get().isPlaying) {
            Bot::get().isRecording = false;
            if (m_recordToggle) m_recordToggle->toggle(false);
            Bot::get().playbackIndex = 0;
        }
        Bot::get().updateAudioPitch();
    }
    
    void onSaveFile(cocos2d::CCObject*) {
        Bot::get().saveMacro("macro.dat");
        geode::Notification::create("Macro Serialized to Storage", geode::NotificationIcon::Success)->show();
    }
    
    void onLoadFile(cocos2d::CCObject*) {
        Bot::get().loadMacro("macro.dat");
        geode::Notification::create("Macro Parsed From Storage", geode::NotificationIcon::Success)->show();
    }

    void onClose(cocos2d::CCObject*) {
        s_activeMenuInstance = nullptr;
        this->removeFromParentAndCleanup(true);
    }
};

void Bot::toggleUI() { 
    if (s_activeMenuInstance) {
        s_activeMenuInstance->onClose(nullptr);
    } else {
        s_activeMenuInstance = BotUI::create();
        if (s_activeMenuInstance) {
            cocos2d::CCDirector::sharedDirector()->getRunningScene()->addChild(s_activeMenuInstance, 500);
        }
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