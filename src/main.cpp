#include "Bot.hpp"

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cmath>

using namespace geode::prelude;

static constexpr int GUI_TAG = 0xB07;

// stored from our keyboard hook so tickPlay can reach it
static CCKeyboardDispatcher* g_dispatcher = nullptr;

namespace mb {

// ═══════════════════════════════════════════════════════════
//  Macro — binary I/O
// ═══════════════════════════════════════════════════════════

static const char MB_MAGIC[4] = {'M','B','O','T'};
static constexpr int MB_VER = 1;

bool Macro::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    int32_t  lid = levelID;
    uint64_t cnt = actions.size();
    f.write(MB_MAGIC, 4);
    f.write(reinterpret_cast<const char*>(&MB_VER),  4);
    f.write(reinterpret_cast<const char*>(&lid),      4);
    f.write(reinterpret_cast<const char*>(&startOffset), 8);
    f.write(reinterpret_cast<const char*>(&cnt),      8);
    for (auto& a : actions) {
        f.write(reinterpret_cast<const char*>(&a.time), 8);
        uint8_t fl = (a.isP2 ? 1u : 0u)
                   | (static_cast<uint8_t>(a.type) << 1u);
        f.write(reinterpret_cast<const char*>(&fl), 1);
    }
    log::info("Saved {} actions, offset {:.6f}s, dur {:.6f}s",
              cnt, startOffset, duration);
    return true;
}

bool Macro::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char mg[4]; f.read(mg, 4);
    if (std::memcmp(mg, MB_MAGIC, 4) != 0) return false;
    int v; f.read(reinterpret_cast<char*>(&v), 4);
    if (v != MB_VER) return false;
    int32_t lid; f.read(reinterpret_cast<char*>(&lid), 4);
    levelID = lid;
    f.read(reinterpret_cast<char*>(&startOffset), 8);
    uint64_t cnt; f.read(reinterpret_cast<char*>(&cnt), 8);
    actions.clear(); actions.reserve(static_cast<size_t>(cnt));
    for (uint64_t i = 0; i < cnt; ++i) {
        InputAction a{};
        f.read(reinterpret_cast<char*>(&a.time), 8);
        uint8_t fl; f.read(reinterpret_cast<char*>(&fl), 1);
        a.isP2 = fl & 1u;
        a.type = static_cast<InputType>((fl >> 1u) & 1u);
        actions.push_back(a);
    }
    log::info("Loaded {} actions, offset {:.6f}s",
              actions.size(), startOffset);
    return true;
}

void Macro::truncateAfter(double t) {
    auto it = std::upper_bound(actions.begin(), actions.end(), t,
        [](double v, const InputAction& a){ return v < a.time; });
    actions.erase(it, actions.end());
}

void Macro::removeAttempt(uint16_t att) {
    actions.erase(std::remove_if(actions.begin(), actions.end(),
        [att](const InputAction& a){ return a.attempt == att; }),
        actions.end());
}

// ═══════════════════════════════════════════════════════════
//  PracticeFix
// ═══════════════════════════════════════════════════════════

PlayerSnap PracticeFix::capture(PlayerObject* p) {
    PlayerSnap s{};
    auto pos = p->getPosition();
    s.x = pos.x; s.y = pos.y;
    s.rot         = p->getRotation();
    s.rotSpd      = p->m_rotationSpeed;
    s.speed       = p->m_playerSpeed;
    s.speedMult   = p->m_speedMultiplier;
    s.gravity     = p->m_gravity;
    s.yVel        = p->m_yVelocity;
    s.xVel        = static_cast<float>(p->m_platformerXVelocity);
    s.vehicleSize = p->m_vehicleSize;
    s.onGround    = p->m_isOnGround;
    s.dashing     = p->m_isDashing;
    return s;
}

void PracticeFix::restore(PlayerObject* p, const PlayerSnap& s) {
    p->setPosition({s.x, s.y});
    p->setRotation(s.rot);
    p->m_rotationSpeed       = s.rotSpd;
    p->m_playerSpeed         = s.speed;
    p->m_speedMultiplier     = s.speedMult;
    p->m_gravity             = s.gravity;
    p->m_yVelocity           = s.yVel;
    p->m_platformerXVelocity = static_cast<double>(s.xVel);
    p->m_vehicleSize         = s.vehicleSize;
    p->m_isOnGround          = s.onGround;
    p->m_isDashing           = s.dashing;
}

void PracticeFix::sync(PlayLayer* layer, double gt) {
    auto* arr = layer->m_checkpointArray;
    if (!arr) return;
    auto cnt = static_cast<size_t>(arr->count());
    while (snaps.size() < cnt) {
        CheckpointSnap snap; snap.gameTime = gt;
        if (layer->m_player1) snap.p1 = capture(layer->m_player1);
        if (layer->m_player2) snap.p2 = capture(layer->m_player2);
        snaps.push_back(snap);
        MacroBot::get().chkTime = gt;
    }
    while (snaps.size() > cnt) {
        snaps.pop_back();
        MacroBot::get().chkTime =
            snaps.empty() ? 0.0 : snaps.back().gameTime;
    }
    syncedCount = cnt;
}

void PracticeFix::apply(PlayLayer* layer) {
    if (snaps.empty()) return;
    auto& s = snaps.back();
    if (layer->m_player1) restore(layer->m_player1, s.p1);
    if (layer->m_player2) restore(layer->m_player2, s.p2);
    MacroBot::get().gameTime = s.gameTime;
}

// ═══════════════════════════════════════════════════════════
//  Speedhack
// ═══════════════════════════════════════════════════════════

bool Speedhack::setText(const std::string& s) {
    try {
        float v = std::stof(s);
        if (v > 0.f && v <= 1000.f) {
            speed = v; buf = s; active = (v != 1.f);
            log::info("Speedhack set to {}", speed);
            return true;
        }
    } catch (...) {}
    return false;
}

// ═══════════════════════════════════════════════════════════
//  MacroBot
// ═══════════════════════════════════════════════════════════

MacroBot& MacroBot::get() { static MacroBot i; return i; }

void MacroBot::init() {
    detectCBF();
    log::info("MacroBot loaded — CBF: {}", static_cast<int>(cbf));
}

void MacroBot::detectCBF() {
    if (Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
        cbf = CBFStatus::Syzzi; return;
    }
    auto* gm = GameManager::sharedState();
    if (gm && gm->getGameVariable("0099")) {
        cbf = CBFStatus::RobTop; return;
    }
    cbf = CBFStatus::None;
}

void MacroBot::enterLevel(PlayLayer* l) {
    layer = l; inLevel = true;
    gameTime = 0.0; chkTime = 0.0;
    deadLast = false; attempt = 0; pauseFlag = false;
    practice = l->m_isPracticeMode;
    if (l->m_level) {
        levelID = l->m_level->m_levelID;
        macro.levelID = levelID;
    }
    fix.reset(); detectCBF();
    log::info("Entered level {}, practice={}", levelID, practice);
}

void MacroBot::exitLevel() {
    if (state == BotState::Recording) stopRec();
    if (state == BotState::Playing)   stopPlay();
    inLevel = false; practice = false; layer = nullptr;
    guiOpen = false;
    auto* sc = CCDirector::sharedDirector()->getRunningScene();
    if (sc) { auto* c = sc->getChildByTag(GUI_TAG);
              if (c) c->removeFromParent(); }
    fix.reset();
}

void MacroBot::onDeath() {
    if (state == BotState::Recording) {
        macro.truncateAfter(chkTime); ++attempt;
    }
    gameTime = chkTime; deadLast = true;
}

void MacroBot::onPauseRestart() {
    if (state == BotState::Recording) macro.clear();
    if (state == BotState::Playing)   playIdx = 0;
    gameTime = 0.0; chkTime = 0.0;
    attempt = 0; deadLast = false; fix.reset();
}

void MacroBot::startRec() {
    if (state == BotState::Playing) stopPlay();
    if (cbf == CBFStatus::None) return;
    macro.clear(); state = BotState::Recording;
    attempt = 0; chkTime = 0.0;
    log::info("Recording started");
}

void MacroBot::stopRec() {
    if (state != BotState::Recording) return;
    macro.duration = gameTime; state = BotState::Idle;
    log::info("Recording stopped: {} actions, {:.4f}s",
              macro.size(), macro.duration);
}

void MacroBot::recInput(bool isP2, InputType type) {
    if (state != BotState::Recording) return;
    // dedup (keyboard hook + PlayerObject hook fire for same click)
    if (!macro.actions.empty()) {
        auto& last = macro.actions.back();
        if (last.isP2 == isP2 && last.type == type &&
            std::abs(last.time - gameTime) < 1e-6)
            return;
    }
    if (macro.empty()) macro.startOffset = gameTime;
    macro.actions.push_back({gameTime, isP2, type, attempt});
}

void MacroBot::startPlay() {
    if (cbf == CBFStatus::None || macro.empty()) return;
    state = BotState::Playing; playIdx = 0;
    playReady = true; gameTime = 0.0;
    log::info("Playback started: {} actions", macro.size());
}

void MacroBot::stopPlay() {
    state = BotState::Idle; playReady = false; playIdx = 0;
    log::info("Playback stopped");
}

// FIX: playback through the keyboard dispatcher —
//      same pipeline CBF and real OS input use.
void MacroBot::tickPlay() {
    if (state != BotState::Playing || !playReady || !layer)
        return;

    while (playIdx < macro.actions.size()) {
        auto& a = macro.actions[playIdx];
        if (a.time > gameTime + 1e-9) break;

        if (g_dispatcher) {
            // P1 = Space, P2 = Left-Shift
            enumKeyCodes key = a.isP2
                ? enumKeyCodes::KEY_Shift
                : enumKeyCodes::KEY_Space;
            g_dispatcher->dispatchKeyboardMSG(
                key,
                a.type == InputType::Press,
                false,
                0.0);
        }
        ++playIdx;
    }
    if (playIdx >= macro.actions.size()) stopPlay();
}

void MacroBot::saveMacro() {
    auto dir = Mod::get()->getSaveDir() / "macros";
    std::filesystem::create_directories(dir);
    auto p = dir / ("level_" + std::to_string(macro.levelID)
                    + ".mbot");
    macro.save(p.string());
}

void MacroBot::loadMacro() {
    auto dir = Mod::get()->getSaveDir() / "macros";
    auto p = dir / ("level_" + std::to_string(levelID) + ".mbot");
    if (macro.load(p.string())) playReady = true;
}

// ═══════════════════════════════════════════════════════════
//  GUILayer  (toggle with K)
//  FIX: CCMenuItemFont instead of CCMenuItemLabel (iOS link)
//  FIX: Geode TextInput with setFilter (decimal support)
//  FIX: added to running scene so it sits above pause menu
//  FIX: scheduleUpdate so time/input labels refresh live
// ═══════════════════════════════════════════════════════════

class GUILayer : public CCLayer {
public:
    CCTextInputNode* speedInput = nullptr;
    CCLabelBMFont*   stateLbl   = nullptr;
    CCLabelBMFont*   infoLbl    = nullptr;
    CCLabelBMFont*   cbfDot     = nullptr;
    CCLabelBMFont*   cbfLbl     = nullptr;
    CCLabelBMFont*   warnLbl    = nullptr;

    static GUILayer* create() {
        auto* r = new GUILayer();
        if (r->init()) { r->autorelease(); return r; }
        delete r; return nullptr;
    }

    // helper: CCMenuItemFont works on all platforms including iOS
    CCMenuItemFont* addBtn(CCMenu* m, const char* t,
                           SEL_MenuHandler sel,
                           float bx, float by)
    {
        auto* it = CCMenuItemFont::create(t, this, sel);
        it->setFontSizeObj(20);
        it->setPosition({bx, by});
        m->addChild(it);
        return it;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        auto ws = CCDirector::sharedDirector()->getWinSize();
        constexpr float pw = 340.f, ph = 310.f;
        float px = (ws.width  - pw) * .5f;
        float py = (ws.height - ph) * .5f;
        auto& bot = MacroBot::get();

        // background
        auto* bg = CCLayerColor::create({0,0,0,210}, pw, ph);
        bg->setPosition({px, py}); addChild(bg, 0);
        auto* bt = CCLayerColor::create({90,90,90,255}, pw, 2);
        bt->setPosition({px, py+ph-2}); addChild(bt, 1);
        auto* bb = CCLayerColor::create({90,90,90,255}, pw, 2);
        bb->setPosition({px, py}); addChild(bb, 1);

        auto* menu = CCMenu::create();
        menu->setPosition({0,0}); addChild(menu, 2);

        auto* title = CCLabelBMFont::create("MacroBot","bigFont.fnt");
        title->setPosition({px+pw*.5f, py+ph-22});
        title->setScale(0.55f); addChild(title, 2);

        // close
        auto* xl = CCLabelBMFont::create("X","bigFont.fnt");
        xl->setScale(0.35f); xl->setColor({255,80,80});
        auto* xb = CCMenuItemLabel::create(
            xl, this, menu_selector(GUILayer::onClose));
        if (xb) {
            xb->setPosition({px+pw-18, py+ph-18});
            menu->addChild(xb);
        }

        // CBF indicator
        const char* cs = "No CBF";
        ccColor3B cc = {255,60,60};
        if (bot.cbf == CBFStatus::Syzzi)  { cs="Syzzi CBF";  cc={60,255,60};  }
        if (bot.cbf == CBFStatus::RobTop) { cs="RobTop CBS"; cc={255,255,60}; }
        float cy = py+ph-55;

        cbfDot = CCLabelBMFont::create(".","bigFont.fnt");
        cbfDot->setPosition({px+20,cy});
        cbfDot->setScale(2.f); cbfDot->setColor(cc);
        addChild(cbfDot, 2);

        cbfLbl = CCLabelBMFont::create(cs,"chatFont.fnt");
        cbfLbl->setAnchorPoint({0,.5f});
        cbfLbl->setPosition({px+38,cy});
        cbfLbl->setScale(0.65f); cbfLbl->setColor(cc);
        addChild(cbfLbl, 2);

        stateLbl = CCLabelBMFont::create("Idle","bigFont.fnt");
        stateLbl->setPosition({px+pw*.5f, py+ph-90});
        stateLbl->setScale(0.45f); addChild(stateLbl, 2);

        infoLbl = CCLabelBMFont::create(
            "Inputs: 0  Time: 0.00s","chatFont.fnt");
        infoLbl->setPosition({px+pw*.5f, py+ph-115});
        infoLbl->setScale(0.55f); addChild(infoLbl, 2);

        float r1y = py+ph-158, cx = px+pw*.5f;
        addBtn(menu,"Record",menu_selector(GUILayer::onRec), cx-100,r1y);
        addBtn(menu,"Stop",  menu_selector(GUILayer::onStop),cx,    r1y);
        addBtn(menu,"Play",  menu_selector(GUILayer::onPlay),cx+100,r1y);

        float r2y = r1y-40;
        addBtn(menu,"Save",menu_selector(GUILayer::onSave),cx-48,r2y);
        addBtn(menu,"Load",menu_selector(GUILayer::onLoad),cx+48,r2y);

        // speedhack row
        float sy = r2y-55;
        auto* sl = CCLabelBMFont::create("Speed:","chatFont.fnt");
        sl->setAnchorPoint({0,.5f});
        sl->setPosition({px+20,sy});
        sl->setScale(0.6f); addChild(sl, 2);

        // text input — allow digits and decimal point
        speedInput = CCTextInputNode::create(
            100.f, 24.f, "1.0", "chatFont.fnt");
        speedInput->setPosition({px+135, sy});
        speedInput->setString("1.0");
        speedInput->setAllowedChars("0123456789.");
        addChild(speedInput, 2);

        addBtn(menu,"Set",menu_selector(GUILayer::onSpdSet),px+240,sy);

        warnLbl = CCLabelBMFont::create(
            "CBF required — install Syzzi CBF","chatFont.fnt");
        warnLbl->setPosition({px+pw*.5f, py+22});
        warnLbl->setScale(0.5f); warnLbl->setColor({255,60,60});
        warnLbl->setVisible(bot.cbf == CBFStatus::None);
        addChild(warnLbl, 2);

        scheduleUpdate();
        return true;
    }

    void update(float) override { refresh(); }

    void refresh() {
        auto& bot = MacroBot::get();

        const char* cs = "No CBF";
        ccColor3B cc = {255,60,60};
        if (bot.cbf == CBFStatus::Syzzi)  { cs="Syzzi CBF";  cc={60,255,60}; }
        if (bot.cbf == CBFStatus::RobTop) { cs="RobTop CBS"; cc={255,255,60}; }
        cbfDot->setColor(cc);
        cbfLbl->setString(cs); cbfLbl->setColor(cc);

        const char* st = "Idle";
        ccColor3B sc = {255,255,255};
        if (bot.state==BotState::Recording){ st="Recording"; sc={255,80,80}; }
        if (bot.state==BotState::Playing)  { st="Playing";   sc={80,200,255}; }
        stateLbl->setString(st); stateLbl->setColor(sc);

        char ib[128];
        std::snprintf(ib, sizeof ib, "Inputs: %zu  Time: %.2fs",
                       bot.macro.size(), bot.gameTime);
        infoLbl->setString(ib);

        if (warnLbl) warnLbl->setVisible(bot.cbf == CBFStatus::None);
    }

    void onRec(CCObject*) {
        auto& b = MacroBot::get(); b.detectCBF();
        if (b.cbf==CBFStatus::None) return;
        b.startRec();
    }
    void onStop(CCObject*) {
        MacroBot::get().stopRec();
        MacroBot::get().stopPlay();
    }
    void onPlay(CCObject*) {
        auto& b = MacroBot::get(); b.detectCBF();
        if (b.cbf==CBFStatus::None) return;
        b.loadMacro();
        if (!b.macro.empty()) b.startPlay();
    }
    void onSave(CCObject*) { MacroBot::get().saveMacro(); }
    void onLoad(CCObject*) { MacroBot::get().loadMacro(); }
void onSpdSet(CCObject*) {
    if (!speedInput) return;
    std::string raw = speedInput->getString();
    if (!raw.empty())
        MacroBot::get().hack.setText(raw);
}
    void onClose(CCObject*) {
        MacroBot::get().guiOpen = false;
        removeFromParent();
    }
};

} // namespace mb

// ═══════════════════════════════════════════════════════════
//  Geode Hooks
// ═══════════════════════════════════════════════════════════

class $modify(MBPlayLayer, PlayLayer) {

    bool init(GJGameLevel* level, bool p1, bool p2) {
        if (!PlayLayer::init(level, p1, p2)) return false;
        mb::MacroBot::get().enterLevel(this);
        return true;
    }

    void update(float dt) {
        auto& bot = mb::MacroBot::get();
        if (!bot.inLevel) { PlayLayer::update(dt); return; }

        float modDt = dt;
        bot.hack.apply(modDt);

        if (bot.state == mb::BotState::Playing)
            bot.tickPlay();

        PlayLayer::update(modDt);

        bot.gameTime += modDt;

        bool dead = (m_player1 && m_player1->m_isDead)
                 || (m_player2 && m_player2->m_isDead);
        if (dead && !bot.deadLast) bot.onDeath();
        if (!dead)                 bot.deadLast = false;

        if (m_isPracticeMode)
            bot.fix.sync(this, bot.gameTime);
    }

    void resetLevel() {
        auto& bot = mb::MacroBot::get();
        if (bot.pauseFlag) {
            bot.onPauseRestart();
            bot.pauseFlag = false;
        }
        PlayLayer::resetLevel();
        if (m_isPracticeMode && bot.inLevel)
            bot.fix.apply(this);
    }

    void onQuit() {
        mb::MacroBot::get().exitLevel();
        PlayLayer::onQuit();
    }
};

class $modify(MBPlayerObj, PlayerObject) {
    bool pushButton(PlayerButton button) {
        auto& bot = mb::MacroBot::get();
        if (bot.state == mb::BotState::Recording
            && bot.inLevel && bot.layer)
        {
            bool p2 = (this == bot.layer->m_player2);
            bot.recInput(p2, mb::InputType::Press);
        }
        return PlayerObject::pushButton(button);
    }
    bool releaseButton(PlayerButton button) {
        auto& bot = mb::MacroBot::get();
        if (bot.state == mb::BotState::Recording
            && bot.inLevel && bot.layer)
        {
            bool p2 = (this == bot.layer->m_player2);
            bot.recInput(p2, mb::InputType::Release);
        }
        return PlayerObject::releaseButton(button);
    }
};

class $modify(MBPause, PauseLayer) {
    void onRestart(CCObject* sender) {
        mb::MacroBot::get().pauseFlag = true;
        PauseLayer::onRestart(sender);
    }
};

class $modify(MBKey, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key,
                             bool down, bool repeat, double d)
    {
        // stash the dispatcher so tickPlay can reach it
        g_dispatcher = this;

        auto& bot = mb::MacroBot::get();

        // K toggles GUI on the running scene (above everything)
        if (down && !repeat && key == enumKeyCodes::KEY_K
            && bot.inLevel)
        {
            bot.guiOpen = !bot.guiOpen;
            auto* scene = CCDirector::sharedDirector()
                              ->getRunningScene();
            if (scene) {
                if (bot.guiOpen) {
                    auto* ex = dynamic_cast<mb::GUILayer*>(
                        scene->getChildByTag(GUI_TAG));
                    if (!ex) {
                        auto* g = mb::GUILayer::create();
                        g->setTag(GUI_TAG);
                        scene->addChild(g, 999999);
                    } else {
                        ex->setVisible(true);
                        ex->refresh();
                    }
                } else {
                    auto* c = scene->getChildByTag(GUI_TAG);
                    if (c) c->removeFromParent();
                }
            }
        }

        // record jump keys at the dispatcher level
        if (bot.state == mb::BotState::Recording
            && bot.inLevel && !repeat)
        {
            if (key == enumKeyCodes::KEY_Space ||
                key == enumKeyCodes::KEY_Up    ||
                key == enumKeyCodes::KEY_W)
            {
                bot.recInput(false,
                    down ? mb::InputType::Press
                         : mb::InputType::Release);
            }
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(
            key, down, repeat, d);
    }
};

$on_mod(Loaded) { mb::MacroBot::get().init(); }