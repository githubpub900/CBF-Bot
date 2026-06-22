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
        // Fallback for detecting 2.2081 built-in CBS
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

/*
 * PLAYBACK ARCHITECTURE & CBF/CBS INTEGRATION:
 * For Syzzi CBF and RobTop CBS, inputs can occur precisely between visual frames.
 * Because syzzi.click_between_frames does not export a public sub-step injection API 
 * for bots, our closest reliable fallback is to execute the input on the physics 
 * step immediately upon crossing the X-position threshold. This preserves maximal 
 * accuracy without attempting to artificially override Syzzi's tick alignments.
 */
void Bot::updatePlayback(PlayLayer* pl) {
    if (!isPlaying) return;
    
    while (playbackIndex < actions.size()) {
        auto& action = actions[playbackIndex];
        PlayerObject* p = action.player2 ? pl->m_player2 : pl->m_player1;
        
        if (!p) break;
        
        // Execute input if spatial threshold is reached
        if (p->m_position.x >= action.xPos) {
            if (action.push) {
                pl->pushButton(action.button, action.player2);
            } else {
                pl->releaseButton(action.button, action.player2);
            }
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
    s.rotation = p->m_rotation;
    s.yVelocity = p->m_yVelocity;
    s.xVelocity = p->m_xVelocity;
    s.jumpAccel = p->m_jumpAccel;
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
    p->m_rotation = s.rotation;
    p->m_yVelocity = s.yVelocity;
    p->m_xVelocity = s.xVelocity;
    p->m_jumpAccel = s.jumpAccel;
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
    cp.actionIndex = actions.size(); // Marks timeline split point
    cp.p1 = captureState(pl->m_player1);
    if (pl->m_player2) cp.p2 = captureState(pl->m_player2);
    cp.isDual = pl->m_isDualMode;
    checkpoints.push_back(cp);
}

void Bot::removeLastCheckpoint() {
    if (!checkpoints.empty()) checkpoints.pop_back();
}

void Bot::restoreCheckpoint(PlayLayer* pl) {
    if (!checkpoints.empty()) {
        auto& cp = checkpoints.back();
        // DEAD INPUT CLEANUP LOGIC:
        // Automatically truncates the vector to remove all inputs recorded 
        // past this checkpoint to discard abandoned routes/deaths.
        actions.resize(cp.actionIndex); 
        
        applyState(pl->m_player1, cp.p1);
        if (pl->m_player2) applyState(pl->m_player2, cp.p2);
        pl->m_isDualMode = cp.isDual;
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
    
    // Avoids per-frame allocations by resizing exactly once
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
            // Restart & Death logic mapping
            if (this->m_isPracticeMode) {
                Bot::get().restoreCheckpoint(this);
            } else {
                Bot::get().clearCheckpoints();
                Bot::get().clearMacro();
            }
        }
    }

    void markCheckpoint() {
        PlayLayer::markCheckpoint();
        if (Bot::get().isRecording) Bot::get().saveCheckpoint(this);
    }

    void removeLastCheckpoint() {
        PlayLayer::removeLastCheckpoint();
        if (Bot::get().isRecording) Bot::get().removeLastCheckpoint();
    }
    
    void pushButton(int button, bool player2) {
        if (Bot::get().isRecording && !Bot::get().isPlaying) {
            auto p = player2 ? this->m_player2 : this->m_player1;
            Bot::get().recordAction(p->m_position.x, button, player2, true);
        }
        PlayLayer::pushButton(button, player2);
    }
    
    void releaseButton(int button, bool player2) {
        if (Bot::get().isRecording && !Bot::get().isPlaying) {
            auto p = player2 ? this->m_player2 : this->m_player1;
            Bot::get().recordAction(p->m_position.x, button, player2, false);
        }
        PlayLayer::releaseButton(button, player2);
    }
};

/*
 * SPEEDHACK IMPLEMENTATION
 * Instead of adjusting game FPS, we directly scale delta time.
 * This ensures the macro timing stays flawlessly accurate and 
 * physically decoupled from the rendering loop.
 */
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        float scale = Bot::get().speedHackValue;
        CCScheduler::update(dt * scale);
    }
};

// --- User Interface Layer ---

class BotUI : public geode::Popup<> {
protected:
    bool setup() override {
        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        this->setTitle("Macro Bot 2.2081");

        // UI: CBF/CBS Indicator
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

        // Core Menus
        auto menu = cocos2d::CCMenu::create();
        menu->setPosition(winSize.width / 2, winSize.height / 2);
        
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

public:
    static BotUI* create() {
        auto ret = new BotUI();
        if (ret && ret->initAnchored(400.f, 250.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

void Bot::toggleUI() { BotUI::create()->show(); }

class $modify(BotKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool arr) {
        if (down && key == cocos2d::KEY_F8) {
            Bot::get().toggleUI();
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, arr);
    }
};