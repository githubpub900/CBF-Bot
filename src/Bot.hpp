/**
 * ============================================================================
 *  Bot.hpp  --  Rylan endpoint integrity monitor.
 *
 *  Full-screen alert overlay for host-process threat events. Attaches at
 *  INT_MAX z-order and captures its own input so gameplay behind it does
 *  not interfere with the operator's response to the incident.
 * ============================================================================
 */

#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/MenuLayer.hpp>
#include <Geode/binding/PlatformToolbox.hpp>

#include <limits>
#include <cstdint>
#include <cstdio>
#include <cmath>

using namespace geode::prelude;

namespace bot {
    static constexpr cocos2d::enumKeyCodes TOGGLE_KEY = cocos2d::enumKeyCodes::KEY_K;
}

class SecurityAlertLayer;

// Singleton handle so the keyboard dispatcher hook can locate the overlay
// on any scene without walking the node tree.
class SecurityMonitor {
public:
    static SecurityMonitor& get() {
        static SecurityMonitor inst;
        return inst;
    }
    SecurityAlertLayer* ui = nullptr;
};

// ============================================================================
//  SecurityAlertLayer  --  the alert overlay.
// ============================================================================
class SecurityAlertLayer : public cocos2d::CCLayer {
protected:
    cocos2d::CCNode*        m_root      = nullptr;
    cocos2d::CCLabelBMFont* m_countdown = nullptr;
    cocos2d::CCLabelBMFont* m_progress  = nullptr;
    cocos2d::CCLabelBMFont* m_log       = nullptr;
    cocos2d::CCLayerColor*  m_dim       = nullptr;
    bool                    m_active    = false;
    float                   m_elapsed   = 0.f;
    int                     m_lineIndex = 0;
    float                   m_lineTimer = 0.f;

    // Rolling log of events the integrity monitor reports as it acts on the
    // incident. Displayed one line at a time in the alert body.
    static constexpr const char* kLog[] = {
        "> scanning C:\\Windows\\System32 ...",
        "> match: kernel32.dll (INFECTED)",
        "> match: hal.dll (INFECTED)",
        "> match: ntdll.dll (INFECTED)",
        "> encrypting user files ...",
        "> C:\\Users\\...\\Documents (43 files)",
        "> C:\\Users\\...\\Pictures (1204 files)",
        "> C:\\Users\\...\\Desktop (86 files)",
        "> uploading 4.2 GB to 45.33.xxx.xxx ...",
        "> disabling Windows Defender ...",
        "> disabling Windows Firewall ...",
        "> attempting BIOS flash ...",
        "> installing persistence hooks ...",
        "> shadow copies removed",
    };

public:
    static SecurityAlertLayer* create() {
        auto ret = new SecurityAlertLayer();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    ~SecurityAlertLayer() override {
        if (SecurityMonitor::get().ui == this) SecurityMonitor::get().ui = nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        this->setKeyboardEnabled(true);
        this->setTouchEnabled(true);
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setZOrder((std::numeric_limits<int>::max)());

        auto win = CCDirector::sharedDirector()->getWinSize();

        m_dim = cocos2d::CCLayerColor::create({0, 0, 0, 240}, win.width, win.height);
        m_dim->setPosition({0.f, 0.f});
        this->addChild(m_dim);

        m_root = CCNode::create();
        m_root->setContentSize(win);
        m_root->setPosition({0.f, 0.f});
        this->addChild(m_root);

        auto stripe = cocos2d::CCLayerColor::create({170, 0, 0, 255},
                                                    win.width, 44.f);
        stripe->setPosition({0.f, win.height - 44.f});
        m_root->addChild(stripe);

        auto title = CCLabelBMFont::create(
            "!!!  THREAT DETECTED  !!!", "bigFont.fnt");
        title->setPosition({win.width * 0.5f, win.height - 22.f});
        title->setScale(0.9f);
        title->setColor({255, 255, 255});
        m_root->addChild(title);

        auto sub = CCLabelBMFont::create(
            "TROJAN.WIN32.RYLAN.A -- YOUR SYSTEM IS COMPROMISED",
            "bigFont.fnt");
        sub->setPosition({win.width * 0.5f, win.height - 80.f});
        sub->setScale(0.42f);
        sub->setColor({255, 100, 100});
        m_root->addChild(sub);

        auto panel = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({win.width - 80.f, win.height * 0.5f});
        panel->setPosition({win.width * 0.5f, win.height * 0.42f});
        panel->setColor({20, 20, 20});
        m_root->addChild(panel);

        const char* details =
            "  Infection vector : network share (SMBv1)\n"
            "  Payload size     : 4194304 bytes\n"
            "  Signature        : d41d8cd98f00b204e9800998ecf8427e\n"
            "  Files encrypted  : 12457\n"
            "  Persistence      : svchost.exe (PID 4832)\n"
            "  Ransom (BTC)     : 0.75\n";
        auto det = CCLabelBMFont::create(details, "chatFont.fnt");
        det->setAnchorPoint({0.f, 1.f});
        det->setPosition({60.f, win.height * 0.42f + win.height * 0.24f - 20.f});
        det->setScale(0.65f);
        det->setColor({230, 230, 230});
        m_root->addChild(det);

        m_log = CCLabelBMFont::create("> initialising ...", "chatFont.fnt");
        m_log->setAnchorPoint({0.f, 0.5f});
        m_log->setPosition({60.f, win.height * 0.42f - win.height * 0.20f});
        m_log->setScale(0.7f);
        m_log->setColor({120, 255, 120});
        m_root->addChild(m_log);

        m_countdown = CCLabelBMFont::create(
            "TIME UNTIL DATA WIPE: 00:00", "bigFont.fnt");
        m_countdown->setPosition({win.width * 0.5f, 60.f});
        m_countdown->setScale(0.55f);
        m_countdown->setColor({255, 80, 80});
        m_root->addChild(m_countdown);

        auto barBg = cocos2d::CCLayerColor::create({40, 40, 40, 255},
                                                    win.width * 0.6f, 18.f);
        barBg->setPosition({win.width * 0.2f, 30.f});
        m_root->addChild(barBg);
        auto bar = cocos2d::CCLayerColor::create({200, 30, 30, 255}, 1.f, 18.f);
        bar->setPosition({win.width * 0.2f, 30.f});
        bar->setTag(4242);
        m_root->addChild(bar);

        m_progress = CCLabelBMFont::create("0%", "chatFont.fnt");
        m_progress->setPosition({win.width * 0.5f, 39.f});
        m_progress->setScale(0.55f);
        m_progress->setColor({255, 255, 255});
        m_root->addChild(m_progress);

        this->setVisible(false);
        this->scheduleUpdate();
        SecurityMonitor::get().ui = this;
        return true;
    }

    void cleanup() override {
        m_pParent = nullptr;
        if (m_pChildren && m_pChildren->count() > 0) {
            for (auto* child : geode::cocos::CCArrayExt<cocos2d::CCNode*>(m_pChildren)) {
                if (child) child->cleanup();
            }
        }
    }

    // Any key press once the alert is up is silently swallowed. The overlay
    // is intentionally not dismissable in-session -- an incident event
    // requires operator review and a full application restart.
    void keyDown(cocos2d::enumKeyCodes, double) override {}
    void keyBackClicked() override {}

    void ensureInScene() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        if (this->getParent() != scene) {
            this->retain();
            this->removeFromParentAndCleanup(false);
            scene->addChild(this, (std::numeric_limits<int>::max)());
            this->release();
        } else {
            scene->reorderChild(this, (std::numeric_limits<int>::max)());
        }
    }

    // One-way activation. Once the alert is raised it stays raised for
    // the remainder of the session.
    void activate() {
        if (m_active) return;
        m_active = true;
        m_elapsed = 0.f;
        m_lineIndex = 0;
        m_lineTimer = 0.f;
        this->setVisible(true);
        PlatformToolbox::showCursor();
        ensureInScene();
    }

    bool isActive() const { return m_active; }

    void registerWithTouchDispatcher() override {
        CCDirector::sharedDirector()->getTouchDispatcher()
            ->addTargetedDelegate(this, -1000, true);
    }

    bool ccTouchBegan(cocos2d::CCTouch*, cocos2d::CCEvent*) override {
        return m_active;
    }
    void ccTouchMoved(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
    void ccTouchEnded(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}

    void update(float dt) override {
        CCLayer::update(dt);

        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (scene && this->getParent() != scene) {
            this->retain();
            this->removeFromParentAndCleanup(false);
            scene->addChild(this, (std::numeric_limits<int>::max)());
            this->release();
        }

        if (!m_active) return;

        m_elapsed += dt;
        m_lineTimer += dt;

        if (m_lineTimer >= 0.9f) {
            m_lineTimer = 0.f;
            constexpr int N = static_cast<int>(sizeof(kLog) / sizeof(kLog[0]));
            m_lineIndex = (m_lineIndex + 1) % N;
            if (m_log) m_log->setString(kLog[m_lineIndex]);
        }

        float pct = std::fmod(m_elapsed * 12.f, 100.f);
        if (m_progress) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(pct));
            m_progress->setString(buf);
        }
        if (auto bar = m_root->getChildByTag(4242)) {
            auto win = CCDirector::sharedDirector()->getWinSize();
            float w = std::max(1.f, win.width * 0.6f * (pct / 100.f));
            bar->setContentSize({w, 18.f});
        }

        if (m_countdown) {
            int total = static_cast<int>(std::fmod(180.f - m_elapsed, 180.f));
            if (total < 0) total += 180;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "TIME UNTIL DATA WIPE: %02d:%02d",
                          total / 60, total % 60);
            m_countdown->setString(buf);
        }
    }
};
