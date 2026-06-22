#include "MacroLayer.hpp"
#include "../MacroBot.hpp"
#include "../CBFIntegration.hpp"
#include "../Speedhack.hpp"
#include "../PracticeFix.hpp"

#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Slider.hpp>
#include <Geode/ui/GeodeUI.hpp>

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
    // Geode v5: Popup::init(width, height) replaces initAnchored.
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

    // ---- Filename input + Save/Load ----
    m_filenameInput = geode::TextInput::create(180.f, "macro filename");
    m_filenameInput->setPosition({cx - 50.f, winSize.height - 190.f});
    m_filenameInput->setScale(0.9f);
    m_mainLayer->addChild(m_filenameInput);

    auto saveBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Save", "bigFont.fnt", "GJ_button_01.png", 0.7f),
        this, menu_selector(MacroLayer::onSaveBtn));
    saveBtn->setPosition({50.f, -5.f});

    auto loadBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Load", "bigFont.fnt", "GJ_button_01.png", 0.7f),
        this, menu_selector(MacroLayer::onLoadBtn));
    loadBtn->setPosition({50.f, -35.f});

    auto fileMenu = cocos2d::CCMenu::create();
    fileMenu->setPosition({cx + 90.f, winSize.height - 185.f});
    fileMenu->addChild(saveBtn);
    fileMenu->addChild(loadBtn);
    m_mainLayer->addChild(fileMenu);

    // ---- Speedhack slider ----
    m_speedLabel = cocos2d::CCLabelBMFont::create("Speed: 1.00x", "chatFont.fnt");
    m_speedLabel->setPosition({cx, winSize.height - 225.f});
    m_speedLabel->setScale(0.9f);
    m_mainLayer->addChild(m_speedLabel);

    m_speedSlider = geode::Slider::create(
        this, menu_selector(MacroLayer::onSpeedSlider), 0.5f);
    m_speedSlider->setPosition({cx, winSize.height - 245.f});
    m_speedSlider->setScale(0.8f);
    m_mainLayer->addChild(m_speedSlider);

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

    // Show the effective physics rate (changes with speed for slow-mo).
    double effRate = Speedhack::get().getEffectiveBaseRate();
    double maxRate = cbf.getMaxPhysicsRate();
    m_physRateLabel->setString(fmt::format(
        "Physics: {:.0f} Hz / {:.0f} Hz max",
        effRate, maxRate
    ).c_str());

    // Update practice fix button label.
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
    std::string name = m_filenameInput->getString();
    if (name.empty()) name = "macro";
    MacroBot::get().saveLastRecorded(name);
    FLAlertLayer::create("Saved", fmt::format("Saved as {}.cbfm", name).c_str(), "OK")->show();
    refreshUI(0.f);
}

void MacroLayer::onLoadBtn(CCObject*) {
    std::string name = m_filenameInput->getString();
    if (name.empty()) {
        FLAlertLayer::create("Load", "Enter a filename first.", "OK")->show();
        return;
    }
    auto path = MacroBot::get().macroDir() / (name + ".cbfm");
    if (MacroBot::get().playFromFile(path.string())) {
        FLAlertLayer::create("Loaded", fmt::format("Loaded {}.cbfm", name).c_str(), "OK")->show();
    } else {
        FLAlertLayer::create("Error", fmt::format("Could not load {}.cbfm", name).c_str(), "OK")->show();
    }
    refreshUI(0.f);
}

void MacroLayer::onSpeedSlider(CCObject*) {
    // Slider value is 0..1; map to 0.001x .. 100x logarithmically.
    float t = m_speedSlider->getValue();
    // Map [0,1] -> [0.001, 100] via log scale: speed = 0.001 * 100000^t
    float speed = 0.001f * std::pow(100000.0f, t);
    MacroBot::get().setSpeed(speed);
    m_speedLabel->setString(fmt::format("Speed: {:.3f}x", speed).c_str());
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

} // namespace cbf
