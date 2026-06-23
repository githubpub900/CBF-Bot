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

using namespace geode::prelude;

namespace bot {

// ==================== CONSTANTS ====================
constexpr const char* CBF_MOD_ID = "syzzi.click_between_frames";
constexpr const char* MOD_ID = "yourname.cbf_macro_bot";
constexpr double TIME_PRECISION = 1e-12; // 12 decimal places for time storage
constexpr double MAX_TIME_VALUE = 1e6;
constexpr float DEFAULT_SPEEDHACK = 1.0f;
constexpr float MIN_SPEEDHACK = 0.01f;
constexpr float MAX_SPEEDHACK = 100.0f;

// ==================== INPUT STRUCTURE ====================
// Stores a single input at a specific time with player info
struct MacroInput {
    double time;          // Time into the level (seconds, high precision)
    bool isPlayer1;       // true = P1, false = P2
    bool isClick;         // true = click, false = release
    bool isHold;          // true = hold, false = tap (for CBF compatibility)

    // For sorting and comparison
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
// Comprehensive checkpoint state for practice bug fix
struct CheckpointState {
    // Player 1 state
    cocos2d::CCPoint p1_position;
    cocos2d::CCPoint p1_lastPosition;
    cocos2d::CCPoint p1_velocity;
    float p1_rotation;
    float p1_rotationSpeed;
    bool p1_isHolding;
    bool p1_isOnGround;
    bool p1_isDashing;
    bool p1_isUpsideDown;
    bool p1_isDead;
    int p1_gameMode; // 0=cube, 1=ship, 2=ball, 3=ufo, 4=wave, 5=robot, 6=spider, 7=swing
    float p1_gravityScale;
    float p1_speed;
    float p1_scale;
    bool p1_isFlipped;

    // Player 2 state (for dual mode)
    cocos2d::CCPoint p2_position;
    cocos2d::CCPoint p2_lastPosition;
    cocos2d::CCPoint p2_velocity;
    float p2_rotation;
    float p2_rotationSpeed;
    bool p2_isHolding;
    bool p2_isOnGround;
    bool p2_isDashing;
    bool p2_isUpsideDown;
    bool p2_isDead;
    int p2_gameMode;
    float p2_gravityScale;
    float p2_speed;
    float p2_scale;
    bool p2_isFlipped;

    // Level state
    float level_time;
    float level_xPos;
    int attemptCount;

    // Additional physics state
    float cameraX;
    float cameraY;
    float cameraZoom;
    bool isDualMode;
    bool isPlatformer;

    // For dead input tracking - which inputs were valid at this checkpoint
    size_t inputIndex; // Index into the macro input vector at this checkpoint
};

// ==================== MACRO FILE FORMAT ====================
// Optimized binary format for CBF macros
// Uses delta encoding for time to minimize file size
struct MacroFile {
    std::string version = "1.0";
    double startTimeOffset = 0.0; // If recording didn't start at 0
    double levelLength = 0.0;
    int levelID = 0;
    std::string levelName;

    // Inputs stored as delta-encoded times for compression
    // First input: absolute time
    // Subsequent inputs: delta from previous
    std::vector<MacroInput> inputs;

    // For CBF: store the exact step/frame info if needed
    uint64_t totalSteps = 0;
    float fps = 0.0f;

    bool saveToFile(const std::string& path);
    bool loadFromFile(const std::string& path);
    std::string serialize() const;
    bool deserialize(const std::string& data);
};

// ==================== CBF STATUS ====================
enum class CBFStatus {
    NONE,        // No CBF enabled - RED
    VANILLA,     // RobTop's Click Between Steps (480 FPS limit) - YELLOW
    SYZZI        // Syzzi's CBF (practically infinite) - GREEN
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

    // For smooth transition
    float transitionSpeed = 5.0f;

    void update(float dt);
    float getDeltaMultiplier() const;
};

// ==================== MAIN BOT CLASS ====================
class MacroBot {
public:
    static MacroBot& getInstance();

    // State management
    void setState(BotState state);
    BotState getState() const { return m_state; }

    // Recording
    void startRecording();
    void stopRecording();
    void recordInput(bool isPlayer1, bool isClick, bool isHold, double time);

    // Playback
    void startPlayback();
    void stopPlayback();
    void updatePlayback(double currentTime);

    // Checkpoint management (Practice Bug Fix)
    void saveCheckpoint(PlayLayer* playLayer);
    void loadCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint);
    void removeCheckpoint(PlayLayer* playLayer, CheckpointObject* checkpoint);
    void clearCheckpoints();

    // Dead input handling
    void onPlayerDeath();
    void onRestartFromCheckpoint();
    void onRestartFromBeginning();
    void onPauseRestart();
    void discardDeadInputs();
    void revertInputsAfterCheckpoint(size_t checkpointIndex);

    // CBF detection
    CBFStatus detectCBFStatus();
    bool isCBFEnabled() const;
    bool isSyzziCBF() const;

    // Speedhack
    void setSpeedhack(float speed);
    float getSpeedhack() const { return m_speedhack.targetSpeed; }
    SpeedhackState& getSpeedhackState() { return m_speedhack; }

    // File I/O
    bool saveMacro(const std::string& filename);
    bool loadMacro(const std::string& filename);
    std::vector<std::string> getSavedMacros();

    // GUI
    void toggleGUI();
    bool isGUIOpen() const { return m_guiOpen; }
    void updateGUI();
    void renderGUI();

    // Time management
    double getCurrentLevelTime() const { return m_currentTime; }
    void setStartTimeOffset(double offset) { m_startTimeOffset = offset; }
    double getStartTimeOffset() const { return m_startTimeOffset; }

    // Input access
    const std::vector<MacroInput>& getInputs() const { return m_inputs; }
    size_t getInputCount() const { return m_inputs.size(); }

    // Settings
    void setShowTrajectory(bool show) { m_showTrajectory = show; }
    bool getShowTrajectory() const { return m_showTrajectory; }

    void setAutoSave(bool autoSave) { m_autoSave = autoSave; }
    bool getAutoSave() const { return m_autoSave; }

    // Cleanup
    void reset();
    void onLevelExit();

    // Keybind handling
    void onKeybindPressed();

private:
    MacroBot() = default;
    ~MacroBot() = default;
    MacroBot(const MacroBot&) = delete;
    MacroBot& operator=(const MacroBot&) = delete;

    // State
    std::atomic<BotState> m_state{BotState::IDLE};
    std::atomic<bool> m_guiOpen{false};

    // Inputs
    std::vector<MacroInput> m_inputs;
    std::vector<MacroInput> m_inputsBackup; // For checkpoint reverts
    std::mutex m_inputMutex;

    // Playback tracking
    size_t m_playbackIndex = 0;
    double m_currentTime = 0.0;
    double m_startTimeOffset = 0.0;
    double m_lastPlaybackTime = 0.0;

    // Checkpoints
    std::map<CheckpointObject*, CheckpointState> m_checkpointStates;
    std::vector<CheckpointObject*> m_checkpointOrder;
    size_t m_lastCheckpointInputIndex = 0;
    bool m_diedSinceCheckpoint = false;

    // Speedhack
    SpeedhackState m_speedhack;

    // Settings
    bool m_showTrajectory = false;
    bool m_autoSave = true;
    std::string m_currentMacroName;

    // CBF cache
    CBFStatus m_cachedCBFStatus = CBFStatus::NONE;
    std::chrono::steady_clock::time_point m_lastCBFCheck;

    // GUI elements
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_cbfStatusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_speedhackLabel = nullptr;
    cocos2d::CCLayer* m_guiLayer = nullptr;

    // Internal methods
    void executeInput(const MacroInput& input);
    void sendClick(bool isPlayer1, bool isClick);
    void updateStatusLabel();
    void createGUI();
    void destroyGUI();
    std::string getMacrosDir();
    void compressInputs(); // Remove redundant/duplicate inputs
    void trimInputsToTime(double time); // Remove inputs after a certain time
};

// ==================== UTILITY FUNCTIONS ====================
namespace utils {
    // High precision time formatting
    std::string formatTime(double time, int decimals = 6);

    // CBF detection helpers
    bool isModLoaded(const std::string& modID);

    // File helpers
    bool ensureDirectory(const std::string& path);
    std::vector<std::string> listFiles(const std::string& dir, const std::string& extension);

    // Input simulation
    void simulatePlayerClick(PlayerObject* player, bool isClick);
    void simulatePushButton(int button, bool isPlayer1);
    void simulateReleaseButton(int button, bool isPlayer1);

    // Player state capture/restore
    CheckpointState capturePlayerState(PlayLayer* playLayer);
    void restorePlayerState(PlayLayer* playLayer, const CheckpointState& state);
}

} // namespace bot