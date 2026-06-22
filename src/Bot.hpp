#pragma once
#include <Geode/Geode.hpp>
#include <vector>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// Enhanced structure for infinite precision inputs
struct BotAction {
    double xPosition;   // Double-precision X position for perfect alignment
    float stepDelta;    // Sub-frame time offset (essential for CBF/CBS)
    int button;         // Action type (jump, left, right, etc.)
    bool isPush;        // Press or release
    bool isPlayer2;     // Target player

    // Sort chronologically by physical coordinate
    bool operator<(const BotAction& other) const {
        if (std::abs(xPosition - other.xPosition) < 0.00001) {
            return stepDelta < other.stepDelta;
        }
        return xPosition < other.xPosition;
    }
};

// Full physics snapshot to eliminate practice mode desyncs
struct CheckpointState {
    double p1XPos;
    double p1YPos;
    double p1YVel;
    float p1Rotation;
    bool p1IsDashing;
    bool p1IsUpsideDown;
    bool p1IsOnGround;
    bool p1IsSliding;

    double p2XPos;
    double p2YPos;
    double p2YVel;
    float p2Rotation;
    bool p2IsDashing;
    bool p2IsUpsideDown;
    bool p2IsOnGround;
    bool p2IsSliding;
};

class BotManager {
public:
    enum class State {
        Idle,
        Recording,
        Playing
    };

    State currentState = State::Idle;
    std::vector<BotAction> macro;
    size_t playbackIndex = 0;
    
    // Mapped checkpoint storage
    std::map<void*, CheckpointState> checkpointData;

    // Configurable parameters
    float speedMultiplier = 1.0f;
    bool practiceFixEnabled = true;
    bool cbfSupportEnabled = true;

    static BotManager& get() {
        static BotManager instance;
        return instance;
    }

    void clearMacro() {
        macro.clear();
        playbackIndex = 0;
    }

    void addAction(double x, float stepDelta, int btn, bool push, bool p2) {
        if (currentState == State::Recording) {
            macro.push_back({x, stepDelta, btn, push, p2});
        }
    }

    // Direct JSON-based File I/O for lightweight, human-readable file sharing
    bool saveMacroToFile(const std::string& filename) {
        auto savePath = Mod::get()->getSaveDir() / filename;
        std::ofstream file(savePath.string());
        if (!file.is_open()) return false;

        file << "{\n  \"actions\": [\n";
        for (size_t i = 0; i < macro.size(); ++i) {
            const auto& action = macro[i];
            file << "    {"
                 << "\"x\":" << action.xPosition << ","
                 << "\"d\":" << action.stepDelta << ","
                 << "\"b\":" << action.button << ","
                 << "\"p\":" << (action.isPush ? 1 : 0) << ","
                 << "\"p2\":" << (action.isPlayer2 ? 1 : 0)
                 << "}" << (i < macro.size() - 1 ? ",\n" : "\n");
        }
        file << "  ]\n}";
        file.close();
        return true;
    }

    bool loadMacroFromFile(const std::string& filename) {
        auto loadPath = Mod::get()->getSaveDir() / filename;
        std::ifstream file(loadPath.string());
        if (!file.is_open()) return false;

        clearMacro();
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("\"x\"") != std::string::npos) {
                double x = 0.0;
                float d = 0.0f;
                int b = 0;
                int p = 0;
                int p2 = 0;

                // Simple robust parsing loop to avoid heavy library dependencies
                auto parseVal = [&](const std::string& key, auto& out) {
                    size_t pos = line.find(key);
                    if (pos != std::string::npos) {
                        size_t valStart = line.find(":", pos) + 1;
                        size_t valEnd = line.find_first_of(",}", valStart);
                        std::string valStr = line.substr(valStart, valEnd - valStart);
                        std::stringstream ss(valStr);
                        ss >> out;
                    }
                };

                parseVal("\"x\"", x);
                parseVal("\"d\"", d);
                parseVal("\"b\"", b);
                parseVal("\"p\"", p);
                parseVal("\"p2\"", p2);

                macro.push_back({x, d, b, p != 0, p2 != 0});
            }
        }
        std::sort(macro.begin(), macro.end());
        return true;
    }
};