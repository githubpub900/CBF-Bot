#pragma once
#include <Geode/Geode.hpp>
#include <vector>
#include <algorithm>

using namespace geode::prelude;

// Struct to hold input data with high-precision time
struct BotInput {
    double time;    // Time of input (handles basically infinite frame decimal precision)
    bool push;      // True if pushed, False if released
    bool player2;   // P1 or P2
    int button;     // Button ID (PlayerButton)
};

// Struct for the practice bug fix and syncing
struct BotCheckpoint {
    double time; // Time at checkpoint creation
    cocos2d::CCPoint pos;
    double yAccel;
    double xAccel;
    double jumpAccel;
    bool isDashing;
    bool isUpsideDown;
    float vehicleSize;
    float rotation;
};

class Bot {
public:
    static Bot& get() {
        static Bot instance;
        return instance;
    }

    std::vector<BotInput> m_inputs;
    std::vector<BotCheckpoint> m_checkpoints;
    
    double m_time = 0.0;
    size_t m_playbackIndex = 0;
    
    bool m_recording = false;
    bool m_playing = false;
    bool m_botTriggered = false; // Prevents recursive loop when bot presses button

    // Check for Syzzi CBF or RobTop CBS
    // Returns: 2 = Syzzi (Green), 1 = RobTop CBS (Yellow), 0 = None (Red)
    int getCBFStatus() {
        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            return 2; 
        }
        // Fallback for RobTop CBS (Standard Game Variable for 'Click Between Steps')
        if (GameManager::get()->getGameVariable("0175") || GameManager::get()->getGameVariable("0173")) {
            return 1;
        }
        return 0;
    }

    void addInput(bool push, bool player2, int button) {
        if (getCBFStatus() == 0) return; // Discard if no CBF

        BotInput input;
        input.time = m_time;
        input.push = push;
        input.player2 = player2;
        input.button = button;

        m_inputs.push_back(input);
    }

    void resetInputs(double time) {
        // Discard dead inputs on practice death/pause reset
        auto it = std::lower_bound(m_inputs.begin(), m_inputs.end(), time,
            [](const BotInput& a, double t) { return a.time < t; });
        m_inputs.erase(it, m_inputs.end());
    }

    void saveCheckpoint(PlayerObject* player) {
        BotCheckpoint cp;
        cp.time = m_time;
        cp.pos = player->getPosition();
        cp.yAccel = player->m_yAccel;
        cp.xAccel = player->m_xAccel;
        cp.jumpAccel = player->m_jumpAccel;
        cp.isDashing = player->m_isDashing;
        cp.isUpsideDown = player->m_isUpsideDown;
        cp.vehicleSize = player->m_vehicleSize;
        cp.rotation = player->getRotation();
        m_checkpoints.push_back(cp);
    }

    void loadCheckpoint(PlayerObject* player) {
        if (m_checkpoints.empty()) return;
        auto& cp = m_checkpoints.back();
        
        player->setPosition(cp.pos);
        player->m_yAccel = cp.yAccel;
        player->m_xAccel = cp.xAccel;
        player->m_jumpAccel = cp.jumpAccel;
        player->m_isDashing = cp.isDashing;
        player->m_isUpsideDown = cp.isUpsideDown;
        player->m_vehicleSize = cp.vehicleSize;
        player->setRotation(cp.rotation);
    }

    void removeLastCheckpoint() {
        if (!m_checkpoints.empty()) {
            m_checkpoints.pop_back();
        }
    }
};