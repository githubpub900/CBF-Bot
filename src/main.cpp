// main.cpp
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include "Bot.hpp"

using namespace geode::prelude;

// =========================================================================
// Hooks – all signatures match GD 2.2081 / Geode v5.7.1 bindings
// =========================================================================

class PlayLayerHook : public Modify<PlayLayerHook, PlayLayer> {
public:
    // storeCheckpoint now takes a CheckpointObject* that it fills.
    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        Bot::checkpointStored(checkpoint);
    }

    // There is no public loadCheckpoint() in the binding.  Manual checkpoint
    // selection triggers checkpointActivated(); automatic death‑reload goes
    // through a hidden internal function we cannot hook.  We therefore rely
    // on the game’s checkpoint being complete – GD 2.2 checkpoints already
    // save all gamemode state.  No extra restoration is needed.
    void checkpointActivated(CheckpointObject* checkpoint) {
        PlayLayer::checkpointActivated(checkpoint);
        // If extra state restoration were required, it would happen here.
    }

    // Update: manual step‑based playback
    void update(float dt) {
        auto* pl = static_cast<PlayLayer*>(this);
        if (Bot::isPlaying()) {
            float step = Bot::m_playBaseStep * Bot::getSpeedhack();
            float remaining = dt;
            while (remaining > 0.000001f) {
                float take = std::min(step, remaining);
                Bot::processPlaybackStep(pl, take);
                remaining -= take;
            }
            return;
        }
        PlayLayer::update(dt);
    }

    // Input recording – handleButton is the single entry point for both CBS and CBF.
    void handleButton(bool down, int button, bool player1) {
        PlayLayer::handleButton(down, button, player1);
        if (Bot::isRecording()) {
            double time = m_gameState.m_levelTime;
            Bot::recordInput(time, down, button);
        }
    }

    // Dead‑input cleanup
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        Bot::onDeath();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::onRestart();
    }
};

// Speedhack: multiply non‑playback frames.  Playback uses its own scaling.
class SchedulerHook : public Modify<SchedulerHook, CCScheduler> {
public:
    void update(float dt) {
        if (!Bot::isPlaying())
            dt *= Bot::getSpeedhack();
        CCScheduler::update(dt);
    }
};

// K‑key toggle – signature now includes a double for timestamp.
class KeyboardHook : public Modify<KeyboardHook, CCKeyboardDispatcher> {
public:
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && key == enumKeyCodes::KEY_K) {
            auto* pl = GameManager::sharedState()->getPlayLayer();
            if (pl) Bot::toggleUI(pl);
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// =========================================================================
// Bot implementation
// =========================================================================

void Bot::startRecording() {
    m_events.clear();
    m_checkpointEventIndices.clear();
    m_recording = true;
}
void Bot::stopRecording() { m_recording = false; }
bool Bot::isRecording() { return m_recording; }

void Bot::startPlayback() {
    if (getCBSMode() == CBSMode::None) return;
    m_playing = true;
    m_playIndex = 0;
    m_playAccum = 0.0;
}
void Bot::stopPlayback() { m_playing = false; }
bool Bot::isPlaying() { return m_playing; }

void Bot::recordInput(double time, bool down, int button) {
    m_events.push_back({time, down, button});
}

// ---------- Dead‑input handling ----------
void Bot::onDeath() {
    if (!m_recording) return;
    if (!m_checkpointEventIndices.empty()) {
        size_t idx = m_checkpointEventIndices.back();
        if (m_events.size() > idx)
            m_events.resize(idx);
    }
}

void Bot::onRestart() {
    if (!m_recording) return;
    m_events.clear();
    m_checkpointEventIndices.clear();
}

// ---------- Checkpoint (only recording index is stored) ----------
void Bot::checkpointStored(CheckpointObject* cp) {
    if (m_recording)
        m_checkpointEventIndices.push_back(m_events.size());
    // GD 2.2's CheckpointObject already captures all necessary player
    // state (dual, gravity, gamemode rotations, size, speed) – no extra
    // fix is required.
}

// ---------- Speedhack ----------
float Bot::getSpeedhack() { return m_speed; }
void Bot::setSpeedhack(float s) { m_speed = std::clamp(s, 0.1f, 10.0f); }
void Bot::speedhackUp() { setSpeedhack(m_speed + 0.1f); }
void Bot::speedhackDown() { setSpeedhack(m_speed - 0.1f); }

// ---------- CBS detection ----------
Bot::CBSMode Bot::getCBSMode() {
    // Syzzi CBF is the preferred high‑precision option.
    if (Loader::get()->isModLoaded("syzzi.click_between_frames"))
        return CBSMode::Syzzi;
    if (GameManager::sharedState()->getGameVariable("0050"))
        return CBSMode::RobTop;
    return CBSMode::None;
}

// ---------- Playback helpers ----------
void Bot::injectInput(const InputEvent& ev) {
    auto* pl = GameManager::sharedState()->getPlayLayer();
    if (!pl) return;
    pl->handleButton(ev.down, ev.button, true);
}

void Bot::processPlaybackStep(PlayLayer* pl, float customDt) {
    // Temporarily disable the playback flag so that the real update()
    // does not try to call processPlaybackStep again.
    m_playing = false;
    pl->PlayLayer::update(customDt);
    m_playing = true;

    m_playAccum += customDt;
    double curTime = pl->m_gameState.m_levelTime;

    while (m_playIndex < m_events.size()) {
        const InputEvent& ev = m_events[m_playIndex];
        if (ev.time <= curTime + 1e-9) { // tiny epsilon
            injectInput(ev);
            ++m_playIndex;
        } else {
            break;
        }
    }
}

// ---------- Macro I/O (delta‑encoded binary) ----------
void Bot::saveMacro(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const char magic[4] = {'G','D','M','B'};
    f.write(magic, 4);
    uint32_t count = m_events.size();
    f.write(reinterpret_cast<const char*>(&count), 4);
    double prev = 0.0;
    for (const auto& ev : m_events) {
        double delta = ev.time - prev;
        f.write(reinterpret_cast<const char*>(&delta), 8);
        uint8_t flags = (ev.down ? 1 : 0) | ((ev.button & 0xF) << 1);
        f.write(reinterpret_cast<const char*>(&flags), 1);
        prev = ev.time;
    }
}

void Bot::loadMacro(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    char magic[4];
    f.read(magic, 4);
    if (std::string(magic,4) != "GDMB") return;
    uint32_t count;
    f.read(reinterpret_cast<char*>(&count), 4);
    m_events.clear();
    m_events.reserve(count);
    double prev = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        double delta;
        f.read(reinterpret_cast<char*>(&delta), 8);
        uint8_t flags;
        f.read(reinterpret_cast<char*>(&flags), 1);
        InputEvent ev;
        ev.time = prev + delta;
        ev.down = flags & 1;
        ev.button = (flags >> 1) & 0xF;
        m_events.push_back(ev);
        prev = ev.time;
    }
}

// ---------- UI ----------
void Bot::toggleUI(PlayLayer* pl) {
    if (m_uiVisible) {
        if (m_uiNode) m_uiNode->removeFromParent();
        m_uiVisible = false;
        return;
    }

    auto winSize = CCDirector::sharedDirector()->getWinSize();
    auto* bg = CCLayerColor::create(ccc4(0,0,0,180));
    bg->setContentSize(CCSizeMake(200, 240));
    bg->setPosition(ccp(winSize.width/2 - 100, winSize.height/2 - 120));
    pl->addChild(bg, 1000);

    // Status label
    auto status = Bot::getCBSMode();
    const char* txt;
    ccColor3B col;
    switch (status) {
        case CBSMode::Syzzi: txt = "CBF: Syzzi (Green)"; col = ccc3(0,255,0); break;
        case CBSMode::RobTop: txt = "CBS: RobTop (Yellow)"; col = ccc3(255,255,0); break;
        default:              txt = "No CBF/CBS (Red)";     col = ccc3(255,0,0); break;
    }
    auto* label = CCLabelBMFont::create(txt, "bigFont.fnt");
    label->setColor(col);
    label->setPosition(ccp(100, 200));
    bg->addChild(label);

    // Speed display (static for simplicity)
    auto* speedLabel = CCLabelBMFont::create(
        ("Speed: " + std::to_string(m_speed).substr(0,4)).c_str(),
        "bigFont.fnt");
    speedLabel->setPosition(ccp(100, -130));
    bg->addChild(speedLabel);

    auto* menu = CCMenu::create();
    menu->setPosition(ccp(100, 180));

    auto addBtn = [&](const char* title, SEL_MenuHandler cb, float y) {
        auto* btn = CCMenuItemLabel::create(CCLabelBMFont::create(title, "bigFont.fnt"), nullptr, cb);
        btn->setPosition(ccp(0, y));
        menu->addChild(btn);
    };

    addBtn("Record", menu_selector(Bot::onRecordButton),  30);
    addBtn("Play",   menu_selector(Bot::onPlayButton),   -10);
    addBtn("Save",   menu_selector(Bot::onSaveButton),   -50);
    addBtn("Load",   menu_selector(Bot::onLoadButton),   -90);

    bg->addChild(menu);
    m_uiNode = bg;
    m_uiVisible = true;
}

// UI callbacks
void Bot::onRecordButton(CCObject*) {
    if (isRecording()) stopRecording();
    else startRecording();
}
void Bot::onPlayButton(CCObject*) {
    if (isPlaying()) stopPlayback();
    else startPlayback();
}
void Bot::onSaveButton(CCObject*) {
    std::string path = CCFileUtils::sharedFileUtils()->getWritablePath() + "macro.gdmb";
    saveMacro(path);
}
void Bot::onLoadButton(CCObject*) {
    std::string path = CCFileUtils::sharedFileUtils()->getWritablePath() + "macro.gdmb";
    loadMacro(path);
}