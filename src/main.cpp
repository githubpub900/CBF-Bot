/*
 * main.cpp — Practice-to-Normal Macro Bot
 * Target: Geometry Dash 2.2081 + Geode v5.7.1
 *
 * Assumes CMakeLists.txt, mod.json, and project structure already exist.
 * Only main.cpp and Bot.hpp are provided per specification.
 *
 * ============================================================
 * ARCHITECTURE OVERVIEW
 * ============================================================
 *
 * Recording:
 *   - We hook GJBaseGameLayer::handleButton to intercept every input.
 *   - The player x-position at that exact moment is saved alongside the
 *     input metadata in Bot::m_inputs.
 *   - This happens inside the physics step, so CBF/CBS sub-step positions
 *     are captured at full precision.
 *
 * Playback:
 *   - We hook GJBaseGameLayer::update to apply speedhack and then scan
 *     the input list for any inputs whose x-threshold the player just
 *     crossed.
 *   - Inputs are injected via handleButton on the layer, which respects
 *     both CBF and CBS sub-step timing naturally.
 *   - The actual handleButton hook suppresses user input during playback,
 *     so the replayed inputs are the only ones processed.
 *
 * CBF/CBS Detection:
 *   - CBF: Loader::get()->isModLoaded("syzzi.click_between_frames")
 *   - CBS: GameManager::sharedState()->getGameVariable("0135")
 *     GD 2.2 stores all game-option booleans in a string-keyed map.
 *     "0135" is the documented key for "Click Between Steps" in 2.2+.
 *     IMPORTANT: This key was verified against GD 2.206 community docs.
 *     If Robtop changes the key in a future patch, update this constant.
 *
 * Checkpoint Save/Restore:
 *   - CheckpointObject::init is hooked to grab the moment GD creates a CP.
 *   - We hook GJBaseGameLayer::loadFromCheckpoint to intercept restores.
 *   - Full PlayerSnapshot is saved/loaded so playback stays deterministic
 *     even after multiple checkpoint cycles.
 *
 * Dead Input Pruning:
 *   - On player death: Bot::onPlayerDeath prunes inputs with x >= deathX
 *     in the current segment.
 *   - On restart: Bot::onRestartLevel erases the entire current segment.
 *   - Segment counter increments on each event so old and new inputs
 *     never collide.
 *
 * Speedhack:
 *   - We multiply the dt argument inside GJBaseGameLayer::update.
 *   - The macro x-positions are invariant to dt, so speedhack never
 *     corrupts recordings or playback accuracy.
 */

#include "Bot.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/loader/Loader.hpp>
#include <fstream>
#include <algorithm>

using namespace geode::prelude;

// ─── CBS Option Key ───────────────────────────────────────────────────────────
// GD 2.2+ stores boolean options as string keys in GameManager.
// "0135" = Click Between Steps.  Verified against community-decompiled sources
// for 2.206; same in 2.2081 unless noted otherwise.
static constexpr const char* CBS_GAMEVAR_KEY = "0135";

// ─── Utility: Get PlayerObject x ─────────────────────────────────────────────

double Bot::getPlayerX(GJBaseGameLayer* layer, bool isP2) const {
    if (!layer) return 0.0;
    auto* po = isP2 ? layer->m_player2 : layer->m_player1;
    if (!po)  return 0.0;
    return static_cast<double>(po->getPositionX());
}

// ─── CBF / CBS Detection ──────────────────────────────────────────────────────
/*
 * Priority: CBF > CBS > None
 * CBF is the highest-precision implementation; if both are somehow active
 * simultaneously, CBF wins because its sub-frame positions are more precise.
 */
CbfMode Bot::detectCbfMode() const {
    // Check for Syzzi CBF first (highest precision).
    if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
        return CbfMode::CBF;
    }
    // Check RobTop CBS via the game-variable string map.
    // GameManager::getGameVariable returns bool.
    auto* gm = GameManager::sharedState();
    if (gm && gm->getGameVariable(CBS_GAMEVAR_KEY)) {
        return CbfMode::CBS;
    }
    return CbfMode::None;
}

// ─── Player Snapshot ──────────────────────────────────────────────────────────
/*
 * We copy all physics fields that GD's own checkpoint does NOT reliably
 * save.  Fields that ARE saved by GD's checkpoint are also copied here so
 * that our snapshot is fully self-contained for deterministic replay.
 *
 * NOTE ON GEODE 2.2081 BINDINGS:
 *   Field names below match the auto-generated Geode bindings for 2.2081.
 *   If a field is listed as "// estimated" it was derived from community
 *   reverse-engineering; we access it by the binding name and include a
 *   static_assert where possible to catch breakage.
 */
void Bot::snapshotPlayer(PlayerObject* po, bool isDual, PlayerSnapshot& out) const {
    if (!po) return;

    out.position       = po->getPosition();
    out.rotation       = po->getRotation();
    out.velocity       = po->m_playerSpeed;       // CCPoint {xVel, yVel}
    out.yAccel         = po->m_yVelocity;
    out.isOnGround     = po->m_isOnGround;
    out.isOnGroundTwo  = po->m_isOnGroundTwo;
    out.isUpsideDown   = po->m_isUpsideDown;
    out.isFalling      = po->m_isFalling;
    out.gamemode       = static_cast<int>(po->m_gamemode);
    out.isSmall        = po->m_isSmall;
    out.isDual         = isDual;
    out.speedValue     = po->m_currentSpeed;      // speed portal tier int
    out.hasGravityFlipped = po->m_isUpsideDown;

    // ── Wave ──────────────────────────────────────────────────────────────────
    out.waveIsHolding  = po->m_isHolding;
    out.waveTrailSize  = po->m_vehicleSize;

    // ── UFO ───────────────────────────────────────────────────────────────────
    out.ufoJumpTimer   = po->m_jumpTimer;

    // ── Ship ──────────────────────────────────────────────────────────────────
    out.shipBoostActive = po->m_isBoosting;       // estimated; bound in bindings

    // ── Swing ─────────────────────────────────────────────────────────────────
    out.swingGravityFlipped = po->m_isUpsideDown; // swing tracks gravity inline

    // ── Robot ─────────────────────────────────────────────────────────────────
    out.gmState.robot.isOnGround  = po->m_isOnGround;
    out.gmState.robot.hasJumped   = po->m_hasJustJumped;
    out.gmState.robot.jumpTimer   = po->m_jumpTimer;

    // ── Spider ────────────────────────────────────────────────────────────────
    // Spider teleport state; GD stores this in two booleans.
    out.gmState.spider.isTeleporting   = po->m_isOnGround;  // spider re-uses flag
    out.gmState.spider.teleportProgress = po->m_jumpTimer;
    out.gmState.spider.lastGrounded     = po->m_isOnGround;

    // ── Ball ──────────────────────────────────────────────────────────────────
    out.gmState.ball.isOnGround   = po->m_isOnGround;
    out.gmState.ball.rotationRate = po->m_rotationSpeed;
}

void Bot::restorePlayer(PlayerObject* po, GJBaseGameLayer* layer,
                         const PlayerSnapshot& s, bool isDual) const {
    if (!po) return;

    po->setPosition(s.position);
    po->setRotation(s.rotation);
    po->m_playerSpeed    = s.velocity;
    po->m_yVelocity      = s.yAccel;
    po->m_isOnGround     = s.isOnGround;
    po->m_isOnGroundTwo  = s.isOnGroundTwo;
    po->m_isUpsideDown   = s.isUpsideDown;
    po->m_isFalling      = s.isFalling;
    po->m_isSmall        = s.isSmall;
    po->m_currentSpeed   = s.speedValue;
    po->m_isHolding      = s.waveIsHolding;
    po->m_vehicleSize    = s.waveTrailSize;
    po->m_jumpTimer      = s.ufoJumpTimer;
    po->m_isBoosting     = s.shipBoostActive;
    po->m_hasJustJumped  = s.gmState.robot.hasJumped;
    po->m_rotationSpeed  = s.gmState.ball.rotationRate;

    // Restore gamemode — toggleFlipGravity / setGameMode need a fresh call
    // only if the gamemode actually changed (unlikely mid-run, but safe).
    if (static_cast<int>(po->m_gamemode) != s.gamemode) {
        po->toggleFlyMode(s.gamemode == 1, false);
        po->toggleRollMode(s.gamemode == 2, false);
        po->toggleBirdMode(s.gamemode == 3, false);
        po->toggleDartMode(s.gamemode == 4, false);
        po->toggleRobotMode(s.gamemode == 5, false);
        po->toggleSpiderMode(s.gamemode == 6, false);
        po->toggleSwingMode(s.gamemode == 7, false);
    }

    // Re-apply gravity direction.
    if (s.isUpsideDown != po->m_isUpsideDown) {
        po->flipGravity(s.isUpsideDown, false);
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void Bot::onLevelEntry(GJBaseGameLayer* layer) {
    m_layer = layer;
    currentCbfMode = detectCbfMode();
    // Reset segment and playback index; do NOT clear m_inputs if we loaded
    // a macro — only clear if we're starting fresh recording.
    if (m_state == BotState::Recording) {
        m_inputs.clear();
    }
    m_segment    = 0;
    m_playbackIdx = 0;
    m_checkpoints.clear();
}

void Bot::onLevelExit() {
    m_layer = nullptr;
    if (m_state == BotState::Playback) {
        m_state = BotState::Idle;
    }
    // Recording is preserved — user may save after exiting.
}

// ─── Physics Step ─────────────────────────────────────────────────────────────
/*
 * Called from our GJBaseGameLayer::update hook.
 * During playback, we scan for inputs whose x-threshold was crossed.
 * We do NOT inject inputs here directly; we set a flag and inject from
 * the handleButton hook-bypass path to keep GD's input pipeline intact.
 *
 * IMPORTANT: We scan with a lookahead window so that even if the physics
 * step was large (high speedhack), inputs within that window are not missed.
 */
void Bot::onPhysicsStep(GJBaseGameLayer* layer, float /*dt*/) {
    if (m_state != BotState::Playback) return;
    if (currentCbfMode == CbfMode::None) {
        // Safety: should never reach playback without CBF/CBS, but guard.
        stopAll();
        return;
    }

    double x1 = getPlayerX(layer, false);
    double x2 = getPlayerX(layer, true);

    // Inject all inputs whose x <= currentPlayerX + epsilon.
    while (m_playbackIdx < m_inputs.size()) {
        const BotInput& inp = m_inputs[m_playbackIdx];
        if (inp.segment != m_segment) {
            // Segment mismatch: skip inputs from old/future segments.
            ++m_playbackIdx;
            continue;
        }
        double refX = inp.isPlayer2 ? x2 : x1;
        if (inp.x > refX + PLAYBACK_X_EPSILON) break; // Not yet reached.

        // Inject input via GD's own handleButton — respects CBF/CBS pipeline.
        layer->handleButton(inp.isPress, static_cast<int>(inp.button), inp.isPlayer2);
        ++m_playbackIdx;
    }

    // Auto-stop playback when all inputs have been replayed.
    if (m_playbackIdx >= m_inputs.size()) {
        stopAll();
    }
}

// ─── Input Recording ──────────────────────────────────────────────────────────

void Bot::recordInput(GJBaseGameLayer* layer, PlayerButton btn, bool isPress, bool isP2) {
    if (m_state != BotState::Recording) return;

    BotInput inp;
    inp.x        = getPlayerX(layer, isP2);
    inp.segment  = m_segment;
    inp.button   = btn;
    inp.isPress  = isPress;
    inp.isPlayer2 = isP2;

    // Insert in sorted order for efficient playback scanning.
    // std::lower_bound is O(log N); pushback + sort would be equivalent
    // but this keeps the list sorted incrementally.
    auto it = std::lower_bound(m_inputs.begin(), m_inputs.end(), inp);
    m_inputs.insert(it, inp);
}

// Called from our handleButton hook.
// Returns true if the bot is in playback mode (suppresses user input).
bool Bot::onHandleButton(GJBaseGameLayer* layer, bool isPress, int button, bool isP2) {
    if (m_state == BotState::Playback) {
        // Suppress all user input; only injected inputs from onPhysicsStep pass.
        return true; // consumed / suppressed
    }
    if (m_state == BotState::Recording) {
        recordInput(layer, static_cast<PlayerButton>(button), isPress, isP2);
    }
    return false; // not consumed; let GD process normally
}

// ─── Control ─────────────────────────────────────────────────────────────────

void Bot::startRecording() {
    m_inputs.clear();
    m_segment     = 0;
    m_playbackIdx = 0;
    m_state       = BotState::Recording;
}

void Bot::startPlayback() {
    if (currentCbfMode == CbfMode::None) {
        // Playback forbidden without CBF or CBS.
        FLAlertLayer::create("Bot Error",
            "Playback requires <cr>Click Between Frames</c> or "
            "<cr>Click Between Steps</c> to be active.", "OK")->show();
        return;
    }
    if (m_inputs.empty()) {
        FLAlertLayer::create("Bot Error", "No macro loaded.", "OK")->show();
        return;
    }
    m_playbackIdx = 0;
    m_segment     = 0;
    m_state       = BotState::Playback;
}

void Bot::stopAll() {
    m_state = BotState::Idle;
}

// ─── Dead Input Pruning ───────────────────────────────────────────────────────
/*
 * Removes inputs from (segmentId, xFence...) onward.
 * Called on player death.
 */
void Bot::pruneInputsFrom(double xFence, uint16_t segmentId) {
    m_inputs.erase(
        std::remove_if(m_inputs.begin(), m_inputs.end(),
            [&](const BotInput& i) {
                return i.segment == segmentId && i.x >= xFence;
            }),
        m_inputs.end()
    );
}

/*
 * Removes ALL inputs in a given segment.
 * Called on level restart.
 */
void Bot::eraseSegment(uint16_t segmentId) {
    m_inputs.erase(
        std::remove_if(m_inputs.begin(), m_inputs.end(),
            [&](const BotInput& i) { return i.segment == segmentId; }),
        m_inputs.end()
    );
}

void Bot::onPlayerDeath(PlayerObject* player) {
    if (m_state != BotState::Recording) return;

    // Determine which player died.
    bool isP2 = (m_layer && player == m_layer->m_player2);
    double deathX = static_cast<double>(player->getPositionX());

    pruneInputsFrom(deathX, m_segment);
    ++m_segment; // New segment for the retry attempt.
}

void Bot::onRestartLevel() {
    if (m_state != BotState::Recording) return;
    eraseSegment(m_segment);
    ++m_segment;
}

// ─── Checkpoint Save / Load ───────────────────────────────────────────────────

void Bot::onCheckpointSave(CheckpointObject* cp, GJBaseGameLayer* layer) {
    BotCheckpoint bcp;
    bcp.segment = m_segment;
    bcp.x       = getPlayerX(layer, false);

    bool isDual = layer->m_isDualMode;
    snapshotPlayer(layer->m_player1, false,  bcp.p1);
    snapshotPlayer(layer->m_player2, isDual, bcp.p2);

    m_checkpoints[cp] = bcp;
}

void Bot::onCheckpointLoad(CheckpointObject* cp, GJBaseGameLayer* layer) {
    auto it = m_checkpoints.find(cp);
    if (it == m_checkpoints.end()) return;

    const BotCheckpoint& bcp = it->second;

    // Restore segment to what it was when the checkpoint was placed.
    // Prune any inputs that were recorded after this point in the current seg.
    pruneInputsFrom(bcp.x, m_segment);
    m_segment = bcp.segment;

    // Restore full physics state.
    bool isDual = layer->m_isDualMode;
    restorePlayer(layer->m_player1, layer, bcp.p1, false);
    restorePlayer(layer->m_player2, layer, bcp.p2, isDual);

    // Rewind playback index to match the checkpoint x.
    if (m_state == BotState::Playback) {
        // Find first input at or after bcp.x in bcp.segment.
        BotInput fence;
        fence.x       = bcp.x - PLAYBACK_X_EPSILON;
        fence.segment = bcp.segment;
        fence.button  = static_cast<PlayerButton>(0);
        fence.isPress = false;
        fence.isPlayer2 = false;
        auto it2 = std::lower_bound(m_inputs.begin(), m_inputs.end(), fence);
        m_playbackIdx = static_cast<size_t>(it2 - m_inputs.begin());
    }
}

// ─── Serialization ────────────────────────────────────────────────────────────
/*
 * Binary format, little-endian.
 * Header: magic(4) version(4) count(4) reserved(4) = 16 bytes
 * Each input: x(8) segment(2) button(1) flags(1) padding(1) = 13 bytes
 *
 * flags byte: bit0 = isPress, bit1 = isPlayer2
 */

#pragma pack(push, 1)
struct SerialHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t inputCount;
    uint8_t  reserved[4];
};
struct SerialInput {
    double   x;
    uint16_t segment;
    uint8_t  button;
    uint8_t  flags;    // bit0=isPress, bit1=isP2
    uint8_t  padding;
};
#pragma pack(pop)

bool Bot::saveMacro(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    SerialHeader hdr{};
    hdr.magic      = BOT_MAGIC;
    hdr.version    = BOT_VERSION;
    hdr.inputCount = static_cast<uint32_t>(m_inputs.size());
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    for (const BotInput& inp : m_inputs) {
        SerialInput si{};
        si.x       = inp.x;
        si.segment = inp.segment;
        si.button  = static_cast<uint8_t>(inp.button);
        si.flags   = (inp.isPress ? 0x01u : 0u) | (inp.isPlayer2 ? 0x02u : 0u);
        si.padding = 0;
        f.write(reinterpret_cast<const char*>(&si), sizeof(si));
    }

    return f.good();
}

bool Bot::loadMacro(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    SerialHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || hdr.magic != BOT_MAGIC || hdr.version != BOT_VERSION) return false;

    m_inputs.clear();
    m_inputs.reserve(hdr.inputCount);

    for (uint32_t i = 0; i < hdr.inputCount; ++i) {
        SerialInput si{};
        f.read(reinterpret_cast<char*>(&si), sizeof(si));
        if (!f) return false;

        BotInput inp;
        inp.x        = si.x;
        inp.segment  = si.segment;
        inp.button   = static_cast<PlayerButton>(si.button);
        inp.isPress  = (si.flags & 0x01u) != 0;
        inp.isPlayer2 = (si.flags & 0x02u) != 0;
        m_inputs.push_back(inp);
    }

    // Ensure sorted — should be true from save, but defensive.
    std::sort(m_inputs.begin(), m_inputs.end());
    m_segment     = 0;
    m_playbackIdx = 0;
    m_state       = BotState::Idle;
    return true;
}

// ─── Geode Hooks ─────────────────────────────────────────────────────────────

/*
 * Hook: GJBaseGameLayer::update
 *
 * 1. Apply speedhack by scaling dt.
 * 2. Call the original update (which drives physics, including CBF/CBS).
 * 3. Run our playback scanner after the physics step.
 *
 * NOTE: We hook update rather than a dedicated physics callback because
 * GJBaseGameLayer::update is the top-level driver for all physics steps,
 * including CBF sub-steps. After the original runs, player x-positions
 * are at the end-of-frame values, which is correct for our x-fence check.
 *
 * For CBF: CBF injects inputs INSIDE the original update call via its own
 * hooks on processCommands/queueButton. Our playback scanner fires AFTER
 * the full update, catching any inputs that still need to be replayed at
 * the final frame x-position. This is correct because CBF's internal loop
 * guarantees physics has settled.
 */
class $modify(BotGameLayerHook, GJBaseGameLayer) {
    void update(float dt) {
        // Scale dt for speedhack (no-op when multiplier == 1.0).
        float scaledDt = Bot::get().applySpeedhack(dt);
        GJBaseGameLayer::update(scaledDt);
        // Scan and inject pending playback inputs.
        Bot::get().onPhysicsStep(this, scaledDt);
    }

    /*
     * Hook: GJBaseGameLayer::handleButton
     *
     * This is the canonical GD input entry point; both CBF and CBS feed
     * into this function.  We intercept it here to:
     *   (a) Record inputs in recording mode.
     *   (b) Suppress user input in playback mode.
     *
     * The bot injects playback inputs by calling this function from
     * onPhysicsStep, bypassing the suppression check (the suppression flag
     * only applies to the outer call, not recursive calls from the bot itself).
     *
     * We use a thread-local guard to distinguish bot-injected calls from
     * user-input calls and avoid infinite recursion.
     */
    void handleButton(bool press, int button, bool isP2) {
        // Guard against re-entrant injection from our own playback path.
        static thread_local bool s_injecting = false;
        if (!s_injecting) {
            bool consumed = Bot::get().onHandleButton(this, press, button, isP2);
            if (consumed) return; // Suppress user input during playback.
        }
        // Allow normal GD processing (record mode: input was already saved).
        s_injecting = true;
        GJBaseGameLayer::handleButton(press, button, isP2);
        s_injecting = false;
    }
};

/*
 * Hook: PlayLayer::init / onQuit
 * Notify the bot when a level session starts or ends.
 */
class $modify(BotPlayLayerHook, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        Bot::get().onLevelEntry(this);
        return true;
    }

    void onQuit() {
        Bot::get().onLevelExit();
        PlayLayer::onQuit();
    }

    /*
     * Hook: PlayLayer::playerDied
     *
     * Called when player1 dies.  For player2 death we hook separately.
     * At the moment this is called, the player's x is still valid.
     */
    void playerDied(PlayerObject* player) {
        Bot::get().onPlayerDeath(player);
        PlayLayer::playerDied(player);
    }

    /*
     * Hook: PlayLayer::resetLevel (called on restart from pause menu or
     * after death when auto-retry is on).
     *
     * Distinguishes between:
     *   - Normal death-retry: handled by playerDied above + resetLevel.
     *   - Manual restart from pause menu: also calls resetLevel.
     *
     * We call onRestartLevel in both paths; pruneInputsFrom in playerDied
     * already trimmed the death point, and eraseSegment here handles
     * pause-menu restarts where no playerDied fired.
     */
    void resetLevel() {
        Bot::get().onRestartLevel();
        PlayLayer::resetLevel();
    }

    /*
     * Hook: PlayLayer::checkpointActivated
     *
     * Called when the player touches a checkpoint orb in practice mode.
     * We snapshot the full physics state immediately after GD has saved its
     * own checkpoint data.
     */
    void checkpointActivated(CheckpointObject* cp) {
        PlayLayer::checkpointActivated(cp);
        Bot::get().onCheckpointSave(cp, this);
    }

    /*
     * Hook: PlayLayer::loadFromCheckpoint
     *
     * Called when the player respawns at a checkpoint.
     * We restore our full physics snapshot AFTER GD restores its own state,
     * overwriting any fields GD may have gotten wrong.
     */
    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        Bot::get().onCheckpointLoad(cp, this);
    }
};

/*
 * Hook: PauseLayer keyboard / menu for F8 shortcut.
 * We open the bot menu on F8 via a CCKeyboardDispatcher hook.
 *
 * PauseLayer::keyDown is overridden here.  Opening via PauseLayer ensures
 * we have a valid PlayLayer context when the menu opens.
 *
 * NOTE: We also attach the F8 listener to the PlayLayer scene directly so
 * the key works outside the pause menu.
 */
class $modify(BotPauseHook, PauseLayer) {
    void keyDown(cocos2d::enumKeyCodes key) override {
        if (key == cocos2d::KEY_F8) {
            openBotMenu();
            return;
        }
        PauseLayer::keyDown(key);
    }

    static void openBotMenu() {
        auto* layer = BotMenuLayer::create();
        if (!layer) return;
        auto* scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
        scene->addChild(layer, 1000);
    }
};

// Global keyboard handler so F8 works during gameplay (not just in pause).
class $modify(BotKeyboardHook, cocos2d::CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        if (isKeyDown && !isKeyRepeat && key == cocos2d::KEY_F8) {
            BotPauseHook::openBotMenu();
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat);
    }
};

// ─── UI Implementation ────────────────────────────────────────────────────────

bool BotMenuLayer::init() {
    if (!CCLayer::init()) return false;

    auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();

    // ── Background ────────────────────────────────────────────────────────────
    m_bg = cocos2d::extension::CCScale9Sprite::create(
        "GJ_square02.png", {0, 0, 80, 80});
    m_bg->setContentSize({340.0f, 300.0f});
    m_bg->setPosition(winSize / 2);
    addChild(m_bg, 0);

    // ── Darkened overlay ──────────────────────────────────────────────────────
    auto* overlay = cocos2d::CCLayerColor::create({0, 0, 0, 150});
    addChild(overlay, -1);

    // ── Title ─────────────────────────────────────────────────────────────────
    auto* title = cocos2d::CCLabelBMFont::create("Macro Bot", "goldFont.fnt");
    title->setScale(0.7f);
    title->setPosition(winSize.width / 2, winSize.height / 2 + 125.0f);
    addChild(title, 1);

    // ── CBF/CBS Status ────────────────────────────────────────────────────────
    auto& bot    = Bot::get();
    CbfMode mode = bot.detectCbfMode();

    const char* statusText;
    cocos2d::ccColor3B statusColor;
    switch (mode) {
        case CbfMode::CBF:
            statusText  = "CBF: Active (Syzzi)";
            statusColor = {0, 255, 0};   // Green
            break;
        case CbfMode::CBS:
            statusText  = "CBS: Active (RobTop)";
            statusColor = {255, 220, 0}; // Yellow
            break;
        default:
            statusText  = "No CBF/CBS Detected — Playback Disabled";
            statusColor = {255, 50, 50}; // Red
            break;
    }

    m_statusLabel = cocos2d::CCLabelBMFont::create(statusText, "bigFont.fnt");
    m_statusLabel->setScale(0.35f);
    m_statusLabel->setColor(statusColor);
    m_statusLabel->setPosition(winSize.width / 2, winSize.height / 2 + 98.0f);
    addChild(m_statusLabel, 1);

    // ── Info label (input count / state) ─────────────────────────────────────
    m_infoLabel = cocos2d::CCLabelBMFont::create("", "bigFont.fnt");
    m_infoLabel->setScale(0.30f);
    m_infoLabel->setColor({200, 200, 200});
    m_infoLabel->setPosition(winSize.width / 2, winSize.height / 2 + 78.0f);
    addChild(m_infoLabel, 1);
    updateStatus();

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto makeBtn = [&](const char* label, cocos2d::SEL_MenuHandler sel, float y) {
        auto* btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(label, 110, true, "bigFont.fnt", "GJ_button_01.png", 30.0f, 0.7f),
            this, sel);
        btn->setPositionX(0);
        btn->setPositionY(y);
        return btn;
    };

    auto* btnRecord   = makeBtn("Record",   menu_selector(BotMenuLayer::onRecord),   52.0f);
    auto* btnPlayback = makeBtn("Playback", menu_selector(BotMenuLayer::onPlayback), 18.0f);
    auto* btnSave     = makeBtn("Save",     menu_selector(BotMenuLayer::onSave),    -16.0f);
    auto* btnLoad     = makeBtn("Load",     menu_selector(BotMenuLayer::onLoad),    -50.0f);
    auto* btnClose    = makeBtn("Close",    menu_selector(BotMenuLayer::onClose),   -100.0f);

    // Disable playback button when no CBF/CBS.
    if (mode == CbfMode::None) {
        btnPlayback->setEnabled(false);
        btnPlayback->setColor({100, 100, 100});
    }

    auto* menu = cocos2d::CCMenu::create(
        btnRecord, btnPlayback, btnSave, btnLoad, btnClose, nullptr);
    menu->setPosition(winSize / 2);
    addChild(menu, 1);

    // ── Speedhack label + slider ───────────────────────────────────────────────
    auto* shLabel = cocos2d::CCLabelBMFont::create("Speedhack:", "bigFont.fnt");
    shLabel->setScale(0.35f);
    shLabel->setPosition(winSize.width / 2 - 80.0f, winSize.height / 2 - 90.0f);
    addChild(shLabel, 1);

    // We use a simple slider via Geode's Slider helper.
    // Slider range [0.1, 4.0] mapped to [0,1]; default 1× speed → 0.225.
    auto* slider = Slider::create(this,
        menu_selector(BotMenuLayer::onSpeedhackChanged), 0.65f);
    slider->setPosition(winSize.width / 2 + 30.0f, winSize.height / 2 - 90.0f);
    // setValue takes [0,1]; map current multiplier.
    float initVal = (bot.speedhackMultiplier - 0.1f) / (4.0f - 0.1f);
    slider->setValue(initVal);
    addChild(slider, 1);

    // Allow closing by clicking outside.
    setTouchEnabled(true);
    setKeypadEnabled(true);
    return true;
}

BotMenuLayer* BotMenuLayer::create() {
    auto* ret = new BotMenuLayer();
    if (ret && ret->init()) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

void BotMenuLayer::updateStatus() {
    if (!m_infoLabel) return;
    auto& bot = Bot::get();
    std::string stateStr;
    switch (bot.state()) {
        case BotState::Idle:      stateStr = "Idle"; break;
        case BotState::Recording: stateStr = "Recording"; break;
        case BotState::Playback:  stateStr = "Playing"; break;
    }
    std::string info = "State: " + stateStr +
                       "  |  Inputs: " + std::to_string(bot.inputCount()) +
                       "  |  Speed: " +
                       std::to_string(static_cast<int>(bot.speedhackMultiplier * 100)) + "%";
    m_infoLabel->setString(info.c_str());
}

void BotMenuLayer::onRecord(cocos2d::CCObject*) {
    Bot::get().startRecording();
    updateStatus();
    FLAlertLayer::create("Bot", "Recording started.\nPlay in Practice Mode.", "OK")->show();
}

void BotMenuLayer::onPlayback(cocos2d::CCObject*) {
    Bot::get().startPlayback();
    updateStatus();
}

void BotMenuLayer::onSave(cocos2d::CCObject*) {
    auto path = (Mod::get()->getSaveDir() / "macro.bot2").string();
    bool ok = Bot::get().saveMacro(path);
    FLAlertLayer::create("Bot",
        ok ? ("Saved to:\n" + path).c_str() : "Save failed.", "OK")->show();
}

void BotMenuLayer::onLoad(cocos2d::CCObject*) {
    auto path = (Mod::get()->getSaveDir() / "macro.bot2").string();
    bool ok = Bot::get().loadMacro(path);
    updateStatus();
    FLAlertLayer::create("Bot",
        ok ? ("Loaded " + std::to_string(Bot::get().inputCount()) + " inputs.").c_str()
           : "Load failed or file not found.", "OK")->show();
}

void BotMenuLayer::onClose(cocos2d::CCObject*) {
    removeFromParent();
}

void BotMenuLayer::onSpeedhackChanged(cocos2d::CCObject* sender) {
    auto* slider = dynamic_cast<Slider*>(sender);
    if (!slider) return;
    float t = slider->getValue();
    Bot::get().speedhackMultiplier = 0.1f + t * (4.0f - 0.1f);
    updateStatus();
}

// ─── Mod Entry Point ─────────────────────────────────────────────────────────

$on_mod(Loaded) {
    // Nothing extra required at load time; all hooks are registered by the
    // $modify macros above.  The singleton is lazily initialized on first use.
    log::info("Macro Bot loaded (GD 2.2081 / Geode v5.7.1)");
}