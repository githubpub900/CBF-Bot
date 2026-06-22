#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>

namespace cbf {

// ============================================================================
//  MacroLayer — v5 Popup (non-templated)
// ----------------------------------------------------------------------------
//  In Geode v5, Popup is no longer templated. Use Popup::init(width, height)
//  instead of initAnchored. The create() pattern stays the same.
// ============================================================================
class MacroLayer : public geode::Popup {
protected:
    bool init();
    bool setup() override { return true; } // setup() is called by init(); we do everything in init()

    void onRecordBtn(cocos2d::CCObject*);
    void onPlayBtn(cocos2d::CCObject*);
    void onStopBtn(cocos2d::CCObject*);
    void onSaveBtn(cocos2d::CCObject*);
    void onLoadBtn(cocos2d::CCObject*);
    void onSpeedSlider(cocos2d::CCObject*);
    void onPracticeFixToggle(cocos2d::CCObject*);
    void onCloseBtn(cocos2d::CCObject*);

    void refreshUI(float dt);

    cocos2d::CCLabelBMFont* m_statusLabel    = nullptr;
    cocos2d::CCLabelBMFont* m_statsLabel     = nullptr;
    cocos2d::CCLabelBMFont* m_speedLabel     = nullptr;
    cocos2d::CCLabelBMFont* m_cbfLabel       = nullptr;
    cocos2d::CCLabelBMFont* m_physRateLabel  = nullptr;
    geode::Slider*          m_speedSlider    = nullptr;
    geode::TextInput*       m_filenameInput  = nullptr;
    CCMenuItemSpriteExtra*  m_practiceFixBtn = nullptr;

public:
    static MacroLayer* create();
    static void toggle();
    void keyBackClicked() override { this->onCloseBtn(nullptr); }
};

} // namespace cbf
