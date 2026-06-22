#include "Speedhack.hpp"
#include "CBFIntegration.hpp"
#include <Geode/loader/Mod.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace cbf {

Speedhack& Speedhack::get() {
    static Speedhack inst;
    return inst;
}

Speedhack::Speedhack() {
    setDefaultFromSettings();
}

void Speedhack::setDefaultFromSettings() {
    float s = static_cast<float>(Mod::get()->getSettingValue<double>("default-speed"));
    if (s <= 0.0f) s = 1.0f;
    m_speed.store(s, std::memory_order_release);
}

void Speedhack::setSpeed(float s) {
    if (s < 0.001f) s = 0.001f;       // hard floor
    if (s > 1000.0f) s = 1000.0f;     // hard ceiling
    m_speed.store(s, std::memory_order_release);
    log::info("[CBFMacroBot] Speedhack set to {:.4f}x (effective base rate: {:.0f} Hz).",
              s, getEffectiveBaseRate());
}

double Speedhack::getEffectiveBaseRate() const {
    // The key formula for choppy-free slow-motion:
    //
    //   effective_baseRate = max(baseRate, min(60 / speed, maxPhysicsRate))
    //
    // - baseRate: CBF's native rate (240 Hz typically)
    // - 60 / speed: the minimum rate needed to keep 60 physics steps per
    //   second of wall-clock at the given speed
    // - maxPhysicsRate: user-configured cap (default 240 Hz, max 24000 Hz)
    //
    // At normal speed (1.0x): effective = max(240, min(60, 24000)) = 240 Hz
    // At 0.1x:               effective = max(240, min(600, 24000)) = 600 Hz
    // At 0.01x:              effective = max(240, min(6000, 24000)) = 6000 Hz
    // At 0.001x:             effective = max(240, min(60000, 24000)) = 24000 Hz
    auto& cbf = CBFIntegration::get();
    double baseRate = cbf.getTicksPerSecond();
    double maxRate  = cbf.getMaxPhysicsRate();
    float  speed    = m_speed.load(std::memory_order_acquire);

    if (speed >= 1.0f) {
        // Fast-forward or normal: use the base rate. CBF / multi-step handles
        // the extra ticks per frame.
        return baseRate;
    }

    // Slow-motion: raise the base rate to maintain wall-clock smoothness.
    double needed = 60.0 / static_cast<double>(speed);
    return std::max(baseRate, std::min(needed, maxRate));
}

Speedhack::StepPlan Speedhack::plan(float rawRenderDt) const {
    StepPlan p{0.0f, 1, 0.0f};
    if (rawRenderDt <= 0.0f) return p;

    float speed = m_speed.load(std::memory_order_acquire);
    auto& cbf = CBFIntegration::get();

    double effRate = getEffectiveBaseRate();
    float dtPerStep = static_cast<float>(1.0 / effRate);

    // Total game-time to advance this render frame:
    //   gameTimeThisFrame = rawRenderDt * speed
    // But we want each step to advance exactly dtPerStep of game-time, so:
    //   stepsThisFrame = round(gameTimeThisFrame / dtPerStep)
    float gameTimeThisFrame = rawRenderDt * speed;
    float wantedSteps = gameTimeThisFrame / dtPerStep;
    uint32_t steps = static_cast<uint32_t>(std::lround(wantedSteps));

    if (steps < 1) steps = 1;
    // Cap steps per frame to protect CPU on slow renders / extreme speeds.
    if (steps > 256) steps = 256;

    p.dtPerStep     = dtPerStep;
    p.stepsThisFrame = steps;
    p.frameDt       = dtPerStep;

    // Log when we're running at an elevated rate (for debugging).
    static uint32_t logCounter = 0;
    if (++logCounter > 600 && effRate > cbf.getTicksPerSecond() + 0.5) {
        logCounter = 0;
        log::info("[CBFMacroBot] Slow-mo: {}x, physics at {:.0f} Hz, {} steps/frame.",
                  speed, effRate, steps);
    }

    return p;
}

} // namespace cbf
