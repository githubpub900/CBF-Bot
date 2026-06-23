#pragma once

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/ButtonSprite.hpp>

#include <vector>
#include <deque>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>
#include <filesystem>

using namespace geode::prelude;

namespace bot {

// ==================== CONSTANTS ====================
constexpr const char* CBF_MOD_ID = "syzzi.click_between_frames";
constexpr const char* MOD_ID = "cbf.macrobot";
constexpr double TIME_PRECISION = 1e-12;
constexpr double MAX_TIME_VALUE = 1e6;
constexpr float DEFAULT_SPEEDHACK = 1.0f;
constexpr float MIN_SPEEDHACK = 0.01f;
constexpr float MAX_SPEEDHACK = 100.0f;

// ==================== INPUT STRUCTURE ====================
struct MacroInput {
    double time;
    bool isPlayer1;
    bool isClick;
    bool isHold;

    bool operator<(const MacroInput& other) const {
        return time < other.time;
    }
    bool operator==(const MacroInput& other) const {
        return std::abs(time - other.time) < TIME_PRECISION && 
               isPlayer1 == other.isPlayer1 && 
               isClick == other.isClick;
    }
};

// ==================== CHECKPOINT STATE ====================
// Note: m_isHolding is not exposed in Geode bindings for 2.2081
// so we track what we can and use handleButton for input restoration
struct CheckpointState {
    cocos2d::CCPoint p1_position;
    cocos2d::CCPoint p1_lastPosition;
    cocos2d::CCPoint p1_velocity;
    float p1_rotation;
    float p1_rotationSpeed;
    bool p1_isOnGround;
    bool p1_isDashing;
    bool p1_isUpsideDown;
    bool p1_isDead;
    int p1_gameMode;
    float p1_gravityScale;
    float p1_speed;
    float p1_scale;
    bool p1_isFlipped;

    cocos2d::CCPoint p2_position;
    cocos2d::CCPoint p2_lastPosition;
    cocos2d::CCPoint p2_velocity;
    float p2_rotation;
    float p2_rotationSpeed;
    bool p2_isOnGround;
    bool p2_isDashing;
    bool p2_isUpsideDown;
    bool p2_isDead;
    int p2_gameMode;
    float p2_gravityScale;
    float p2_speed;
    float p2_scale;
    bool p2_isFlipped;

    float level_time;
    float level_xPos;
    int attemptCount;

    float cameraX;
    float cameraY;
    float cameraZoom;
    bool isDualMode;
    bool isPlatformer;

    size_t inputIndex;
};

// ==================== MACRO FILE FORMAT ====================
struct MacroFile {
    std::string version = "1.0";
    double startTimeOffset = 0.0;
    double levelLength = 0.0;
    int levelID = 0;
    std::string levelName;
    std::vector<MacroInput> inputs;
    uint64_t totalSteps = 0;
    float fps = 0.0f;

    bool saveToFile(const std::string& path);
    bool loadFromFile(const std::string& path);
};

// ==================== CBF STATUS ====================
enum class CBFStatus {
    NONE,
    VANILLA,
    SYZZI
};

// ==================== BOT STATE ====================
enum class BotState {
    IDLE,
    RECORDING,
    PLAYING,
    PAUSED
};

// ==================== SPEEDHACK ====================
struct SpeedhackState {
    float targetSpeed = 1.0f;
    float currentSpeed = 1.0f;
    bool enabled = false;
    float transitionSpeed = 5.0f;

    void update(float dt);
    float getDeltaMultiplier() const;
};

// ==================== MAIN BOT CLASS ====================
class MacroBot {
public:
    static MacroBot& getInstance();

    void setState(BotState state);
    BotState getState() const { return m_state; }

    void startRecording();
    void stopRecording();
    void recordInput(bool isPlayer1, bool isClick, bool isHold, double time);

    void startPlayback();
    void stopPlayback();
    void updatePlayback(double currentTime);

    void saveCheckpoint(PlayLayer* playLayer);
    void loadCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint);
    void removeCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint);
    void clearCheckpoints();

    void onPlayerDeath();
    void onRestartFromCheckpoint();
    void onRestartFromBeginning();
    void onPauseRestart();
    void discardDeadInputs();
    void revertInputsAfterCheckpoint(size_t checkpointIndex);

    CBFStatus detectCBFStatus();
    bool isCBFEnabled() const;
    bool isSyzziCBF() const;

    void setSpeedhack(float speed);
    float getSpeedhack() const { return m_speedhack.targetSpeed; }
    SpeedhackState& getSpeedhackState() { return m_speedhack; }

    bool saveMacro(const std::string& filename);
    bool loadMacro(const std::string& filename);
    std::vector<std::string> getSavedMacros();

    void toggleGUI();
    bool isGUIOpen() const { return m_guiOpen; }
    void updateGUI();
    void renderGUI();

    double getCurrentLevelTime() const { return m_currentTime; }
    void setStartTimeOffset(double offset) { m_startTimeOffset = offset; }
    double getStartTimeOffset() const { return m_startTimeOffset; }

    const std::vector<MacroInput>& getInputs() const { return m_inputs; }
    size_t getInputCount() const { return m_inputs.size(); }

    void setShowTrajectory(bool show) { m_showTrajectory = show; }
    bool getShowTrajectory() const { return m_showTrajectory; }

    void setAutoSave(bool autoSave) { m_autoSave = autoSave; }
    bool getAutoSave() const { return m_autoSave; }

    void reset();
    void onLevelExit();
    void onKeybindPressed();

    // Public for hooks
    std::map<CheckpointObject*, CheckpointState> m_checkpointStates;
    std::vector<CheckpointObject*> m_checkpointOrder;
    double m_currentTime = 0.0;

private:
    MacroBot() = default;
    ~MacroBot() = default;
    MacroBot(const MacroBot&) = delete;
    MacroBot& operator=(const MacroBot&) = delete;

    std::atomic<BotState> m_state{BotState::IDLE};
    std::atomic<bool> m_guiOpen{false};

    std::vector<MacroInput> m_inputs;
    std::vector<MacroInput> m_inputsBackup;
    std::mutex m_inputMutex;

    size_t m_playbackIndex = 0;
    double m_startTimeOffset = 0.0;
    double m_lastPlaybackTime = 0.0;

    size_t m_lastCheckpointInputIndex = 0;
    bool m_diedSinceCheckpoint = false;

    SpeedhackState m_speedhack;

    bool m_showTrajectory = false;
    bool m_autoSave = true;
    std::string m_currentMacroName;

    CBFStatus m_cachedCBFStatus = CBFStatus::NONE;
    std::chrono::steady_clock::time_point m_lastCBFCheck;

    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_cbfStatusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_speedhackLabel = nullptr;
    cocos2d::CCLayer* m_guiLayer = nullptr;

    void executeInput(const MacroInput& input);
    void sendClick(bool isPlayer1, bool isClick);
    void updateStatusLabel();
    void createGUI();
    void destroyGUI();
    std::string getMacrosDir();
    void compressInputs();
    void trimInputsToTime(double time);
};

// ==================== UTILITY FUNCTIONS ====================
namespace utils {
    std::string formatTime(double time, int decimals = 6);
    bool isModLoaded(const std::string& modID);
    bool ensureDirectory(const std::string& path);
    std::vector<std::string> listFiles(const std::string& dir, const std::string& extension);
    void simulatePlayerClick(PlayerObject* player, bool isClick);
    void simulatePushButton(int button, bool isPlayer1);
    void simulateReleaseButton(int button, bool isPlayer1);
    CheckpointState capturePlayerState(PlayLayer* playLayer);
    void restorePlayerState(PlayLayer* playLayer, const CheckpointState& state);
}

} // namespace bot