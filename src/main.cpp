#include "Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>

using namespace geode::prelude;

namespace bot {
    namespace {
        constexpr int kOverlayTag = 0xB072;

        std::string timingModeName(TimingMode mode) {
            switch (mode) {
                case TimingMode::Cbf: return "Syzzi CBF";
                case TimingMode::Cbs: return "RobTop CBS";
                case TimingMode::None: default: return "Unavailable";
            }
        }

        cocos2d::ccColor3B timingModeColor(TimingMode mode) {
            switch (mode) {
                case TimingMode::Cbf: return ccc3(0, 255, 0);
                case TimingMode::Cbs: return ccc3(255, 220, 0);
                case TimingMode::None: default: return ccc3(255, 0, 0);
            }
        }

        CCMenuItemSpriteExtra* makeButton(const char* title, CCObject* target, SEL_MenuHandler handler) {
            auto* sprite = ButtonSprite::create(title);
            return CCMenuItemSpriteExtra::create(sprite, target, handler);
        }

        // Speedhacks the background music stream dynamically
        void updateAudioSpeed(double speed) {
            auto ae = FMODAudioEngine::sharedEngine();
            if (!ae) return;
            
            float s = static_cast<float>(speed);
            if (ae->m_backgroundMusicChannel) {
                ae->m_backgroundMusicChannel->setPitch(s);
            }
        }
    }

    class BotOverlay final : public CCLayerColor, public TextInputDelegate {
    public:
        static BotOverlay* create() {
            auto* ret = new BotOverlay();
            if (ret && ret->init()) {
                ret->autorelease();
                return ret;
            }
            CC_SAFE_DELETE(ret);
            return nullptr;
        }

        bool init() override {
            if (!CCLayerColor::initWithColor(ccc4(0, 0, 0, 160))) return false;

            auto win = CCDirector::sharedDirector()->getWinSize();
            setContentSize(win);
            setAnchorPoint({0.f, 0.f});
            setPosition({0.f, 0.f});
            setZOrder(std::numeric_limits<int>::max() - 4);

            auto* title = CCLabelBMFont::create("Practice Macro Bot", "bigFont.fnt");
            title->setScale(0.8f);
            title->setAnchorPoint({0.f, 1.f});
            title->setPosition({16.f, win.height - 18.f});
            addChild(title);

            m_status = CCLabelBMFont::create("", "bigFont.fnt");
            m_status->setScale(0.5f);
            m_status->setAnchorPoint({0.f, 1.f});
            m_status->setPosition({16.f, win.height - 44.f});
            addChild(m_status);

            auto* menu = CCMenu::create();
            menu->setAnchorPoint({0.f, 0.f});
            menu->setPosition({0.f, 0.f});
            addChild(menu);

            std::array<CCMenuItemSpriteExtra*, 5> buttons {
                makeButton("Record", this, menu_selector(BotOverlay::onRecord)),
                makeButton("Play", this, menu_selector(BotOverlay::onPlay)),
                makeButton("Save", this, menu_selector(BotOverlay::onSave)),
                makeButton("Load", this, menu_selector(BotOverlay::onLoad)),
                makeButton("Close", this, menu_selector(BotOverlay::onClose)),
            };

            // Compacted layout row 1: buttons
            float x = 70.f;
            float y = win.height - 85.f;
            float spacingX = 90.f;
            for (auto* btn : buttons) {
                btn->setPosition({x, y});
                menu->addChild(btn);
                x += spacingX;
            }

            // Dedicated layout row 2: Speedhack Input Field
            float labelX = 70.f;
            float labelY = win.height - 125.f;

            auto* speedLabel = CCLabelBMFont::create("Speed:", "bigFont.fnt");
            speedLabel->setScale(0.5f);
            speedLabel->setAnchorPoint({0.f, 0.5f});
            speedLabel->setPosition({labelX, labelY});
            addChild(speedLabel);

            m_speedInput = CCTextInputNode::create(80.f, 30.f, "1.0", "bigFont.fnt");
            m_speedInput->setLabelPlaceholderColor({150, 150, 150});
            m_speedInput->setAllowedChars("0123456789.");
            m_speedInput->setMaxLabelLength(5);
            m_speedInput->setPosition({labelX + 85.f, labelY});
            m_speedInput->setDelegate(this);

            // Text background wrapper
            auto* inputBg = CCLayerColor::create(ccc4(0, 0, 0, 100), 90.f, 30.f);
            inputBg->setPosition({labelX + 40.f, labelY - 15.f});
            addChild(inputBg);

            addChild(m_speedInput);

            // Populate speed string
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << BotManager::get().speedhack();
            m_speedInput->setString(ss.str());

            scheduleUpdate();
            refresh();
            return true;
        }

        void update(float) override {
            refresh();
        }

        void onEnter() override {
            CCLayerColor::onEnter();
            BotManager::get().setOverlay(this);
            refresh();
        }

        void onExit() override {
            BotManager::get().setOverlay(nullptr);
            CCLayerColor::onExit();
        }

        void textChanged(CCTextInputNode* input) override {
            if (input == m_speedInput) {
                std::string text = input->getString();
                if (!text.empty()) {
                    try {
                        double val = std::stod(text);
                        if (val > 0.0) {
                            BotManager::get().setSpeedhack(val);
                        }
                    } catch (...) {}
                }
            }
        }

    private:
        void refresh() {
            auto& bot = BotManager::get();
            auto mode = bot.detectedMode();

            std::ostringstream ss;
            ss << timingModeName(mode)
               << " | " << (bot.isRecording() ? "Recording" : bot.isPlayingBack() ? "Playback" : "Idle")
               << " | events: " << bot.events().size();
            if (bot.isPlayingBack()) ss << " | idx: " << bot.playbackIndex();
            ss << " | speed: " << std::fixed << std::setprecision(2) << bot.speedhack() << "x";
            ss << (bot.hasPlaybackData() ? " | loaded" : " | empty");

            m_status->setString(ss.str().c_str());
            m_status->setColor(timingModeColor(mode));
        }

        void onRecord(CCObject*) {
            auto& bot = BotManager::get();
            if (bot.isRecording()) bot.stopRecording(true);
            else bot.startRecording();
            refresh();
        }

        void onPlay(CCObject*) {
            auto& bot = BotManager::get();
            if (bot.isPlayingBack()) bot.stopPlayback();
            else bot.startPlayback();
            refresh();
        }

        void onSave(CCObject*) {
            BotManager::get().saveMacro();
            refresh();
        }

        void onLoad(CCObject*) {
            BotManager::get().loadMacro();
            refresh();
        }

        void onClose(CCObject*) {
            removeFromParentAndCleanup(true);
        }

        CCLabelBMFont* m_status = nullptr;
        CCTextInputNode* m_speedInput = nullptr;
    };

    BotManager& BotManager::get() {
        static BotManager s_instance;
        return s_instance;
    }

    TimingMode BotManager::detectedMode() const {
        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) return TimingMode::Cbf;
        return TimingMode::Cbs;
    }

    bool BotManager::canPlayBack() const {
        return detectedMode() != TimingMode::None;
    }

    bool BotManager::hasPlaybackData() const {
        return !m_events.empty();
    }

    bool BotManager::isRecording() const {
        return m_recording;
    }

    bool BotManager::isPlayingBack() const {
        return m_playback;
    }

    bool BotManager::isInjectingPlayback() const {
        return m_inPlaybackInjection;
    }

    double BotManager::speedhack() const {
        return m_speedhack;
    }

    void BotManager::toggleOverlay() {
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;

        if (scene->getChildByTag(kOverlayTag)) {
            if (auto* existing = dynamic_cast<BotOverlay*>(scene->getChildByTag(kOverlayTag))) {
                existing->removeFromParentAndCleanup(true);
            }
            m_overlay = nullptr;
            return;
        }

        auto* overlay = BotOverlay::create();
        if (!overlay) return;
        scene->addChild(overlay, std::numeric_limits<int>::max() - 4, kOverlayTag);
        m_overlay = overlay;
    }

    void BotManager::setOverlay(BotOverlay* overlay) {
        m_overlay = overlay;
    }

    BotOverlay* BotManager::overlay() const {
        return m_overlay;
    }

    void BotManager::startRecording() {
        m_mode = detectedMode();
        m_recording = true;
        m_playback = false;
        m_dead = false;
        m_inPlaybackInjection = false;
        m_inUpdateSplit = false;
        m_playbackIndex = 0;
        m_events.clear();
        m_checkpoints.clear();
        m_checkpointLookup.clear();
        m_recordBaseSec = 0.0; // Absolutized timeline
        m_playbackStartSec = 0.0;
        m_deathCutoffSec = 0.0;
    }

    void BotManager::stopRecording(bool keepData) {
        m_recording = false;
        if (!keepData) {
            m_events.clear();
            m_checkpoints.clear();
            m_checkpointLookup.clear();
        }
    }

    void BotManager::startPlayback() {
        if (!canPlayBack() || !hasPlaybackData()) return;
        m_mode = detectedMode();
        m_playback = true;
        m_recording = false;

        // Automatically fast-forward playhead index to match where the player is in the run right now!
        double currentSec = m_layer ? m_layer->m_currentTime : 0.0;
        m_playbackIndex = 0;
        while (m_playbackIndex < m_events.size() && m_events[m_playbackIndex].timeSec < currentSec) {
            m_playbackIndex++;
        }

        m_dead = false;
        refreshPlayers(m_layer);
    }

    void BotManager::stopPlayback() {
        m_playback = false;
        m_playbackIndex = 0;
        m_inPlaybackInjection = false;
        m_inUpdateSplit = false;
    }

    void BotManager::setSpeedhack(double value) {
        if (!std::isfinite(value)) return;
        m_speedhack = std::max(0.01, value);
        updateAudioSpeed(m_speedhack);
    }

    std::filesystem::path BotManager::defaultMacroPath() const {
        auto saveDir = Mod::get()->getSaveDir();
        std::filesystem::create_directories(saveDir);
        return saveDir / "practice_macro.prbm";
    }

    std::filesystem::path BotManager::macroPath() const {
        return m_macroPath.empty() ? defaultMacroPath() : m_macroPath;
    }

    const std::vector<MacroEvent>& BotManager::events() const {
        return m_events;
    }

    std::size_t BotManager::playbackIndex() const {
        return m_playbackIndex;
    }

    void BotManager::onSceneEnter(PlayLayer* layer) {
        m_layer = layer;
        refreshPlayers(layer);
        if (m_recording) {
            m_recordBaseSec = 0.0;
        }
    }

    void BotManager::onSceneExit() {
        m_layer = nullptr;
        m_cachedP1 = nullptr;
        m_cachedP2 = nullptr;
        m_inPlaybackInjection = false;
        m_inUpdateSplit = false;
    }

    void BotManager::refreshPlayers(PlayLayer* layer) {
        if (!layer) {
            m_cachedP1 = nullptr;
            m_cachedP2 = nullptr;
            return;
        }

        m_cachedP1 = layer->m_player1;
        m_cachedP2 = layer->m_player2;
    }

    PlayerObject* BotManager::player1() const { return m_cachedP1; }
    PlayerObject* BotManager::player2() const { return m_cachedP2; }

    void BotManager::onButton(PlayerObject* player, PlayerButton button, bool down) {
        if (!m_recording || m_inPlaybackInjection) return;
        if (!m_layer) return;

        // CBF updates timeline sub-steps sequentially, making layer->m_currentTime absolute and hyper-precise.
        double preciseGameTime = m_layer->m_currentTime;

        MacroEvent ev;
        ev.timeSec = preciseGameTime; 
        ev.button = static_cast<std::uint8_t>(button);
        
        bool isPlayer1 = (player == m_layer->m_player1);
        ev.flags = static_cast<std::uint8_t>((down ? 1 : 0) | (isPlayer1 ? 2 : 0));
        m_events.push_back(ev);
    }

    void BotManager::trimAfterCurrentTime(PlayLayer* layer) {
        if (!layer) return;
        double cutoff = layer->m_currentTime;
        if (cutoff < 0) cutoff = 0;

        std::erase_if(m_events, [cutoff](MacroEvent const& ev) { return ev.timeSec > cutoff; });
        std::erase_if(m_checkpoints, [cutoff](CheckpointSnapshot const& cp) { return cp.timeSec > cutoff; });
        std::erase_if(m_checkpointLookup, [cutoff](auto const& pair) { return pair.second.timeSec > cutoff; });
        if (m_playbackIndex > m_events.size()) m_playbackIndex = m_events.size();
    }

    void BotManager::clearDeadState() {
        m_dead = false;
        m_deathCutoffSec = 0.0;
    }

    void BotManager::onDeath(PlayLayer*) {
        m_dead = true;
        m_deathCutoffSec = m_layer ? m_layer->m_currentTime : 0.0;
    }

    void BotManager::onRestartPre(PlayLayer* layer) {
        trimAfterCurrentTime(layer);
    }

    void BotManager::onRestartPost(PlayLayer* layer) {
        clearDeadState();
        m_playbackIndex = 0; // Seamlessly reset playhead back to zero on restart
        if (m_recording) {
            m_recordBaseSec = 0.0;
        }
    }

    PlayerSnapshot BotManager::captureSnapshot(PlayerObject* player) {
        PlayerSnapshot snapshot;
        if (!player) return snapshot;

        snapshot.position = player->getPosition();
        snapshot.rotation = player->getRotation();
        snapshot.vehicleSize = player->getScale();
        return snapshot;
    }

    void BotManager::applySnapshot(PlayerObject* player, PlayerSnapshot const& snapshot) {
        if (!player) return;
        player->setPosition(snapshot.position);
        player->setRotation(snapshot.rotation);
        player->setScale(snapshot.vehicleSize);
    }

    void BotManager::onCheckpointStore(PlayLayer* layer, CheckpointObject* checkpoint) {
        if (!layer || !checkpoint) return;
        CheckpointSnapshot snapshot;
        snapshot.timeSec = layer->m_currentTime;
        snapshot.player1 = captureSnapshot(m_cachedP1);
        snapshot.player2 = captureSnapshot(m_cachedP2);
        m_checkpoints.push_back(snapshot);
        m_checkpointLookup[checkpoint] = snapshot;
    }

    void BotManager::onCheckpointLoad(PlayLayer* layer, CheckpointObject* checkpoint) {
        if (!layer || !checkpoint) return;
        auto found = m_checkpointLookup.find(checkpoint);
        if (found == m_checkpointLookup.end()) return;

        refreshPlayers(layer);
        applySnapshot(m_cachedP1, found->second.player1);
        applySnapshot(m_cachedP2, found->second.player2);

        // Re-align playhead to the loaded checkpoint time!
        double targetTime = found->second.timeSec;
        m_playbackIndex = 0;
        while (m_playbackIndex < m_events.size() && m_events[m_playbackIndex].timeSec < targetTime) {
            m_playbackIndex++;
        }

        clearDeadState();
    }

    void BotManager::onLevelComplete(PlayLayer*) {
        stopRecording(true);
        stopPlayback();
    }

    void BotManager::saveMacro() {
        auto path = macroPath();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::error("Practice macro bot: failed to open {} for writing", path.string());
            return;
        }

        std::vector<std::uint8_t> buffer;
        buffer.reserve(m_events.size() * 10 + 64);

        auto appendRaw = [&](auto value) {
            auto* ptr = reinterpret_cast<const std::uint8_t*>(&value);
            buffer.insert(buffer.end(), ptr, ptr + sizeof(value));
        };

        buffer.insert(buffer.end(), {'P', 'R', 'B', 'M'});
        std::uint16_t version = 2; // Incremented file format version to reflect robust seconds-based timing
        appendRaw(version);
        std::uint8_t mode = static_cast<std::uint8_t>(detectedMode());
        appendRaw(mode);
        double recordedSpeed = m_speedhack;
        appendRaw(recordedSpeed);

        // Binary dump of events containing pure high-precision time double fields
        std::uint64_t eventSize = m_events.size();
        appendRaw(eventSize);
        for (auto const& ev : m_events) {
            appendRaw(ev.timeSec);
            buffer.push_back(ev.flags);
            buffer.push_back(ev.button);
        }

        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }

    bool BotManager::readMacroFromDisk() {
        auto path = macroPath();
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::uint8_t* cursor = bytes.data();
        const std::uint8_t* end = bytes.data() + bytes.size();

        auto readRaw = [&](auto& value) -> bool {
            if (cursor + sizeof(value) > end) return false;
            std::memcpy(&value, cursor, sizeof(value));
            cursor += sizeof(value);
            return true;
        };

        if (end - cursor < 4 || std::memcmp(cursor, "PRBM", 4) != 0) return false;
        cursor += 4;

        std::uint16_t version = 0;
        std::uint8_t mode = 0;
        double recordedSpeed = 1.0;
        if (!readRaw(version) || !readRaw(mode) || !readRaw(recordedSpeed)) return false;
        if (version != 2) return false;

        std::uint64_t eventCount = 0;
        if (!readRaw(eventCount)) return false;

        m_events.clear();
        m_events.reserve(static_cast<std::size_t>(eventCount));

        for (std::uint64_t i = 0; i < eventCount; ++i) {
            MacroEvent ev;
            if (!readRaw(ev.timeSec)) return false;
            if (cursor + 2 > end) return false;
            ev.flags = *cursor++;
            ev.button = *cursor++;
            m_events.push_back(ev);
        }

        m_mode = static_cast<TimingMode>(mode);
        return true;
    }

    void BotManager::loadMacro() {
        if (!readMacroFromDisk()) {
            log::warn("Practice macro bot: failed to load macro from {}", macroPath().string());
            return;
        }
        stopRecording(true);
        stopPlayback();
        m_playbackIndex = 0;
    }

    void BotManager::onGameUpdate(PlayLayer* layer, float dt, std::function<void(float)> const& originalUpdate) {
        m_layer = layer;
        refreshPlayers(layer);

        // Keep audio speed aligned
        updateAudioSpeed(m_speedhack);

        if (m_playback && (!canPlayBack() || !hasPlaybackData())) {
            stopPlayback();
        }

        const double scaledDt = static_cast<double>(dt);
        if (!m_playback || !canPlayBack() || !hasPlaybackData()) {
            originalUpdate(static_cast<float>(scaledDt));
            return;
        }

        if (m_inUpdateSplit) {
            originalUpdate(static_cast<float>(scaledDt));
            return;
        }

        m_inUpdateSplit = true;
        const double startTime = layer->m_currentTime;
        const double endTime = startTime + scaledDt;

        // Playback updates split and process clicks relative to exact simulation seconds
        while (m_playbackIndex < m_events.size() && m_events[m_playbackIndex].timeSec <= endTime) {
            const auto& ev = m_events[m_playbackIndex];
            const double delta = std::max(0.0, ev.timeSec - layer->m_currentTime);
            if (delta > 0.0) {
                originalUpdate(static_cast<float>(delta));
            }

            refreshPlayers(layer);
            m_inPlaybackInjection = true;
            
            bool isPlayer1 = (ev.flags & 2) != 0;
            bool down = (ev.flags & 1) != 0;
            layer->handleButton(down, static_cast<int>(ev.button), isPlayer1);

            m_inPlaybackInjection = false;

            ++m_playbackIndex;
        }

        const double remaining = std::max(0.0, endTime - layer->m_currentTime);
        if (remaining > 0.0) {
            originalUpdate(static_cast<float>(remaining));
        }

        m_inUpdateSplit = false;
        if (m_playbackIndex >= m_events.size()) {
            stopPlayback();
        }
    }
}

// Multi-step high-accuracy custom scheduler updates bypass the engine physics caps and allow safe, infinite speedhacking.
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& bot = bot::BotManager::get();
        double speed = bot.speedhack();
        
        auto playLayer = PlayLayer::get();
        if (playLayer && !bot.isInjectingPlayback() && std::abs(speed - 1.0) > 1e-4) {
            static double accumulator = 0.0;
            accumulator += static_cast<double>(dt) * speed;
            
            int updatesRun = 0;
            const double targetStep = static_cast<double>(dt);
            while (accumulator >= targetStep && updatesRun < 100) {
                CCScheduler::update(static_cast<float>(targetStep));
                accumulator -= targetStep;
                updatesRun++;
            }
        } else {
            CCScheduler::update(dt);
        }
    }
};

// Intercept keyboard messages at the absolute engine source!
// This fixes keys being swallowed on both Windows and Android.
class $modify(BotKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool isRepeat, double dt) {
        if (down && key == cocos2d::enumKeyCodes::KEY_K) {
            auto* scene = CCDirector::sharedDirector()->getRunningScene();
            if (scene) {
                // Ensure the overlay is only toggleable when active in a main level or menu
                bool inTogglableScene = scene->getChildByType<PlayLayer>(0) != nullptr || 
                                         scene->getChildByType<MenuLayer>(0) != nullptr;
                if (inTogglableScene) {
                    bot::BotManager::get().toggleOverlay();
                    return true;
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, isRepeat, dt);
    }
};

class $modify(BotGJBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        auto& bot = bot::BotManager::get();
        if (bot.isRecording() && !bot.isInjectingPlayback()) {
            auto* player = isPlayer1 ? m_player1 : m_player2;
            bot.onButton(player, static_cast<PlayerButton>(button), down);
        }
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    void onEnter() {
        PlayLayer::onEnter();
        bot::BotManager::get().onSceneEnter(this);
    }

    void onExit() {
        bot::BotManager::get().onSceneExit();
        PlayLayer::onExit();
    }

    void update(float dt) {
        bot::BotManager::get().onGameUpdate(this, dt, [this](float step) {
            PlayLayer::update(step);
        });
    }

    void resetLevel() {
        bot::BotManager::get().onRestartPre(this);
        PlayLayer::resetLevel();
        bot::BotManager::get().onRestartPost(this);
    }

    void fullReset() {
        bot::BotManager::get().onRestartPre(this);
        PlayLayer::fullReset();
        bot::BotManager::get().onRestartPost(this);
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        bot::BotManager::get().onCheckpointStore(this, checkpoint);
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        bot::BotManager::get().onCheckpointLoad(this, checkpoint);
    }

    void levelComplete() {
        bot::BotManager::get().onLevelComplete(this);
        PlayLayer::levelComplete();
    }
};