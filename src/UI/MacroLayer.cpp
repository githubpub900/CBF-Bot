#include "MacroLayer.hpp"
#include "../MacroBot.hpp"
#include "../CBFIntegration.hpp"
#include "../Speedhack.hpp"
#include "../PracticeFix.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <cmath>

using namespace geode::prelude;

namespace cbf {

MacroLayer* MacroLayer::create() {
    auto ret = new MacroLayer();
    if (ret && ret->init()) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

void MacroLayer::toggle() {
    auto scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return;
    if (auto existing = static_cast<MacroLayer*>(scene->getChildByTag(0xCBF1))) {
        existing->onCloseBtn(nullptr);
        return;
    }
    auto layer = MacroLayer::create();
    if (layer) {
        layer->setTag(0xCBF1);
        layer->setZOrder(1000);
        scene->addChild(layer);
    }
}

bool MacroLayer::init() {
    // Geode v5: Popup::init(width, height) sets up the popup background.
    if (!Popup::init(380.f, 340.f)) return false;

    setTitle("CBF Macro Bot");

    auto winSize = m_mainLayer->getContentSize();
    float cx = winSize.width / 2;

    // ---- Status label ----
    m_statusLabel = cocos2d::CCLabelBMFont::create("Status: Idle", "bigFont.fnt");
    m_statusLabel->setPosition({cx, winSize.height - 40.f});
    m_statusLabel->setScale(0.7f);
    m_mainLayer->addChild(m_statusLabel);

    // ---- Stats label ----
    m_statsLabel = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    m_statsLabel->setPosition({cx, winSize.height - 60.f});
    m_statsLabel->setScale(0.9f);
    m_mainLayer->addChild(m_statsLabel);

    // ---- CBF + physics rate status ----
    m_cbfLabel = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    m_cbfLabel->setPosition({cx, winSize.height - 80.f});
    m_cbfLabel->setScale(0.8f);
    m_mainLayer->addChild(m_cbfLabel);

    m_physRateLabel = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
    m_physRateLabel->setPosition({cx, winSize.height - 98.f});
    m_physRateLabel->setScale(0.75f);
    m_mainLayer->addChild(m_physRateLabel);

    // ---- Action buttons: Record / Play / Stop ----
    auto recordBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Record", "bigFont.fnt", "GJ_button_01.png", 0.8f),
        this, menu_selector(MacroLayer::onRecordBtn));
    recordBtn->setPosition({-90.f, 30.f});

    auto playBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Play", "bigFont.fnt", "GJ_button_01.png", 0.8f),
        this, menu_selector(MacroLayer::onPlayBtn));
    playBtn->setPosition({0.f, 30.f});

    auto stopBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Stop", "bigFont.fnt", "GJ_button_06.png", 0.8f),
        this, menu_selector(MacroLayer::onStopBtn));
    stopBtn->setPosition({90.f, 30.f});

    auto actionMenu = cocos2d::CCMenu::create();
    actionMenu->setPosition({cx, winSize.height - 140.f});
    actionMenu->addChild(recordBtn);
    actionMenu->addChild(playBtn);
    actionMenu->addChild(stopBtn);
    m_mainLayer->addChild(actionMenu);

    // ---- Save / Load buttons (uses fixed "macro.cbfm" filename) ----
    auto saveBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Save", "bigFont.fnt", "GJ_button_01.png", 0.7f),
        this, menu_selector(MacroLayer::onSaveBtn));
    saveBtn->setPosition({-45.f, 0.f});

    auto loadBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_01.png", 0.7f),
        this, menu_selector(MacroLayer::onLoadBtn));
    loadBtn->setPosition({45.f, 0.f});

    auto fileMenu = cocos2d::CCMenu::create();
    fileMenu->setPosition({cx, winSize.height - 185.f});
    fileMenu->addChild(saveBtn);
    fileMenu->addChild(loadBtn);
    m_mainLayer->addChild(fileMenu);

    // ---- Speed control: label + Speed- / Speed+ buttons ----
    m_speedLabel = cocos2d::CCLabelBMFont::create("Speed: 1.00x", "chatFont.fnt");
    m_speedLabel->setPosition({cx, winSize.height - 215.f});
    m_speedLabel->setScale(0.9f);
    m_mainLayer->addChild(m_speedLabel);

    auto speedDownBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Speed-", "bigFont.fnt", "GJ_button_05.png", 0.7f),
        this, menu_selector(MacroLayer::onSpeedDownBtn));
    speedDownBtn->setPosition({-60.f, 0.f});

    auto speedUpBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Speed+", "bigFont.fnt", "GJ_button_05.png", 0.7f),
        this, menu_selector(MacroLayer::onSpeedUpBtn));
    speedUpBtn->setPosition({60.f, 0.f});

    auto speedMenu = cocos2d::CCMenu::create();
    speedMenu->setPosition({cx, winSize.height - 240.f});
    speedMenu->addChild(speedDownBtn);
    speedMenu->addChild(speedUpBtn);
    m_mainLayer->addChild(speedMenu);

    // ---- Practice fix toggle ----
    m_practiceFixBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Practice Fix: ON", "bigFont.fnt", "GJ_button_01.png", 0.7f),
        this, menu_selector(MacroLayer::onPracticeFixToggle));
    m_practiceFixBtn->setPosition({0.f, 25.f});

    auto fixMenu = cocos2d::CCMenu::create();
    fixMenu->setPosition({cx, 55.f});
    fixMenu->addChild(m_practiceFixBtn);
    m_mainLayer->addChild(fixMenu);

    // ---- Close button ----
    auto closeBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_06.png", 0.8f),
        this, menu_selector(MacroLayer::onCloseBtn));
    closeBtn->setPosition({0.f, -10.f});

    auto closeMenu = cocos2d::CCMenu::create();
    closeMenu->setPosition({cx, 25.f});
    closeMenu->addChild(closeBtn);
    m_mainLayer->addChild(closeMenu);

    // Schedule UI refresh.
    this->schedule(schedule_selector(MacroLayer::refreshUI), 0.1f);

    refreshUI(0.f);
    return true;
}

void MacroLayer::refreshUI(float /*dt*/) {
    auto& bot = MacroBot::get();
    auto state = bot.state();

    const char* stateStr = "Idle";
    switch (state) {
        case MacroBot::State::Idle:          stateStr = "Idle"; break;
        case MacroBot::State::Recording:     stateStr = "Recording"; break;
        case MacroBot::State::Playing:       stateStr = "Playing"; break;
        case MacroBot::State::PlayingPending: stateStr = "Play (pending)"; break;
    }
    m_statusLabel->setString(fmt::format("Status: {}", stateStr).c_str());

    if (state == MacroBot::State::Recording) {
        auto s = bot.recordStats();
        m_statsLabel->setString(fmt::format(
            "{} events | tick {} | P1:{} P2:{}",
            s.eventCount, s.lastTick, s.p1Held ? "Y" : "N", s.p2Held ? "Y" : "N"
        ).c_str());
    } else if (state == MacroBot::State::Playing || state == MacroBot::State::PlayingPending) {
        auto s = bot.playStats();
        m_statsLabel->setString(fmt::format(
            "{}/{} events | tick {}/{}",
            s.eventsApplied, s.totalEvents, s.currentTick, s.totalTicks
        ).c_str());
    } else {
        m_statsLabel->setString("");
    }

    auto& cbf = CBFIntegration::get();
    bool syzzi = cbf.isSyzziCBFLoaded();
    bool builtin = cbf.isBuiltinCBFActive();
    m_cbfLabel->setString(fmt::format(
        "CBF: {} | tps: {:.0f}",
        syzzi ? "Syzzi+Built-in" : (builtin ? "Built-in (GD 2.2081)" : "per-frame"),
        cbf.getTicksPerSecond()
    ).c_str());

    double effRate = Speedhack::get().getEffectiveBaseRate();
    double maxRate = cbf.getMaxPhysicsRate();
    m_physRateLabel->setString(fmt::format(
        "Physics: {:.0f} Hz / {:.0f} Hz max",
        effRate, maxRate
    ).c_str());

    float speed = Speedhack::get().getSpeed();
    m_speedLabel->setString(fmt::format("Speed: {:.3f}x", speed).c_str());

    if (m_practiceFixBtn) {
        bool en = PracticeFix::get().isEnabled();
        auto sprite = ButtonSprite::create(
            en ? "Practice Fix: ON" : "Practice Fix: OFF",
            "bigFont.fnt",
            en ? "GJ_button_01.png" : "GJ_button_06.png",
            0.7f
        );
        m_practiceFixBtn->setSprite(sprite);
    }
}

void MacroLayer::onRecordBtn(CCObject*) {
    auto& bot = MacroBot::get();
    if (bot.isRecording()) bot.stopRecording();
    else                   bot.startRecording();
    refreshUI(0.f);
}

void MacroLayer::onPlayBtn(CCObject*) {
    MacroBot::get().playLastRecorded();
    refreshUI(0.f);
}

void MacroLayer::onStopBtn(CCObject*) {
    auto& bot = MacroBot::get();
    bot.stopRecording();
    bot.stopPlayback();
    refreshUI(0.f);
}

void MacroLayer::onSaveBtn(CCObject*) {
    if (MacroBot::get().saveLastRecorded("macro")) {
        FLAlertLayer::create("Saved", "Saved as macro.cbfm", "OK")->show();
    } else {
        FLAlertLayer::create("Error", "Nothing to save.", "OK")->show();
    }
    refreshUI(0.f);
}

void MacroLayer::onLoadBtn(CCObject*) {
    auto path = MacroBot::get().macroDir() / "macro.cbfm";
    if (MacroBot::get().playFromFile(path.string())) {
        FLAlertLayer::create("Loaded", "Loaded macro.cbfm", "OK")->show();
    } else {
        FLAlertLayer::create("Error", "Could not load macro.cbfm", "OK")->show();
    }
    refreshUI(0.f);
}

void MacroLayer::onSpeedDownBtn(CCObject*) {
    float speed = Speedhack::get().getSpeed();
    // Multiply by 0.75 for fine-grained control. Min 0.001x.
    speed = std::max(0.001f, speed * 0.75f);
    MacroBot::get().setSpeed(speed);
    refreshUI(0.f);
}

void MacroLayer::onSpeedUpBtn(CCObject*) {
    float speed = Speedhack::get().getSpeed();
    // Multiply by 1.33 for fine-grained control. Max 1000x.
    speed = std::min(1000.0f, speed * 1.33f);
    MacroBot::get().setSpeed(speed);
    refreshUI(0.f);
}

void MacroLayer::onPracticeFixToggle(CCObject*) {
    bool en = !PracticeFix::get().isEnabled();
    PracticeFix::get().setEnabled(en);
    refreshUI(0.f);
}

void MacroLayer::onCloseBtn(CCObject*) {
    this->removeFromParent();
}

void MacroLayer::keyBackClicked() {
    this->onCloseBtn(nullptr);
}

} // namespace cbf
