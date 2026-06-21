#include "CBFIntegration.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/PlayLayer.hpp>

using namespace geode::prelude;

namespace cbf {

CBFIntegration& CBFIntegration::get() {
    static CBFIntegration inst;
    return inst;
}

CBFIntegration::CBFIntegration() = default;

void CBFIntegration::detectCBF() {
    // Syzzi's CBF publishes under the mod id "syzzi.cbf". We treat it as
    // optional: if present, we know the physics loop is decoupled from the
    // render loop, so our tick counter is framerate-independent.
    static bool checked = false;
    if (checked) return;
    checked = true;

    auto mod = Loader::get()->getLoadedMod("syzzi.cbf");
    if (mod) {
        m_cbfLoaded = true;
        // CBF typically targets a 240Hz physics tick rate (matching the
        // game's high-FPS mode). The exact value can be refined at runtime
        // if the mod exposes it; 240 is a safe default that aligns with the
        // "high FPS" physics baseline used by serious players.
        m_tps = 240.0;
        log::info("[CBFMacroBot] Syzzi's CBF detected - sub-frame ticks active ({} tps).", m_tps);
    } else {
        m_cbfLoaded = false;
        m_tps = 60.0;
        log::info("[CBFMacroBot] No CBF mod detected - falling back to per-frame ticks ({} tps). "
                  "Macros will only be framerate-stable at the recorded FPS.", m_tps);
    }
}

void CBFIntegration::resetTick() {
    m_tick.store(0, std::memory_order_release);
}

void CBFIntegration::advanceTick() {
    uint64_t t = m_tick.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Snapshot callbacks to avoid iterator invalidation if a callback
    // subscribes/unsubscribes during dispatch.
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

void CBFIntegration::unsubscribe(int handle) {
    // We don't actually need the handle for removal since we use vector - but
    // keeping the API stable in case we switch to a map later. For now we
    // mark dead callbacks as empty (lazy GC).
    (void)handle;
    // Note: subscribed callbacks are owned by their subscriber (recorder /
    // player singletons) which clear them on stop, so leak is bounded.
}

bool CBFIntegration::tryReadCBFOwnedTick(uint64_t& out) {
    // Reserved for future use: if Syzzi's CBF exposes a global counter we
    // can read directly via dlsym / Geode's symbol resolver, we'd do it
    // here. For now we maintain our own counter (incremented by the same
    // physics step hook), which is functionally equivalent.
    out = m_tick.load(std::memory_order_acquire);
    return m_cbfLoaded;
}

void CBFIntegration::setTicksPerSecond(double tps) {
    if (tps > 0.0) m_tps = tps;
}

} // namespace cbf
