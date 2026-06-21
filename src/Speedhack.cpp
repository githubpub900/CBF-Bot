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
    if (s > 1000.0f) s = 1000.0f;     // hard ceiling - physics can't keep up beyond this
    m_speed.store(s, std::memory_order_release);
    log::info("[CBFMacroBot] Speedhack set to {:.3f}x.", s);
}

Speedhack::StepPlan Speedhack::plan(float rawDt) const {
    StepPlan p{rawDt, 1};
    if (rawDt <= 0.0f) return p;

    float speed = m_speed.load(std::memory_order_acquire);
    float scaled = rawDt * speed;
    if (scaled <= 0.0f) return p;

    auto& cbf = CBFIntegration::get();
    double tps = cbf.getTicksPerSecond();
    if (tps <= 0.0) tps = 60.0;
    float tickDt = static_cast<float>(1.0 / tps);

    if (cbf.isCBFActive()) {
        // CBF will internally break `scaled` into multiple sub-tick physics
        // steps. We hand it the full scaled dt and let CBF do the work.
        // This is what makes the speedhack frame-rate-independent: at any
        // FPS, CBF advances the correct number of physics ticks.
        p.scaledDt = scaled;
        p.steps    = 1;
        return p;
    }

    // No CBF: do our own sub-stepping so high speeds don't stutter.
    // Each sub-step advances exactly one tick of physics, which keeps the
    // macro's tick stream consistent regardless of how lumpy the render
    // FPS is.
    float wantedSteps = scaled / tickDt;
    uint32_t steps = static_cast<uint32_t>(std::lround(wantedSteps));
    if (steps < 1) steps = 1;
    if (steps > 64) steps = 64; // hard cap - beyond this physics can't keep up

    p.scaledDt = tickDt;
    p.steps    = steps;
    return p;
}

} // namespace cbf
