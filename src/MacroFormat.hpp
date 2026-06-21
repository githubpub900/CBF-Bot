#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>

namespace cbf {

// ============================================================================
//  Macro binary format ("CBFM" - Click Between Frames Macro)
// ----------------------------------------------------------------------------
//  Designed for three goals:
//    1. Tiny file size   - varint + delta encoding. A typical 2-minute level
//                          with ~500 input changes is ~1-3 KB on disk.
//    2. Huge tick range  - 64-bit tick counter, so "infinite frames" is fine.
//                          varint encoding keeps small deltas at 1 byte.
//    3. Fast load/save   - single sequential pass, no random access needed.
//
//  Layout:
//
//    [FileHeader - 40 bytes, fixed]
//    [Event * N  - variable, streamed]
//
//  FileHeader:
//    magic[4]         "CBFM"
//    version          uint16  (currently 1)
//    flags            uint16  (bit0: CBF mode, bit1: 2-player, bit2: has-meta)
//    level_id         int32
//    start_tick       uint64
//    end_tick         uint64
//    recorded_fps     float32
//    speed            float32
//    reserved         uint32   (zero - room to grow)
//
//  Event:
//    delta_ticks      varint  (zigzag-encoded signed 64-bit)
//    action           uint8   bit0: p1 button held after this event
//                             bit1: p2 button held after this event
//                             bit2: p1 button just pressed (edge)
//                             bit3: p1 button just released (edge)
//                             bit4: p2 button just pressed (edge)
//                             bit5: p2 button just released (edge)
//                             bit6: checkpoint marker
//                             bit7: reserved
//
//  Notes:
//    - delta_ticks is "ticks elapsed since the previous event".
//    - The first event's delta is relative to start_tick.
//    - An event with delta=0 means "two state changes happened on the same
//      tick" (common at frame-perfect transitions). Both changes apply.
//    - action byte uses both "held" and "edge" bits so a player can either
//      diff against previous state OR use edges directly. Most compact form
//      is to only store the "held" bits and let the player compute edges,
//      but we keep edge bits for sub-tick accuracy when CBF is active.
// ============================================================================

#pragma pack(push, 1)
struct FileHeader {
    char     magic[4];       // 'C','B','F','M'
    uint16_t version;        // format version
    uint16_t flags;          // see FlagBits
    int32_t  level_id;       // GD level id (-1 if unknown)
    uint64_t start_tick;     // tick at which recording started
    uint64_t end_tick;       // last tick captured
    float    recorded_fps;   // FPS at record time (info only)
    float    speed;          // speedhack multiplier used while recording
    uint32_t reserved;       // must be 0
};
static_assert(sizeof(FileHeader) == 40, "FileHeader must be 40 bytes");
#pragma pack(pop)

enum FlagBits : uint16_t {
    FlagCBFMode   = 1u << 0,   // recorded with CBF (sub-frame ticks)
    FlagTwoPlayer = 1u << 1,   // dual-player level
    FlagHasMeta   = 1u << 2,   // extended metadata follows header (future)
};

// Action byte bit layout for an Event.
enum ActionBits : uint8_t {
    ActP1Held      = 1u << 0,
    ActP2Held      = 1u << 1,
    ActP1PressEdge = 1u << 2,
    ActP1RelEdge   = 1u << 3,
    ActP2PressEdge = 1u << 4,
    ActP2RelEdge   = 1u << 5,
    ActCheckpoint  = 1u << 6,
    ActReserved    = 1u << 7,
};

struct Event {
    int64_t  delta_ticks;  // signed so zigzag makes sense; usually >= 0
    uint8_t  action;
};

// ---------------------------------------------------------------------------
//  varint (LEB128) with zigzag encoding - same scheme as protobuf.
//  Small deltas (0..63) cost 1 byte. 64..8191 cost 2 bytes. Etc.
//  A 64-bit tick counter therefore costs 1-10 bytes per event, with the
//  vast majority of events being 1-2 bytes.
// ---------------------------------------------------------------------------
inline uint64_t zigzagEncode(int64_t v) {
    return static_cast<uint64_t>((v << 1) ^ (v >> 63));
}
inline int64_t zigzagDecode(uint64_t v) {
    return static_cast<int64_t>((v >> 1) ^ -static_cast<int64_t>(v & 1));
}

inline void writeVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

inline bool readVarint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    while (true) {
        if (p >= end) return false;
        uint8_t b = *p++;
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) return false; // malformed
    }
    out = result;
    return true;
}

// ---------------------------------------------------------------------------
//  MacroData - in-memory representation of a loaded macro.
// ---------------------------------------------------------------------------
struct MacroData {
    FileHeader         header{};
    std::vector<Event> events;

    void reset() {
        std::memset(&header, 0, sizeof(header));
        std::memcpy(header.magic, "CBFM", 4);
        header.version = 1;
        events.clear();
    }

    bool isCBF() const { return (header.flags & FlagCBFMode) != 0; }
    bool isTwoPlayer() const { return (header.flags & FlagTwoPlayer) != 0; }
};

// ---------------------------------------------------------------------------
//  saveMacro / loadMacro - single-pass binary I/O.
// ---------------------------------------------------------------------------
inline bool saveMacro(const std::string& path, const MacroData& macro) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    f.write(reinterpret_cast<const char*>(&macro.header), sizeof(FileHeader));
    if (!f) return false;

    // Pre-serialize the event stream into a small buffer to keep I/O in one
    // big write call. This is faster than many small writes and keeps the
    // file layout simple.
    std::vector<uint8_t> buf;
    buf.reserve(macro.events.size() * 3); // typical ~3 bytes/event
    for (const Event& e : macro.events) {
        writeVarint(buf, zigzagEncode(e.delta_ticks));
        buf.push_back(e.action);
    }
    if (!buf.empty()) {
        f.write(reinterpret_cast<const char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
    }
    return static_cast<bool>(f);
}

inline bool loadMacro(const std::string& path, MacroData& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    f.read(reinterpret_cast<char*>(&out.header), sizeof(FileHeader));
    if (!f) return false;
    if (std::memcmp(out.header.magic, "CBFM", 4) != 0) return false;
    if (out.header.version != 1) return false;

    // Read remainder of file.
    std::vector<uint8_t> buf(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());

    out.events.clear();
    out.events.reserve(buf.size() / 3); // estimate

    const uint8_t* p    = buf.data();
    const uint8_t* end  = p + buf.size();
    while (p < end) {
        uint64_t v;
        if (!readVarint(p, end, v)) return false;
        if (p >= end) return false;
        uint8_t action = *p++;
        out.events.push_back({ zigzagDecode(v), action });
    }
    return true;
}

// Helper: compute total tick span covered by a macro (end - start).
inline uint64_t macroTickSpan(const MacroData& m) {
    uint64_t span = 0;
    for (const Event& e : m.events) {
        if (e.delta_ticks > 0) span += static_cast<uint64_t>(e.delta_ticks);
    }
    return span;
}

} // namespace cbf
