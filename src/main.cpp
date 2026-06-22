// main.cpp
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include "Bot.hpp"

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

class PlayLayerHook : public Modify<PlayLayerHook, PlayLayer> {
public:
    // ---- Initialisation ----
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        // Ensure CBS mode check is fresh
        Bot::getCBSMode();
        return true;
    }

    // ---- Update: manual step‑based playback ----
    void update(float dt) {
        auto* pl = static_cast<PlayLayer*>(this);
        if (Bot::isPlaying()) {
            // Speedhack scales the base step size.
            float step = Bot::m_playBaseStep * Bot::getSpeedhack();
            // Remaining time we need to simulate in this frame
            float remaining = dt;
            while (remaining > 0.000001f) {
                float take = std::min(step, remaining);
                Bot::processPlaybackStep(pl, take);
                remaining -= take;
            }
            // The game expects the frame to be “done”; we do NOT call the
            // original update because we already advanced physics manually.
            return;
        }

        // Normal operation (recording or neither)
        if (Bot::isRecording()) {
            // The original update will call handleButton and our recordInput
            // will capture events automatically via the handleButton hook below.
        }
        PlayLayer::update(dt);
    }

    // ---- Input recording ----
    void handleButton(bool down, int button, bool player1) {
        // Always call original first so flags are set correctly.
        PlayLayer::handleButton(down, button, player1);
        if (Bot::isRecording()) {
            // m_gameState.m_levelTime is the absolute game time.
            double time = m_gameState.m_levelTime;
            Bot::recordInput(time, down, button);
        }
    }

    // ---- Checkpoint store/load (bug fix) ----
    CheckpointObject* storeCheckpoint() {
        auto* cp = PlayLayer::storeCheckpoint();
        Bot::checkpointStored(cp);
        return cp;
    }

    void loadCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadCheckpoint(cp);
        Bot::checkpointLoaded(cp);
    }

    // ---- Death / Restart ----
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        Bot::onDeath();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::onRestart();
    }
};

// Scheduler hook for speedhack (only when NOT in playback – playback
// uses manual stepping and already applies speedhack internally)
class SchedulerHook : public Modify<SchedulerHook, CCScheduler> {
public:
    void update(float dt) {
        if (!Bot::isPlaying()) {
            dt *= Bot::getSpeedhack();
        }
        CCScheduler::update(dt);
    }
};

// Keyboard hook for K
class KeyboardHook : public Modify<KeyboardHook, CCKeyboardDispatcher> {
public:
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool alt) {
        if (down && key == enumKeyCodes::KEY_K) {
            auto* pl = GameManager::sharedState()->getPlayLayer();
            if (pl) Bot::toggleUI(pl);
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, alt);
    }
};

// ---------------------------------------------------------------------------
// Bot implementation
// ---------------------------------------------------------------------------

void Bot::startRecording() {
    m_events.clear();
    m_checkpointEventIndices.clear();
    m_recording = true;
}

void Bot::stopRecording() {
    m_recording = false;
}

bool Bot::isRecording() { return m_recording; }

void Bot::startPlayback() {
    if (getCBSMode() == CBSMode::None) return; // disabled
    m_playing = true;
    m_playIndex = 0;
    m_playAccum = 0.0;
    m_didInjectThisStep = false;
}

void Bot::stopPlayback() { m_playing = false; }
bool Bot::isPlaying() { return m_playing; }

void Bot::recordInput(double time, bool down, int button) {
    m_events.push_back({time, down, button});
}

void Bot::onDeath() {
    if (!m_recording) return;
    // Truncate events after last checkpoint.
    if (!m_checkpointEventIndices.empty()) {
        size_t lastIdx = m_checkpointEventIndices.back();
        if (m_events.size() > lastIdx)
            m_events.resize(lastIdx);
    } else {
        // No checkpoint yet – keep everything (player hasn't progressed)
    }
}

void Bot::onRestart() {
    if (!m_recording) return;
    m_events.clear();
    m_checkpointEventIndices.clear();
}

void Bot::checkpointStored(CheckpointObject* cp) {
    if (!m_recording && !m_playing) return;

    if (m_recording) {
        m_checkpointEventIndices.push_back(m_events.size());
    }

    // ----- Save extra state -----
    auto* pl = GameManager::sharedState()->getPlayLayer();
    if (!pl) return;
    ExtraCPData extra{};
    auto* p1 = pl->m_player1;
    auto* p2 = pl->m_player2;
    if (p1) {
        extra.dual = p1->m_isDual;
        extra.gravityFlipped = p1->m_isUpsideDown;
        extra.sizeMod = p1->m_vehicleSize;
        extra.speedPortal = p1->m_playerSpeed; // approximate, adjust if needed
        // Rotations – these are the fields used by each gamemode
        extra.shipRotation = p1->m_rotation;
        extra.ballRotation = p1->m_ballRotation;
        extra.ufoRotation = p1->m_ufoRotation;
        extra.waveRotation = p1->m_waveRotation;
        extra.robotRotation = p1->m_robotRotation;
        extra.spiderRotation = p1->m_spiderRotation;
        extra.swingRotation = p1->m_swingRotation;
    }
    m_cpExtra[cp] = extra;
}

void Bot::checkpointLoaded(CheckpointObject* cp) {
    auto it = m_cpExtra.find(cp);
    if (it == m_cpExtra.end()) return;
    ExtraCPData& e = it->second;
    auto* pl = GameManager::sharedState()->getPlayLayer();
    if (!pl) return;
    auto* p1 = pl->m_player1;
    if (p1) {
        p1->m_isDual = e.dual;
        p1->m_isUpsideDown = e.gravityFlipped;
        p1->m_vehicleSize = e.sizeMod;
        p1->m_playerSpeed = e.speedPortal;
        p1->m_rotation = e.shipRotation;
        p1->m_ballRotation = e.ballRotation;
        p1->m_ufoRotation = e.ufoRotation;
        p1->m_waveRotation = e.waveRotation;
        p1->m_robotRotation = e.robotRotation;
        p1->m_spiderRotation = e.spiderRotation;
        p1->m_swingRotation = e.swingRotation;
        // Restore dual state for player2 if needed – simplified.
    }
}

float Bot::getSpeedhack() { return m_speed; }
void Bot::setSpeedhack(float s) { m_speed = std::max(0.1f, std::min(s, 10.0f)); }
void Bot::speedhackUp() { setSpeedhack(m_speed + 0.1f); }
void Bot::speedhackDown() { setSpeedhack(m_speed - 0.1f); }

Bot::CBSMode Bot::getCBSMode() {
    if (Loader::get()->isModLoaded("syzzi.click_between_frames"))
        return CBSMode::Syzzi;
    if (GameManager::sharedState()->getGameVariable("0050"))
        return CBSMode::RobTop;
    return CBSMode::None;
}

// ---------------------------------------------------------------------------
// Playback stepping
// ---------------------------------------------------------------------------
void Bot::injectInput(const InputEvent& ev) {
    auto* pl = GameManager::sharedState()->getPlayLayer();
    if (!pl) return;
    // Directly call the low‑level input handler.
    // We use handleButton to trigger the game’s normal input processing.
    pl->handleButton(ev.down, ev.button, true);
}

void Bot::processPlaybackStep(PlayLayer* pl, float customDt) {
    // Advance physics one step.
    // We call the original update with our custom delta.
    // To avoid recursion through our hook, we temporarily disable playback flag.
    m_playing = false;
    pl->PlayLayer::update(customDt);  // call the real update
    m_playing = true;

    m_playAccum += customDt;
    double curTime = pl->m_gameState.m_levelTime;

    // Inject all events whose time has come.
    while (m_playIndex < m_events.size()) {
        const InputEvent& ev = m_events[m_playIndex];
        if (ev.time <= curTime + 0.0000001) { // tiny epsilon
            injectInput(ev);
            m_playIndex++;
        } else {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Macro I/O (simple binary format)
// ---------------------------------------------------------------------------
void Bot::saveMacro(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    char magic[4] = {'G','D','M','B'};
    f.write(magic, 4);
    uint32_t count = m_events.size();
    f.write(reinterpret_cast<const char*>(&count), 4);
    double prev = 0.0;
    for (auto& ev : m_events) {
        double delta = ev.time - prev;
        f.write(reinterpret_cast<const char*>(&delta), 8);
        uint8_t flags = (ev.down ? 1 : 0) | ((ev.button & 0xF) << 1);
        f.write(reinterpret_cast<const char*>(&flags), 1);
        prev = ev.time;
    }
    f.close();
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
    f.close();
}

// ---------------------------------------------------------------------------
// UI (simple Cocos2d overlay)
// ---------------------------------------------------------------------------
void Bot::toggleUI(PlayLayer* pl) {
    if (m_uiVisible) {
        if (m_uiNode) m_uiNode->removeFromParent();
        m_uiVisible = false;
        return;
    }
    auto winSize = CCDirector::sharedDirector()->getWinSize();

    // Background
    auto* bg = CCLayerColor::create(ccc4(0,0,0,180));
    bg->setContentSize(CCSize(200, 240));
    bg->setPosition(ccp(winSize.width/2 - 100, winSize.height/2 - 120));
    bg->setAnchorPoint(ccp(0,0));
    pl->addChild(bg, 1000);

    auto* menu = CCMenu::create();
    menu->setPosition(ccp(100, 220));

    // Status label
    auto status = Bot::getCBSMode();
    const char* statusTxt;
    ccColor3B statusCol;
    switch (status) {
        case CBSMode::Syzzi: statusTxt = "CBF: Syzzi (Green)"; statusCol = ccc3(0,255,0); break;
        case CBSMode::RobTop: statusTxt = "CBS: RobTop (Yellow)"; statusCol = ccc3(255,255,0); break;
        default: statusTxt = "No CBF/CBS (Red)"; statusCol = ccc3(255,0,0); break;
    }
    auto* label = CCLabelBMFont::create(statusTxt, "bigFont.fnt");
    label->setColor(statusCol);
    label->setPosition(ccp(100, 200));
    bg->addChild(label);

    // Buttons
    auto createBtn = [](const char* title, SEL_MenuHandler callback) {
        auto* btn = CCMenuItemSpriteExtra::create(
            CCLabelBMFont::create(title, "bigFont.fnt"),
            nullptr,
            nullptr,
            callback
        );
        return btn;
    };

    auto* recBtn = createBtn("Record", menu_selector(Bot::onRecordButton));
    recBtn->setPosition(ccp(0, 30));
    menu->addChild(recBtn);

    auto* playBtn = createBtn("Play", menu_selector(Bot::onPlayButton));
    playBtn->setPosition(ccp(0, -10));
    menu->addChild(playBtn);

    auto* saveBtn = createBtn("Save", menu_selector(Bot::onSaveButton));
    saveBtn->setPosition(ccp(0, -50));
    menu->addChild(saveBtn);

    auto* loadBtn = createBtn("Load", menu_selector(Bot::onLoadButton));
    loadBtn->setPosition(ccp(0, -90));
    menu->addChild(loadBtn);

    // Speedhack
    auto* spdLabel = CCLabelBMFont::create("Speed: 1.0", "bigFont.fnt");
    spdLabel->setPosition(ccp(100, -130));
    bg->addChild(spdLabel);
    // We need references to update, so store them.
    // For simplicity we use static member or tag.

    bg->addChild(menu);
    m_uiNode = bg;
    m_uiVisible = true;

    // Update speedhack display
    // (we can store the label and update it per frame, omitted for brevity)
}

// Button callbacks (need to be static members of Bot or free functions)
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

// Additional static callback declarations in Bot.hpp (omitted here for space).