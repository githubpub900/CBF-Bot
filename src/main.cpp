#include "Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
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
        constexpr std::int64_t kNanosPerSecond = 1'000'000'000LL;

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

        std::int64_t toNs(double seconds) {
            return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kNanosPerSecond)));
        }

        double fromNs(std::int64_t ns) {
            return static_cast<double>(ns) / static_cast<double>(kNanosPerSecond);
        }

        CCMenuItemSpriteExtra* makeButton(const char* title, CCObject* target, SEL_MenuHandler handler) {
            auto* sprite = ButtonSprite::create(title);
            return CCMenuItemSpriteExtra::create(sprite, target, handler);
        }

        std::int64_t currentLayerTimeNs(PlayLayer* layer) {
            return layer ? toNs(layer->m_currentTime) : 0;
        }
    }

    class BotOverlay final : public CCLayerColor {
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
            if (!CCLayer::init()) return false;
            setColor(ccc3(0, 0, 0));
            setOpacity(160);

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

            std::array<CCMenuItemSpriteExtra*, 7> buttons {
                makeButton("Record", this, menu_selector(BotOverlay::onRecord)),
                makeButton("Play", this, menu_selector(BotOverlay::onPlay)),
                makeButton("Save", this, menu_selector(BotOverlay::onSave)),
                makeButton("Load", this, menu_selector(BotOverlay::onLoad)),
                makeButton("Speed -", this, menu_selector(BotOverlay::onSpeedDown)),
                makeButton("Speed +", this, menu_selector(BotOverlay::onSpeedUp)),
                makeButton("Close", this, menu_selector(BotOverlay::onClose)),
            };

            float x = 88.f;
            float y = win.height - 96.f;
            for (auto* btn : buttons) {
                btn->setPosition({x, y});
                menu->addChild(btn);
                x += 118.f;
                if (x > win.width - 140.f) {
                    x = 88.f;
                    y -= 44.f;
                }
            }

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

        void onSpeedDown(CCObject*) {
            auto& bot = BotManager::get();
            bot.setSpeedhack(bot.speedhack() * 0.5);
            refresh();
        }

        void onSpeedUp(CCObject*) {
            auto& bot = BotManager::get();
            bot.setSpeedhack(bot.speedhack() * 2.0);
            refresh();
        }

        void onClose(CCObject*) {
            removeFromParentAndCleanup(true);
        }

        CCLabelBMFont* m_status = nullptr;
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
        m_recordBaseNs = m_layer ? toNs(m_layer->m_currentTime) : 0;
        m_playbackStartNs = 0;
        m_deathCutoffNs = 0;
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
        m_playbackIndex = 0;
        m_playbackStartNs = m_layer ? toNs(m_layer->m_currentTime) : 0;
        m_inPlaybackInjection = false;
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
        if (m_recording && !m_recordBaseNs) {
            m_recordBaseNs = layer ? toNs(layer->m_currentTime) : 0;
        }
    }

    void BotManager::onSceneExit() {
        m_layer = nullptr;
        m_cachedP1 = nullptr;
        m_cachedP2 = nullptr;
        m_inPlaybackInjection = false;
        m_inUpdateSplit = false;
        stopRecording(true);
        stopPlayback();
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
        if (m_events.empty() && !m_recordBaseNs) m_recordBaseNs = currentLayerTimeNs(m_layer);

        MacroEvent ev;
        ev.timeNs = currentLayerTimeNs(m_layer) - m_recordBaseNs;
        ev.button = static_cast<std::uint8_t>(button);
        ev.flags = static_cast<std::uint8_t>((down ? 1 : 0) | ((player && player == m_cachedP1) ? 2 : 0));
        m_events.push_back(ev);
    }

    void BotManager::trimAfterCurrentTime(PlayLayer* layer) {
        if (!layer) return;
        auto cutoff = currentLayerTimeNs(layer) - m_recordBaseNs;
        if (cutoff < 0) cutoff = 0;

        std::erase_if(m_events, [cutoff](MacroEvent const& ev) { return ev.timeNs > cutoff; });
        std::erase_if(m_checkpoints, [cutoff](CheckpointSnapshot const& cp) { return cp.timeNs > cutoff; });
        std::erase_if(m_checkpointLookup, [cutoff](auto const& pair) { return pair.second.timeNs > cutoff; });
        if (m_playbackIndex > m_events.size()) m_playbackIndex = m_events.size();
    }

    void BotManager::clearDeadState() {
        m_dead = false;
        m_deathCutoffNs = 0;
    }

    void BotManager::onDeath(PlayLayer*) {
        m_dead = true;
        m_deathCutoffNs = m_layer ? currentLayerTimeNs(m_layer) : 0;
    }

    void BotManager::onRestartPre(PlayLayer* layer) {
        trimAfterCurrentTime(layer);
    }

    void BotManager::onRestartPost(PlayLayer* layer) {
        clearDeadState();
        if (m_recording) {
            m_recordBaseNs = layer ? toNs(layer->m_currentTime) : 0;
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
        snapshot.timeNs = currentLayerTimeNs(layer) - m_recordBaseNs;
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

        if (m_recording) {
            m_recordBaseNs = currentLayerTimeNs(layer) - found->second.timeNs;
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
        buffer.reserve(m_events.size() * 6 + 64);

        auto appendRaw = [&](auto value) {
            auto* ptr = reinterpret_cast<const std::uint8_t*>(&value);
            buffer.insert(buffer.end(), ptr, ptr + sizeof(value));
        };

        buffer.insert(buffer.end(), {'P', 'R', 'B', 'M'});
        std::uint16_t version = 1;
        appendRaw(version);
        std::uint8_t mode = static_cast<std::uint8_t>(detectedMode());
        appendRaw(mode);
        double recordedSpeed = m_speedhack;
        appendRaw(recordedSpeed);

        writeVarUInt(buffer, static_cast<std::uint64_t>(m_events.size()));
        std::int64_t last = 0;
        for (auto const& ev : m_events) {
            std::uint64_t delta = static_cast<std::uint64_t>(std::max<std::int64_t>(0, ev.timeNs - last));
            writeVarUInt(buffer, delta);
            buffer.push_back(ev.flags);
            buffer.push_back(ev.button);
            last = ev.timeNs;
        }

        writeVarUInt(buffer, 0);
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
        if (version != 1) return false;

        std::uint64_t eventCount = 0;
        if (!readVarUInt(cursor, end, eventCount)) return false;

        m_events.clear();
        m_events.reserve(static_cast<std::size_t>(eventCount));

        std::int64_t running = 0;
        for (std::uint64_t i = 0; i < eventCount; ++i) {
            std::uint64_t delta = 0;
            if (!readVarUInt(cursor, end, delta)) return false;
            if (cursor + 2 > end) return false;
            running += static_cast<std::int64_t>(delta);
            MacroEvent ev;
            ev.timeNs = running;
            ev.flags = *cursor++;
            ev.button = *cursor++;
            m_events.push_back(ev);
        }

        std::uint64_t checkpointCount = 0;
        if (!readVarUInt(cursor, end, checkpointCount)) return false;
        (void)checkpointCount;

        m_mode = static_cast<TimingMode>(mode);
        (void)recordedSpeed;
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

    std::uint64_t BotManager::zigZagEncode(std::int64_t value) {
        return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
    }

    std::int64_t BotManager::zigZagDecode(std::uint64_t value) {
        return static_cast<std::int64_t>((value >> 1) ^ (~(value & 1) + 1));
    }

    void BotManager::writeVarUInt(std::vector<std::uint8_t>& out, std::uint64_t value) {
        while (value >= 0x80) {
            out.push_back(static_cast<std::uint8_t>(value | 0x80));
            value >>= 7;
        }
        out.push_back(static_cast<std::uint8_t>(value));
    }

    bool BotManager::readVarUInt(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint64_t& value) {
        value = 0;
        int shift = 0;
        while (cursor < end && shift < 64) {
            std::uint8_t byte = *cursor++;
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return true;
            shift += 7;
        }
        return false;
    }

    void BotManager::onGameUpdate(PlayLayer* layer, float dt, std::function<void(float)> const& originalUpdate) {
        m_layer = layer;
        refreshPlayers(layer);

        if (m_playback && (!canPlayBack() || !hasPlaybackData())) {
            stopPlayback();
        }

        const double scaledDt = static_cast<double>(dt) * m_speedhack;
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
        const std::int64_t endNs = toNs(endTime);

        while (m_playbackIndex < m_events.size() && m_events[m_playbackIndex].timeNs <= endNs) {
            const auto& ev = m_events[m_playbackIndex];
            const double eventTime = fromNs(ev.timeNs);
            const double delta = std::max(0.0, eventTime - layer->m_currentTime);
            if (delta > 0.0) {
                originalUpdate(static_cast<float>(delta / m_speedhack));
            }

            refreshPlayers(layer);
            m_inPlaybackInjection = true;
            auto* targetPlayer = (ev.flags & 2) ? m_cachedP1 : m_cachedP2;
            if (targetPlayer) {
                auto button = static_cast<PlayerButton>(ev.button);
                if (ev.flags & 1) targetPlayer->pushButton(button);
                else targetPlayer->releaseButton(button);
            }
            m_inPlaybackInjection = false;

            ++m_playbackIndex;
        }

        const double remaining = std::max(0.0, endTime - layer->m_currentTime);
        if (remaining > 0.0) {
            originalUpdate(static_cast<float>(remaining / m_speedhack));
        }

        m_inUpdateSplit = false;
        if (m_playbackIndex >= m_events.size()) {
            stopPlayback();
        }
    }
}

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

class $modify(BotPlayerObject, PlayerObject) {
    void pushButton(PlayerButton button) {
        PlayerObject::pushButton(button);
        if (!bot::BotManager::get().isInjectingPlayback()) {
            bot::BotManager::get().onButton(this, button, true);
        }
    }

    void releaseButton(PlayerButton button) {
        PlayerObject::releaseButton(button);
        if (!bot::BotManager::get().isInjectingPlayback()) {
            bot::BotManager::get().onButton(this, button, false);
        }
    }
};