#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bot {
    enum class TimingMode : std::uint8_t {
        None = 0,
        Cbf = 1,
        Cbs = 2,
    };

    struct MacroEvent {
        double timeSec = 0.0;
        std::uint8_t button = 0;
        std::uint8_t flags = 0; // bit 0 = down, bit 1 = player1
    };

    struct PlayerSnapshot {
        // Position / orientation
        cocos2d::CCPoint position{};
        float rotation = 0.f;

        // High-value motion state
        double totalTime = 0.0;
        double platformerXVelocity = 0.0;
        float playerSpeed = 0.f;
        float vehicleSize = 1.f;
        float slopeVelocity = 0.f;
        double scaleXRelated = 0.0;
        double scaleXRelatedTime = 0.0;
        float xVelocityRelated = 0.f;
        float xVelocityRelated2 = 0.f;
        float yVelocityRelated3 = 0.f;
        float audioScale = 1.f;
        float unkAngle1 = 0.f;

        // Mode / gravity state
        bool isShip = false;
        bool isBird = false;
        bool isBall = false;
        bool isDart = false;
        bool isRobot = false;
        bool isSpider = false;
        bool isSwing = false;
        bool isUpsideDown = false;
        bool isDead = false;
        bool isOnGround = false;
        bool isOnGround2 = false;
        bool isOnGround4 = false;
        bool isGoingLeft = false;
        bool isSideways = false;
        bool maybeUpsideDownSlope = false;
        bool maybeIsBoosted = false;
        bool maybeGoingCorrectSlopeDirection = false;
        bool isLocked = false;
        bool controlsDisabled = false;
        bool holdingRight = false;
        bool holdingLeft = false;
        bool leftPressedFirst = false;
        bool decreaseBoostSlide = false;
        bool unkA29 = false;
        bool isBeingSpawnedByDualPortal = false;
        bool defaultMiniIcon = false;
        bool swapColors = false;
        bool switchDashFireColor = false;
        bool hasEverJumped = false;
        bool hasEverHitRing = false;
        bool fixGravityBug = false;
        bool reverseSync = false;
        bool wasTeleported = false;

        // State buckets that govern custom physics behavior in different gamemodes.
        int stateOnGround = 0;
        std::uint8_t stateUnk = 0;
        std::uint8_t stateNoStickX = 0;
        std::uint8_t stateNoStickY = 0;
        std::uint8_t stateUnk2 = 0;
        int stateBoostX = 0;
        int stateBoostY = 0;
        int maybeStateForce2 = 0;
        int stateScale = 0;
        int groundObjectMaterial = 0;
        int reverseRelated = 0;
        int followRelated = 0;
        int dashFireFrame = 0;
        int collidingWithSlopeId = 0;
        int maybeSlidingTime = 0;
        int nextColorKey = 0;

        // Object references are intentionally not serialized; vanilla checkpoint loading
        // already reconstructs them, and persisting raw pointers would be unsafe.
        cocos2d::CCPoint lastGroundedPos{};
        cocos2d::CCPoint lastPortalPos{};
        cocos2d::CCPoint shipRotation{};
        cocos2d::CCPoint unk3918{};
        cocos2d::CCPoint unk3920{};
        float unkUnused3 = 0.f;
        float unkA99 = 0.f;
        float unk838 = 0.f;
        float unk38cc = 0.f;
        float unk3900 = 0.f;
        float unk3778 = 0.f;
        float unk3780 = 0.f;
        float unk3784 = 0.f;
        double maybeReverseSpeed = 0.0;
        double maybeReverseAcceleration = 0.0;
        double maybeSlopeForce = 0.0;
        double physDeltaRelated = 0.0;
        double maybeSlidingStartTime = 0.0;
        double changedDirectionsTime = 0.0;
        double slopeEndTime = 0.0;
        double dashStartTime = 0.0;
        double slopeStartTime = 0.0;
        double lastLandTime = 0.0;
        double maybeHasStopped = 0.0;
        double totalTimeRelated = 0.0;
    };

    struct CheckpointSnapshot {
        double timeSec = 0.0;
        PlayerSnapshot player1;
        PlayerSnapshot player2;
    };

    class BotOverlay;

    class BotManager {
    public:
        static BotManager& get();

        TimingMode detectedMode() const;
        bool canPlayBack() const;
        bool hasPlaybackData() const;
        bool isRecording() const;
        bool isPlayingBack() const;
        bool isInjectingPlayback() const;
        double speedhack() const;

        void toggleOverlay();
        void setOverlay(BotOverlay* overlay);
        BotOverlay* overlay() const;

        void startRecording();
        void stopRecording(bool keepData = true);
        void startPlayback();
        void stopPlayback();
        void saveMacro();
        void loadMacro();
        void setSpeedhack(double value);

        void onGameUpdate(PlayLayer* layer, float dt, std::function<void(float)> const& originalUpdate);
        void onButton(PlayerObject* player, PlayerButton button, bool down);
        void onDeath(PlayLayer* layer);
        void onRestartPre(PlayLayer* layer);
        void onRestartPost(PlayLayer* layer);
        void onCheckpointStore(PlayLayer* layer, CheckpointObject* checkpoint);
        void onCheckpointLoad(PlayLayer* layer, CheckpointObject* checkpoint);
        void onLevelComplete(PlayLayer* layer);
        void onSceneEnter(PlayLayer* layer);
        void onSceneExit();

        void trimAfterCurrentTime(PlayLayer* layer);
        void clearDeadState();
        void refreshPlayers(PlayLayer* layer);
        PlayerObject* player1() const;
        PlayerObject* player2() const;

        const std::vector<MacroEvent>& events() const;
        std::size_t playbackIndex() const;
        std::filesystem::path macroPath() const;

    private:
        BotManager() = default;

        std::filesystem::path defaultMacroPath() const;
        bool readMacroFromDisk();

        static PlayerSnapshot captureSnapshot(PlayerObject* player);
        static void applySnapshot(PlayerObject* player, PlayerSnapshot const& snapshot);

        TimingMode m_mode = TimingMode::None;
        bool m_recording = false;
        bool m_playback = false;
        bool m_dead = false;
        bool m_inPlaybackInjection = false;
        bool m_inUpdateSplit = false;
        double m_speedhack = 1.0;
        double m_recordBaseSec = 0.0;
        double m_playbackStartSec = 0.0;
        double m_deathCutoffSec = 0.0;

        std::vector<MacroEvent> m_events;
        std::vector<CheckpointSnapshot> m_checkpoints;
        std::unordered_map<CheckpointObject*, CheckpointSnapshot> m_checkpointLookup;
        std::size_t m_playbackIndex = 0;

        PlayLayer* m_layer = nullptr;
        PlayerObject* m_cachedP1 = nullptr;
        PlayerObject* m_cachedP2 = nullptr;

        BotOverlay* m_overlay = nullptr;
        std::filesystem::path m_macroPath;
    };
}