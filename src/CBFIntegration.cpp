#include "CBFIntegration.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>

using namespace geode::prelude;

namespace cbf {

CBFIntegration& CBFIntegration::get() {
    static CBFIntegration inst;
    return inst;
}

CBFIntegration::CBFIntegration() {
    m_maxRate = static_cast<double>(Mod::get()->getSettingValue<int64_t>("max-physics-rate"));
    if (m_maxRate < 60.0) m_maxRate = 60.0;
    if (m_maxRate > 24000.0) m_maxRate = 24000.0;
}

void CBFIntegration::detectCBF() {
    static bool checked = false;
    if (checked) return;
    checked = true;

    // Check for Syzzi's CBF mod.
    auto mod = Loader::get()->getLoadedMod("syzzi.click_between_frames");
    if (mod) {
        m_syzziLoaded = true;
        m_tps = 240.0; // Syzzi's CBF typically targets 240 Hz
        log::info("[CBFMacroBot] Syzzi's CBF detected - enhanced sub-frame precision ({} tps).", m_tps);
    } else {
        m_syzziLoaded = false;
        // GD 2.2081's built-in CBF also runs at ~240 Hz when enabled.
        m_tps = 240.0;
        log::info("[CBFMacroBot] No Syzzi CBF mod. Using GD 2.2081 built-in CBF (m_clickBetweenSteps).");
    }
}

bool CBFIntegration::isBuiltinCBFActive() const {
    auto pl = PlayLayer::get();
    if (!pl) return false;
    // GD 2.2081's built-in CBF flag. When true, inputs are processed at
    // sub-frame tick boundaries rather than per render frame.
    return pl->m_clickBetweenSteps;
}

uint64_t CBFIntegration::getCurrentTick() const {
    auto pl = PlayLayer::get();
    if (!pl) return m_tick.load(std::memory_order_acquire);
    // GD's own tick counter - the authoritative source.
    return static_cast<uint64_t>(pl->m_tickIndex);
}

void CBFIntegration::resetTick() {
    m_tick.store(0, std::memory_order_release);
}

void CBFIntegration::advanceTick() {
    // Prefer GD's own m_tickIndex (read lazily in getCurrentTick). This local
    // counter is a fallback for when GD's counter isn't available (e.g. in
    // edge cases during layer transitions).
    uint64_t t = m_tick.fetch_add(1, std::memory_order_acq_rel) + 1;

    // Also read GD's tick if available, and use whichever is higher.
    auto pl = PlayLayer::get();
    if (pl) {
        uint64_t gdTick = static_cast<uint64_t>(pl->m_tickIndex);
        if (gdTick > t) t = gdTick;
    }

    auto snap = m_callbacks;
    for (auto& cb : snap) {
        if (cb) cb(t);
    }
}

int CBFIntegration::subscribe(TickCallback cb) {
    int h = m_nextHandle++;
    m_callbacks.push_back(std::move(cb));
    return h;
}

void CBFIntegration::unsubscribe(int /*handle*/) {
    // Callbacks are owned by long-lived singletons (recorder/player) that
    // clear them on stop. Lazy GC is fine here.
}

void CBFIntegration::setBuiltinCBF(bool enabled) {
    auto pl = PlayLayer::get();
    if (!pl) return;
    pl->m_clickBetweenSteps = enabled;
    if (enabled) {
        log::info("[CBFMacroBot] GD built-in CBF enabled on active PlayLayer.");
    } else {
        log::info("[CBFMacroBot] GD built-in CBF disabled on active PlayLayer.");
    }
}

void CBFIntegration::setTicksPerSecond(double tps) {
    if (tps > 0.0) m_tps = tps;
}

void CBFIntegration::setMaxPhysicsRate(double hz) {
    if (hz < 60.0) hz = 60.0;
    if (hz > 24000.0) hz = 24000.0;
    m_maxRate = hz;
}

} // namespace cbf
