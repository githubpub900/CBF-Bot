#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <map>
#include <vector>

using namespace geode::prelude;

// ==========================================
// ENUMS & DATA STRUCTURES
// ==========================================

enum class BotMode {
    Disabled,
    Recording,
    Playing
};

enum class CBFState {
    None,       // Red
    RobTopCBS,  // Yellow
    SyzziCBF    // Green
};

// Stores the precise time-based input
struct MacroInput {
    double time;        // Extremely high precision time (double gives ~15-17 decimal places)
    int button;         // 1 = Jump, 2 = Left, 3 = Right, etc.
    bool push;          // true = push, false = release
    bool isPlayer2;     // true = player 2, false = player 1
};

// Extensive practice bug fix state
struct PlayerCheckpointState {
    CCPoint m_position;
    double m_yVelocity;
    double m_platformerXVelocity;
    float m_rotation;
    float m_rotationRate;
    bool m_isDashing;
    bool m_isSliding;
    bool m_isUpsideDown;
    bool m_isMini;
    int m_vehicleSize;
};

struct CheckpointData {
    double m_time;
    PlayerCheckpointState m_p1;
    PlayerCheckpointState m_p2;
};

// ==========================================
// BOT SINGLETON CLASS
// ==========================================

class Bot {
private:
    Bot() {} // Singleton
    
    BotMode m_mode = BotMode::Disabled;
    CBFState m_cbfState = CBFState::None;
    
    std::vector<MacroInput> m_inputs;
    std::map<CheckpointObject*, CheckpointData> m_checkpoints;
    
    size_t m_playbackIndex = 0;
    double m_currentLevelTime = 0.0;
    
    float m_speedhack = 1.0f;
    bool m_botUIOpen = false;

public:
    static Bot& get() {
        static Bot instance;
        return instance;
    }

    // --- State Management ---
    void setMode(BotMode mode);
    BotMode getMode() const { return m_mode; }
    
    void updateCBFState();
    CBFState getCBFState() const { return m_cbfState; }

    void setSpeedhack(float speed);
    float getSpeedhack() const { return m_speedhack; }

    void toggleUI();
    bool isUIOpen() const { return m_botUIOpen; }
    void setUIOpen(bool open) { m_botUIOpen = open; }

    // --- Core Logic ---
    void updateTime(double dt);
    double getTime() const { return m_currentLevelTime; }
    void setTime(double time) { m_currentLevelTime = time; }
    
    void recordInput(int button, bool push, bool isPlayer2);
    void processPlayback(GJBaseGameLayer* layer);
    
    void discardDeadInputs(double revertTime);
    void reset();

    // --- Practice Bug Fix ---
    void saveCheckpoint(CheckpointObject* cp, PlayLayer* layer);
    void loadCheckpoint(CheckpointObject* cp, PlayLayer* layer);

private:
    PlayerCheckpointState capturePlayerState(PlayerObject* player);
    void applyPlayerState(PlayerObject* player, const PlayerCheckpointState& state);
};

// ==========================================
// UI LAYER
// ==========================================

class BotUI : public geode::Popup<> {
protected:
    bool setup() override;
    
    void onRecord(CCObject*);
    void onPlay(CCObject*);
    void onDisable(CCObject*);
    void onSpeedhackChange(CCObject*);
    void onClose(CCObject*) override;

    CCLabelBMFont* m_statusLabel;
    CCLabelBMFont* m_cbfIndicator;
    CCTextInputNode* m_speedhackInput;

public:
    static BotUI* create();
    void updateUIState();
};