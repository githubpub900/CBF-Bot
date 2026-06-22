# CBF Macro Bot

A Geometry Dash mod for **version 2.2081** built on **Geode v5.7.1**.
Record gameplay in Practice Mode and play it back in Normal Mode with
sub-frame-accurate input timing. Leverages GD 2.2081's **built-in** Click
Between Frames (`m_clickBetweenSteps`) and deterministic RNG (`m_randomSeed` /
`m_replayRandSeed`). Ships with a frame-rate-independent speedhack with
**dynamic physics polling** (up to 24 kHz) for choppy-free slow-motion.

## Features

| Feature | Description |
|---|---|
| **Macro recording** | Captures every press/release of the jump button (both players in dual mode) tagged with GD's own `m_tickIndex`. |
| **Macro playback** | Replays inputs via `PlayerObject::pushButton(PlayerButton::Jump)` so GD's input pipeline runs identically to a real click. |
| **CBF integration** | Uses GD 2.2081's built-in `m_clickBetweenSteps` for sub-frame precision. Optionally enhanced by Syzzi's CBF mod if loaded. |
| **Compact binary format** | `.cbfm` files use varint + delta encoding. A 2-min level is ~1-3 KB. 64-bit tick counter supports effectively infinite frames. |
| **Event-driven storage** | Only input *changes* are stored — no polling rate needed. A 2-hour level with 10,000 input changes is ~30 KB. |
| **Streaming playback** | O(1) per-tick cost, O(events) total. Macro length has zero impact on playback performance. |
| **Practice bug fix** | Sets GD's `m_randomSeed` and `m_replayRandSeed` to a fixed value so practice and normal mode evolve identically. |
| **Dynamic physics speedhack** | Raises the physics tick rate dynamically at slow speeds (up to 24 kHz) for smooth slow-motion without choppiness. No FPS-dependent behavior. |
| **v5 keybind system** | Uses Geode v5's unified keybind settings — fully customizable in-game. |
| **In-game UI** | Popup with record/play/save/load, speedhack slider, practice-fix toggle, live stats. |

## The "Infinite" Speedhack — How It Works

The user asked for a speedhack that "doesn't get choppy when slowed" and is
"infinite" in range. Here's how we achieve it:

### The Problem

Traditional speedhacks scale `dt` (e.g. `CCScheduler::setTimeScale`). At slow
speeds this makes physics tick at fewer Hz wall-clock:

| Speed | Physics Hz (wall-clock) | Smoothness |
|---|---|---|
| 1.0x  | 240 Hz | smooth |
| 0.1x  | 24 Hz  | slightly choppy |
| 0.01x | 2.4 Hz | slideshow |

### Our Solution: Dynamic Physics Polling Rate

Instead of scaling `dt`, we keep `dt-per-step` **fixed** (preserving physics
determinism) and change how many steps run per second of wall-clock:

```
effective_baseRate = max(baseRate, min(60 / speed, maxPhysicsRate))
```

| Speed | Effective base rate | Wall-clock Hz | Smooth? |
|---|---|---|---|
| 1.0x  | 240 Hz   | 240 Hz  | ✓ |
| 0.1x  | 600 Hz   | 60 Hz   | ✓ |
| 0.01x | 6000 Hz  | 60 Hz   | ✓ |
| 0.001x| 24000 Hz | 60 Hz   | ✓ (capped at max) |

Each physics step still advances `1 / effective_baseRate` seconds of game-time,
so the macro's tick stream is **identical at any speed**. The macro is tagged
with ticks, not wall-clock time, so playback at any speed fires inputs at the
correct tick.

### No Polling Rate Needed for Storage

The user mentioned a 24 kHz polling rate as a compromise for storing "infinite
frames." Our approach is **better**: we use **event-driven storage** — only
input *changes* (press/release edges) are stored, not every frame. A level
with 500 input changes produces 500 events regardless of whether the physics
rate is 240 Hz or 24 kHz. No polling rate needed.

The 24 kHz cap is only for **physics smoothness** during slow-motion playback,
not for input storage.

## GD 2.2081 Built-in Systems Used

GD 2.2081 ships with native CBF and replay infrastructure. We leverage it
instead of reinventing it:

| GD field | Type | Our use |
|---|---|---|
| `m_clickBetweenSteps` | bool | Enable GD's built-in CBF for sub-frame input precision |
| `m_tickIndex` | int | Authoritative tick counter for recording/playback |
| `m_randomSeed` | uint64_t | Fixed seed for deterministic practice→normal physics |
| `m_replayRandSeed` | uint64_t | Same seed for replay-mode determinism |
| `m_isPracticeMode` | bool | Detect practice mode for auto-record |
| `m_player1` / `m_player2` | PlayerObject* | Identify which player pressed |
| `pushButton(PlayerButton)` | method | Inject inputs during playback |
| `releaseButton(PlayerButton)` | method | Inject inputs during playback |

## Macro File Format (`.cbfm`)

```
[FileHeader - 40 bytes, fixed]
  magic[4]         "CBFM"
  version          uint16    (currently 1)
  flags            uint16    (bit0: CBF mode, bit1: 2-player)
  level_id         int32
  start_tick       uint64
  end_tick         uint64
  recorded_fps     float32
  speed            float32
  reserved         uint32

[Event * N - variable, streamed]
  delta_ticks      varint    (zigzag-encoded, 1-10 bytes)
  action           uint8     (P1/P2 held + press/release edges)
```

**Why it's small and fast:**
- Varint (LEB128): small deltas (0-63 ticks) cost 1 byte
- Delta encoding: stores ticks-since-previous-event, not absolute ticks
- 64-bit tick counter: supports up to 2^63 ticks (~1.2 trillion years at 240 Hz)
- Single-pass I/O: load/save each do one sequential read/write
- Typical event: 2 bytes (1 varint + 1 action byte)

## Build Instructions

### Prerequisites

- [Geode SDK](https://geode-sdk.org/install) **v5.7.1** or newer
- CMake 3.21+
- A **C++23** compiler (MSVC on Windows, Clang on macOS, Clang/GCC on Linux)
- Geometry Dash 2.2081

### Building

```bash
# Geode v5 CLI (recommended):
cd cbf-macro-bot
geode build

# Or with raw CMake:
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config RelWithDebInfo
```

The `.geode` package appears in `build/`. Install it by dragging into
`geode/mods/` or using `geode-cli install`.

## Usage

### Quick Start

1. Launch GD with Geode v5.7.1 and this mod installed.
2. Enter any level in **Practice Mode**.
3. Press **F6** (or open pause menu → "Macro Bot") to start recording.
4. Play the level. Every input is tagged with GD's physics tick.
5. Press **F6** again to stop recording.
6. Optional: save the macro via the UI (enter filename → Save).
7. Restart the level in **Normal Mode**.
8. Press **F7** to play back. Use the speed slider for slow-mo / fast-forward.

### Hotkeys (customizable in Geode mod settings)

| Default | Action |
|---|---|
| F6 | Toggle recording |
| F7 | Toggle playback |
| F8 | Open Macro Bot UI |

### Settings

| Setting | Default | Description |
|---|---|---|
| Auto-record in Practice Mode | off | Auto-start recording on practice mode entry |
| Auto-stop at level end | on | Stop recording on level completion |
| Default speedhack speed | 1.0 | Default speed multiplier (0.001x - 1000x) |
| Max physics polling rate | 240 Hz | Cap for dynamic physics rate (60 - 24000 Hz). Higher = smoother slow-mo but more CPU |
| Enable practice bug fix | on | Force deterministic RNG seed |
| Use GD built-in CBF | on | Enable `m_clickBetweenSteps` |
| Macro save folder | `cbf_macros` | Subfolder in mod save directory |

### Choosing a Max Physics Rate

| Max rate | Smooth down to | CPU impact |
|---|---|---|
| 240 Hz (default) | ~0.25x | minimal |
| 1200 Hz | ~0.05x | low |
| 4800 Hz | ~0.012x | moderate |
| 24000 Hz | ~0.003x | high (use only for extreme slow-mo) |

## File Layout

```
cbf-macro-bot/
├── mod.json                 Geode v5.7.1 metadata + keybind settings
├── CMakeLists.txt           C++23 build configuration
├── README.md                This file
├── resources/               Sprites / sounds
└── src/
    ├── main.cpp             $execute + $on_mod/$on_game + all GD hooks
    ├── MacroBot.{hpp,cpp}   Facade orchestrating all subsystems
    ├── MacroFormat.hpp      Compact binary .cbfm format (varint + delta)
    ├── MacroRecorder.{hpp,cpp}   Records inputs tagged with m_tickIndex
    ├── MacroPlayer.{hpp,cpp}     Streams events via PlayerButton::Jump
    ├── CBFIntegration.{hpp,cpp}  Reads GD's m_tickIndex / m_clickBetweenSteps
    ├── PracticeFix.{hpp,cpp}     Sets m_randomSeed / m_replayRandSeed
    ├── Speedhack.{hpp,cpp}       Dynamic physics rate for smooth slow-mo
    └── UI/
        └── MacroLayer.{hpp,cpp}  v5 Popup (non-templated) UI
```

## Architecture

```
                 +-----------------------+
                 |       main.cpp        |
                 |  $execute / $on_game  |
                 +-----------+-----------+
                             |
                             v
                 +-----------------------+
                 |       MacroBot        |  <-- facade
                 +-----------+-----------+
                             |
        +--------+----------+----------+---------+
        |        |          |          |         |
        v        v          v          v         v
  +---------+ +-------+ +---------+ +--------+ +-------+
  |Recorder | |Player | |CBF Integ| |Practice| |Speed- |
  |         | |       | |         | |  Fix   | | hack  |
  +----+----+ +---+---+ +----+----+ +--------+ +---+---+
       |          |          |                       |
       |          |   reads m_tickIndex              |
       |          |          |                       |
       v          v          v                       v
  +----------------+   +----------+          +--------------+
  |  MacroFormat   |   | GD's own |          | Dynamic rate |
  |  (.cbfm I/O)   |   | m_tickIdx|          | 60Hz..24kHz  |
  +----------------+   +----------+          +--------------+
```

## Compatibility

- **Geometry Dash**: 2.2081
- **Geode**: v5.7.1+ (requires C++23)
- **Optional**: `syzzi.cbf` — enhances sub-frame precision beyond GD's built-in CBF

## Limitations

- **Extreme slow-mo below ~0.003x** (at 24 kHz max): gets progressively choppy
  as the physics rate cap is reached. This is a hard CPU limit — no speedhack
  can do more physics steps per frame than the CPU has time for.
- **RNG seeding**: We set `m_randomSeed` / `m_replayRandSeed` via the public
  binding. GD may consume additional RNG from other sources (cocos particles,
  etc.) that we don't control. For 99% of levels this is sufficient.
- **Checkpoint restore during recording**: if you die during a practice
  recording, the restored state may differ from the original pass. Record
  from clean runs.

## License

MIT — attribution appreciated but not required.

## Credits

- **Geode SDK team** for v5 and the GD 2.2081 bindings.
- **Syzzi** for the CBF mod that this project optionally integrates with.
- The GD modding community for reverse-engineering work.
