#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>

namespace cbf {

// ============================================================================
//  MacroLayer — Geode v5 Popup (non-templated)
// ----------------------------------------------------------------------------
//  v5 changes:
//    - Popup is non-templated. Override init() and call Popup::init(w, h).
//    - No more setup() virtual method.
//    - geode::Slider no longer exists — use buttons instead.
//    - Use <Geode/ui/GeodeUI.hpp> umbrella header for all UI components.
// ============================================================================
class MacroLayer : public geode::Popup {
protected:
    bool init() override;

    void onRecordBtn(cocos2d::CCObject*);
    void onPlayBtn(cocos2d::CCObject*);
    void onStopBtn(cocos2d::CCObject*);
    void onSaveBtn(cocos2d::CCObject*);
    void onLoadBtn(cocos2d::CCObject*);
    void onSpeedDownBtn(cocos2d::CCObject*);
    void onSpeedUpBtn(cocos2d::CCObject*);
    void onPracticeFixToggle(cocos2d::CCObject*);
    void onCloseBtn(cocos2d::CCObject*);

    void refreshUI(float dt);

    cocos2d::CCLabelBMFont* m_statusLabel    = nullptr;
    cocos2d::CCLabelBMFont* m_statsLabel     = nullptr;
    cocos2d::CCLabelBMFont* m_speedLabel     = nullptr;
    cocos2d::CCLabelBMFont* m_cbfLabel       = nullptr;
    cocos2d::CCLabelBMFont* m_physRateLabel  = nullptr;
    CCMenuItemSpriteExtra*  m_practiceFixBtn = nullptr;

public:
    static MacroLayer* create();
    static void toggle();
    void keyBackClicked() override;
};

} // namespace cbf
