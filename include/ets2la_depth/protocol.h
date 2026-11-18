#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace ets2la_depth {

constexpr uint32_t kControlMagic = 0x44435445;  // "ETCD"
constexpr uint32_t kDataMagic = 0x44445445;     // "ETDD"

// Bump whenever ControlBlock/DataHeader layout or semantics change in any way.
constexpr uint32_t kProtocolVersion = 1;

constexpr uint32_t kSlotCount = 6;              // frames kept in the ring
constexpr uint32_t kMaxReaders = 32;            // heartbeat table size
constexpr uint32_t kReaderTimeoutMs = 10000;    // reader considered gone after this

constexpr uint64_t kControlBytes = 4096;
constexpr uint64_t kDataHeaderBytes = 4096;
constexpr uint64_t kSlotHeaderBytes = 256;      // pixels start this far into a slot

inline constexpr const wchar_t* kControlName = L"Local\\ETS2LA_Depth_Control";
inline constexpr const wchar_t* kEventNames[2] = {
    L"Local\\ETS2LA_Depth_Frame0",
    L"Local\\ETS2LA_Depth_Frame1",
};

inline void make_data_name(wchar_t* out, size_t count, uint32_t generation) {
    std::swprintf(out, count, L"Local\\ETS2LA_Depth_Data_%u", generation);
}

struct ReaderEntry {                        // 64 bytes, one cache line per reader
    std::atomic<uint64_t> id;               // 0 = free, else (pid << 32) | counter
    std::atomic<uint64_t> heartbeat_ms;     // GetTickCount64() at last sign of life
    uint8_t pad[48];
};

struct ControlBlock {
    // cache line 0: written once by the writer
    uint32_t magic;                         //   0
    uint32_t version;                       //   4
    uint32_t slot_count;                    //   8
    uint32_t max_readers;                   //  12
    uint64_t qpc_frequency;                 //  16
    uint32_t writer_pid;                    //  24
    uint32_t pad0;                          //  28
    uint8_t pad1[32];                       //  32
    // cache line 1..: updated per frame by the writer
    std::atomic<uint32_t> generation;       //  64  data mapping currently in use
    std::atomic<uint32_t> publishing;       //  68  1 while the DLL is actively publishing
    std::atomic<uint64_t> latest_frame;     //  72  index of newest complete frame, 0 = none
    std::atomic<uint64_t> frames_published; //  80
    std::atomic<uint64_t> frames_dropped;   //  88  frames the depth side had to skip
    std::atomic<uint64_t> last_publish_qpc; //  96
    uint8_t pad2[24];                       // 104
    ReaderEntry readers[kMaxReaders];       // 128
};

struct DataHeader {                         // lives in the first 4096 bytes
    uint32_t magic;                         //  0
    uint32_t version;                       //  4
    uint32_t generation;                    //  8
    uint32_t slot_count;                    // 12
    uint64_t slot_stride;                   // 16  bytes from one slot to the next
    uint64_t slots_offset;                  // 24  == kDataHeaderBytes
    uint32_t width;                         // 32  render resolution of the game
    uint32_t height;                        // 36
    uint32_t stride;                        // 40  bytes per row (tightly packed)
    uint32_t format;                        // 44  DXGI_FORMAT value (54 = R16_FLOAT)
    uint32_t bytes_per_pixel;               // 48
    uint32_t pad0;                          // 52
    uint64_t total_size;                    // 56
};

struct SlotHeader {                         // 64 bytes, pixels follow at +kSlotHeaderBytes
    std::atomic<uint64_t> seq;              //  0  even = stable, odd = being written
    uint64_t frame_index;                   //  8
    uint64_t timestamp_qpc;                 // 16  QPC when the game called Present()
    uint64_t publish_qpc;                   // 24  QPC when the frame became readable
    uint8_t pad[32];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(sizeof(ReaderEntry) == 64, "ReaderEntry layout");
static_assert(sizeof(SlotHeader) == 64, "SlotHeader layout");
static_assert(offsetof(ControlBlock, version) == 4, "ControlBlock layout");
static_assert(offsetof(ControlBlock, slot_count) == 8, "ControlBlock layout");
static_assert(offsetof(ControlBlock, max_readers) == 12, "ControlBlock layout");
static_assert(offsetof(ControlBlock, qpc_frequency) == 16, "ControlBlock layout");
static_assert(offsetof(ControlBlock, generation) == 64, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latest_frame) == 72, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frames_published) == 80, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frames_dropped) == 88, "ControlBlock layout");
static_assert(offsetof(ControlBlock, last_publish_qpc) == 96, "ControlBlock layout");
static_assert(offsetof(ControlBlock, readers) == 128, "ControlBlock layout");
static_assert(sizeof(ControlBlock) <= kControlBytes, "ControlBlock too large");
static_assert(offsetof(DataHeader, version) == 4, "DataHeader layout");
static_assert(offsetof(DataHeader, generation) == 8, "DataHeader layout");
static_assert(offsetof(DataHeader, slot_count) == 12, "DataHeader layout");
static_assert(offsetof(DataHeader, slot_stride) == 16, "DataHeader layout");
static_assert(offsetof(DataHeader, width) == 32, "DataHeader layout");
static_assert(offsetof(DataHeader, total_size) == 56, "DataHeader layout");
static_assert(sizeof(DataHeader) <= kDataHeaderBytes, "DataHeader too large");

inline constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
inline constexpr uint64_t slot_stride_for(uint64_t payload_bytes) {
    return align_up(kSlotHeaderBytes + payload_bytes, 4096);
}

enum class DepthLayout : uint32_t { Unknown = 0, Float16 };

constexpr float kSkyValue = -65504.0f;

inline DepthLayout depth_layout(uint32_t dxgi_format) {
    return dxgi_format == 54 ? DepthLayout::Float16 : DepthLayout::Unknown;  // DXGI_FORMAT_R16_FLOAT
}
inline uint32_t bytes_per_pixel_for(uint32_t dxgi_format) {
    return depth_layout(dxgi_format) == DepthLayout::Float16 ? 2 : 0;
}
inline const char* depth_layout_name(DepthLayout l) {
    return l == DepthLayout::Float16 ? "Float16" : "unknown";
}

inline float half_to_float(uint16_t h) {
    const uint32_t sign = uint32_t(h >> 15) << 31;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            uint32_t e = 127 - 15 + 1;
            uint32_t m = mantissa;
            while ((m & 0x400u) == 0) { m <<= 1; --e; }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

}