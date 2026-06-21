#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Slider.hpp>

namespace cbf {

// ============================================================================
//  MacroLayer
// ----------------------------------------------------------------------------
//  In-game popup UI for controlling the macro bot. Opened from the pause
//  menu ("Macro Bot" button) or via the F8 key (registered separately).
//
//  Layout (top to bottom):
//    - Status label (Idle / Recording / Playing)
//    - Stats label (events, ticks)
//    - [Record] [Play] [Stop]  buttons
//    - [Save] [Load]           buttons + filename input
//    - Speedhack slider (0.1x .. 10x) + value label
//    - [Practice Fix] toggle
//    - Speedhack slider (0.1x .. 10x) + value label
//    - [Practice Fix] toggle
//    - CBF status indicator
// ============================================================================
class MacroLayer : public geode::Popup<> {
protected:
    bool setup() override;

    // Button handlers
    void onRecordBtn(cocos2d::CCObject*);
    void onPlayBtn(cocos2d::CCObject*);
    void onStopBtn(cocos2d::CCObject*);
    void onSaveBtn(cocos2d::CCObject*);
    void onLoadBtn(cocos2d::CCObject*);
    void onSpeedSlider(cocos2d::CCObject*);
    void onPracticeFixToggle(cocos2d::CCObject*);
    void onCloseBtn(cocos2d::CCObject*);

    // Refreshes the status / stats labels. Called on every state change
    // and also on a periodic scheduler tick so the UI stays live while
    // recording/playing.
    void refreshUI(float dt);

    // UI elements we need to update dynamically.
    cocos2d::CCLabelBMFont* m_statusLabel  = nullptr;
    cocos2d::CCLabelBMFont* m_statsLabel   = nullptr;
    cocos2d::CCLabelBMFont* m_speedLabel   = nullptr;
    cocos2d::CCLabelBMFont* m_cbfLabel     = nullptr;
    geode::Slider*          m_speedSlider  = nullptr;
    geode::TextInput*       m_filenameInput = nullptr;
    CCMenuItemSpriteExtra*  m_practiceFixBtn = nullptr;

public:
    static MacroLayer* create();
    static void toggle();
    void keyBackClicked() override { this->onCloseBtn(nullptr); }
};

} // namespace cbf
