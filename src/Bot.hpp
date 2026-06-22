#pragma once

/*
 * Bot.hpp — Practice-to-Normal Macro Bot
 * Target: Geometry Dash 2.2081 + Geode v5.7.1
 *
 * ============================================================
 * RECORDING FORMAT: X-POSITION BASED
 * ============================================================
 *
 * We store every input as (xPosition, playerButton, isPress, player2).
 * xPosition comes from PlayerObject::m_position.x at the exact moment
 * the input is processed inside the physics step — NOT at render time.
 *
 * WHY X-POSITION INSTEAD OF FRAMES OR TIMESTAMPS?
 *
 *  1. DETERMINISM: GD physics is deterministic given the same x-position.
 *     Two runs with identical x-position inputs always produce identical
 *     physics outcomes, regardless of FPS, speedhack, or CBF sub-stepping.
 *
 *  2. CBF/CBS COMPATIBILITY: Both Syzzi CBF and RobTop CBS inject inputs
 *     inside the physics substep. x-position is the only quantity that is
 *     monotonically increasing AND available at sub-frame precision inside
 *     those physics steps. Frame index is meaningless for CBF because CBF
 *     can insert many physics steps per rendered frame.
 *
 *  3. SPEEDHACK INVARIANCE: Speedhack changes how quickly x advances per
 *     real second, but inputs stored by x-position replay identically at
 *     any speed. Frame-based or time-based formats would need conversion.
 *
 *  4. SMALL FILE SIZE: We only store one record per click/release — file
 *     size scales with CPS, not run length. A 5-minute run at 20 CPS is
 *     ~6000 records × 9 bytes = ~54 KB uncompressed.
 *
 *  5. HIGH-CPS SUPPORT: No quantization; inputs recorded between frames
 *     from CBF sub-steps have distinct x-positions and replay perfectly.
 *
 * LIMITATION: x-position wraps in some auto-scroll segments and multi-
 * stage levels. We handle this by also storing a "segment ID" counter
 * that increments on each level restart / checkpoint restore, making the
 * (segmentId, x) tuple globally unique within a recording session.
 *
 * ============================================================
 * CBF vs CBS INTEGRATION
 * ============================================================
 *
 * Syzzi Click Between Frames (syzzi.click_between_frames):
 *   - Hooks GJBaseGameLayer::processCommands or equivalent to call
 *     GJBaseGameLayer::handleButton at arbitrary sub-frame positions.
 *   - We detect CBF by checking Loader::get()->isModLoaded("syzzi.click_between_frames").
 *   - During recording under CBF, inputs arrive inside physics updates with
 *     x-positions that are NOT aligned to rendered-frame boundaries. We store
 *     them as-is; the fractional x-position carries all necessary precision.
 *   - During playback under CBF, we inject via GJBaseGameLayer::handleButton
 *     inside our own physics-step hook, which CBF will also be driving. The
 *     x-threshold comparison fires inputs at the correct sub-frame point.
 *
 * RobTop Click Between Steps (CBS):
 *   - Built into GD 2.2+; activated when "Click Between Steps" is toggled
 *     in the game options.  Internally it quantizes inputs to ~480 FPS
 *     equivalent physics steps.
 *   - We detect CBS by reading GameManager::m_clickBetweenSteps (bool field
 *     at verified offset, see main.cpp).
 *   - Under CBS the x-position precision is limited to ~1/480 of a second
 *     at 1× speed, but our format stores that precision faithfully with no
 *     artificial quantization.
 *   - Playback under CBS: same injection path; CBS will handle the sub-step
 *     timing; our x-threshold fires the input at the correct CBS sub-step.
 *
 * FALLBACK: If neither CBF nor CBS is active, playback is disabled entirely
 * because frame-level accuracy cannot faithfully reproduce CBF recordings
 * and may desync Normal Mode runs.
 *
 * ============================================================
 * CHECKPOINT / PRACTICE BUG FIX
 * ============================================================
 *
 * GD's built-in checkpoints do NOT save all physics state. We maintain a
 * parallel BotCheckpoint that stores the full state required for accurate
 * macro replay (see BotCheckpoint below).
 *
 * On checkpoint save: we snapshot every field listed in BotCheckpoint.
 * On checkpoint load: we restore all fields BEFORE the physics step runs.
 * We also advance our "dead-input fence" so any inputs recorded before this
 * x-position in a previous failed attempt are discarded on the next death.
 *
 * ============================================================
 * DEAD INPUT CLEANUP
 * ============================================================
 *
 * When a player dies at x = D:
 *   - Any input with x >= D in the current segment is erased.
 *   - The segment counter increments so future inputs cannot collide with
 *     inputs recorded before the restart.
 *
 * When the player presses Restart from the pause menu:
 *   - We erase all inputs with segmentId == currentSegment AND x >= 0
 *     (i.e., the entire current segment).
 *   - segmentCounter increments.
 *
 * ============================================================
 * SPEEDHACK
 * ============================================================
 *
 * We implement speedhack by scaling the dt argument passed to
 * GJBaseGameLayer::update before it reaches the physics engine.
 * This is equivalent to changing game speed without touching FPS.
 * Macro timing is invariant because it is x-position based, not dt-based.
 *
 * ============================================================
 * SERIALIZATION FORMAT (binary, little-endian)
 * ============================================================
 *
 * Header (16 bytes):
 *   u32  magic        = 0x424F5432  ("BOT2")
 *   u32  version      = 1
 *   u32  inputCount
 *   u8   reserved[4]
 *
 * Per-input record (13 bytes each):
 *   f64  xPosition    (8 bytes) — player x at input moment
 *   u16  segmentId    (2 bytes) — restart/death counter
 *   u8   button       (1 byte)  — PlayerButton enum value
 *   u8   flags        (1 byte)  — bit0=isPress, bit1=isPlayer2
 *   u8   padding      (1 byte)  — reserved, must be 0
 *
 * Total overhead: 16 + N × 13 bytes.  At 20 CPS for 5 minutes: ~54 KB.
 */

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CheckpointObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

// ─── Constants ────────────────────────────────────────────────────────────────

constexpr uint32_t BOT_MAGIC   = 0x424F5432u; // "BOT2"
constexpr uint32_t BOT_VERSION = 1u;

// Tolerance window for x-position matching during playback.
// We fire a queued input when playerX >= input.x - PLAYBACK_X_EPSILON.
// This compensates for the discrete physics step size; it should be smaller
// than the minimum possible physics step width (~0.001 at 480 FPS, 1× speed).
constexpr double PLAYBACK_X_EPSILON = 0.0005;

// ─── Input Record ─────────────────────────────────────────────────────────────

struct BotInput {
    double      x;          ///< Player x-position when input was registered
    uint16_t    segment;    ///< Monotonic counter; increments on each death/restart
    PlayerButton button;    ///< Which button (Jump, Left, Right, etc.)
    bool        isPress;    ///< true = button down, false = button up
    bool        isPlayer2;  ///< Dual-mode second player

    // Sorting: inputs are ordered by (segment, x) so binary search works.
    bool operator<(const BotInput& o) const noexcept {
        if (segment != o.segment) return segment < o.segment;
        return x < o.x;
    }
};
static_assert(sizeof(PlayerButton) == 1 || sizeof(PlayerButton) <= 4,
    "Adjust BotInput serialization if PlayerButton grows beyond 1 byte");

// ─── Gamemode-specific physics state ─────────────────────────────────────────
// We store the fields that GD's built-in checkpoint omits but that affect
// deterministic replay (e.g. spider teleport phase, robot animation tick).

struct RobotState {
    bool isOnGround;
    bool hasJumped;
    float jumpTimer;
};

struct SpiderState {
    bool isTeleporting;
    float teleportProgress;
    bool lastGrounded;
};

struct BallState {
    bool isOnGround;
    float rotationRate;
};

// Union-style storage — only the active gamemode's fields matter.
struct GamemodeState {
    RobotState  robot  {};
    SpiderState spider {};
    BallState   ball   {};
};

// ─── Full Checkpoint Snapshot ─────────────────────────────────────────────────
/*
 * BotCheckpoint stores every piece of physics state needed so that macro
 * playback remains valid after any number of checkpoint reloads.
 *
 * Fields are taken directly from PlayerObject and GJBaseGameLayer members
 * documented in the Geode/GD headers for 2.2081.
 */

struct PlayerSnapshot {
    // ── Spatial ──────────────────────────────────────────────────────────────
    cocos2d::CCPoint position;      ///< m_position
    cocos2d::CCPoint velocity;      ///< m_playerVelocity (x/y speed)
    float            yAccel;        ///< m_yVelocity (raw vertical acceleration)
    float            rotation;      ///< getRotation()

    // ── Physics flags ────────────────────────────────────────────────────────
    bool isOnGround;                ///< m_isOnGround
    bool isOnGroundTwo;             ///< m_isOnGroundTwo (ceiling-standing)
    bool isUpsideDown;              ///< m_isUpsideDown (gravity flip)
    bool isFalling;                 ///< m_isFalling

    // ── Gamemode ─────────────────────────────────────────────────────────────
    int  gamemode;                  ///< PlayerMode enum (cube=0, ship=1, …)
    bool isSmall;                   ///< m_isSmall
    bool isDual;                    ///< captured from GJBaseGameLayer dual flag

    // ── Speed / portals ──────────────────────────────────────────────────────
    int  speedValue;                ///< current speed portal tier (0–4)
    bool hasGravityFlipped;         ///< mirror of m_isUpsideDown for clarity

    // ── Gamemode-specific ────────────────────────────────────────────────────
    GamemodeState gmState;

    // ── Wave-specific ────────────────────────────────────────────────────────
    bool    waveIsHolding;          ///< Is the player currently holding jump?
    float   waveTrailSize;          ///< Restore visual state too

    // ── UFO-specific ─────────────────────────────────────────────────────────
    float   ufoJumpTimer;           ///< Cooldown before another UFO jump

    // ── Ship-specific ────────────────────────────────────────────────────────
    bool    shipBoostActive;

    // ── Swing-specific ───────────────────────────────────────────────────────
    bool    swingGravityFlipped;
};

struct BotCheckpoint {
    uint16_t        segment;        ///< Segment ID at save time
    double          x;              ///< x-position at save time (used as fence)
    PlayerSnapshot  p1;
    PlayerSnapshot  p2;             ///< Only meaningful in dual mode
};

// ─── CBF / CBS Detection Result ───────────────────────────────────────────────

enum class CbfMode {
    None,    ///< Neither CBF nor CBS active — playback forbidden
    CBS,     ///< RobTop built-in Click Between Steps (~480 FPS precision)
    CBF,     ///< Syzzi Click Between Frames (sub-frame, highest precision)
};

// ─── Bot State Machine ────────────────────────────────────────────────────────

enum class BotState {
    Idle,
    Recording,
    Playback,
};

// ─── Main Bot Class ───────────────────────────────────────────────────────────

class Bot {
public:
    // Singleton
    static Bot& get() {
        static Bot instance;
        return instance;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void onLevelEntry(GJBaseGameLayer* layer);
    void onLevelExit();
    void onPhysicsStep(GJBaseGameLayer* layer, float dt);
    void onPlayerDeath(PlayerObject* player);
    void onCheckpointSave(CheckpointObject* cp, GJBaseGameLayer* layer);
    void onCheckpointLoad(CheckpointObject* cp, GJBaseGameLayer* layer);
    void onRestartLevel();

    // ── Input events (called from our handleButton hook) ──────────────────────
    void recordInput(GJBaseGameLayer* layer, PlayerButton btn, bool isPress, bool isP2);
    // Returns true if the bot consumed the input (playback mode)
    bool onHandleButton(GJBaseGameLayer* layer, bool isPress, int button, bool isP2);

    // ── Control ───────────────────────────────────────────────────────────────
    void startRecording();
    void startPlayback();
    void stopAll();

    // ── Serialization ─────────────────────────────────────────────────────────
    bool saveMacro(const std::string& path);
    bool loadMacro(const std::string& path);

    // ── Speedhack ─────────────────────────────────────────────────────────────
    float speedhackMultiplier = 1.0f;
    /// Called from our GJBaseGameLayer::update hook to scale dt.
    float applySpeedhack(float dt) const { return dt * speedhackMultiplier; }

    // ── CBF/CBS detection ─────────────────────────────────────────────────────
    CbfMode detectCbfMode() const;
    CbfMode currentCbfMode = CbfMode::None;

    // ── State queries ─────────────────────────────────────────────────────────
    BotState state()         const { return m_state; }
    bool     isRecording()   const { return m_state == BotState::Recording; }
    bool     isPlayback()    const { return m_state == BotState::Playback; }
    size_t   inputCount()    const { return m_inputs.size(); }

private:
    Bot() = default;

    // ── Core data ─────────────────────────────────────────────────────────────
    BotState                m_state         = BotState::Idle;
    std::vector<BotInput>   m_inputs;           ///< All recorded inputs, sorted by (segment,x)
    size_t                  m_playbackIdx   = 0;///< Next input to inject during playback
    uint16_t                m_segment       = 0;///< Current death/restart counter

    // Checkpoints: keyed by CheckpointObject pointer for O(1) lookup
    std::unordered_map<CheckpointObject*, BotCheckpoint> m_checkpoints;

    // The layer pointer (valid only during a level session)
    GJBaseGameLayer*        m_layer         = nullptr;

    // ── Helpers ───────────────────────────────────────────────────────────────
    double getPlayerX(GJBaseGameLayer* layer, bool isP2 = false) const;
    void   snapshotPlayer(PlayerObject* po, bool isDual, PlayerSnapshot& out) const;
    void   restorePlayer(PlayerObject* po, GJBaseGameLayer* layer,
                         const PlayerSnapshot& snap, bool isDual) const;
    void   pruneInputsFrom(double xFence, uint16_t segmentId);
    void   eraseSegment(uint16_t segmentId);

    // Advance playback pointer to first input not yet past current x
    void advancePlaybackIndex(double currentX);
};

// ─── UI Forward Declarations ──────────────────────────────────────────────────

class BotMenuLayer : public cocos2d::CCLayer {
public:
    static BotMenuLayer* create();
    bool init() override;

private:
    void onRecord(cocos2d::CCObject*);
    void onPlayback(cocos2d::CCObject*);
    void onSave(cocos2d::CCObject*);
    void onLoad(cocos2d::CCObject*);
    void onClose(cocos2d::CCObject*);
    void onSpeedhackChanged(cocos2d::CCObject*);
    void updateStatus();

    cocos2d::CCLabelBMFont* m_statusLabel    = nullptr;
    cocos2d::CCLabelBMFont* m_infoLabel      = nullptr;
    cocos2d::extension::CCScale9Sprite* m_bg = nullptr;
};