#include "Bot.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <iomanip>

using namespace geode::prelude;

// ==========================================
// BOT LOGIC IMPLEMENTATION
// ==========================================

void Bot::setMode(BotMode mode) {
    updateCBFState();
    if (m_cbfState == CBFState::None) {
        // Enforce CBF requirement
        m_mode = BotMode::Disabled;
        FLAlertLayer::create("Bot Error", "You must have <cg>Syzzi's CBF</c> or <cy>RobTop's CBS</c> enabled to use this bot.", "OK")->show();
        return;
    }
    m_mode = mode;
    if (mode == BotMode::Playing) {
        m_playbackIndex = 0; // Reset index for playback
    }
}

void Bot::updateCBFState() {
    auto loader = geode::Loader::get();
    if (loader->isModLoaded("syzzi.click_between_frames")) {
        m_cbfState = CBFState::SyzziCBF;
    } else {
        // In 2.2081, RobTop's CBS is often stored as a GameManager variable or PlayLayer setting.
        // Assuming GameManager variable for built-in CBS (usually "0175" or similar, checking general option)
        bool robTopCBS = GameManager::sharedState()->getGameVariable("0175"); 
        if (robTopCBS) {
            m_cbfState = CBFState::RobTopCBS;
        } else {
            m_cbfState = CBFState::None;
        }
    }
}

void Bot::setSpeedhack(float speed) {
    if (speed <= 0.0f) speed = 1.0f;
    m_speedhack = speed;
    CCDirector::sharedDirector()->getScheduler()->setTimeScale(speed);
}

void Bot::updateTime(double dt) {
    m_currentLevelTime += dt;
}

void Bot::recordInput(int button, bool push, bool isPlayer2) {
    if (m_mode != BotMode::Recording) return;

    MacroInput input;
    input.time = m_currentLevelTime;
    input.button = button;
    input.push = push;
    input.isPlayer2 = isPlayer2;
    
    m_inputs.push_back(input);
}

void Bot::processPlayback(GJBaseGameLayer* layer) {
    if (m_mode != BotMode::Playing) return;

    // Process all inputs that should have occurred by this exact micro-time.
    // Time-based execution naturally supports Syzzi's dynamic physics steps.
    while (m_playbackIndex < m_inputs.size()) {
        const auto& nextInput = m_inputs[m_playbackIndex];
        
        if (m_currentLevelTime >= nextInput.time) {
            // Execute the input
            layer->handleButton(nextInput.push, nextInput.button, !nextInput.isPlayer2);
            m_playbackIndex++;
        } else {
            break; // Next input is in the future
        }
    }
}

void Bot::discardDeadInputs(double revertTime) {
    if (m_mode != BotMode::Recording) return;
    
    // Remove all inputs that happened AFTER the time we are reverting to
    m_inputs.erase(
        std::remove_if(m_inputs.begin(), m_inputs.end(),
            [revertTime](const MacroInput& input) {
                return input.time > revertTime;
            }),
        m_inputs.end()
    );
}

void Bot::reset() {
    m_inputs.clear();
    m_checkpoints.clear();
    m_playbackIndex = 0;
    m_currentLevelTime = 0.0;
}

void Bot::toggleUI() {
    if (!m_botUIOpen) {
        auto ui = BotUI::create();
        ui->show();
        m_botUIOpen = true;
    }
}

PlayerCheckpointState Bot::capturePlayerState(PlayerObject* player) {
    PlayerCheckpointState state;
    if (!player) return state;
    
    state.m_position = player->getPosition();
    state.m_yVelocity = player->m_yVelocity;
    state.m_xVelocity = player->m_xVelocity;
    state.m_rotation = player->getRotation();
    state.m_isDashing = player->m_isDashing;
    state.m_isSliding = player->m_isSliding;
    state.m_isUpsideDown = player->m_isUpsideDown;
    state.m_vehicleSize = player->m_vehicleSize;
    state.m_jumpPower = player->m_jumpPower;
    
    return state;
}

void Bot::applyPlayerState(PlayerObject* player, const PlayerCheckpointState& state) {
    if (!player) return;
    
    player->setPosition(state.m_position);
    player->m_yVelocity = state.m_yVelocity;
    player->m_xVelocity = state.m_xVelocity;
    player->setRotation(state.m_rotation);
    player->m_isDashing = state.m_isDashing;
    player->m_isSliding = state.m_isSliding;
    player->m_isUpsideDown = state.m_isUpsideDown;
    player->m_vehicleSize = state.m_vehicleSize;
    player->m_jumpPower = state.m_jumpPower;
}

void Bot::saveCheckpoint(CheckpointObject* cp, PlayLayer* layer) {
    CheckpointData data;
    data.m_time = m_currentLevelTime;
    data.m_p1 = capturePlayerState(layer->m_player1);
    data.m_p2 = capturePlayerState(layer->m_player2);
    
    m_checkpoints[cp] = data;
}

void Bot::loadCheckpoint(CheckpointObject* cp, PlayLayer* layer) {
    if (m_checkpoints.find(cp) != m_checkpoints.end()) {
        const auto& data = m_checkpoints[cp];
        
        // Restore time and perfectly restore state to fix practice bugs
        m_currentLevelTime = data.m_time;
        applyPlayerState(layer->m_player1, data.m_p1);
        applyPlayerState(layer->m_player2, data.m_p2);
        
        // Cull dead inputs automatically
        discardDeadInputs(m_currentLevelTime);
        
        // If playing back, rollback the playback index to match the time
        if (m_mode == BotMode::Playing) {
            m_playbackIndex = 0;
            while (m_playbackIndex < m_inputs.size() && m_inputs[m_playbackIndex].time <= m_currentLevelTime) {
                m_playbackIndex++;
            }
        }
    }
}


// ==========================================
// GEODE HOOKS
// ==========================================

class $modify(MyPlayLayer, PlayLayer) {
    void update(float dt) {
        // High precision time accumulation independent of FPS limitations
        Bot::get().updateTime(static_cast<double>(dt));
        Bot::get().processPlayback(this);
        
        PlayLayer::update(dt);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        
        // If resetting from the beginning (not from a checkpoint)
        if (!this->m_isPracticeMode || this->m_checkpointArray->count() == 0) {
            Bot::get().setTime(0.0);
            Bot::get().discardDeadInputs(0.0);
            if (Bot::get().getMode() == BotMode::Playing) {
                Bot::get().setMode(BotMode::Playing); // resets index
            }
        }
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        Bot::get().saveCheckpoint(cp, this);
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        Bot::get().loadCheckpoint(cp, this);
    }
};

class $modify(MyGJBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool push, int button, bool isPlayer1) {
        // Prevent manual input during playback to avoid desync
        if (Bot::get().getMode() == BotMode::Playing) {
            // Allow exit/pause buttons through, but block gameplay buttons
            if (button != 0) return; 
        }
        
        GJBaseGameLayer::handleButton(push, button, isPlayer1);
        Bot::get().recordInput(button, push, !isPlayer1);
    }
};

class $modify(MyKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyModifiers) {
        if (key == KEY_K && isKeyDown) {
            Bot::get().toggleUI();
            return true; // Consume the keypress
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyModifiers);
    }
};


// ==========================================
// UI IMPLEMENTATION
// ==========================================

bool BotUI::setup() {
    auto winSize = CCDirector::sharedDirector()->getWinSize();
    this->setTitle("Macro Engine v2.0");

    Bot::get().updateCBFState();

    // -- Indicator Setup --
    m_cbfIndicator = CCLabelBMFont::create(".", "bigFont.fnt");
    m_cbfIndicator->setPosition({m_mainLayer->getContentSize().width / 2 + 100, m_mainLayer->getContentSize().height - 25});
    m_cbfIndicator->setScale(2.0f);
    m_mainLayer->addChild(m_cbfIndicator);

    // -- Status Label --
    m_statusLabel = CCLabelBMFont::create("Status: Disabled", "goldFont.fnt");
    m_statusLabel->setPosition({m_mainLayer->getContentSize().width / 2, m_mainLayer->getContentSize().height - 60});
    m_statusLabel->setScale(0.6f);
    m_mainLayer->addChild(m_statusLabel);

    // -- Buttons --
    auto menu = CCMenu::create();
    menu->setPosition({m_mainLayer->getContentSize().width / 2, m_mainLayer->getContentSize().height / 2 + 20});

    auto btnRecord = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Record"), this, menu_selector(BotUI::onRecord)
    );
    btnRecord->setPosition({-70, 0});
    menu->addChild(btnRecord);

    auto btnPlay = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Play"), this, menu_selector(BotUI::onPlay)
    );
    btnPlay->setPosition({70, 0});
    menu->addChild(btnPlay);

    auto btnDisable = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Disable"), this, menu_selector(BotUI::onDisable)
    );
    btnDisable->setPosition({0, -45});
    menu->addChild(btnDisable);

    m_mainLayer->addChild(menu);

    // -- Speedhack Input --
    auto speedhackBg = CCScale9Sprite::create("square02_small.png");
    speedhackBg->setContentSize({80, 30});
    speedhackBg->setOpacity(100);
    speedhackBg->setPosition({m_mainLayer->getContentSize().width / 2, 45});
    m_mainLayer->addChild(speedhackBg);

    m_speedhackInput = CCTextInputNode::create(80, 30, "Speed", "bigFont.fnt");
    m_speedhackInput->setPosition(speedhackBg->getPosition());
    m_speedhackInput->setLabelPlaceholderColor({200, 200, 200});
    m_speedhackInput->setAllowedChars("0123456789.");
    m_speedhackInput->setString(std::to_string(Bot::get().getSpeedhack()).c_str());
    m_speedhackInput->setDelegate(this); // Need standard input delegate handling or use Geode TextInput
    m_mainLayer->addChild(m_speedhackInput);

    auto speedLabel = CCLabelBMFont::create("Speedhack:", "bigFont.fnt");
    speedLabel->setPosition({m_mainLayer->getContentSize().width / 2, 75});
    speedLabel->setScale(0.4f);
    m_mainLayer->addChild(speedLabel);
    
    // Apply speed hack button
    auto applyMenu = CCMenu::create();
    applyMenu->setPosition({m_mainLayer->getContentSize().width / 2 + 80, 45});
    auto applyBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Apply", "goldFont.fnt", "GJ_button_01.png", .6f),
        this, menu_selector(BotUI::onSpeedhackChange)
    );
    applyMenu->addChild(applyBtn);
    m_mainLayer->addChild(applyMenu);

    updateUIState();
    return true;
}

void BotUI::updateUIState() {
    auto mode = Bot::get().getMode();
    if (mode == BotMode::Recording) m_statusLabel->setString("Status: Recording");
    else if (mode == BotMode::Playing) m_statusLabel->setString("Status: Playing");
    else m_statusLabel->setString("Status: Disabled");

    auto cbf = Bot::get().getCBFState();
    if (cbf == CBFState::SyzziCBF) m_cbfIndicator->setColor({0, 255, 0});       // Green
    else if (cbf == CBFState::RobTopCBS) m_cbfIndicator->setColor({255, 255, 0}); // Yellow
    else m_cbfIndicator->setColor({255, 0, 0});                                   // Red
}

void BotUI::onRecord(CCObject*) {
    Bot::get().setMode(BotMode::Recording);
    updateUIState();
}

void BotUI::onPlay(CCObject*) {
    Bot::get().setMode(BotMode::Playing);
    updateUIState();
}

void BotUI::onDisable(CCObject*) {
    Bot::get().setMode(BotMode::Disabled);
    updateUIState();
}

void BotUI::onSpeedhackChange(CCObject*) {
    try {
        float speed = std::stof(m_speedhackInput->getString());
        Bot::get().setSpeedhack(speed);
        FLAlertLayer::create("Success", "Speedhack applied via Scheduler.", "OK")->show();
    } catch (...) {
        FLAlertLayer::create("Error", "Invalid speed format.", "OK")->show();
    }
}

void BotUI::onClose(CCObject* sender) {
    Bot::get().setUIOpen(false);
    Popup::onClose(sender);
}

BotUI* BotUI::create() {
    auto ret = new BotUI();
    if (ret && ret->initAnchored(300, 220)) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}