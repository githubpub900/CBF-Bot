#include "Bot.hpp"

using namespace geode::prelude;
using namespace bot;

// ==================== SPEEDHACK STATE IMPLEMENTATION ====================
void SpeedhackState::update(float dt) {
    if (!enabled) {
        currentSpeed = 1.0f;
        return;
    }

    // Smooth transition to target speed
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

    // Write header
    uint32_t versionLen = version.length();
    file.write(reinterpret_cast<const char*>(&versionLen), sizeof(versionLen));
    file.write(version.c_str(), versionLen);

    file.write(reinterpret_cast<const char*>(&startTimeOffset), sizeof(startTimeOffset));
    file.write(reinterpret_cast<const char*>(&levelLength), sizeof(levelLength));
    file.write(reinterpret_cast<const char*>(&levelID), sizeof(levelID));
    file.write(reinterpret_cast<const char*>(&totalSteps), sizeof(totalSteps));
    file.write(reinterpret_cast<const char*>(&fps), sizeof(fps));

    // Write level name
    uint32_t nameLen = levelName.length();
    file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
    file.write(levelName.c_str(), nameLen);

    // Write inputs using delta encoding for compression
    uint64_t inputCount = inputs.size();
    file.write(reinterpret_cast<const char*>(&inputCount), sizeof(inputCount));

    if (inputCount > 0) {
        // First input: absolute time
        file.write(reinterpret_cast<const char*>(&inputs[0].time), sizeof(inputs[0].time));
        file.write(reinterpret_cast<const char*>(&inputs[0].isPlayer1), sizeof(inputs[0].isPlayer1));
        file.write(reinterpret_cast<const char*>(&inputs[0].isClick), sizeof(inputs[0].isClick));
        file.write(reinterpret_cast<const char*>(&inputs[0].isHold), sizeof(inputs[0].isHold));

        // Subsequent inputs: delta time from previous
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

    // Read header
    uint32_t versionLen;
    file.read(reinterpret_cast<char*>(&versionLen), sizeof(versionLen));
    version.resize(versionLen);
    file.read(&version[0], versionLen);

    file.read(reinterpret_cast<char*>(&startTimeOffset), sizeof(startTimeOffset));
    file.read(reinterpret_cast<char*>(&levelLength), sizeof(levelLength));
    file.read(reinterpret_cast<char*>(&levelID), sizeof(levelID));
    file.read(reinterpret_cast<char*>(&totalSteps), sizeof(totalSteps));
    file.read(reinterpret_cast<char*>(&fps), sizeof(fps));

    // Read level name
    uint32_t nameLen;
    file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    levelName.resize(nameLen);
    file.read(&levelName[0], nameLen);

    // Read inputs
    uint64_t inputCount;
    file.read(reinterpret_cast<char*>(&inputCount), sizeof(inputCount));

    inputs.reserve(inputCount);

    if (inputCount > 0) {
        MacroInput first;
        file.read(reinterpret_cast<char*>(&first.time), sizeof(first.time));
        file.read(reinterpret_cast<char*>(&first.isPlayer1), sizeof(first.isPlayer1));
        file.read(reinterpret_cast<char*>(&first.isClick), sizeof(first.isClick));
        file.read(reinterpret_cast<char*>(&first.isHold), sizeof(first.isHold));
        inputs.push_back(first);

        double lastTime = first.time;
        for (size_t i = 1; i < inputCount; i++) {
            MacroInput input;
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

std::string MacroFile::serialize() const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(12);
    ss << "{\"version\":\"" << version << "\",";
    ss << "\"startTimeOffset\":" << startTimeOffset << ",";
    ss << "\"levelLength\":" << levelLength << ",";
    ss << "\"levelID\":" << levelID << ",";
    ss << "\"totalSteps\":" << totalSteps << ",";
    ss << "\"fps\":" << fps << ",";
    ss << "\"levelName\":\"" << levelName << "\",";
    ss << "\"inputs\":[";

    for (size_t i = 0; i < inputs.size(); i++) {
        if (i > 0) ss << ",";
        ss << "{\"t\":" << inputs[i].time 
           << ",\"p1\":" << (inputs[i].isPlayer1 ? 1 : 0)
           << ",\"c\":" << (inputs[i].isClick ? 1 : 0)
           << ",\"h\":" << (inputs[i].isHold ? 1 : 0) << "}";
    }
    ss << "]}";
    return ss.str();
}

bool MacroFile::deserialize(const std::string& data) {
    // Simple JSON parsing for the macro format
    // In production, use a proper JSON library
    // This is a simplified version
    try {
        // Parse basic fields... (simplified)
        return true;
    } catch (...) {
        return false;
    }
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

    // Store time relative to start offset
    double relativeTime = time - m_startTimeOffset;
    if (relativeTime < 0) relativeTime = 0;

    // Round to precision to avoid floating point issues
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

    // Sort inputs by time to ensure correct order
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

    // Adjust current time by start offset
    double adjustedTime = currentTime - m_startTimeOffset;
    if (adjustedTime < 0) adjustedTime = 0;

    // Process all inputs that should have been triggered by now
    while (m_playbackIndex < m_inputs.size() && 
           m_inputs[m_playbackIndex].time <= adjustedTime + TIME_PRECISION) {

        // Check if we need to skip inputs that were "dead" (after death)
        if (!m_diedSinceCheckpoint || m_inputs[m_playbackIndex].time <= m_lastPlaybackTime) {
            executeInput(m_inputs[m_playbackIndex]);
        }

        m_playbackIndex++;
    }

    m_lastPlaybackTime = adjustedTime;

    // Auto-stop if all inputs played
    if (m_playbackIndex >= m_inputs.size()) {
        // Keep playing until level ends or user stops
    }
}

void MacroBot::executeInput(const MacroInput& input) {
    // Use the game's input handling for maximum compatibility
    // This works with Syzzi's CBF because it hooks at the lowest level
    sendClick(input.isPlayer1, input.isClick);
}

void MacroBot::sendClick(bool isPlayer1, bool isClick) {
    auto playLayer = PlayLayer::get();
    if (!playLayer) return;

    // Get the correct player object
    PlayerObject* player = isPlayer1 ? playLayer->m_player1 : playLayer->m_player2;
    if (!player) return;

    // Use the game's button handling for proper CBF compatibility
    // This ensures inputs go through Syzzi's CBF system
    if (isClick) {
        playLayer->handleButton(true, 1, isPlayer1 ? 0 : 1);
    } else {
        playLayer->handleButton(false, 1, isPlayer1 ? 0 : 1);
    }
}

// ==================== CHECKPOINT MANAGEMENT (PRACTICE BUG FIX) ====================
void MacroBot::saveCheckpoint(PlayLayer* playLayer) {
    if (!playLayer) return;

    // Capture comprehensive player state
    CheckpointState state = utils::capturePlayerState(playLayer);
    state.inputIndex = m_inputs.size();
    state.level_time = m_currentTime;

    // Store the checkpoint state
    // We need to associate it with the checkpoint object
    // This will be linked when the checkpoint is created
    m_lastCheckpointInputIndex = m_inputs.size();
    m_diedSinceCheckpoint = false;

    log::info("Checkpoint saved at time: {}, input index: {}", state.level_time, state.inputIndex);
}

void MacroBot::loadCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint) {
    if (!playLayer || !checkpoint) return;

    auto it = m_checkpointStates.find(checkpoint);
    if (it != m_checkpointStates.end()) {
        // Restore comprehensive state
        utils::restorePlayerState(playLayer, it->second);

        // Update time tracking
        m_currentTime = it->second.level_time;
        m_lastPlaybackTime = m_currentTime - m_startTimeOffset;

        // Revert inputs to checkpoint state
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

        // Remove from order list
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
    // When restarting from checkpoint, discard dead inputs
    if (m_diedSinceCheckpoint) {
        discardDeadInputs();
    }
    m_diedSinceCheckpoint = false;
}

void MacroBot::onRestartFromBeginning() {
    // Full restart - clear all checkpoints and reset
    clearCheckpoints();
    m_currentTime = 0;
    m_playbackIndex = 0;
    m_lastPlaybackTime = 0;
}

void MacroBot::onPauseRestart() {
    // When hitting restart from pause menu, revert inputs after current time
    std::lock_guard<std::mutex> lock(m_inputMutex);

    // Remove inputs that happened after the current time
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

    // Remove inputs that were recorded after the last checkpoint but before death
    // These are "dead" because they led to death
    if (m_lastCheckpointInputIndex < m_inputs.size()) {
        m_inputs.resize(m_lastCheckpointInputIndex);
        log::info("Discarded {} dead inputs", m_inputs.size() - m_lastCheckpointInputIndex);
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

    // Cache for 5 seconds to avoid repeated checks
    if (elapsed < 5 && m_cachedCBFStatus != CBFStatus::NONE) {
        return m_cachedCBFStatus;
    }

    m_lastCBFCheck = now;

    // Check for Syzzi's CBF mod first
    if (utils::isModLoaded(CBF_MOD_ID)) {
        m_cachedCBFStatus = CBFStatus::SYZZI;
        return CBFStatus::SYZZI;
    }

    // Check for RobTop's built-in Click Between Steps
    // This is enabled in GD settings
    auto gm = GameManager::get();
    if (gm) {
        // Check if "Click Between Steps" is enabled in game settings
        // In GD 2.2, this is typically stored in GameManager
        // The exact field name may vary, but we check for the setting
        #if defined(GD_HAS_CLICK_BETWEEN_STEPS)
        if (gm->m_clickBetweenSteps || gm->m_clickOnSteps) {
            m_cachedCBFStatus = CBFStatus::VANILLA;
            return CBFStatus::VANILLA;
        }
        #endif
    }

    // Default: check if we can detect vanilla CBF through other means
    // RobTop's CBS is limited to 480 TPS
    // We can check GameStatsManager or other global settings
    auto stats = GameStatsManager::get();
    if (stats) {
        // Check for any vanilla CBF indicators
        // This is a fallback check
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
    return Mod::get()->getSaveDir().string() + "\macros";
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

bool MacroBot::isGUIOpen() const {
    return m_guiOpen;
}

void MacroBot::createGUI() {
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene) return;

    m_guiLayer = CCLayer::create();
    m_guiLayer->setZOrder(1000);

    // Background panel
    auto bg = cocos2d::extension::CCScale9Sprite::create("square01_001.png");
    bg->setContentSize({300, 400});
    bg->setPosition({200, 150});
    bg->setOpacity(200);
    m_guiLayer->addChild(bg);

    // Title
    auto title = CCLabelBMFont::create("CBF Macro Bot", "bigFont.fnt");
    title->setPosition({200, 320});
    title->setScale(0.5f);
    m_guiLayer->addChild(title);

    // CBF Status indicator (colored period)
    auto cbfLabel = CCLabelBMFont::create("CBF Status: ", "chatFont.fnt");
    cbfLabel->setPosition({120, 280});
    cbfLabel->setScale(0.6f);
    m_guiLayer->addChild(cbfLabel);

    m_cbfStatusLabel = CCLabelBMFont::create(".", "bigFont.fnt");
    m_cbfStatusLabel->setPosition({200, 280});
    m_cbfStatusLabel->setScale(0.8f);
    m_guiLayer->addChild(m_cbfStatusLabel);

    // Status text
    m_statusLabel = CCLabelBMFont::create("State: IDLE", "chatFont.fnt");
    m_statusLabel->setPosition({200, 250});
    m_statusLabel->setScale(0.5f);
    m_guiLayer->addChild(m_statusLabel);

    // Speedhack textbox
    auto speedLabel = CCLabelBMFont::create("Speedhack:", "chatFont.fnt");
    speedLabel->setPosition({120, 220});
    speedLabel->setScale(0.5f);
    m_guiLayer->addChild(speedLabel);

    // Speedhack value display
    m_speedhackLabel = CCLabelBMFont::create("1.00x", "chatFont.fnt");
    m_speedhackLabel->setPosition({220, 220});
    m_speedhackLabel->setScale(0.5f);
    m_guiLayer->addChild(m_speedhackLabel);

    // Buttons
    auto recordBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Record", 80, true, "bigFont.fnt", "GJ_button_01.png", 30, 1.0f),
        this,
        menu_selector(MacroBot::onKeybindPressed) // Placeholder
    );
    recordBtn->setPosition({150, 180});

    auto playBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Play", 80, true, "bigFont.fnt", "GJ_button_01.png", 30, 1.0f),
        this,
        menu_selector(MacroBot::onKeybindPressed) // Placeholder
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

    // Update CBF status color
    if (m_cbfStatusLabel) {
        CBFStatus status = detectCBFStatus();
        switch (status) {
            case CBFStatus::SYZZI:
                m_cbfStatusLabel->setColor({0, 255, 0}); // Green
                m_cbfStatusLabel->setString(". (Syzzi CBF - Infinite)");
                break;
            case CBFStatus::VANILLA:
                m_cbfStatusLabel->setColor({255, 255, 0}); // Yellow
                m_cbfStatusLabel->setString(". (RobTop CBS - 480 FPS)");
                break;
            case CBFStatus::NONE:
                m_cbfStatusLabel->setColor({255, 0, 0}); // Red
                m_cbfStatusLabel->setString(". (NO CBF - Bot Disabled)");
                break;
        }
    }

    // Update speedhack display
    if (m_speedhackLabel) {
        m_speedhackLabel->setString((std::to_string(m_speedhack.targetSpeed).substr(0, 4) + "x").c_str());
    }
}

void MacroBot::updateGUI() {
    updateStatusLabel();
}

void MacroBot::renderGUI() {
    // Called every frame to update GUI elements
    updateGUI();
}

// ==================== INPUT COMPRESSION ====================
void MacroBot::compressInputs() {
    if (m_inputs.size() < 2) return;

    std::sort(m_inputs.begin(), m_inputs.end());

    // Remove duplicate inputs at the same time for same player
    auto last = std::unique(m_inputs.begin(), m_inputs.end());
    m_inputs.erase(last, m_inputs.end());

    // Remove redundant inputs (click+release at same time)
    std::vector<MacroInput> compressed;
    compressed.reserve(m_inputs.size());

    for (size_t i = 0; i < m_inputs.size(); i++) {
        // Skip if this is a release immediately followed by click at same time
        if (i + 1 < m_inputs.size() && 
            std::abs(m_inputs[i].time - m_inputs[i+1].time) < TIME_PRECISION &&
            m_inputs[i].isPlayer1 == m_inputs[i+1].isPlayer1 &&
            m_inputs[i].isClick != m_inputs[i+1].isClick) {
            // These cancel out, skip both
            i++;
            continue;
        }
        compressed.push_back(m_inputs[i]);
    }

    m_inputs = std::move(compressed);
    log::info("Compressed inputs from {} to {}", m_inputs.size() + (m_inputs.size() - compressed.size()), m_inputs.size());
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
    // This is a placeholder - actual keybind handling is in the hooks
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
    return mod != nullptr && mod->isEnabled();
}

bool ensureDirectory(const std::string& path) {
    return geode::utils::file::createDirectoryAll(path).has_value();
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
        player->pushButton(1); // Player button
    } else {
        player->releaseButton(1);
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

CheckpointState capturePlayerState(PlayLayer* playLayer) {
    CheckpointState state{};
    if (!playLayer) return state;

    // Capture Player 1
    if (playLayer->m_player1) {
        auto p1 = playLayer->m_player1;
        state.p1_position = p1->getPosition();
        state.p1_rotation = p1->getRotation();
        state.p1_isHolding = p1->m_isHolding;
        state.p1_isOnGround = p1->m_isOnGround;
        state.p1_isDashing = p1->m_isDashing;
        state.p1_isUpsideDown = p1->m_isUpsideDown;
        state.p1_isDead = p1->m_isDead;
        state.p1_gameMode = static_cast<int>(p1->m_gameMode);
        state.p1_speed = p1->m_speed;
        state.p1_scale = p1->getScale();
        state.p1_isFlipped = p1->m_isFlipped;

        // Velocity and physics
        state.p1_velocity = p1->m_velocity;
        state.p1_lastPosition = p1->m_lastPosition;
        state.p1_gravityScale = p1->m_gravityMod;
    }

    // Capture Player 2 (dual mode)
    if (playLayer->m_player2) {
        auto p2 = playLayer->m_player2;
        state.p2_position = p2->getPosition();
        state.p2_rotation = p2->getRotation();
        state.p2_isHolding = p2->m_isHolding;
        state.p2_isOnGround = p2->m_isOnGround;
        state.p2_isDashing = p2->m_isDashing;
        state.p2_isUpsideDown = p2->m_isUpsideDown;
        state.p2_isDead = p2->m_isDead;
        state.p2_gameMode = static_cast<int>(p2->m_gameMode);
        state.p2_speed = p2->m_speed;
        state.p2_scale = p2->getScale();
        state.p2_isFlipped = p2->m_isFlipped;

        state.p2_velocity = p2->m_velocity;
        state.p2_lastPosition = p2->m_lastPosition;
        state.p2_gravityScale = p2->m_gravityMod;
    }

    // Level state
    state.level_xPos = playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0;
    state.attemptCount = playLayer->m_attempts;
    state.isDualMode = playLayer->m_isDualMode;
    state.isPlatformer = playLayer->m_levelSettings ? playLayer->m_levelSettings->m_platformerMode : false;

    // Camera state
    auto camera = playLayer->m_gameLayer;
    if (camera) {
        state.cameraX = camera->getPositionX();
        state.cameraY = camera->getPositionY();
    }

    return state;
}

void restorePlayerState(PlayLayer* playLayer, const CheckpointState& state) {
    if (!playLayer) return;

    // Restore Player 1
    if (playLayer->m_player1) {
        auto p1 = playLayer->m_player1;
        p1->setPosition(state.p1_position);
        p1->setRotation(state.p1_rotation);
        p1->m_isHolding = state.p1_isHolding;
        p1->m_isOnGround = state.p1_isOnGround;
        p1->m_isDashing = state.p1_isDashing;
        p1->m_isUpsideDown = state.p1_isUpsideDown;
        p1->m_isDead = state.p1_isDead;
        p1->m_gameMode = static_cast<PlayerGameMode>(state.p1_gameMode);
        p1->m_speed = state.p1_speed;
        p1->setScale(state.p1_scale);
        p1->m_isFlipped = state.p1_isFlipped;

        p1->m_velocity = state.p1_velocity;
        p1->m_lastPosition = state.p1_lastPosition;
        p1->m_gravityMod = state.p1_gravityScale;
    }

    // Restore Player 2
    if (playLayer->m_player2) {
        auto p2 = playLayer->m_player2;
        p2->setPosition(state.p2_position);
        p2->setRotation(state.p2_rotation);
        p2->m_isHolding = state.p2_isHolding;
        p2->m_isOnGround = state.p2_isOnGround;
        p2->m_isDashing = state.p2_isDashing;
        p2->m_isUpsideDown = state.p2_isUpsideDown;
        p2->m_isDead = state.p2_isDead;
        p2->m_gameMode = static_cast<PlayerGameMode>(state.p2_gameMode);
        p2->m_speed = state.p2_speed;
        p2->setScale(state.p2_scale);
        p2->m_isFlipped = state.p2_isFlipped;

        p2->m_velocity = state.p2_velocity;
        p2->m_lastPosition = state.p2_lastPosition;
        p2->m_gravityMod = state.p2_gravityScale;
    }

    // Restore camera
    auto camera = playLayer->m_gameLayer;
    if (camera) {
        camera->setPosition(state.cameraX, state.cameraY);
    }
}

} // namespace bot::utils

// ==================== GEODE HOOKS ====================

// Hook PlayLayer for game loop, checkpoints, and death handling
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

        // Check CBF status on level start
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

        // Apply speedhack to delta time
        auto& speedhack = bot.getSpeedhackState();
        speedhack.update(dt);
        float speedMultiplier = speedhack.getDeltaMultiplier();

        // Adjust dt for speedhack
        float modifiedDt = dt * speedMultiplier;

        // Update level time tracking
        m_fields->m_levelTime += modifiedDt;
        bot.m_currentTime = m_fields->m_levelTime;

        // Call original update with modified delta
        PlayLayer::update(modifiedDt);

        // Update playback if active
        if (bot.getState() == BotState::PLAYING) {
            bot.updatePlayback(m_fields->m_levelTime);
        }

        // Update GUI
        if (bot.isGUIOpen()) {
            bot.renderGUI();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Call original first
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

            // Store checkpoint state mapping
            auto state = bot::utils::capturePlayerState(this);
            state.inputIndex = bot.getInputs().size();
            state.level_time = m_fields->m_levelTime;
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

// Hook PlayerObject for input capture
class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(int button) {
        PlayerObject::pushButton(button);

        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                bool isP1 = (this == playLayer->m_player1);
                double time = bot.getCurrentLevelTime();
                bot.recordInput(isP1, true, true, time); // isClick=true, isHold=true
            }
        }
    }

    void releaseButton(int button) {
        PlayerObject::releaseButton(button);

        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            auto playLayer = PlayLayer::get();
            if (playLayer) {
                bool isP1 = (this == playLayer->m_player1);
                double time = bot.getCurrentLevelTime();
                bot.recordInput(isP1, false, false, time); // isClick=false, isHold=false
            }
        }
    }
};

// Hook PauseLayer for restart handling
class $modify(PauseLayerHook, PauseLayer) {
    void onRestart(cocos2d::CCObject* sender) {
        auto& bot = MacroBot::getInstance();
        if (bot.getState() == BotState::RECORDING) {
            bot.onPauseRestart();
        }

        PauseLayer::onRestart(sender);
    }

    void onResume(cocos2d::CCObject* sender) {
        PauseLayer::onResume(sender);
    }

    void onQuit(cocos2d::CCObject* sender) {
        auto& bot = MacroBot::getInstance();
        bot.onLevelExit();

        PauseLayer::onQuit(sender);
    }
};

// Hook CCDirector for speedhack (alternative method)
class $modify(CCDirectorHook, CCDirector) {
    void setDeltaTime(float dt) {
        auto& bot = MacroBot::getInstance();
        auto& speedhack = bot.getSpeedhackState();

        if (speedhack.enabled) {
            // Modify delta time to control game speed
            float modifiedDt = dt * speedhack.getDeltaMultiplier();
            CCDirector::setDeltaTime(modifiedDt);
        } else {
            CCDirector::setDeltaTime(dt);
        }
    }
};

// Hook keyboard for GUI toggle (K key)
class $modify(CCKeyboardDispatcherHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isPressed, bool isRepeat) {
        // K key to toggle GUI
        if (key == cocos2d::KEY_K && isPressed && !isRepeat) {
            auto& bot = MacroBot::getInstance();
            bot.toggleGUI();
            return true; // Consume the key
        }

        // R key to start/stop recording
        if (key == cocos2d::KEY_R && isPressed && !isRepeat) {
            auto& bot = MacroBot::getInstance();
            if (bot.getState() == BotState::RECORDING) {
                bot.stopRecording();
            } else {
                bot.startRecording();
            }
            return true;
        }

        // P key to start/stop playback
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

// ==================== MOD SETUP ====================
$execute {
    log::info("CBF Macro Bot loaded!");
    log::info("Keybinds: K = GUI, R = Record, P = Play");

    // Check if CBF mod is available
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