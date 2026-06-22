#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "Bot.hpp"
#include <iomanip>

using namespace geode::prelude;

enum class CBFStatus {
    None,
    RobTop,
    Syzzi
};

// Determine which CBF is actively running on the client by checking settings
CBFStatus getCBFStatus() {
    auto syzziMod = Loader::get()->getLoadedMod("syzzi.click_between_frames");
    if (syzziMod && syzziMod->isEnabled()) {
        bool isActive = true; // Fallback assume true if setting lookup fails
        if (syzziMod->hasSetting("cbf-enabled")) {
            isActive = syzziMod->getSettingValue<bool>("cbf-enabled");
        } else if (syzziMod->hasSetting("enabled")) {
            isActive = syzziMod->getSettingValue<bool>("enabled");
        }
        
        if (isActive) {
            return CBFStatus::Syzzi;
        }
    }
    
    if (GameManager::sharedState()->getGameVariable("0115")) {
        return CBFStatus::RobTop;
    }
    
    return CBFStatus::None;
}

// ==========================================
// SPEEDHACK & FMOD PITCH IMPLEMENTATION
// ==========================================
class $modify(CCScheduler) {
    void update(float dt) {
        float speed = BotManager::get().speedMultiplier;
        if (speed < 0.01f) speed = 0.01f;

        // Ensure speedhack does NOT affect players during death/respawn animations
        if (auto playLayer = PlayLayer::get()) {
            if (playLayer->m_player1 && playLayer->m_player1->m_isDead) {
                speed = 1.0f;
            }
        }

        // Scale game update ticks (engine speed)
        CCScheduler::update(dt * speed);

        // Scale background music pitch (audio speed)
        if (auto engine = FMODAudioEngine::sharedEngine()) {
            if (engine->m_backgroundMusicChannel) {
                engine->m_backgroundMusicChannel->setPitch(speed);
            }
        }
    }
};

// ==========================================
// INPUT RECORDING (GJBaseGameLayer Hooks)
// ==========================================
class $modify(MyGJBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool push, int button, bool isPlayer2) {
        GJBaseGameLayer::handleButton(push, button, isPlayer2);

        auto& bot = BotManager::get();
        if (bot.currentState == BotManager::State::Recording) {
            PlayerObject* player = isPlayer2 ? this->m_player2 : this->m_player1;
            if (player) {
                float delta = CCDirector::sharedDirector()->getDeltaTime();
                bot.addAction(player->m_position.x, delta, button, push, isPlayer2);
            }
        }
    }
};

// ==========================================
// CUSTOM UI MENU FOR MANAGING THE MACRO BOT
// ==========================================
class BotMenuLayer : public FLAlertLayer, public TextInputDelegate {
private:
    CCTextInputNode* m_fileNameInput = nullptr;
    CCTextInputNode* m_speedInput = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_macroSizeLabel = nullptr;
    CCMenuItemToggler* m_recordToggle = nullptr;
    CCMenuItemToggler* m_playToggle = nullptr;

public:
    static BotMenuLayer* create() {
        auto ret = new BotMenuLayer();
        if (ret && ret->init(280.0f, 260.0f, "GJ_square01.png", "CBF Bot Control Panel")) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init(float width, float height, const char* bg, const char* title) {
        if (!FLAlertLayer::init(150)) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        auto bgNode = CCScale9Sprite::create(bg);
        bgNode->setContentSize({width, height});
        bgNode->setPosition(winSize / 2);
        this->addChild(bgNode);

        auto titleLabel = CCLabelBMFont::create(title, "goldFont.fnt");
        titleLabel->setPosition(winSize.width / 2, winSize.height / 2 + height / 2 - 20.0f);
        titleLabel->setScale(0.7f);
        this->addChild(titleLabel);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu);

        auto closeSprite = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSprite, this, menu_selector(BotMenuLayer::onClose)
        );
        closeBtn->setPosition(winSize.width / 2 - width / 2 + 15.0f, winSize.height / 2 + height / 2 - 15.0f);
        menu->addChild(closeBtn);

        auto& bot = BotManager::get();

        // Status
        std::string statusText = "Status: ";
        if (bot.currentState == BotManager::State::Recording) statusText += "Recording";
        else if (bot.currentState == BotManager::State::Playing) statusText += "Playing";
        else statusText += "Idle";

        m_statusLabel = CCLabelBMFont::create(statusText.c_str(), "chatFont.fnt");
        m_statusLabel->setPosition(winSize.width / 2, winSize.height / 2 + 75.0f);
        m_statusLabel->setScale(0.6f);
        this->addChild(m_statusLabel);

        // CBF Indicator inside GUI
        std::string cbfText = "CBF: ";
        ccColor3B cbfColor = {255, 0, 0};
        auto status = getCBFStatus();
        if (status == CBFStatus::Syzzi) {
            cbfText += "Syzzi (Active)";
            cbfColor = {0, 255, 0};
        } else if (status == CBFStatus::RobTop) {
            cbfText += "RobTop (Active)";
            cbfColor = {255, 255, 0};
        } else {
            cbfText += "Disabled";
            cbfColor = {255, 50, 50};
        }

        auto cbfLabel = CCLabelBMFont::create(cbfText.c_str(), "chatFont.fnt");
        cbfLabel->setPosition(winSize.width / 2, winSize.height / 2 + 55.0f);
        cbfLabel->setScale(0.55f);
        cbfLabel->setColor(cbfColor);
        this->addChild(cbfLabel);

        std::string sizeText = "Recorded Inputs: " + std::to_string(bot.macro.size());
        m_macroSizeLabel = CCLabelBMFont::create(sizeText.c_str(), "chatFont.fnt");
        m_macroSizeLabel->setPosition(winSize.width / 2, winSize.height / 2 + 35.0f);
        m_macroSizeLabel->setScale(0.55f);
        this->addChild(m_macroSizeLabel);

        // Action Toggles (Checkboxes)
        auto offRec = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto onRec = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        m_recordToggle = CCMenuItemToggler::create(offRec, onRec, this, menu_selector(BotMenuLayer::onToggleRecord));
        m_recordToggle->setPosition(winSize.width / 2 - 50.0f, winSize.height / 2 + 5.0f);
        m_recordToggle->toggle(bot.currentState == BotManager::State::Recording);
        menu->addChild(m_recordToggle);

        auto recLabel = CCLabelBMFont::create("Record", "chatFont.fnt");
        recLabel->setPosition(winSize.width / 2 - 50.0f, winSize.height / 2 - 20.0f);
        recLabel->setScale(0.5f);
        this->addChild(recLabel);

        auto offPlay = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto onPlay = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        m_playToggle = CCMenuItemToggler::create(offPlay, onPlay, this, menu_selector(BotMenuLayer::onTogglePlay));
        m_playToggle->setPosition(winSize.width / 2 + 50.0f, winSize.height / 2 + 5.0f);
        m_playToggle->toggle(bot.currentState == BotManager::State::Playing);
        menu->addChild(m_playToggle);

        auto playLabel = CCLabelBMFont::create("Play", "chatFont.fnt");
        playLabel->setPosition(winSize.width / 2 + 50.0f, winSize.height / 2 - 20.0f);
        playLabel->setScale(0.5f);
        this->addChild(playLabel);

        // Speedhack Box Label
        auto speedLabel = CCLabelBMFont::create("Engine Speed:", "chatFont.fnt");
        speedLabel->setPosition(winSize.width / 2 - 60.0f, winSize.height / 2 - 40.0f);
        speedLabel->setScale(0.55f);
        this->addChild(speedLabel);

        // Input text box for Speedhack speed values
        m_speedInput = CCTextInputNode::create(70.0f, 30.0f, "speed", "chatFont.fnt");
        m_speedInput->setPosition(winSize.width / 2 + 40.0f, winSize.height / 2 - 40.0f);
        m_speedInput->setLabelPlaceholderColor({150, 150, 150});
        m_speedInput->setDelegate(this);
        m_speedInput->setAllowedChars("0123456789.");
        
        std::stringstream speedStream;
        speedStream << std::fixed << std::setprecision(2) << bot.speedMultiplier;
        m_speedInput->setString(speedStream.str());
        this->addChild(m_speedInput);

        // File Operations
        m_fileNameInput = CCTextInputNode::create(140.0f, 30.0f, "macro_name", "chatFont.fnt");
        m_fileNameInput->setPosition(winSize.width / 2 - 35.0f, winSize.height / 2 - 80.0f);
        m_fileNameInput->setString("my_macro.json");
        m_fileNameInput->setLabelPlaceholderColor({150, 150, 150});
        this->addChild(m_fileNameInput);

        auto saveSprite = ButtonSprite::create("Save", 0.6f);
        auto saveBtn = CCMenuItemSpriteExtra::create(
            saveSprite, this, menu_selector(BotMenuLayer::onSave)
        );
        saveBtn->setPosition(winSize.width / 2 + 75.0f, winSize.height / 2 - 70.0f);
        menu->addChild(saveBtn);

        auto loadSprite = ButtonSprite::create("Load", 0.6f);
        auto loadBtn = CCMenuItemSpriteExtra::create(
            loadSprite, this, menu_selector(BotMenuLayer::onLoad)
        );
        loadBtn->setPosition(winSize.width / 2 + 75.0f, winSize.height / 2 - 95.0f);
        menu->addChild(loadBtn);

        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);

        return true;
    }

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

    void textChanged(CCTextInputNode* node) override {
        if (node == m_speedInput) {
            std::string inputStr = node->getString();
            if (!inputStr.empty()) {
                try {
                    float value = std::stof(inputStr);
                    if (value >= 0.01f && value <= 10.0f) {
                        BotManager::get().speedMultiplier = value;
                    }
                } catch (...) {}
            }
        }
    }

    void onToggleRecord(CCObject* sender) {
        auto& bot = BotManager::get();
        // Checkboxes in Cocos automatically invert state before calling the callback
        if (m_recordToggle->isToggled()) {
            bot.currentState = BotManager::State::Recording;
            bot.clearMacro();
            if (m_playToggle->isToggled()) {
                m_playToggle->toggle(false); // Disable Play
            }
        } else {
            bot.currentState = BotManager::State::Idle;
        }
        updateLabels();
    }

    void onTogglePlay(CCObject* sender) {
        auto& bot = BotManager::get();
        if (m_playToggle->isToggled()) {
            bot.currentState = BotManager::State::Playing;
            bot.playbackIndex = 0;
            std::sort(bot.macro.begin(), bot.macro.end());
            if (m_recordToggle->isToggled()) {
                m_recordToggle->toggle(false); // Disable Record
            }
        } else {
            bot.currentState = BotManager::State::Idle;
        }
        updateLabels();
    }

    void onSave(CCObject*) {
        auto& bot = BotManager::get();
        std::string fName = m_fileNameInput->getString();
        if (fName.empty()) fName = "macro.json";
        if (bot.saveMacroToFile(fName)) {
            FLAlertLayer::create("Success", "Macro saved to " + fName, "OK")->show();
        } else {
            FLAlertLayer::create("Error", "Could not save macro file.", "OK")->show();
        }
    }

    void onLoad(CCObject*) {
        auto& bot = BotManager::get();
        std::string fName = m_fileNameInput->getString();
        if (fName.empty()) fName = "macro.json";
        if (bot.loadMacroFromFile(fName)) {
            FLAlertLayer::create("Loaded", "Successfully parsed " + std::to_string(bot.macro.size()) + " actions!", "OK")->show();
            updateLabels();
        } else {
            FLAlertLayer::create("Error", "Macro file not found or corrupted.", "OK")->show();
        }
    }

    void updateLabels() {
        auto& bot = BotManager::get();
        std::string statusText = "Status: ";
        if (bot.currentState == BotManager::State::Recording) statusText += "Recording";
        else if (bot.currentState == BotManager::State::Playing) statusText += "Playing";
        else statusText += "Idle";
        m_statusLabel->setString(statusText.c_str());

        std::string sizeText = "Recorded Inputs: " + std::to_string(bot.macro.size());
        m_macroSizeLabel->setString(sizeText.c_str());
    }
};

// Pause Layer Button Hook
class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = this->getChildByID("right-button-menu");
        if (!menu) menu = this->getChildByID("left-button-menu");
        if (!menu) return;

        auto botIcon = CCSprite::createWithSpriteFrameName("GJ_plusBtn_001.png");
        auto botBtn = CCMenuItemSpriteExtra::create(
            botIcon, this, menu_selector(MyPauseLayer::onOpenBotMenu)
        );
        menu->addChild(botBtn);
        menu->updateLayout();
    }

    void onOpenBotMenu(CCObject* sender) {
        auto layer = BotMenuLayer::create();
        CCDirector::sharedDirector()->getRunningScene()->addChild(layer, 100);
    }
};

// ==========================================
// PLAYBACK, LOGIC, & CHECKPOINT HOOKS
// ==========================================
class $modify(MyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto& bot = BotManager::get();
        bot.playbackIndex = 0;
        bot.isDead = false;

        if (this->m_isPracticeMode) {
            bot.currentState = BotManager::State::Recording;
            bot.clearMacro();
        }

        return true;
    }

    // Process commands BEFORE physics update for flawless Playback accuracy
    void update(float dt) {
        auto& bot = BotManager::get();
        if (bot.currentState == BotManager::State::Playing && !bot.macro.empty()) {
            while (bot.playbackIndex < bot.macro.size()) {
                BotAction& action = bot.macro[bot.playbackIndex];
                
                PlayerObject* player = action.isPlayer2 ? this->m_player2 : this->m_player1;
                if (!player) break;

                if (player->m_position.x >= action.xPosition) {
                    this->handleButton(action.isPush, action.button, action.isPlayer2);
                    bot.playbackIndex++;
                } else {
                    break;
                }
            }
        }
        
        PlayLayer::update(dt);
    }

    void destroyPlayer(PlayerObject* p, GameObject* g) {
        PlayLayer::destroyPlayer(p, g);
        // Mark player as dead to prevent overlapping jump registrations in the macro
        BotManager::get().isDead = true;
    }

    void removeLastCheckpoint() {
        PlayLayer::removeLastCheckpoint();
        
        auto& bot = BotManager::get();
        if (this->m_isPracticeMode && bot.currentState == BotManager::State::Recording) {
            if (this->m_checkpointArray->count() > 0) {
                // Fetch the new last checkpoint's position and truncate any inputs beyond it
                auto lastCp = this->m_checkpointArray->lastObject();
                if (bot.checkpointData.contains(lastCp)) {
                    bot.removeInputsAfterX(bot.checkpointData[lastCp].p1XPos);
                }
            } else {
                // If there are no checkpoints left, clear the entire macro buffer
                bot.clearMacro();
            }
        }
    }

    // --- PRACTICE MODE SNAPSHOTS ---
    void markCheckpoint() {
        PlayLayer::markCheckpoint();
        
        auto& bot = BotManager::get();
        if (!bot.practiceFixEnabled) return;

        if (this->m_checkpointArray->count() > 0) {
            auto checkpoint = this->m_checkpointArray->lastObject();
            
            CheckpointState state;
            
            state.p1XPos = this->m_player1->m_position.x;
            state.p1YPos = this->m_player1->m_position.y;
            state.p1YVel = this->m_player1->m_yVelocity;
            state.p1Rotation = this->m_player1->getRotation();
            state.p1Gravity = this->m_player1->m_gravity;
            state.p1IsDashing = this->m_player1->m_isDashing;
            state.p1IsUpsideDown = this->m_player1->m_isUpsideDown;
            state.p1IsOnGround = this->m_player1->m_isOnGround;
            state.p1IsSliding = this->m_player1->m_isSliding;

            if (this->m_player2) {
                state.p2XPos = this->m_player2->m_position.x;
                state.p2YPos = this->m_player2->m_position.y;
                state.p2YVel = this->m_player2->m_yVelocity;
                state.p2Rotation = this->m_player2->getRotation();
                state.p2Gravity = this->m_player2->m_gravity;
                state.p2IsDashing = this->m_player2->m_isDashing;
                state.p2IsUpsideDown = this->m_player2->m_isUpsideDown;
                state.p2IsOnGround = this->m_player2->m_isOnGround;
                state.p2IsSliding = this->m_player2->m_isSliding;
            }

            bot.checkpointData[checkpoint] = state;
        }
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        
        auto& bot = BotManager::get();
        double currentX = this->m_player1->m_position.x;

        if (bot.currentState == BotManager::State::Playing) {
            bot.playbackIndex = 0;
            for (const auto& action : bot.macro) {
                if (action.xPosition < currentX) {
                    bot.playbackIndex++;
                } else {
                    break;
                }
            }
        } else if (bot.currentState == BotManager::State::Recording) {
            // Aggressively clear any inputs that were mapped past this spawn point
            bot.removeInputsAfterX(currentX);
        }

        if (bot.practiceFixEnabled && bot.checkpointData.contains(checkpoint)) {
            CheckpointState state = bot.checkpointData[checkpoint];
            
            this->m_player1->m_position.x = state.p1XPos;
            this->m_player1->m_position.y = state.p1YPos;
            this->m_player1->m_yVelocity = state.p1YVel;
            this->m_player1->setRotation(state.p1Rotation);
            this->m_player1->m_gravity = state.p1Gravity;
            this->m_player1->m_isDashing = state.p1IsDashing;
            this->m_player1->m_isUpsideDown = state.p1IsUpsideDown;
            this->m_player1->m_isOnGround = state.p1IsOnGround;
            this->m_player1->m_isSliding = state.p1IsSliding;

            if (this->m_player2) {
                this->m_player2->m_position.x = state.p2XPos;
                this->m_player2->m_position.y = state.p2YPos;
                this->m_player2->m_yVelocity = state.p2YVel;
                this->m_player2->setRotation(state.p2Rotation);
                this->m_player2->m_gravity = state.p2Gravity;
                this->m_player2->m_isDashing = state.p2IsDashing;
                this->m_player2->m_isUpsideDown = state.p2IsUpsideDown;
                this->m_player2->m_isOnGround = state.p2IsOnGround;
                this->m_player2->m_isSliding = state.p2IsSliding;
            }
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& bot = BotManager::get();
        bot.isDead = false;

        if (bot.currentState == BotManager::State::Playing) {
            bot.playbackIndex = 0;
        } else if (bot.currentState == BotManager::State::Recording) {
            // Discard timeline completely if it's a full retry from the beginning
            if (!this->m_isPracticeMode || this->m_checkpointArray->count() == 0) {
                bot.clearMacro();
            } else {
                // If it's a practice retry, trim to the latest active checkpoint
                auto lastCp = this->m_checkpointArray->lastObject();
                if (bot.checkpointData.contains(lastCp)) {
                    bot.removeInputsAfterX(bot.checkpointData[lastCp].p1XPos);
                }
            }
        }
    }
};