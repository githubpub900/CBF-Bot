#include "Bot.hpp"
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

// Custom GUI for the Macro Bot
class BotUI : public geode::Popup<> {
protected:
    bool setup() override {
        this->setTitle("Macro Bot Settings");
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        
        // --- CBF Indicator ---
        int cbfStatus = Bot::get().getCBFStatus();
        auto dotLabel = CCLabelBMFont::create(".", "bigFont.fnt");
        dotLabel->setScale(2.5f);
        
        if (cbfStatus == 2) dotLabel->setColor({0, 255, 0});      // Syzzi (Green)
        else if (cbfStatus == 1) dotLabel->setColor({255, 255, 0}); // RobTop CBS (Yellow)
        else dotLabel->setColor({255, 0, 0});                       // None (Red)

        // Position period next to the title
        dotLabel->setPosition({
            m_title->getPositionX() + m_title->getContentSize().width / 2.f + 15.f,
            m_title->getPositionY() + 5.f
        });
        m_mainLayer->addChild(dotLabel);

        // --- Speedhack Textbox ---
        auto speedInput = TextInput::create(150.f, "Speed (e.g. 0.5)");
        speedInput->setPosition(m_mainLayer->getContentSize() / 2.f);
        speedInput->setFilter("0123456789.");
        
        speedInput->setCallback([](const std::string& str) {
            if (str.empty()) return;
            try {
                float speed = std::stof(str);
                // Detached from physics/frame logic, alters step scale
                CCDirector::sharedDirector()->getScheduler()->setTimeScale(speed);
            } catch(...) {}
        });
        m_mainLayer->addChild(speedInput);

        return true;
    }
public:
    static BotUI* create() {
        auto ret = new BotUI();
        if (ret && ret->initAnchored(260.f, 180.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// Keyboard Dispatcher hook for the "K" key GUI toggle
class $modify(BotKeybindHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isDown, bool isRepeat) {
        if (key == enumKeyCodes::KEY_K && isDown && !isRepeat) {
            if (!CCDirector::get()->getRunningScene()->getChildByID("bot-macro-ui")) {
                auto ui = BotUI::create();
                ui->setID("bot-macro-ui");
                ui->show();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isDown, isRepeat);
    }
};

// Main game logic hook
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        auto& bot = Bot::get();
        bot.m_time = 0.0;
        bot.m_playbackIndex = 0;
        bot.m_botTriggered = false;
        bot.m_checkpoints.clear();
        
        if (this->m_isPracticeMode) {
            // Start recording fresh macro in Practice Mode
            bot.m_recording = true;
            bot.m_playing = false;
            bot.m_inputs.clear(); 
        } else {
            // Playback macro in Normal Mode
            bot.m_recording = false;
            if (!bot.m_inputs.empty()) {
                bot.m_playing = true;
            }
        }
        
        return true;
    }
    
    void update(float dt) {
        PlayLayer::update(dt);
        
        auto& bot = Bot::get();
        bot.m_time += dt; // Inherently captures decimals to the maximal precision of IEEE 754 (Double)
        
        if (bot.m_playing && bot.getCBFStatus() != 0) {
            while (bot.m_playbackIndex < bot.m_inputs.size()) {
                auto& input = bot.m_inputs[bot.m_playbackIndex];
                
                // If it's time for the bot to execute an input
                if (input.time <= bot.m_time) {
                    bot.m_botTriggered = true; 
                    if (input.push) {
                        this->pushButton(input.button, input.player2);
                    } else {
                        this->releaseButton(input.button, input.player2);
                    }
                    bot.m_botTriggered = false;
                    bot.m_playbackIndex++;
                } else {
                    break;
                }
            }
        }
    }
    
    void resetLevel() {
        PlayLayer::resetLevel();
        
        auto& bot = Bot::get();
        double restoreTime = 0.0;
        
        // Sync time with Practice Checkpoints
        if (this->m_isPracticeMode && !bot.m_checkpoints.empty()) {
            restoreTime = bot.m_checkpoints.back().time;
            bot.loadCheckpoint(this->m_player1); // Accurately set velocity/player state
        }
        
        bot.m_time = restoreTime;
        
        if (bot.m_recording) {
            bot.resetInputs(restoreTime); // Discard dead inputs
        } else if (bot.m_playing) {
            bot.m_playbackIndex = 0;
            // Shift playback head to matching time coordinate
            while (bot.m_playbackIndex < bot.m_inputs.size() && 
                   bot.m_inputs[bot.m_playbackIndex].time < restoreTime) {
                bot.m_playbackIndex++;
            }
        }
    }
    
    void pushButton(int button, bool player2) {
        PlayLayer::pushButton(button, player2);
        auto& bot = Bot::get();
        
        if (bot.m_recording && !bot.m_botTriggered) {
            bot.addInput(true, player2, button);
        }
    }
    
    void releaseButton(int button, bool player2) {
        PlayLayer::releaseButton(button, player2);
        auto& bot = Bot::get();
        
        if (bot.m_recording && !bot.m_botTriggered) {
            bot.addInput(false, player2, button);
        }
    }
    
    void createCheckpoint() {
        PlayLayer::createCheckpoint();
        if (this->m_isPracticeMode) {
            Bot::get().saveCheckpoint(this->m_player1);
        }
    }
    
    void removeLastCheckpoint() {
        PlayLayer::removeLastCheckpoint();
        if (this->m_isPracticeMode) {
            Bot::get().removeLastCheckpoint();
        }
    }
};