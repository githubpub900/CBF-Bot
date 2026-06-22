#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "Bot.hpp"

using namespace geode::prelude;

// ==========================================
// SPEEDHACK IMPLEMENTATION
// ==========================================
class $modify(CCScheduler) {
    void update(float dt) {
        float speed = BotManager::get().speedMultiplier;
        // Avoid division by zero or negative time steps
        if (speed < 0.01f) speed = 0.01f;
        CCScheduler::update(dt * speed);
    }
};

// ==========================================
// INPUT RECORDING (CBF / CBS COMPATIBLE)
// ==========================================
class $modify(MyPlayerObject, PlayerObject) {
    void pushButton(PlayerButton p0) {
        PlayerObject::pushButton(p0);
        
        auto& bot = BotManager::get();
        if (bot.currentState == BotManager::State::Recording) {
            bool isP2 = (PlayLayer::get() && PlayLayer::get()->m_player2 == this);
            // Get frame offset / step delta for hyper-precise microsecond timing
            float delta = CCDirector::sharedDirector()->getDeltaTime();
            bot.addAction(this->m_position.x, delta, static_cast<int>(p0), true, isP2);
        }
    }

    void releaseButton(PlayerButton p0) {
        PlayerObject::releaseButton(p0);
        
        auto& bot = BotManager::get();
        if (bot.currentState == BotManager::State::Recording) {
            bool isP2 = (PlayLayer::get() && PlayLayer::get()->m_player2 == this);
            float delta = CCDirector::sharedDirector()->getDeltaTime();
            bot.addAction(this->m_position.x, delta, static_cast<int>(p0), false, isP2);
        }
    }
};

// ==========================================
// CUSTOM UI MENU FOR MANAGING THE MACRO BOT
// ==========================================
class BotMenuLayer : public FLAlertLayer {
private:
    CCTextInputNode* m_fileNameInput = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_macroSizeLabel = nullptr;

public:
    static BotMenuLayer* create() {
        auto ret = new BotMenuLayer();
        if (ret && ret->init(280.0f, 220.0f, "GJ_square01.png", "CBF Bot Control Panel")) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init(float width, float height, const char* bg, const char* title) {
        if (!FLAlertLayer::init(150)) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        // Background structure
        auto bgNode = CCScale9Sprite::create(bg);
        bgNode->setContentSize({width, height});
        bgNode->setPosition(winSize / 2);
        this->addChild(bgNode);

        // Menu title
        auto titleLabel = CCLabelBMFont::create(title, "goldFont.fnt");
        titleLabel->setPosition(winSize.width / 2, winSize.height / 2 + height / 2 - 20.0f);
        titleLabel->setScale(0.7f);
        this->addChild(titleLabel);

        // Main Menu
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        this->addChild(menu);

        // Close button
        auto closeSprite = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSprite, this, menu_selector(BotMenuLayer::onClose)
        );
        closeBtn->setPosition(winSize.width / 2 - width / 2 + 15.0f, winSize.height / 2 + height / 2 - 15.0f);
        menu->addChild(closeBtn);

        auto& bot = BotManager::get();

        // Status Indicators
        std::string statusText = "Status: ";
        if (bot.currentState == BotManager::State::Recording) statusText += "Recording";
        else if (bot.currentState == BotManager::State::Playing) statusText += "Playing";
        else statusText += "Idle";

        m_statusLabel = CCLabelBMFont::create(statusText.c_str(), "chatFont.fnt");
        m_statusLabel->setPosition(winSize.width / 2, winSize.height / 2 + 55.0f);
        m_statusLabel->setScale(0.6f);
        this->addChild(m_statusLabel);

        std::string sizeText = "Recorded Inputs: " + std::to_string(bot.macro.size());
        m_macroSizeLabel = CCLabelBMFont::create(sizeText.c_str(), "chatFont.fnt");
        m_macroSizeLabel->setPosition(winSize.width / 2, winSize.height / 2 + 35.0f);
        m_macroSizeLabel->setScale(0.55f);
        this->addChild(m_macroSizeLabel);

        // Control Toggles using proper overload signatures (char const*, float scale)
        auto recordSprite = ButtonSprite::create("Record", 0.8f);
        auto recordBtn = CCMenuItemSpriteExtra::create(
            recordSprite, this, menu_selector(BotMenuLayer::onToggleRecord)
        );
        recordBtn->setPosition(winSize.width / 2 - 65.0f, winSize.height / 2 - 5.0f);
        menu->addChild(recordBtn);

        auto playSprite = ButtonSprite::create("Play", 0.8f);
        auto playBtn = CCMenuItemSpriteExtra::create(
            playSprite, this, menu_selector(BotMenuLayer::onTogglePlay)
        );
        playBtn->setPosition(winSize.width / 2 + 65.0f, winSize.height / 2 - 5.0f);
        menu->addChild(playBtn);

        // Speedhack controls
        auto speedLabel = CCLabelBMFont::create("Engine Speed:", "chatFont.fnt");
        speedLabel->setPosition(winSize.width / 2 - 50.0f, winSize.height / 2 - 45.0f);
        speedLabel->setScale(0.5f);
        this->addChild(speedLabel);

        std::string currentSpeedStr = std::to_string(bot.speedMultiplier).substr(0, 4) + "x";
        auto speedValLabel = CCLabelBMFont::create(currentSpeedStr.c_str(), "goldFont.fnt");
        speedValLabel->setPosition(winSize.width / 2 + 50.0f, winSize.height / 2 - 45.0f);
        speedValLabel->setScale(0.5f);
        speedValLabel->setTag(101); // Identify for easy updates
        this->addChild(speedValLabel);

        auto speedDownSprite = CCSprite::createWithSpriteFrameName("edit_leftBtn_001.png");
        auto speedDownBtn = CCMenuItemSpriteExtra::create(
            speedDownSprite, this, menu_selector(BotMenuLayer::onSpeedDown)
        );
        speedDownBtn->setPosition(winSize.width / 2 + 10.0f, winSize.height / 2 - 45.0f);
        menu->addChild(speedDownBtn);

        auto speedUpSprite = CCSprite::createWithSpriteFrameName("edit_rightBtn_001.png");
        auto speedUpBtn = CCMenuItemSpriteExtra::create(
            speedUpSprite, this, menu_selector(BotMenuLayer::onSpeedUp)
        );
        speedUpBtn->setPosition(winSize.width / 2 + 90.0f, winSize.height / 2 - 45.0f);
        menu->addChild(speedUpBtn);

        // File Save / Load UI Text Field
        m_fileNameInput = CCTextInputNode::create(150.0f, 30.0f, "macro_name", "chatFont.fnt");
        m_fileNameInput->setPosition(winSize.width / 2 - 30.0f, winSize.height / 2 - 80.0f);
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

        // Make touch responders active
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);

        return true;
    }

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

    void onToggleRecord(CCObject*) {
        auto& bot = BotManager::get();
        if (bot.currentState == BotManager::State::Recording) {
            bot.currentState = BotManager::State::Idle;
        } else {
            bot.currentState = BotManager::State::Recording;
            bot.clearMacro();
        }
        updateLabels();
    }

    void onTogglePlay(CCObject*) {
        auto& bot = BotManager::get();
        if (bot.currentState == BotManager::State::Playing) {
            bot.currentState = BotManager::State::Idle;
        } else {
            bot.currentState = BotManager::State::Playing;
            bot.playbackIndex = 0;
            std::sort(bot.macro.begin(), bot.macro.end());
        }
        updateLabels();
    }

    void onSpeedDown(CCObject*) {
        auto& bot = BotManager::get();
        bot.speedMultiplier = std::max(0.1f, bot.speedMultiplier - 0.1f);
        updateSpeedLabel();
    }

    void onSpeedUp(CCObject*) {
        auto& bot = BotManager::get();
        bot.speedMultiplier = std::min(5.0f, bot.speedMultiplier + 0.1f);
        updateSpeedLabel();
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

    void updateSpeedLabel() {
        auto label = static_cast<CCLabelBMFont*>(this->getChildByTag(101));
        if (label) {
            std::string speedStr = std::to_string(BotManager::get().speedMultiplier).substr(0, 4) + "x";
            label->setString(speedStr.c_str());
        }
    }
};

// Hook into Pause Layer to trigger our panel easily
class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = this->getChildByID("right-button-menu");
        if (!menu) menu = this->getChildByID("left-button-menu");
        if (!menu) return;

        // Custom mod button on pause screen
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
// PLAYBACK, LOGIC, & ROBUST PRACTICE FIXES
// ==========================================
class $modify(MyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto& bot = BotManager::get();
        bot.playbackIndex = 0;

        // Reset state transition
        if (this->m_isPracticeMode) {
            bot.currentState = BotManager::State::Recording;
            bot.clearMacro();
        } else {
            // Keep current status (Idle / Playing)
        }

        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto& bot = BotManager::get();
        if (bot.currentState != BotManager::State::Playing || bot.macro.empty()) return;

        // Execute precise sub-step inputs
        while (bot.playbackIndex < bot.macro.size()) {
            BotAction& action = bot.macro[bot.playbackIndex];
            
            PlayerObject* player = action.isPlayer2 ? this->m_player2 : this->m_player1;
            if (!player) break;

            // Accurate boundary matching for sub-frame executions
            if (player->m_position.x >= action.xPosition) {
                // Call input functions directly on the PlayerObject
                if (action.isPush) {
                    player->pushButton(static_cast<PlayerButton>(action.button));
                } else {
                    player->releaseButton(static_cast<PlayerButton>(action.button));
                }
                bot.playbackIndex++;
            } else {
                break;
            }
        }
    }

    // --- ACCURATE PRACTICE CHECKS (ZERO DESYNC ENGINE) ---
    void markCheckpoint() {
        PlayLayer::markCheckpoint();
        
        auto& bot = BotManager::get();
        if (!bot.practiceFixEnabled) return;

        if (this->m_checkpointArray->count() > 0) {
            auto checkpoint = this->m_checkpointArray->lastObject();
            
            CheckpointState state;
            
            // Player 1 precise physics state
            state.p1XPos = this->m_player1->m_position.x;
            state.p1YPos = this->m_player1->m_position.y;
            state.p1YVel = this->m_player1->m_yVelocity;
            state.p1Rotation = this->m_player1->getRotation();
            state.p1IsDashing = this->m_player1->m_isDashing;
            state.p1IsUpsideDown = this->m_player1->m_isUpsideDown;
            state.p1IsOnGround = this->m_player1->m_isOnGround;
            state.p1IsSliding = this->m_player1->m_isSliding;

            // Player 2 precise physics state
            if (this->m_player2) {
                state.p2XPos = this->m_player2->m_position.x;
                state.p2YPos = this->m_player2->m_position.y;
                state.p2YVel = this->m_player2->m_yVelocity;
                state.p2Rotation = this->m_player2->getRotation();
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
        
        // Match player X timeline position to index
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
            // Drop recording timeline points generated ahead of load marker
            bot.macro.erase(
                std::remove_if(bot.macro.begin(), bot.macro.end(),
                    [currentX](const BotAction& a) { return a.xPosition >= currentX; }),
                bot.macro.end()
            );
        }

        // Apply physical corrections directly back onto physics loop
        if (bot.practiceFixEnabled && bot.checkpointData.contains(checkpoint)) {
            CheckpointState state = bot.checkpointData[checkpoint];
            
            // Restore Player 1
            this->m_player1->m_position.x = state.p1XPos;
            this->m_player1->m_position.y = state.p1YPos;
            this->m_player1->m_yVelocity = state.p1YVel;
            this->m_player1->setRotation(state.p1Rotation);
            this->m_player1->m_isDashing = state.p1IsDashing;
            this->m_player1->m_isUpsideDown = state.p1IsUpsideDown;
            this->m_player1->m_isOnGround = state.p1IsOnGround;
            this->m_player1->m_isSliding = state.p1IsSliding;

            // Restore Player 2
            if (this->m_player2) {
                this->m_player2->m_position.x = state.p2XPos;
                this->m_player2->m_position.y = state.p2YPos;
                this->m_player2->m_yVelocity = state.p2YVel;
                this->m_player2->setRotation(state.p2Rotation);
                this->m_player2->m_isDashing = state.p2IsDashing;
                this->m_player2->m_isUpsideDown = state.p2IsUpsideDown;
                this->m_player2->m_isOnGround = state.p2IsOnGround;
                this->m_player2->m_isSliding = state.p2IsSliding;
            }
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (BotManager::get().currentState == BotManager::State::Playing) {
            BotManager::get().playbackIndex = 0;
        }
    }
};