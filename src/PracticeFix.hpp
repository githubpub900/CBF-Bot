#pragma once

#include <Geode/Geode.hpp>

namespace cbf {

// ============================================================================
//  PracticeFix
// ----------------------------------------------------------------------------
//  Goal: a level's physics MUST evolve identically whether the player is in
//  Practice Mode or Normal Mode, so a macro recorded in Practice Mode plays
//  back bit-for-bit identical in Normal Mode.
//
//  Known root causes of the "practice bug" (desync between practice & normal):
//
//    1. Variable dt passed to PlayLayer::update. In practice mode the game
//       can briefly stall (checkpoint placement, pause/resume), and the
//       resulting dt spike shifts physics forward by a non-integer number
//       of ticks. This accumulates and desyncs the run.
//
//    2. Non-deterministic RNG. GD uses several PRNGs (GameManager random,
//       particle seeds, etc.) that can drift between practice and normal
//       sessions. Some objects consume RNG differently depending on mode.
//
//    3. Checkpoint-restore state loss. When the player dies and respawns at
//       a checkpoint, RobTop's restore is "good enough for a human" but
//       not byte-exact: small floats (residual velocity, rotation snap)
//       differ from the original pass.
//
//    4. Start timing. Practice Mode can start the player one tick earlier
//       or later than Normal Mode depending on the fade-in animation.
//
//  What we do:
//    - Hook PlayLayer::update to clamp dt to at most one CBF tick worth of
//      time. This eliminates dt spikes (root cause #1).
//    - Provide a fixed RNG seed that's identical in practice and normal
//      mode (root cause #2). We reseed on level start.
//    - Force a clean physics reset on level start (root cause #4).
//    - For checkpoints: we don't try to fix RobTop's restore byte-for-byte;
//      instead, since we RECORD inputs from a clean practice run (no
//      deaths), checkpoint restore never happens during recording. The
//      recorded macro is therefore immune to #3.
//
//  This gives us a "really accurate" practice fix: deterministic dt, fixed
//  RNG, clean start. Combined with CBF for sub-frame timing, practice and
//  normal modes become indistinguishable from the macro's perspective.
// ============================================================================
class PracticeFix {
public:
    static PracticeFix& get();

    // Called by MacroBot on mod load.
    void install();

    // Called on PlayLayer init.
    void onLevelStart(bool isPracticeMode);

    // Called on PlayLayer destroy.
    void onLevelEnd();

    bool isEnabled() const;
    void setEnabled(bool e);

    // Returns the dt that SHOULD be used for the current frame given the
    // active speedhack multiplier. This is what PlayLayer::update receives
    // after our hook clamps it.
    float computeFixedDt(float rawDt) const;

    // Apply deterministic RNG seed. Safe to call repeatedly.
    void applyDeterministicSeed();

private:
    PracticeFix();
    bool m_installed = false;
    bool m_enabled   = true;
    bool m_inLevel   = false;
    bool m_isPractice = false;
    unsigned m_seed = 0x43524F57u; // 'CROW' - arbitrary stable seed
};

} // namespace cbf
