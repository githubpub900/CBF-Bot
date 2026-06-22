#include "PracticeFix.hpp"
#include "CBFIntegration.hpp"
#include "Speedhack.hpp"
#include <Geode/loader/Mod.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GameManager.hpp>

using namespace geode::prelude;

namespace cbf {

PracticeFix& PracticeFix::get() {
    static PracticeFix inst;
    return inst;
}

PracticeFix::PracticeFix() {
    m_enabled = Mod::get()->getSettingValue<bool>("practice-fix-enabled");
}

void PracticeFix::install() {
    if (m_installed) return;
    m_installed = true;
    // Hooks are registered in main.cpp via $modify. This function loads
    // the enabled/disabled setting at startup.
}

bool PracticeFix::isEnabled() const { return m_enabled; }
void PracticeFix::setEnabled(bool e) { m_enabled = e; }

void PracticeFix::onLevelStart(bool isPracticeMode) {
    m_inLevel   = true;
    m_isPractice = isPracticeMode;
    applyDeterministicSeed();
}

void PracticeFix::onLevelEnd() {
    m_inLevel    = false;
    m_isPractice = false;
}

float PracticeFix::computeFixedDt(float rawDt) const {
    // The practice-fix dt clamp is now mostly handled by the Speedhack's
    // dynamic physics rate. We keep a generous spike cap here to kill
    // practice-mode pause/resume stalls (which can hand us a 200ms+ dt).
    if (!m_enabled) return rawDt;

    auto& cbf = CBFIntegration::get();
    double tps = cbf.getTicksPerSecond();
    if (tps <= 0.0) tps = 60.0;
    float tickDt = static_cast<float>(1.0 / tps);

    // 4x tick cap: allows up to 4 physics ticks per frame (handles 60fps
    // at 240Hz CBF without complaint) but kills 100ms+ stalls.
    float cap = tickDt * 4.0f;
    if (rawDt > cap) rawDt = cap;
    if (rawDt < 0.0f) rawDt = 0.0f;
    return rawDt;
}

void PracticeFix::applyDeterministicSeed() {
    // GD 2.2081 has BUILT-IN deterministic RNG:
    //   GJBaseGameLayer::m_randomSeed     (uint64_t)
    //   GJBaseGameLayer::m_replayRandSeed (uint64_t)
    //
    // GD's own replay system uses m_replayRandSeed to reproduce physics
    // identically. We set both to our fixed seed so that practice and
    // normal mode start from the same PRNG state, eliminating the classic
    // practice-mode desync.
    auto pl = PlayLayer::get();
    if (!pl) return;

    // Use a stable seed. GD's replay system typically uses the level's
    // hash or a timestamp; we use a fixed value for cross-mode consistency.
    m_seed = 0x43524F57u; // 'CROW'

    if (m_enabled) {
        pl->m_randomSeed     = m_seed;
        pl->m_replayRandSeed = m_seed;
    }

    log::info("[CBFMacroBot] Practice fix: RNG seed=0x{:08X} (mode={}).",
              m_seed, m_isPractice ? "practice" : "normal");
}

} // namespace cbf
