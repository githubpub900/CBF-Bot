// ═══════════════════════════════════════════════════════════════
// main.cpp — GDBot Implementation  (GD 2.2081 / Geode v5.7.1)
// ═══════════════════════════════════════════════════════════════
//
// Binary macro format (.gdm) — only stores actions, NOT per-frame data:
//
//   [4 B]  magic  "GDBM"
//   [1 B]  version (1)
//   [8 B]  levelID (int64)
//   [8 B]  recording-start-offset (double)
//   [4 B]  input count (uint32)
//   [11 B] per input: [8B time][1B isPress][1B player][1B button]
//
//   8 000 inputs → ~88 KB.  50 000 inputs → ~550 KB.
// ═══════════════════════════════════════════════════════════════

#include "Bot.hpp"

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/TextInput.hpp>

#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>

using namespace geode::prelude;
using namespace cocos2d;

// forward-declare so toggleGUI can create/destroy it
class BotPanel;

// ═══════════════════════════════════════════════════════════════
// § 1  MacroBot — Core
// ═══════════════════════════════════════════════════════════════

void MacroBot::init() {
    updateCBFStatus();
    auto dir = Mod::get()->getSaveDir() / "macros";
    std::filesystem::create_directories(dir);
    log::info("GDBot initialised — CBF: {}", cbf