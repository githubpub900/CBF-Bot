#pragma once

#include <Geode/Geode.hpp>

using namespace geode::prelude;

enum class EngineType {
    None,
    CBS,       // RobTop's Built-in Click Between Steps
    SyzziCBF   // Syzzi's Click Between Frames Mod
};

// State representation for exact deterministic checkpoint restoration
struct PlayerState {
    cocos2d::CCPoint position;
    float rotation;
    double yVelocity;
    double xVelocity;
    bool isUpsideDown;
    bool isOnGround;
    bool isDashing;
    bool isSliding;
    float vehicleSize;
    float speed;
    bool isShip;
    bool isBird;
    bool isBall;
    bool isDart;
    bool isRobot;
    bool isSpider;
    bool isSwing;
};

// Practice Mode bug-fix checkpoint storage
struct CheckpointData {
    PlayerState p1;
    PlayerState p2;
    size_t actionIndex; // Truncation index for dead-input cleanup
    bool isDual;
};

struct MacroAction {
    float xPos;
    int button;
    bool player2;
    bool push;
};

class Bot {
public:
    EngineType engine = EngineType::None;
    bool isRecording = false;
    bool isPlaying = false;
    float speedHackValue = 1.0f;
    size_t playbackIndex = 0;

    std::vector<MacroAction> actions;
    std::vector<CheckpointData> checkpoints;

    static Bot& get();

    void detectEngine();
    void toggleUI();
    void recordAction(float xPos, int button, bool player2, bool push);
    void updatePlayback(PlayLayer* pl);
    
    // Checkpoint & Death Management
    void saveCheckpoint(PlayLayer* pl);
    void removeLastCheckpoint();
    void restoreCheckpoint(PlayLayer* pl);
    void clearCheckpoints();
    void clearMacro();

    // Serialization
    void saveMacro(const std::string& filename);
    void loadMacro(const std::string& filename);

private:
    PlayerState captureState(PlayerObject* p);
    void applyState(PlayerObject* p, const PlayerState& s);
};