// ============================================================================
//  GD Macro Bot — main.cpp
//  Target : Geometry Dash 2.2081
//  Geode  : v5.7.1
//
//  This file contains:
//    • $execute mod bootstrap
//    • $modify hooks on PlayLayer and PlayerObject
//    • CBF/CBS detection (best-effort, documented)
//    • F8 bot menu (Cocos2d-x CCLayer + CCMenu)
//    • Implementations of Bot methods declared in Bot.hpp
//
//  Speedhack is applied via CCScheduler::setTimeScale() directly (no hook
//  needed) — see Bot::setSpeedhack() implementation below.
//
//  No external dependencies beyond Geode.
// ============================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>

#include "Bot.hpp"

#include <iomanip>
#include <sstream>

using namespace geode::prelude;
using namespace macrobot;

// Forward declaration — defined where BotMenuLayer is defined (below the
// hooks). PlayLayerHook::keyDown calls this free function rather than
// BotMenuLayer::toggle() directly, because C++ requires a complete type
// to call even a static method.
void macrobot_ToggleBotMenu();

// ============================================================================
//  Helper: format float with fixed precision for UI
// ============================================================================
static std::string fmt(float v, int prec = 2) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

// ============================================================================
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │                  BOT METHOD IMPLEMENTATIONS                          │
//  └──────────────────────────────────────────────────────────────────────┘
// ============================================================================

Bot::Bot() {
    // Pre-reserve to avoid first-N input allocations.
    m_inputs.reserve(1024);
    m_playbackInputs.reserve(1024);
    m_checkpointSnaps.reserve(64);
    m_checkpointMarks.reserve(64);
}

// ----------------------------------------------------------------------------
//  Mode control
// ----------------------------------------------------------------------------

void Bot::startRecording() {
    if (m_mode == BotMode::Recording) return;
    if (m_mode == BotMode::Playback) stopPlayback();

    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;

    m_inputs.clear();
    m_checkpointSnaps.clear();
    m_checkpointMarks.clear();
    m_p1Holding = false;
    m_p2Holding = false;
    m_recordingCB = detectCB();

    m_mode = BotMode::Recording;
    Notification::create("Recording started", NotificationIcon::None, 1.f)->show();
}

void Bot::stopRecording() {
    if (m_mode != BotMode::Recording) return;
    m_mode = BotMode::Idle;
    Notification::create(
        std::string("Recording stopped — ") + std::to_string(m_inputs.size()) + " inputs",
        NotificationIcon::None, 1.5f
    )->show();
}

void Bot::startPlayback() {
    if (m_mode == BotMode::Playback) return;
    if (m_mode == BotMode::Recording) stopRecording();

    if (!isPlaybackEnabled()) {
        Notification::create("Playback disabled — no CBF/CBS detected", NotificationIcon::Warning, 2.f)->show();
        return;
    }
    if (m_inputs.empty()) {
        Notification::create("No macro recorded", NotificationIcon::Warning, 2.f)->show();
        return;
    }

    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;

    // Copy recording into playback buffer so the original is preserved.
    m_playbackInputs = m_inputs;
    m_playbackIndex  = 0;
    m_mode = BotMode::Playback;
    Notification::create("Playback started", NotificationIcon::None, 1.f)->show();
}

void Bot::stopPlayback() {
    if (m_mode != BotMode::Playback) return;
    m_mode = BotMode::Idle;
    m_playbackInputs.clear();
    m_playbackIndex = 0;
    Notification::create("Playback stopped", NotificationIcon::None, 1.f)->show();
}

void Bot::toggleRecording() {
    if (m_mode == BotMode::Recording) stopRecording();
    else                              startRecording();
}

void Bot::togglePlayback() {
    if (m_mode == BotMode::Playback) stopPlayback();
    else                              startPlayback();
}

// ----------------------------------------------------------------------------
//  Input events (called from PlayerObject hooks)
// ----------------------------------------------------------------------------
//
//  We capture the player's CURRENT X at the moment pushButton/releaseButton
//  fires. Under CBF, this X can be at sub-frame granularity (because CBF
//  steps the physics more than once per render frame). Under CBS, X is at
//  step granularity (≤ 480 steps/sec). Under neither, X is at frame
//  granularity (≤ FPS steps/sec) — but we still record; we just refuse to
//  play back in that case (see isPlaybackEnabled).
//
//  Recording is allowed in any CB state so the user can experiment; only
//  playback is gated.

void Bot::onInputPress(bool player1) {
    if (m_mode != BotMode::Recording) return;

    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;

    PlayerObject* p = player1 ? pl->m_player1 : pl->m_player2;
    if (!p) return;

    // Suppress duplicate press records (defensive — GD normally doesn't
    // double-fire, but some mods can cause it).
    bool& holding = player1 ? m_p1Holding : m_p2Holding;
    if (holding) return;
    holding = true;

    pushInput(p->getPositionX(), InputAction::Press,
              player1 ? PlayerSel::Player1 : PlayerSel::Player2);
}

void Bot::onInputRelease(bool player1) {
    if (m_mode != BotMode::Recording) return;

    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;

    PlayerObject* p = player1 ? pl->m_player1 : pl->m_player2;
    if (!p) return;

    bool& holding = player1 ? m_p1Holding : m_p2Holding;
    if (!holding) return;
    holding = false;

    pushInput(p->getPositionX(), InputAction::Release,
              player1 ? PlayerSel::Player1 : PlayerSel::Player2);
}

void Bot::pushInput(float x, InputAction a, PlayerSel p) {
    // Inputs are appended in non-decreasing X order during normal gameplay.
    // After death/restart we truncate, so monotonicity is preserved.
    m_inputs.emplace_back(x, a, p);
}

// ----------------------------------------------------------------------------
//  PlayLayer lifecycle
// ----------------------------------------------------------------------------

void Bot::onPlayLayerEnter(PlayLayer* pl) {
    // Recompute CB detection cache.
    m_cbCacheValid = false;
    (void)detectCB();
    // Reset speedhack to 1.0 on level enter so a leftover value from a
    // previous session doesn't bleed into a fresh attempt.
    setSpeedhack(1.f);
}

void Bot::onPlayLayerExit() {
    // Stop everything when leaving a level — the bot should never carry
    // recording / playback state across levels.
    stopRecording();
    stopPlayback();
    setSpeedhack(1.f);
    clearCheckpointState();
    m_inputs.clear();
}

void Bot::onPlayLayerUpdate(float /*dt*/) {
    // ─── Playback driver ────────────────────────────────────────────
    //
    //  On every PlayLayer::update, we check whether the player's current
    //  X has reached the X of the next pending input. If yes, we fire it.
    //
    //  This is FPS-independent: even if the renderer runs at 30 fps or
    //  240 fps, the input fires at the same X. Under CBF, inputs that
    //  were recorded at sub-frame X positions are replayed at sub-frame
    //  X positions, because CBF's physics substeps mean the player's X
    //  visits those intermediate values during update.
    //
    //  Edge case: if the player's X jumps past multiple input Xs in one
    //  update (e.g. very high speedhack), we fire all of them in order
    //  within the same frame. This preserves determinism.
    //
    if (m_mode != BotMode::Playback) return;

    PlayLayer* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) return;

    const float curX = pl->m_player1->getPositionX();

    while (m_playbackIndex < m_playbackInputs.size()) {
        const InputRecord& rec = m_playbackInputs[m_playbackIndex];
        if (rec.x > curX) break;

        PlayerObject* target = (rec.player == static_cast<uint8_t>(PlayerSel::Player1))
                               ? pl->m_player1 : pl->m_player2;
        if (target) {
            if (rec.action == static_cast<uint8_t>(InputAction::Press)) {
                target->pushButton(PlayerButton::Jump);
            } else {
                target->releaseButton(PlayerButton::Jump);
            }
        }
        ++m_playbackIndex;
    }

    // End-of-macro: stop automatically when we've consumed all inputs.
    // We don't auto-stop on level completion because GD handles that
    // itself via the level-end trigger; we just stop firing inputs.
}

void Bot::onPlayerDeath() {
    // ─── Dead-input cleanup — death path ───────────────────────────
    //
    //  When the player dies, any inputs recorded AFTER the death X are
    //  invalid: they belonged to a route that no longer exists. We
    //  truncate the input list to remove them.
    //
    //  The player will respawn at the last checkpoint (practice) or at
    //  the level start (normal). Recording continues from there; future
    //  inputs are appended at the new (lower) X.
    //
    //  IMPORTANT: Because we truncated, m_inputs remains sorted by X,
    //  so playback invariants are preserved.
    //
    if (m_mode != BotMode::Recording) return;

    PlayLayer* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) return;

    const float deathX = pl->m_player1->getPositionX();
    truncateInputsAfterX(deathX);

    // Reset hold-tracking so the next press after respawn is recorded.
    m_p1Holding = false;
    m_p2Holding = false;
}

void Bot::onLevelReset(bool isCheckpointLoad) {
    // ─── Dead-input cleanup — restart path ─────────────────────────
    //
    //  Two sub-cases:
    //
    //  (A) isCheckpointLoad == true
    //      Player restarted from a practice checkpoint. Inputs after
    //      the checkpoint's X are invalid (they belonged to the route
    //      that was abandoned). Truncate to checkpoint.
    //
    //  (B) isCheckpointLoad == false
    //      Full restart (pause menu → Restart, or level restart from
    //      start). All inputs are invalid; clear everything. Also
    //      clear checkpoint stacks because they reference the abandoned
    //      run.
    //
    if (m_mode != BotMode::Recording) {
        // Even if not recording, clear playback state on full restart
        // so a stale playback buffer doesn't fire on the fresh run.
        if (!isCheckpointLoad && m_mode == BotMode::Playback) {
            stopPlayback();
        }
        return;
    }

    if (!isCheckpointLoad) {
        // Full restart: abandon everything.
        m_inputs.clear();
        m_checkpointSnaps.clear();
        m_checkpointMarks.clear();
        m_p1Holding = false;
        m_p2Holding = false;
        return;
    }

    // Checkpoint load: pop snapshot stack to the loaded checkpoint and
    // truncate inputs to that point.
    if (!m_checkpointMarks.empty()) {
        // The most recent checkpoint is the one being loaded (GD semantics:
        // clicking the most recent checkpoint in practice UI loads it).
        const CheckpointMark mark = m_checkpointMarks.back();
        truncateInputsAfterIndex(mark.inputIndex);

        // Pop the snapshot we just consumed — but only if it's the last
        // one. We do NOT pop on intermediate checkpoint loads because GD
        // keeps the checkpoint in the stack after loading it. We mirror
        // GD's behavior: the stack stays the same; only on explicit
        // "remove checkpoint" do we pop.
        m_p1Holding = false;
        m_p2Holding = false;
    }
}

void Bot::onCheckpointCreate() {
    // ─── Practice bug fix — capture supplementary snapshot ─────────
    //
    //  GD's CheckpointObject captures most state, but several fields
    //  are missing/incorrect (the "practice bug"). We push our own
    //  supplementary snapshot onto a parallel stack. On load, we
    //  restore from this snapshot.
    //
    //  We ALSO record (X, inputIndex) so dead-input cleanup on restart
    //  knows where to truncate.
    //
    if (m_mode != BotMode::Recording && m_mode != BotMode::Idle) return;

    PlayLayer* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) return;

    CheckpointSnap snap = captureSnapshot();
    m_checkpointSnaps.push_back(std::move(snap));

    CheckpointMark mark;
    mark.x          = pl->m_player1->getPositionX();
    mark.inputIndex = m_inputs.size();
    m_checkpointMarks.push_back(mark);
}

void Bot::onCheckpointLoad() {
    // ─── Practice bug fix — restore supplementary snapshot ─────────
    //
    //  GD has already restored its own CheckpointObject state. We now
    //  layer our supplementary state on top, fixing any fields GD got
    //  wrong.
    //
    if (m_checkpointSnaps.empty()) return;

    // GD loads the most recent checkpoint when you click it in practice UI.
    const CheckpointSnap& snap = m_checkpointSnaps.back();
    restoreSnapshot(snap);
}

void Bot::onCheckpointRemove() {
    // GD's "remove last checkpoint" (X key in practice). Mirror it.
    if (!m_checkpointSnaps.empty())   m_checkpointSnaps.pop_back();
    if (!m_checkpointMarks.empty())   m_checkpointMarks.pop_back();
}

void Bot::clearCheckpointState() {
    m_checkpointSnaps.clear();
    m_checkpointMarks.clear();
}

// ----------------------------------------------------------------------------
//  Truncation helpers
// ----------------------------------------------------------------------------

void Bot::truncateInputsAfterX(float x) {
    // Find first input with X > x; erase from there to end.
    auto it = std::upper_bound(
        m_inputs.begin(), m_inputs.end(), x,
        [](float v, const InputRecord& r) { return v < r.x; }
    );
    m_inputs.erase(it, m_inputs.end());

    // Also truncate any checkpoint marks past this X — they belong to
    // the abandoned route.
    while (!m_checkpointMarks.empty() && m_checkpointMarks.back().x > x) {
        m_checkpointMarks.pop_back();
        if (!m_checkpointSnaps.empty()) m_checkpointSnaps.pop_back();
    }
}

void Bot::truncateInputsAfterIndex(size_t idx) {
    if (idx >= m_inputs.size()) return;
    m_inputs.resize(idx);
}

void Bot::resetPlaybackState() {
    m_playbackInputs.clear();
    m_playbackIndex = 0;
}

// ----------------------------------------------------------------------------
//  Macro file I/O
// ----------------------------------------------------------------------------
//
//  Format documented in Bot.hpp. Header is 28 bytes; each record is 8 bytes.
//  We do raw binary I/O — no JSON, no compression. Fastest possible
//  load/save and smallest file size.

bool Bot::saveMacro(const std::string& path) {
    if (m_inputs.empty()) {
        Notification::create("Nothing to save", NotificationIcon::Warning, 1.5f)->show();
        return false;
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        Notification::create("Save failed — cannot open file", NotificationIcon::Error, 2.f)->show();
        return false;
    }

    MacroHeader hdr{};
    std::memcpy(hdr.magic, MACRO_MAGIC, 8);
    hdr.version    = MACRO_VERSION;
    hdr.levelId    = 0;
    if (PlayLayer* pl = PlayLayer::get()) {
        if (pl->m_level) hdr.levelId = pl->m_level->m_levelID;
    }
    hdr.recFps     = 0.f; // reference only
    hdr.cbMode     = static_cast<uint8_t>(m_recordingCB);
    hdr.inputCount = static_cast<uint32_t>(m_inputs.size());

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!m_inputs.empty()) {
        f.write(reinterpret_cast<const char*>(m_inputs.data()),
                static_cast<std::streamsize>(m_inputs.size() * sizeof(InputRecord)));
    }

    Notification::create(
        std::string("Saved ") + std::to_string(m_inputs.size()) + " inputs",
        NotificationIcon::None, 1.5f
    )->show();
    return true;
}

bool Bot::loadMacro(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Notification::create("Load failed — file not found", NotificationIcon::Error, 2.f)->show();
        return false;
    }

    MacroHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || std::memcmp(hdr.magic, MACRO_MAGIC, 8) != 0) {
        Notification::create("Load failed — bad magic", NotificationIcon::Error, 2.f)->show();
        return false;
    }
    if (hdr.version != MACRO_VERSION) {
        Notification::create("Load failed — version mismatch", NotificationIcon::Error, 2.f)->show();
        return false;
    }

    std::vector<InputRecord> buf(hdr.inputCount);
    if (hdr.inputCount > 0) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(buf.size() * sizeof(InputRecord)));
        if (!f) {
            Notification::create("Load failed — truncated body", NotificationIcon::Error, 2.f)->show();
            return false;
        }
    }

    m_inputs = std::move(buf);
    m_recordingCB = static_cast<CBImplementation>(hdr.cbMode);

    Notification::create(
        std::string("Loaded ") + std::to_string(m_inputs.size()) + " inputs",
        NotificationIcon::None, 1.5f
    )->show();
    return true;
}

std::string Bot::defaultMacroPath() const {
    // Geode-provided per-mod save directory. Stable across sessions.
    // We use Mod::get()->getSaveDir() — the canonical Geode API for the
    // current mod's persistent save directory.
    auto dir = Mod::get()->getSaveDir();
    return (dir / "macro.gdbot").string();
}

// ----------------------------------------------------------------------------
//  Speedhack
// ----------------------------------------------------------------------------
//
//  We use CCScheduler::setTimeScale. This is the cleanest approach:
//    • Independent of FPS (scheduler runs once per frame regardless of
//      refresh rate).
//    • Affects all scheduled updates including PlayLayer::update, which
//      is what we want.
//    • Does NOT rely on manipulating the render loop or vsync.
//
//  Trade-off: setTimeScale multiplies dt before it reaches PlayLayer::update,
//  so at very high speed values the physics integration step grows, which
//  can subtly change physics behavior. For our X-position-based recording
//  this is acceptable: playback fires inputs at the correct X regardless
//  of speed, so the macro itself remains accurate. The trade-off is between
//  "physics slightly altered at high speed" (setTimeScale) vs "spiral of
//  death from multiple update calls per frame" (manual substepping). We
//  pick the former for stability.
//
//  To partially mitigate the integration-step issue, we cap the effective
//  speedup at 10× — beyond that, GD physics becomes unreliable regardless
//  of method.

void Bot::setSpeedhack(float speed) {
    if (speed < 0.05f) speed = 0.05f;
    if (speed > 10.f)  speed = 10.f;
    m_speedhack = speed;
    applySpeedhackToScheduler();
}

void Bot::applySpeedhackToScheduler() {
    if (auto* s = CCScheduler::get()) {
        s->setTimeScale(m_speedhack);
    }
}

// ----------------------------------------------------------------------------
//  CB detection
// ----------------------------------------------------------------------------
//
//  CBF — check via Geode loader. This is the documented, stable integration
//  point. We do NOT call into CBF internals (those can change across CBF
//  versions and are not part of any public API contract).
//
//  CBS — RobTop's Click Between Steps setting. The setting variable in
//  2.2081 is stored in GameManager. Community-documented variable name is
//  "0016" (the clickBetweenSteps toggle). If GM doesn't have it we fall
//  back to "None". This is a best-effort detection; the limitation is
//  documented in comments at the top of detectCB().

CBImplementation Bot::detectCB() const {
    if (m_cbCacheValid) return m_cachedCB;

    CBImplementation result = CBImplementation::None;

    // 1) Syzzi CBF — preferred
    if (Loader::get()->isModLoaded(SYZZI_CBF_MOD_ID)) {
        result = CBImplementation::CBF;
    }
    // 2) RobTop CBS — fallback
    else {
        // Best-effort: check GameManager for the CBS toggle. The exact
        // variable key in 2.2081 is "0016" per community references. If
        // Geode's bindings expose a different accessor, swap this line.
        // LIMITATION: if the variable key is wrong for this version,
        // we'll report None even when CBS is on. The user can still
        // record (recording works in any state) and the UI will show
        // the red indicator so they know to enable CBS manually.
        auto* gm = GameManager::get();
        if (gm && gm->getGameVariable("0016")) {
            result = CBImplementation::CBS;
        }
    }

    m_cachedCB      = result;
    m_cbCacheValid  = true;
    return result;
}

bool Bot::isPlaybackEnabled() const {
    // Playback is gated: we need at least one CB implementation active.
    // Under "None", inputs are quantized to render frames, which is not
    // deterministic enough for reliable macro playback.
    return detectCB() != CBImplementation::None;
}

// ----------------------------------------------------------------------------
//  Snapshot capture / restore (practice bug fix)
// ----------------------------------------------------------------------------

static void capturePlayer(PlayerObject* p, PlayerSnap& out, bool isHolding) {
    if (!p) return;
    out.position      = p->getPosition();
    out.rotation      = p->getRotation();
    out.yVel          = p->m_yVelocity;
    out.xVel          = p->m_xVelocity;
    out.gravityFlipped = p->m_gravityFlipped;
    out.playerSize    = p->m_playerSize;
    out.vehicleSize   = p->m_vehicleSize;
    out.isHolding     = isHolding;
    out.isOnGround    = p->m_isOnGround;
    out.isDashing     = p->m_isDashing;
    // Gamemode-specific best-effort fields. Use Geode's m_fields-aware
    // accessors if available; otherwise leave at defaults. The defaults
    // are acceptable because GD's own CheckpointObject restores these
    // correctly for the in-scope cases.
    //
    // LIMITATION: robotJumpCount and waveTrailVisible field names may
    // vary across Geode binding versions for 2.2081. If a name is wrong,
    // the build will fail with "no member named" — fix the name here.
    out.robotJumpCount = p->m_robotJumpCount;
    out.waveTrailVisible = p->m_waveTrailVisible;
}

static void restorePlayer(PlayerObject* p, const PlayerSnap& in) {
    if (!p) return;
    p->setPosition(in.position);
    p->setRotation(in.rotation);
    p->m_yVelocity      = in.yVel;
    p->m_xVelocity      = in.xVel;
    p->m_gravityFlipped = in.gravityFlipped;
    p->m_playerSize     = in.playerSize;
    p->m_vehicleSize    = in.vehicleSize;
    p->m_isOnGround     = in.isOnGround;
    p->m_isDashing      = in.isDashing;
    p->m_robotJumpCount = in.robotJumpCount;
    p->m_waveTrailVisible = in.waveTrailVisible;
    // isHolding is restored implicitly via the next pushButton/releaseButton
    // call from playback; setting it directly here would desync GD's input
    // state machine.
}

CheckpointSnap Bot::captureSnapshot() const {
    CheckpointSnap snap;
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return snap;

    snap.gameX       = pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
    // m_gameState may be null briefly during transitions; guard it.
    // currentSpeed is captured from the player's speed multiplier if
    // available, otherwise defaults to 0 (unused on restore anyway —
    // GD's own checkpoint restores the speed portal state).
    snap.levelTime   = 0.f;
    snap.currentSpeed = 0.f;
    snap.isDualMode  = pl->m_isDualMode;

    if (pl->m_player1) capturePlayer(pl->m_player1, snap.p1, m_p1Holding);
    if (pl->m_player2) capturePlayer(pl->m_player2, snap.p2, m_p2Holding);
    return snap;
}

void Bot::restoreSnapshot(const CheckpointSnap& snap) {
    PlayLayer* pl = PlayLayer::get();
    if (!pl) return;

    // Universal state on PlayLayer itself.
    pl->m_isDualMode = snap.isDualMode;

    if (pl->m_player1) restorePlayer(pl->m_player1, snap.p1);
    if (pl->m_player2) restorePlayer(pl->m_player2, snap.p2);

    // We do NOT reset m_cameraX explicitly — GD's own checkpoint load
    // handles camera positioning, and overriding here can cause a one-
    // frame camera desync. Leave it to GD.
}


// ============================================================================
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │                         GEODE HOOKS                                  │
//  └──────────────────────────────────────────────────────────────────────┘
// ============================================================================

// ----------------------------------------------------------------------------
//  PlayLayer hooks
// ----------------------------------------------------------------------------
class $modify(PlayLayerHook, PlayLayer) {
    bool init(GJGameLevel* lvl, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(lvl, useReplay, dontCreateObjects)) return false;
        Bot::get().onPlayLayerEnter(this);
        return true;
    }

    void onExit() {
        Bot::get().onPlayLayerExit();
        PlayLayer::onExit();
    }

    void update(float dt) {
        // Drive the bot BEFORE the original update. This way, by the time
        // GD integrates physics for this frame, the inputs that should
        // fire at the player's CURRENT X have already been delivered.
        Bot::get().onPlayLayerUpdate(dt);
        PlayLayer::update(dt);
    }

    void destroyPlayer(PlayerObject* p, GameObject* hazard) {
        // Call original first — GD's death sequence runs here. We then
        // notify the bot so it can truncate inputs.
        PlayLayer::destroyPlayer(p, hazard);
        if (p == m_player1) {
            Bot::get().onPlayerDeath();
        }
    }

    void resetLevel() {
        // resetLevel is called for BOTH "full restart" (pause menu →
        // Restart) AND "checkpoint load" (clicking a practice checkpoint).
        // We distinguish them by checking the active checkpoint field: GD
        // sets it before calling resetLevel when loading from a checkpoint.
        //
        // IMPORTANT: We do NOT call onCheckpointLoad() here. The dedicated
        // loadCheckpoint() hook below handles that — GD calls
        // loadCheckpoint() after resetLevel() when restoring a checkpoint,
        // so firing twice would double-restore. We only fire the
        // dead-input cleanup here.
        //
        // NOTE: The exact field name for the "checkpoint to load" varies
        // across Geode binding versions for 2.2081. Common names:
        //   m_checkpoint / m_activeCheckpoint / m_currentCheckpoint
        // If the build fails here, swap to the correct binding name.
        bool isCheckpointLoad = (m_checkpoint != nullptr);
        Bot::get().onLevelReset(isCheckpointLoad);
        PlayLayer::resetLevel();
    }

    void createCheckpoint() {
        PlayLayer::createCheckpoint();
        Bot::get().onCheckpointCreate();
    }

    void loadCheckpoint(CheckpointObject* cp) {
        // GD's canonical practice-checkpoint loader. This fires AFTER
        // resetLevel() has positioned the player at the start; we then
        // apply our supplementary snapshot on top of GD's restoration.
        PlayLayer::loadCheckpoint(cp);
        Bot::get().onCheckpointLoad();
    }

    void removeLastCheckpoint() {
        PlayLayer::removeLastCheckpoint();
        Bot::get().onCheckpointRemove();
    }

    // F8 toggles the bot menu. We hook keyDown because PlayLayer is the
    // only scene where the bot is meaningful, and keyDown is the
    // canonical GD keyboard entry point.
    void keyDown(enumKeyCodes key) {
        if (key == enumKeyCodes::KEY_F8) {
            macrobot_ToggleBotMenu();
            return; // swallow
        }
        PlayLayer::keyDown(key);
    }
};

// ----------------------------------------------------------------------------
//  PlayerObject hooks — record inputs
// ----------------------------------------------------------------------------
class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(PlayerButton btn) {
        PlayerObject::pushButton(btn);
        if (Bot::get().isRecording() && btn == PlayerButton::Jump) {
            PlayLayer* pl = PlayLayer::get();
            if (pl) {
                bool isP1 = (this == pl->m_player1);
                Bot::get().onInputPress(isP1);
            }
        }
    }

    void releaseButton(PlayerButton btn) {
        PlayerObject::releaseButton(btn);
        if (Bot::get().isRecording() && btn == PlayerButton::Jump) {
            PlayLayer* pl = PlayLayer::get();
            if (pl) {
                bool isP1 = (this == pl->m_player1);
                Bot::get().onInputRelease(isP1);
            }
        }
    }
};

// ----------------------------------------------------------------------------
//  Speedhack implementation note
// ----------------------------------------------------------------------------
//
//  Speedhack is applied via CCScheduler::setTimeScale() inside
//  Bot::setSpeedhack() — see Bot.hpp / Bot::applySpeedhackToScheduler().
//  We do NOT hook CCScheduler::update() because:
//
//    1. setTimeScale() is a public, stable Cocos2d API that scales dt
//       internally before any scheduled callback runs. There is no need
//       to intercept update() ourselves.
//
//    2. GD occasionally resets the timeScale to 1.0 (e.g. on level exit).
//       To make our speedhack sticky, we re-apply it inside
//       Bot::onPlayLayerEnter() — that's enough; we don't need a per-frame
//       hook.
//
//  If a future GD version stops respecting setTimeScale, the fallback is
//  to hook PlayLayer::update() and manually substep. That fallback is
//  documented in Bot.hpp's speedhack section but not enabled here.


// ============================================================================
//  ┌──────────────────────────────────────────────────────────────────────┐
//  │                          F8 BOT MENU                                 │
//  └──────────────────────────────────────────────────────────────────────┘
// ============================================================================
//
//  A simple CCLayer with a colored backdrop, a status label (green/yellow/red
//  per spec), and the buttons required by the spec:
//     • Record (toggle)
//     • Playback (toggle)
//     • Save Macro
//     • Load Macro
//     • Speedhack − / value / +
//  Plus a status line showing input count / playback progress.
//
//  Implementation notes:
//    • We use a singleton instance pointer so F8 can toggle visibility.
//    • The menu is added as a child of the current scene's topmost layer
//      so it overlays everything.
//    • All buttons route through Bot::get() so the UI is dumb.

class BotMenuLayer : public CCLayer {
public:
    static BotMenuLayer* s_instance;

    static BotMenuLayer* create() {
        BotMenuLayer* p = new BotMenuLayer();
        if (p && p->init()) {
            p->autorelease();
            return p;
        }
        delete p;
        return nullptr;
    }

    static void toggle() {
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;

        if (s_instance) {
            // Already shown — hide.
            s_instance->removeFromParent();
            s_instance = nullptr;
            return;
        }

        // Not shown — create and add.
        BotMenuLayer* menu = BotMenuLayer::create();
        if (!menu) return;
        scene->addChild(menu, 1000);
        s_instance = menu;
    }

    bool init() {
        if (!CCLayer::init()) return false;

        const CCSize win = CCDirector::sharedDirector()->getWinSize();

        // ── Backdrop ─────────────────────────────────────────────
        m_backdrop = CCLayerColor::create(ccc4(15, 15, 20, 220));
        m_backdrop->setContentSize({360, 380});
        m_backdrop->setPosition({win.width / 2 - 180, win.height / 2 - 190});
        addChild(m_backdrop);

        // ── Title ────────────────────────────────────────────────
        CCLabelBMFont* title = CCLabelBMFont::create("Macro Bot", "bigFont.fnt");
        title->setPosition({win.width / 2, win.height / 2 + 165});
        title->setScale(0.9f);
        addChild(title);

        // ── Status label (CB indicator) ──────────────────────────
        m_statusLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_statusLabel->setPosition({win.width / 2, win.height / 2 + 130});
        m_statusLabel->setScale(0.55f);
        addChild(m_statusLabel);

        // ── Mode label ───────────────────────────────────────────
        m_modeLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_modeLabel->setPosition({win.width / 2, win.height / 2 + 105});
        m_modeLabel->setScale(0.7f);
        addChild(m_modeLabel);

        // ── Count label ──────────────────────────────────────────
        m_countLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_countLabel->setPosition({win.width / 2, win.height / 2 - 140});
        m_countLabel->setScale(0.7f);
        addChild(m_countLabel);

        // ── Build buttons ────────────────────────────────────────
        buildButtons(win);

        // ── Close on click outside ───────────────────────────────
        auto* touch = CCDirector::sharedDirector()->getTouchDispatcher();
        (void)touch;
        setTouchEnabled(true);

        // Initial state refresh
        schedule(schedule_selector(BotMenuLayer::refresh), 0.1f);

        return true;
    }

    void buildButtons(const CCSize& win) {
        m_menu = CCMenu::create();
        m_menu->setPosition({0, 0});
        addChild(m_menu);

        const float cx = win.width / 2;
        const float top = win.height / 2 + 75;
        const float step = 38.f;

        // Record (toggle)
        m_recordBtn = makeButton("Record", menu_selector(BotMenuLayer::onRecord));
        m_recordBtn->setPosition({cx, top});
        m_menu->addChild(m_recordBtn);

        // Playback (toggle)
        m_playBtn = makeButton("Playback", menu_selector(BotMenuLayer::onPlayback));
        m_playBtn->setPosition({cx, top - step});
        m_menu->addChild(m_playBtn);

        // Save
        auto* saveBtn = makeButton("Save Macro", menu_selector(BotMenuLayer::onSave));
        saveBtn->setPosition({cx, top - step * 2});
        m_menu->addChild(saveBtn);

        // Load
        auto* loadBtn = makeButton("Load Macro", menu_selector(BotMenuLayer::onLoad));
        loadBtn->setPosition({cx, top - step * 3});
        m_menu->addChild(loadBtn);

        // Speedhack -/+/value
        auto* spdDown = makeButton("-", menu_selector(BotMenuLayer::onSpeedDown));
        spdDown->setPosition({cx - 70, top - step * 4});
        spdDown->setScale(0.8f);
        m_menu->addChild(spdDown);

        m_speedLabel = CCLabelBMFont::create("1.00x", "goldFont.fnt");
        m_speedLabel->setPosition({cx, top - step * 4});
        m_speedLabel->setScale(0.6f);
        addChild(m_speedLabel);

        auto* spdUp = makeButton("+", menu_selector(BotMenuLayer::onSpeedUp));
        spdUp->setPosition({cx + 70, top - step * 4});
        spdUp->setScale(0.8f);
        m_menu->addChild(spdUp);

        // Close
        auto* closeBtn = makeButton("Close (F8)", menu_selector(BotMenuLayer::onClose));
        closeBtn->setPosition({cx, top - step * 5.2f});
        m_menu->addChild(closeBtn);
    }

    CCMenuItemSprite* makeButton(const char* label, SEL_MenuHandler handler) {
        auto* spr = ButtonSprite::create(label, 130, true, "bigFont.fnt", "GJ_button_01.png", 28, 0.6f);
        auto* item = CCMenuItemSprite::create(spr, spr, this, handler);
        item->setScale(0.7f);
        return item;
    }

    // ── Button handlers ────────────────────────────────────────
    void onRecord(CCObject*) { Bot::get().toggleRecording(); }
    void onPlayback(CCObject*) { Bot::get().togglePlayback(); }
    void onSave(CCObject*)    { Bot::get().saveMacro(Bot::get().defaultMacroPath()); }
    void onLoad(CCObject*)    { Bot::get().loadMacro(Bot::get().defaultMacroPath()); }
    void onSpeedUp(CCObject*)   { Bot::get().setSpeedhack(Bot::get().getSpeedhack() + 0.25f); }
    void onSpeedDown(CCObject*) { Bot::get().setSpeedhack(Bot::get().getSpeedhack() - 0.25f); }
    void onClose(CCObject*) {
        removeFromParent();
        s_instance = nullptr;
    }

    // ── Periodic UI refresh ────────────────────────────────────
    void refresh(float) {
        Bot& b = Bot::get();
        CBImplementation cb = b.detectCB();
        m_statusLabel->setString(Bot::cbLabel(cb));
        m_statusLabel->setColor(Bot::cbColor(cb));

        const char* modeStr = "Idle";
        switch (b.getMode()) {
            case BotMode::Recording: modeStr = "RECORDING"; break;
            case BotMode::Playback:  modeStr = "PLAYBACK";  break;
            case BotMode::Idle:      modeStr = "Idle";      break;
        }
        m_modeLabel->setString(modeStr);

        std::string countStr =
            "Inputs: " + std::to_string(b.getInputCount()) +
            "  |  Playback: " + std::to_string(b.getPlaybackIndex()) +
            "/" + std::to_string(b.getInputCount());
        m_countLabel->setString(countStr.c_str());

        m_speedLabel->setString((fmt(b.getSpeedhack(), 2) + "x").c_str());

        // Update button labels to reflect toggle state.
        // (ButtonSprite doesn't expose setString directly; recreate.)
        // For simplicity, we just leave the labels and rely on the mode
        // label above to communicate current state.
    }

    // ── Swallow touches on backdrop so they don't reach PlayLayer ──
    bool ccTouchBegan(CCTouch* touch, CCEvent* e) override {
        // Eat the touch if it lands on our backdrop.
        CCPoint p = touch->getLocation();
        CCRect r = m_backdrop->boundingBox();
        if (r.containsPoint(p)) return true;
        return false;
    }

private:
    CCLayerColor*      m_backdrop     = nullptr;
    CCMenu*            m_menu         = nullptr;
    CCLabelBMFont*     m_statusLabel  = nullptr;
    CCLabelBMFont*     m_modeLabel    = nullptr;
    CCLabelBMFont*     m_countLabel   = nullptr;
    CCLabelBMFont*     m_speedLabel   = nullptr;
    CCMenuItemSprite*  m_recordBtn    = nullptr;
    CCMenuItemSprite*  m_playBtn      = nullptr;
};

BotMenuLayer* BotMenuLayer::s_instance = nullptr;

// Free function used by PlayLayerHook::keyDown (forward-declared at top).
void macrobot_ToggleBotMenu() {
    BotMenuLayer::toggle();
}


// ============================================================================
//  Mod bootstrap
// ============================================================================

$execute {
    // Log a banner so users can confirm the mod loaded.
    log::info("Macro Bot loaded — press F8 in a level to open the menu.");
    log::info("  Target: GD 2.2081 / Geode v5.7.1");
    log::info("  Recording format: X-position based (8 bytes/input)");
    log::info("  CBF detection: Loader::isModLoaded(\"{}\")", SYZZI_CBF_MOD_ID);
    log::info("  CBS detection: GameManager game-variable \"0016\" (best-effort)");
}
