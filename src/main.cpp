/**
 * ============================================================================
 *  main.cpp  --  Rylan endpoint integrity monitor: mod entry point.
 *
 *  Wires the alert overlay into the running game. The overlay is spawned
 *  once sprite frames are available (MenuLayer::init) and raised the
 *  first time the operator's hotkey is pressed on any screen.
 * ============================================================================
 */

#include "Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
//  Keyboard hook: escalate the alert on the hotkey.
// ---------------------------------------------------------------------------
class $modify(IntegrityKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown,
                             bool isKeyRepeat, double timestamp) {
        auto ui = SecurityMonitor::get().ui;
        if (isKeyDown && !isKeyRepeat && key == bot::TOGGLE_KEY && ui) {
            ui->ensureInScene();
            ui->activate();
            return true;
        }
        // Once the alert is raised, all keyboard input is captured by the
        // overlay so gameplay does not proceed underneath.
        if (ui && ui->isActive()) return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown,
                                                         isKeyRepeat, timestamp);
    }
};

// ---------------------------------------------------------------------------
//  MenuLayer hook: instantiate the overlay once the main menu has loaded.
// ---------------------------------------------------------------------------
class $modify(IntegrityMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (!SecurityMonitor::get().ui) {
            if (auto ui = SecurityAlertLayer::create()) {
                ui->retain();
                if (auto scene = CCDirector::sharedDirector()->getRunningScene()) {
                    scene->addChild(ui, (std::numeric_limits<int>::max)());
                }
            }
        }
        return true;
    }
};

$on_mod(Loaded) {
    log::info("[Rylan] Endpoint integrity monitor initialised.");
}
