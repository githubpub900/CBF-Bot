#include "Bot.hpp"

using namespace geode::prelude;
using namespace bot;

// ==================== SPEEDHACK STATE IMPLEMENTATION ====================
void SpeedhackState::update(float dt) {
    if (!enabled) {
        currentSpeed = 1.0f;
        return;
    }

    float diff = targetSpeed - currentSpeed;
    if (std::abs(diff) < 0.001f) {
        currentSpeed = targetSpeed;
    } else {
        currentSpeed += diff * std::min(1.0f, dt * transitionSpeed);
    }
}

float SpeedhackState::getDeltaMultiplier() const {
    return enabled ? (1.0f / currentSpeed) : 1.0f;
}

// ==================== MACRO FILE IMPLEMENTATION ====================
bool MacroFile::saveToFile(const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t versionLen = static_cast<uint32_t>(version.length());
    file.write(reinterpret_cast<const char*>(&versionLen), sizeof(versionLen));
    file.write(version.c_str(), versionLen);

    file.write(reinterpret_cast<const char*>(&startTimeOffset), sizeof(startTimeOffset));
    file.write(reinterpret_cast<const char*>(&levelLength), sizeof(levelLength));
    file.write(reinterpret_cast<const char*>(&levelID), sizeof(levelID));
    file.write(reinterpret_cast<const char*>(&totalSteps), sizeof(totalSteps));
    file.write(reinterpret_cast<const char*>(&fps), sizeof(fps));

    uint32_t nameLen = static_cast<uint32_t>(levelName.length());
    file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    file.write(levelName.c_str(), nameLen);

    uint64_t inputCount = inputs.size();
    file.write(reinterpret_cast<const char*>(&inputCount), sizeof(inputCount));

    if (inputCount > 0) {
        file.write(reinterpret_cast<const char*>(&inputs[0].time), sizeof(inputs[0].time));
        file.write(reinterpret_cast<const char*>(&inputs[0].isPlayer1), sizeof(inputs[0].isPlayer1));
        file.write(reinterpret_cast<const char*>(&inputs[0].isClick), sizeof(inputs[0].isClick));
        file.write(reinterpret_cast<const char*>(&inputs[0].isHold), sizeof(inputs[0].isHold));

        for (size_t i = 1; i < inputCount; i++) {
            double delta = inputs[i].time - inputs[i-1].time;
            file.write(reinterpret_cast<const char*>(&delta), sizeof(delta));
            file.write(reinterpret_cast<const char*>(&inputs[i].isPlayer1), sizeof(inputs[i].isPlayer1));
            file.write(reinterpret_cast<const char*>(&inputs[i].isClick), sizeof(inputs[i].isClick));
            file.write(reinterpret_cast<const char*>(&inputs[i].isHold), sizeof(inputs[i].isHold));
        }
    }

    file.close();
    return true;
}

bool MacroFile::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    inputs.clear();

    uint32_t versionLen;
    file.read(reinterpret_cast<char*>(&versionLen), sizeof(versionLen));
    version.resize(versionLen);
    file.read(&version[0], versionLen);

    file.read(reinterpret_cast<char*>(&startTimeOffset), sizeof(startTimeOffset));
    file.read(reinterpret_cast<char*>(&levelLength), sizeof(levelLength));
    file.read(reinterpret_cast<char*>(&levelID), sizeof(levelID));
    file.read(reinterpret_cast<char*>(&totalSteps), sizeof(totalSteps));
    file.read(reinterpret_cast<char*>(&fps), sizeof(fps));

    uint32_t nameLen;
    file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    levelName.resize(nameLen);
    file.read(&levelName[0], nameLen);

    uint64_t inputCount;
    file.read(reinterpret_cast<char*>(&inputCount), sizeof(inputCount));

    inputs.reserve(inputCount);

    if (inputCount > 0) {
        MacroInput first{};
        file.read(reinterpret_cast<char*>(&first.time), sizeof(first.time));
        file.read(reinterpret_cast<char*>(&first.isPlayer1), sizeof(first.isPlayer1));
        file.read(reinterpret_cast<char*>(&first.isClick), sizeof(first.isClick));
        file.read(reinterpret_cast<char*>(&first.isHold), sizeof(first.isHold));
        inputs.push_back(first);

        double lastTime = first.time;
        for (size_t i = 1; i < inputCount; i++) {
            MacroInput input{};
            double delta;
            file.read(reinterpret_cast<char*>(&delta), sizeof(delta));
            file.read(reinterpret_cast<char*>(&input.isPlayer1), sizeof(input.isPlayer1));
            file.read(reinterpret_cast<char*>(&input.isClick), sizeof(input.isClick));
            file.read(reinterpret_cast<char*>(&input.isHold), sizeof(input.isHold));
            input.time = lastTime + delta;
            lastTime = input.time;
            inputs.push_back(input);
        }
    }

    file.close();
    return true;
}

// ==================== MACRO BOT SINGLETON ====================
MacroBot& MacroBot::getInstance() {
    static MacroBot instance;
    return instance;
}

void MacroBot::setState(BotState state) {
    BotState oldState = m_state.exchange(state);
    if (oldState != state) {
        log::info("Bot state changed: {} -> {}", static_cast<int>(oldState), static_cast<int>(state));
        updateStatusLabel();
    }
}

// ==================== RECORDING ====================
void MacroBot::startRecording() {
    if (!isCBFEnabled()) {
        FLAlertLayer::create("CBF Required", 
            "This bot requires Click Between Frames to be enabled.\nPlease install syzzi.click_between_frames or enable RobTop's Click Between Steps.", 
            "OK")->show();
        return;
    }

    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_inputs.clear();
    m_inputsBackup.clear();
    m_playbackIndex = 0;
    m_startTimeOffset = m_currentTime;
    m_diedSinceCheckpoint = false;
    m_lastCheckpointInputIndex = 0;

    setState(BotState::RECORDING);
    log::info("Started recording macro at time: {}", m_startTimeOffset);
}

void MacroBot::stopRecording() {
    if (m_state != BotState::RECORDING) return;

    std::lock_guard<std::mutex> lock(m_inputMutex);
    compressInputs();
    setState(BotState::IDLE);

    if (m_autoSave && !m_inputs.empty()) {
        saveMacro("autosave_" + std::to_string(static_cast<int>(time(nullptr))));
    }

    log::info("Stopped recording. Total inputs: {}", m_inputs.size());
}

void MacroBot::recordInput(bool isPlayer1, bool isClick, bool isHold, double time) {
    if (m_state != BotState::RECORDING) return;

    std::lock_guard<std::mutex> lock(m_inputMutex);

    double relativeTime = time - m_startTimeOffset;
    if (relativeTime < 0) relativeTime = 0;

    relativeTime = std::round(relativeTime / TIME_PRECISION) * TIME_PRECISION;

    MacroInput input;
    input.time = relativeTime;
    input.isPlayer1 = isPlayer1;
    input.isClick = isClick;
    input.isHold = isHold;

    m_inputs.push_back(input);
}

// ==================== PLAYBACK ====================
void MacroBot::startPlayback() {
    if (m_inputs.empty()) {
        FLAlertLayer::create("No Macro", "No macro inputs to play back.", "OK")->show();
        return;
    }

    if (!isCBFEnabled()) {
        FLAlertLayer::create("CBF Required", 
            "CBF must be enabled for playback.", 
            "OK")->show();
        return;
    }

    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_playbackIndex = 0;
    m_lastPlaybackTime = 0;

    std::sort(m_inputs.begin(), m_inputs.end());

    setState(BotState::PLAYING);
    log::info("Started playback. {} inputs to play.", m_inputs.size());
}

void MacroBot::stopPlayback() {
    if (m_state != BotState::PLAYING) return;
    setState(BotState::IDLE);
    log::info("Stopped playback.");
}

void MacroBot::updatePlayback(double currentTime) {
    if (m_state != BotState::PLAYING) return;

    std::lock_guard<std::mutex> lock(m_inputMutex);

    double adjustedTime = currentTime - m_startTimeOffset;
    if (adjustedTime < 0) adjustedTime = 0;

    while (m_playbackIndex < m_inputs.size() && 
           m_inputs[m_playbackIndex].time <= adjustedTime + TIME_PRECISION) {

        if (!m_diedSinceCheckpoint || m_inputs[m_playbackIndex].time <= m_lastPlaybackTime) {
            executeInput(m_inputs[m_playbackIndex]);
        }

        m_playbackIndex++;
    }

    m_lastPlaybackTime = adjustedTime;
}

void MacroBot::executeInput(const MacroInput& input) {
    sendClick(input.isPlayer1, input.isClick);
}

void MacroBot::sendClick(bool isPlayer1, bool isClick) {
    auto playLayer = PlayLayer::get();
    if (!playLayer) return;

    if (isClick) {
        playLayer->handleButton(true, static_cast<int>(PlayerButton::Jump), isPlayer1 ? 0 : 1);
    } else {
        playLayer->handleButton(false, static_cast<int>(PlayerButton::Jump), isPlayer1 ? 0 : 1);
    }
}

// ==================== CHECKPOINT MANAGEMENT ====================
void MacroBot::saveCheckpoint(PlayLayer* playLayer) {
    if (!playLayer) return;

    CheckpointState state = utils::capturePlayerState(playLayer);
    state.inputIndex = m_inputs.size();
    state.level_time = static_cast<float>(m_currentTime);

    m_lastCheckpointInputIndex = m_inputs.size();
    m_diedSinceCheckpoint = false;

    log::info("Checkpoint saved at time: {}, input index: {}", state.level_time, state.inputIndex);
}

void MacroBot::loadCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint) {
    if (!playLayer || !checkpoint) return;

    auto it = m_checkpointStates.find(checkpoint);
    if (it != m_checkpointStates.end()) {
        utils::restorePlayerState(playLayer, it->second);

        m_currentTime = it->second.level_time;
        m_lastPlaybackTime = m_currentTime - m_startTimeOffset;

        revertInputsAfterCheckpoint(it->second.inputIndex);

        m_diedSinceCheckpoint = false;
        log::info("Checkpoint loaded. Reverted to input index: {}", it->second.inputIndex);
    }
}

void MacroBot::removeCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint) {
    if (!checkpoint) return;

    auto it = m_checkpointStates.find(checkpoint);
    if (it != m_checkpointStates.end()) {
        m_checkpointStates.erase(it);

        auto orderIt = std::find(m_checkpointOrder.begin(), m_checkpointOrder.end(), checkpoint);
        if (orderIt != m_checkpointOrder.end()) {
            m_checkpointOrder.erase(orderIt);
        }
    }
}

void MacroBot::clearCheckpoints() {
    m_checkpointStates.clear();
    m_checkpointOrder.clear();
    m_lastCheckpointInputIndex = 0;
    m_diedSinceCheckpoint = false;
}

// ==================== DEAD INPUT HANDLING ====================
void MacroBot::onPlayerDeath() {
    m_diedSinceCheckpoint = true;
    log::info("Player died - marking inputs after checkpoint {} as dead", m_lastCheckpointInputIndex);
}

void MacroBot::onRestartFromCheckpoint() {
    if (m_diedSinceCheckpoint) {
        discardDeadInputs();
    }
    m_diedSinceCheckpoint = false;
}

void MacroBot::onRestartFromBeginning() {
    clearCheckpoints();
    m_currentTime = 0;
    m_playbackIndex = 0;
    m_lastPlaybackTime = 0;
}

void MacroBot::onPauseRestart() {
    std::lock_guard<std::mutex> lock(m_inputMutex);

    double currentRelativeTime = m_currentTime - m_startTimeOffset;
    auto it = std::remove_if(m_inputs.begin(), m_inputs.end(),
        [currentRelativeTime](const MacroInput& input) {
            return input.time > currentRelativeTime;
        });
    m_inputs.erase(it, m_inputs.end());

    log::info("Pause restart: removed inputs after time {}", currentRelativeTime);
}

void MacroBot::discardDeadInputs() {
    std::lock_guard<std::mutex> lock(m_inputMutex);

    if (m_lastCheckpointInputIndex < m_inputs.size()) {
        m_inputs.resize(m_lastCheckpointInputIndex);
        log::info("Discarded dead inputs. Remaining: {}", m_inputs.size());
    }
}

void MacroBot::revertInputsAfterCheckpoint(size_t checkpointIndex) {
    std::lock_guard<std::mutex> lock(m_inputMutex);

    if (checkpointIndex < m_inputs.size()) {
        m_inputs.resize(checkpointIndex);
        log::info("Reverted inputs to checkpoint index: {}", checkpointIndex);
    }
}

// ==================== CBF DETECTION ====================
CBFStatus MacroBot::detectCBFStatus() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastCBFCheck).count();

    if (elapsed < 5 && m_cachedCBFStatus != CBFStatus::NONE) {
        return m_cachedCBFStatus;
    }

    m_lastCBFCheck = now;

    if (utils::isModLoaded(CBF_MOD_ID)) {
        m_cachedCBFStatus = CBFStatus::SYZZI;
        return CBFStatus::SYZZI;
    }

    m_cachedCBFStatus = CBFStatus::NONE;
    return CBFStatus::NONE;
}

bool MacroBot::isCBFEnabled() const {
    CBFStatus status = const_cast<MacroBot*>(this)->detectCBFStatus();
    return status != CBFStatus::NONE;
}

bool MacroBot::isSyzziCBF() const {
    CBFStatus status = const_cast<MacroBot*>(this)->detectCBFStatus();
    return status == CBFStatus::SYZZI;
}

// ==================== SPEEDHACK ====================
void MacroBot::setSpeedhack(float speed) {
    m_speedhack.targetSpeed = std::clamp(speed, MIN_SPEEDHACK, MAX_SPEEDHACK);
    m_speedhack.enabled = (m_speedhack.targetSpeed != 1.0f);
    log::info("Speedhack set to: {}", m_speedhack.targetSpeed);
}

// ==================== FILE I/O ====================
bool MacroBot::saveMacro(const std::string& filename) {
    std::string dir = getMacrosDir();
    if (!utils::ensureDirectory(dir)) {
        log::error("Failed to create macros directory");
        return false;
    }

    std::string path = dir + "\\" + filename + ".cbfmacro";

    MacroFile file;
    file.version = "1.0";
    file.startTimeOffset = m_startTimeOffset;
    file.levelLength = m_currentTime;
    file.inputs = m_inputs;

    auto playLayer = PlayLayer::get();
    if (playLayer && playLayer->m_level) {
        file.levelID = playLayer->m_level->m_levelID;
        file.levelName = playLayer->m_level->m_levelName;
    }

    if (file.saveToFile(path)) {
        m_currentMacroName = filename;
        log::info("Macro saved to: {}", path);
        return true;
    }

    return false;
}

bool MacroBot::loadMacro(const std::string& filename) {
    std::string path = getMacrosDir() + "\\" + filename + ".cbfmacro";

    MacroFile file;
    if (!file.loadFromFile(path)) {
        log::error("Failed to load macro from: {}", path);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_inputs = file.inputs;
    m_startTimeOffset = file.startTimeOffset;
    m_currentMacroName = filename;

    log::info("Macro loaded: {} inputs from {}", m_inputs.size(), path);
    return true;
}

std::vector<std::string> MacroBot::getSavedMacros() {
    return utils::listFiles(getMacrosDir(), ".cbfmacro");
}

std::string MacroBot::getMacrosDir() {
    return Mod::get()->getSaveDir().string() + "\\macros";
}

// ==================== GUI ====================
void MacroBot::toggleGUI() {
    m_guiOpen = !m_guiOpen;
    if (m_guiOpen) {
        createGUI();
    } else {
        destroyGUI();
    }
}

void MacroBot::createGUI() {
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene) return;

    m_guiLayer = CCLayer::create();
    m_guiLayer->setZOrder(1000);

    auto bg = cocos2d::extension::CCScale9Sprite::create("square01_001.png");
    bg->setContentSize({300, 400});
    bg->setPosition({200, 150});
    bg->setOpacity(200);
    m_guiLayer->addChild(bg);

    auto title = CCLabelBMFont::create("CBF Macro Bot", "bigFont.fnt");
    title->setPosition({200, 320});
    title->setScale(0.5f);
    m_guiLayer->addChild(title);

    auto cbfLabel = CCLabelBMFont::create("CBF Status: ", "chatFont.fnt");
    cbfLabel->setPosition({120, 280});
    cbfLabel->setScale(0.6f);
    m_guiLayer->addChild(cbfLabel);

    m_cbfStatusLabel = CCLabelBMFont::create(".", "bigFont.fnt");
    m_cbfStatusLabel->setPosition({200, 280});
    m_cbfStatusLabel->setScale(0.8f);
    m_guiLayer->addChild(m_cbfStatusLabel);

    m_statusLabel = CCLabelBMFont::create("State: IDLE", "chatFont.fnt");
    m_statusLabel->setPosition({200, 250});
    m_statusLabel->setScale(0.5f);
    m_guiLayer->addChild(m_statusLabel);

    auto speedLabel = CCLabelBMFont::create("Speedhack:", "chatFont.fnt");
    speedLabel->setPosition({120, 220});
    speedLabel->setScale(0.5f);
    m_guiLayer->addChild(speedLabel);

    m_speedhackLabel = CCLabelBMFont::create("1.00x", "chatFont.fnt");
    m_speedhackLabel->setPosition({220, 220});
    m_speedhackLabel->setScale(0.5f);
    m_guiLayer->addChild(m_speedhackLabel);

    auto recordSprite = ButtonSprite::create("Record", 80, true, "bigFont.fnt", "GJ_button_01.png", 30, 1.0f);
    auto playSprite = ButtonSprite::create("Play", 80, true, "bigFont.fnt", "GJ_button_01.png", 30, 1.0f);

    auto recordBtn = CCMenuItemSpriteExtra::create(
        recordSprite, nullptr, nullptr
    );
    recordBtn->setPosition({150, 180});

    auto playBtn = CCMenuItemSpriteExtra::create(
        playSprite, nullptr, nullptr
    );
    playBtn->setPosition({250, 180});

    auto menu = CCMenu::create();
    menu->addChild(recordBtn);
    menu->addChild(playBtn);
    menu->setPosition({0, 0});
    m_guiLayer->addChild(menu);

    scene->addChild(m_guiLayer);
    updateStatusLabel();
}

void MacroBot::destroyGUI() {
    if (m_guiLayer) {
        m_guiLayer->removeFromParentAndCleanup(true);
        m_guiLayer = nullptr;
        m_statusLabel = nullptr;
        m_cbfStatusLabel = nullptr;
        m_speedhackLabel = nullptr;
    }
}

void MacroBot::updateStatusLabel() {
    if (!m_statusLabel) return;

    std::string stateStr;
    switch (m_state) {
        case BotState::IDLE: stateStr = "IDLE"; break;
        case BotState::RECORDING: stateStr = "RECORDING"; break;
        case BotState::PLAYING: stateStr = "PLAYING"; break;
        case BotState::PAUSED: stateStr = "PAUSED"; break;
    }

    m_statusLabel->setString(("State: " + stateStr).c_str());

    if (m_cbfStatusLabel) {
        CBFStatus status = detectCBFStatus();
        switch (status) {
            case CBFStatus::SYZZI:
                m_cbfStatusLabel->setColor({0, 255, 0});
                m_cbfStatusLabel->setString(". (Syzzi CBF - Infinite)");
                break;
            case CBFStatus::VANILLA:
                m_cbfStatusLabel->setColor({255, 255, 0});
                m_cbfStatusLabel->setString(". (RobTop CBS - 480 FPS)");
                break;
            case CBFStatus::NONE:
                m_cbfStatusLabel->setColor({255, 0, 0});
                m_cbfStatusLabel->setString(". (NO CBF - Bot Disabled)");
                break;
        }
    }

    if (m_speedhackLabel) {
        m_speedhackLabel->setString((std::to_string(m_speedhack.targetSpeed).substr(0, 4) + "x").c_str());
    }
}

void MacroBot::updateGUI() {
    updateStatusLabel();
}

void MacroBot::renderGUI() {
    updateGUI();
}

// ==================== INPUT COMPRESSION ====================
void MacroBot::compressInputs() {
    if (m_inputs.size() < 2) return;

    std::sort(m_inputs.begin(), m_inputs.end());

    auto last = std::unique(m_inputs.begin(), m_inputs.end());
    m_inputs.erase(last, m_inputs.end());

    std::vector<MacroInput> compressed;
    compressed.reserve(m_inputs.size());

    for (size_t i = 0; i < m_inputs.size(); i++) {
        if (i + 1 < m_inputs.size() && 
            std::abs(m_inputs[i].time - m_inputs[i+1].time) < TIME_PRECISION &&
            m_inputs[i].isPlayer1 == m_inputs[i+1].isPlayer1 &&
            m_inputs[i].isClick != m_inputs[i+1].isClick) {
            i++;
            continue;
        }
        compressed.push_back(m_inputs[i]);
    }

    m_inputs = std::move(compressed);
    log::info("Compressed inputs to {}", m_inputs.size());
}

void MacroBot::trimInputsToTime(double time) {
    auto it = std::remove_if(m_inputs.begin(), m_inputs.end(),
        [time](const MacroInput& input) {
            return input.time > time;
        });
    m_inputs.erase(it, m_inputs.end());
}

// ==================== CLEANUP ====================
void MacroBot::reset() {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_inputs.clear();
    m_inputsBackup.clear();
    m_playbackIndex = 0;
    m_currentTime = 0;
    m_startTimeOffset = 0;
    m_lastPlaybackTime = 0;
    m_diedSinceCheckpoint = false;
    m_lastCheckpointInputIndex = 0;
    clearCheckpoints();
    setState(BotState::IDLE);
}

void MacroBot::onLevelExit() {
    if (m_state == BotState::RECORDING) {
        stopRecording();
    }
    stopPlayback();
    reset();
    destroyGUI();
}

void MacroBot::onKeybindPressed() {
    // Placeholder
}

// ==================== UTILITY IMPLEMENTATIONS ====================
namespace bot::utils {

std::string formatTime(double time, int decimals) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(decimals) << time;
    return ss.str();
}

bool isModLoaded(const std::string& modID) {
    auto mod = Loader::get()->getLoadedMod(modID);
    return mod != nullptr && mod->isLoaded();
}

bool ensureDirectory(const std::string& path) {
    auto result = geode::utils::file::createDirectoryAll(path);
    return result.isOk();
}

std::vector<std::string> listFiles(const std::string& dir, const std::string& extension) {
    std::vector<std::string> files;
    if (!std::filesystem::exists(dir)) return files;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            files.push_back(entry.path().stem().string());
        }
    }
    return files;
}

void simulatePlayerClick(PlayerObject* player, bool isClick) {
    if (!player) return;
    if (isClick) {
        player->pushButton(PlayerButton::Jump);
    } else {
        player->releaseButton(PlayerButton::Jump);
    }
}

void simulatePushButton(int button, bool isPlayer1) {
    auto playLayer = PlayLayer::get();
    if (playLayer) {
        playLayer->handleButton(true, button, isPlayer1 ? 0 : 1);
    }
}

void simulateReleaseButton(int button, bool isPlayer1) {
    auto playLayer = PlayLayer::get();
    if (playLayer) {
        playLayer->handleButton(false, button, isPlayer1 ? 0 : 1);
    }
}

static cocos2d::CCPoint getPlayerVelocity(PlayerObject* player) {
    return ccp(0, 0);
}

static cocos2d::CCPoint getPlayerLastPosition(PlayerObject* player) {
    return ccp(0, 0);
}

CheckpointState capturePlayerState(PlayLayer* playLayer) {
    CheckpointState state{};
    if (!playLayer) return state;

    if (playLayer->m_player1) {
        auto p1 = playLayer->m_player1;
        state.p1_position = p1->getPosition();
        state.p1_rotation = p1->getRotation();
        state.p1_isHolding = p1->m_isHolding;
        state.p1_isOnGround = p1->m_isOnGround;
        state.p1_isDashing = p1->m_isDashing;
        state.p1_isUpsideDown = p1->m_isUpsideDown;
        state.p1_isDead = p1->m_isDead;
        state.p1_gameMode = 0;
        state.p1_speed = 1.0f;
        state.p1_scale = p1->getScale();
        state.p1_isFlipped = false;

        state.p1_velocity = getPlayerVelocity(p1);
        state.p1_lastPosition = getPlayerLastPosition(p1);
        state.p1_gravityScale = 1.0f;
    }

    if (playLayer->m_player2) {
        auto p2 = playLayer->m_player2;
        state.p2_position = p2->getPosition();
        state.p2_rotation = p2->getRotation();
        state.p2_isHolding = p2->m_isHolding;
        state.p2_isOnGround = p2->m_isOnGround;
        state.p2_isDashing = p2->m_isDashing;
        state.p2_isUpsideDown = p2->m_isUpsideDown;
        state.p2_isDead = p2->m_isDead;
        state.p2_gameMode = 0;
        state.p2_speed = 1.0f;
        state.p2_scale = p2->getScale();
        state.p2_isFlipped = false;

        state.p2_velocity = getPlayerVelocity(p2);
        state.p2_lastPosition = getPlayerLastPosition(p2);
        state.p2_gravityScale = 1.0f;
    }

    state.level_xPos = playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0;
    state.attemptCount = playLayer->m_attempts;
    state.isDualMode = false;
    state.isPlatformer = false;

    return state;
}

void restorePlayerState(PlayLayer* playLayer, const CheckpointState& state) {
    if (!playLayer) return;

    if (playLayer->m_player1) {
        auto p1 = playLayer->m_player1;
        p1->setPosition(state.p1_position);
        p1->setRotation(state.p1_rotation);
        p1->m_isHolding = state.p1_isHolding;
        p1->m_isOnGround = state.p1_isOnGround;
        p1->m_isDashing = state.p1_isDashing;
        p1->m_isUpsideDown = state.p1_isUpsideDown;
        p1->m_isDead = state.p1_isDead;
        p1->setScale(state.p1_scale);
    }

    if (playLayer->m_player2) {
        auto p2 = playLayer->m_player2;
        p2->setPosition(state.p2_position);
        p2->setRotation(state.p2_rotation);
        p2->m_isHolding = state.p2_isHolding;
        p2->m_isOnGround = state.p2_isOnGround;
        p2->m_isDashing = state.p2_isDashing;
        p2->m_isUpsideDown = state.p2_isUpsideDown;
        p2->m_isDead = state.p2_isDead;
        p2->setScale(state.p2_scale);
    }
}

} // namespace bot::utils

// ==================== GEODE HOOKS ====================

class $modify(PlayLayerHook, PlayLayer) {
    struct Fields {
        bool m_isRecording = false;
        bool m_isPlaying = false;
        double m_levelTime = 0.0;
        float m_accumulatedDelta = 0.0f;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto& bot = MacroBot::getInstance();
        bot.reset();

        CBFStatus status = bot.detectCBFStatus();
        if (status == CBFStatus::NONE) {
            log::warn("CBF not detected! Bot functionality disabled.");
        } else {
            log::info("CBF detected: {}", status == CBFStatus::SYZZI ? "Syzzi (Infinite)" : "RobTop (480 FPS)");
        }

        return true;
    }

    void update(float dt) {
        auto& bot = MacroBot::getInstance();

        auto& speedhack = bot.getSpeedhackState();
        speedhack.update(dt);
        float speedMultiplier = speedhack.getDeltaMultiplier();

        float modifiedDt = dt * speedMultiplier;

        m_fields->m_levelTime += modifiedDt;
        bot.m_currentTime = m_fields->m_levelTime;

        PlayLayer::update(modifiedDt);

        if (bot.getState() == BotState::PLAYING) {
            bot.updatePlayback(m_fields->m_levelTime);
        }

        if (bot.isGUIOpen()) {
            bot.renderGUI();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);

        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            bot.onPlayerDeath();
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        auto& bot = MacroBot::getInstance();
        m_fields->m_levelTime = 0.0;
        bot.m_currentTime = 0.0;

        if (bot.getState() == BotState::PLAYING) {
            bot.onRestartFromBeginning();
        }
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);

        auto& bot = MacroBot::getInstance();
        bot.loadCheckpoint(this, checkpoint);
        bot.onRestartFromCheckpoint();
    }

    void removeAllCheckpoints() {
        PlayLayer::removeAllCheckpoints();

        auto& bot = MacroBot::getInstance();
        bot.clearCheckpoints();
    }

    CheckpointObject* createCheckpoint() {
        auto checkpoint = PlayLayer::createCheckpoint();

        if (checkpoint) {
            auto& bot = MacroBot::getInstance();
            bot.saveCheckpoint(this);

            auto state = bot::utils::capturePlayerState(this);
            state.inputIndex = bot.getInputs().size();
            state.level_time = static_cast<float>(m_fields->m_levelTime);
            bot.m_checkpointStates[checkpoint] = state;
            bot.m_checkpointOrder.push_back(checkpoint);
        }

        return checkpoint;
    }

    void onQuit() {
        auto& bot = MacroBot::getInstance();
        bot.onLevelExit();

        PlayLayer::onQuit();
    }
};

class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(PlayerButton button) {
        PlayerObject::pushButton(button);

        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                bool isP1 = (this == playLayer->m_player1);
                double time = bot.getCurrentLevelTime();
                bot.recordInput(isP1, true, true, time);
            }
        }
    }

    void releaseButton(PlayerButton button) {
        PlayerObject::releaseButton(button);

        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                bool isP1 = (this == playLayer->m_player1);
                double time = bot.getCurrentLevelTime();
                bot.recordInput(isP1, false, false, time);
            }
        }
    }
};

class $modify(PauseLayerHook, PauseLayer) {
    void onRestart(cocos2d::CCObject* sender) {
        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            bot.onPauseRestart();
        }

        PauseLayer::onRestart(sender);
    }

    void onQuit(cocos2d::CCObject* sender) {
        auto& bot = MacroBot::getInstance();
        bot.onLevelExit();

        PauseLayer::onQuit(sender);
    }
};

class $modify(CCDirectorHook, CCDirector) {
    void setDeltaTime(float dt) {
        auto& bot = MacroBot::getInstance();
        auto& speedhack = bot.getSpeedhackState();

        if (speedhack.enabled) {
            float modifiedDt = dt * speedhack.getDeltaMultiplier();
            CCDirector::setDeltaTime(modifiedDt);
        } else {
            CCDirector::setDeltaTime(dt);
        }
    }
};

class $modify(CCKeyboardDispatcherHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isPressed, bool isRepeat) {
        if (key == cocos2d::KEY_K && isPressed && !isRepeat) {
            auto& bot = MacroBot::getInstance();
            bot.toggleGUI();
            return true;
        }

        if (key == cocos2d::KEY_R && isPressed && !isRepeat) {
            auto& bot = MacroBot::getInstance();
            if (bot.getState() == BotState::RECORDING) {
                bot.stopRecording();
            } else {
                bot.startRecording();
            }
            return true;
        }

        if (key == cocos2d::KEY_P && isPressed && !isRepeat) {
            auto& bot = MacroBot::getInstance();
            if (bot.getState() == BotState::PLAYING) {
                bot.stopPlayback();
            } else {
                bot.startPlayback();
            }
            return true;
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isPressed, isRepeat);
    }
};

$execute {
    log::info("CBF Macro Bot loaded!");
    log::info("Keybinds: K = GUI, R = Record, P = Play");

    auto& bot = MacroBot::getInstance();
    CBFStatus status = bot.detectCBFStatus();

    switch (status) {
        case CBFStatus::SYZZI:
            log::info("Syzzi's CBF detected - Full functionality available");
            break;
        case CBFStatus::VANILLA:
            log::info("RobTop's CBS detected - Limited to 480 FPS precision");
            break;
        case CBFStatus::NONE:
            log::warn("No CBF detected - Bot will not function without CBF");
            break;
    }
}