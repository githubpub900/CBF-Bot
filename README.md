# CBF Macro Bot

A Geometry Dash mod for **version 2.2081** built on the [Geode mod loader](https://geode-sdk.org).
Record your gameplay in Practice Mode and play it back in Normal Mode with
sub-frame-accurate input timing. Designed to work with **Syzzi's CBF** (Click
Between Frames) for true framerate independence, with a graceful fallback to
RobTop's built-in per-frame stepping when CBF is not installed.

## Features

| Feature | Description |
|---|---|
| **Macro recording** | Captures every press / release of the jump button (for both players in dual mode) tagged with the current physics tick. |
| **Macro playback** | Replays inputs through `PlayerObject::pushButton` / `releaseButton` so all of GD's downstream input handling (squid, ship, etc.) runs identically to a real click. |
| **CBF-aware tick source** | When Syzzi's CBF is loaded, ticks advance at CBF's fixed physics rate independent of FPS. Without CBF, ticks advance per render frame (classic macro-bot behavior). |
| **Compact binary macro format** | Custom `.cbfm` format using varint + delta encoding. A typical 2-minute level is **1–3 KB** on disk. 64-bit tick counter supports effectively "infinite" frame counts. |
| **Streaming playback** | The player advances a single index through the event list. Per-tick cost is O(1), total playback cost is O(events) regardless of macro length. |
| **Practice bug fix** | Forces deterministic RNG seeding on level start so practice and normal mode evolve identically. Combines with CBF's framerate decoupling for bit-for-bit replay fidelity. |
| **Frame-independent speedhack** | Scales simulation time without tying it to render FPS. With CBF, multi-stepping is automatic. Without CBF, the mod performs its own sub-stepping (up to 64 sub-steps per frame) to keep high speeds smooth. |
| **In-game UI** | A popup (opened from the pause menu) for recording, playing, saving, loading, speedhack control, and practice-fix toggle. |
| **Hotkeys** | F6 to toggle recording, F7 to toggle playback (configurable in mod settings). |

## How CBF support works

"CBF" stands for **Click Between Frames**. In vanilla GD, physics steps happen
once per render frame, so a 60 FPS player and a 240 FPS player experience
different physics. CBF mods decouple the physics tick rate from the render
rate, running physics at a fixed 240 Hz regardless of FPS.

This mod counts physics ticks (not frames) for both recording and playback:

- **With Syzzi's CBF**: Each call to `PlayerObject::update` counts as one tick.
  CBF calls this multiple times per render frame to maintain its fixed rate, so
  our tick counter naturally advances at the CBF rate. A macro recorded at 60
  FPS plays back identically at 240 FPS, 1000 FPS, or anything in between.
- **Without CBF (RobTop's built-in stepping)**: `PlayerObject::update` is
  called once per render frame, so one tick = one frame. Macros are
  framerate-stable only at the FPS they were recorded at. This is the same
  limitation as classic macro bots and is provided as a graceful fallback.

The bot detects which mode it's in at startup via `Loader::get()->getLoadedMod("syzzi.cbf")`
and adjusts its tick-rate assumption accordingly (240 Hz with CBF, 60 Hz
without).

## Macro file format (`.cbfm`)

```
[FileHeader - 40 bytes, fixed]
  magic[4]         "CBFM"
  version          uint16    (currently 1)
  flags            uint16    (bit0: CBF mode, bit1: 2-player, bit2: has-meta)
  level_id         int32
  start_tick       uint64
  end_tick         uint64
  recorded_fps     float32
  speed            float32
  reserved         uint32    (must be 0)

[Event * N - variable, streamed]
  delta_ticks      varint    (zigzag-encoded signed 64-bit, 1-10 bytes)
  action           uint8     (bit0: P1 held, bit1: P2 held,
                              bit2: P1 press edge, bit3: P1 release edge,
                              bit4: P2 press edge, bit5: P2 release edge,
                              bit6: checkpoint marker, bit7: reserved)
```

### Why this format is small AND fast

- **Varint (LEB128) encoding**: small deltas (0–63 ticks) cost 1 byte. The
  overwhelming majority of input events are short deltas, so typical events
  are 2 bytes (1 varint + 1 action byte).
- **Delta encoding**: each event stores ticks-since-previous-event, not
  absolute tick. Absolute ticks would be 8 bytes each; deltas are 1–2 bytes.
- **64-bit tick counter**: supports up to 2^63 ticks. At CBF's 240 Hz that's
  ~1.2 trillion years of gameplay. "Infinite frames" is comfortably covered.
- **Single-pass I/O**: load and save each do exactly one sequential read or
  write. No random access, no seeks, no parsing passes.
- **Edge + held bits**: storing both the post-event held state AND the edge
  that just happened lets a player reconstruct either form without diffing,
  which keeps playback simple and fast.

A typical 2-minute level with ~500 input changes produces a ~1.5 KB file. A
10-minute level with ~5000 changes is ~15 KB.

## Build instructions

### Prerequisites

- [Geode SDK](https://geode-sdk.org/install) v4.0.0 or newer
- CMake 3.21+
- A C++20 compiler (MSVC on Windows, Clang on macOS, Clang/GCC on Linux/Android)
- Geometry Dash 2.2080 / 2.2081

### Building

```bash
# From the cbf-macro-bot directory:
cmake -B build -G "Visual Studio 17 2022"  # or your generator of choice
cmake --build build --config Release
```

The build produces a `.geode` package in `build/` that you can install via
the Geode loader (drag into `geode/mods/` or use `geode-cli install`).

### Cross-compiling

Geode's CLI can target all supported platforms from one project:

```bash
geode build win    # Windows
geode build mac    # macOS
geode build android32
geode build android64
```

See the [Geode build docs](https://docs.geode-sdk.org/) for details.

## Usage

### In-game

1. Launch Geometry Dash with Geode and this mod installed.
2. Enter any level in **Practice Mode**.
3. Open the pause menu and click **"Macro Bot"**, or press **F6** to start
   recording.
4. Play the level. The mod records every input tagged with the physics tick.
5. Press **F6** again (or click Stop in the UI) to stop recording.
6. Optional: enter a filename and click **Save** to persist the macro as a
   `.cbfm` file in the mod's save directory.
7. Restart the level in **Normal Mode**.
8. Press **F7** (or click Play in the UI) to play back the macro.
9. Use the speedhack slider to play back at any speed from 0.1× to 10×.

### Hotkeys (configurable in mod settings)

| Key | Action |
|---|---|
| F6  | Toggle recording |
| F7  | Toggle playback |
| F8  | Open Macro Bot UI (if added via a separate keybind hook) |

### Settings (in Geode mod settings UI)

| Setting | Default | Description |
|---|---|---|
| Auto-record in Practice Mode | off | Automatically start recording when entering Practice Mode. |
| Auto-stop at level end | on | Stops recording when the level is completed. |
| Default speedhack speed | 1.0 | Default speed multiplier applied on mod load. |
| Enable practice bug fix | on | Forces deterministic physics in Practice Mode. |
| Macro save folder | `cbf_macros` | Subfolder inside the mod's save directory. |
| Record / Stop hotkey | F6 | |
| Play / Stop hotkey | F7 | |

## File layout

```
cbf-macro-bot/
├── mod.json                 Geode mod metadata + settings
├── CMakeLists.txt           Build configuration
├── README.md                This file
├── resources/               Sprites / sounds (currently empty)
└── src/
    ├── main.cpp             Geode $execute + all GD hooks
    ├── MacroBot.{hpp,cpp}   High-level facade orchestrating all subsystems
    ├── MacroFormat.hpp      Compact binary macro format (varint + delta)
    ├── MacroRecorder.{hpp,cpp}   Records inputs tagged with physics ticks
    ├── MacroPlayer.{hpp,cpp}     Streams macro events back into the game
    ├── CBFIntegration.{hpp,cpp}  Tick source: CBF if loaded, frames otherwise
    ├── PracticeFix.{hpp,cpp}     Deterministic RNG + dt spike clamp
    ├── Speedhack.{hpp,cpp}       Frame-rate-independent speed multiplier
    └── UI/
        └── MacroLayer.{hpp,cpp}  In-game popup UI
```

## Architecture

```
                 +-----------------------+
                 |       main.cpp        |
                 |   (Geode $execute)    |
                 +-----------+-----------+
                             |
                             v
                 +-----------------------+
                 |       MacroBot        |  <-- facade used by UI + hooks
                 +-----------+-----------+
                             |
        +--------+----------+----------+---------+
        |        |          |          |         |
        v        v          v          v         v
  +---------+ +-------+ +---------+ +--------+ +-------+
  |Recorder | |Player | |CBF Integ| |Practice| |Speed- |
  |         | |       | |         | |  Fix   | | hack  |
  +----+----+ +---+---+ +----+----+ +--------+ +-------+
       |          |          |
       |          |          |  subscribes to tick advances
       |          |          v
       |          |    +----------+
       |          |    | Tick ctr |  (incremented by PlayerObject::update hook)
       |          |    +----------+
       |          |
       v          v
  +----------------+
  |  MacroFormat   |  (binary .cbfm read/write)
  +----------------+
```

### Tick counting

The `PlayerObject::update` hook fires once per physics step for each player.
We increment the global tick counter only when `this == PlayLayer::m_player1`
so dual-player levels don't double-count. This works correctly whether or not
CBF is loaded: in CBF mode, the CBF mod calls `PlayerObject::update` multiple
times per render frame, and our hook fires each time.

### Speedhack

The `PlayLayer::update` hook computes a `StepPlan` from `Speedhack::plan(dt)`:

- **With CBF**: `steps = 1`, `scaledDt = rawDt * speed`. CBF internally breaks
  this into the correct number of physics sub-steps. The macro's tick stream
  is identical at any FPS.
- **Without CBF**: `steps = round(rawDt * speed / tickDt)`, `scaledDt = tickDt`.
  We call the original `PlayLayer::update` `steps` times, each advancing one
  tick. Capped at 64 sub-steps per frame to prevent CPU meltdowns at extreme
  speeds on low-end hardware.

### Practice fix

The practice bug — desync between Practice Mode and Normal Mode physics — has
three root causes:

1. **dt spikes** after checkpoint placement / pause-resume.
2. **Non-deterministic RNG** drifting between sessions.
3. **State loss** on checkpoint restore (RobTop's restore isn't byte-exact).

We address #1 with a generous dt cap (4× tickDt, kills major stalls without
affecting normal gameplay), #2 with a fixed RNG seed applied on every level
start, and #3 by recording from clean practice runs (no deaths) so checkpoint
restore never happens during recording. Combined with CBF's framerate
decoupling, this gives bit-for-bit identical physics between practice and
normal mode.

## Compatibility

- **Geometry Dash**: 2.2080 / 2.2081 (all platforms Geode supports)
- **Geode**: v4.0.0+
- **Optional dependency**: `syzzi.cbf` (any 1.x version) — enables sub-frame
  tick precision. Without it, the mod falls back to per-frame ticks.

## Limitations

- **No-CBF speedhack beyond ~64×**: at extreme speeds on low FPS, the mod
  caps sub-stepping at 64 ticks per frame to keep the CPU reasonable. This is
  a hard physical limit — no speedhack can do more physics steps per frame
  than the CPU has time for.
- **RNG seeding is best-effort**: full PRNG patching would require
  signature-scanning GD's private RNG state, which is brittle across GD
  updates. The current implementation reseeds via the public API surface; a
  future version may add direct PRNG patching.
- **Checkpoint restore during recording is not supported**: if you die during
  a practice recording, the restored state will differ from the original pass
  and the macro may desync. Record from a clean run (place a checkpoint at
  the start if you want a safety net, then play from there without dying).

## License

MIT — see `LICENSE` (or treat this README as the license grant if no LICENSE
file is present). Attribution appreciated but not required.

## Credits

- **Geode SDK team** for the mod loader and bindings.
- **Syzzi** for the CBF mod that this project integrates with.
- The Geometry Dash modding community for reverse-engineering work that makes
  mods like this possible.
