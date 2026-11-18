#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace ets2la_depth {

inline uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

struct DepthInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    uint32_t bytes_per_pixel = 0;
    uint64_t frame_index = 0;
    uint64_t timestamp_qpc = 0;
    uint64_t publish_qpc = 0;

    DepthLayout layout() const { return depth_layout(format); }
    size_t size_bytes() const { return size_t(stride) * height; }

    // Maps a pixel of another image of the same game view (for example an ets2la_capture
    // frame of frame_width x frame_height) to the pixel of the depth map that covers
    // its center. The depth map is rendered at the game's render resolution, which
    // can differ from the window size.
    void map_from_frame_pixel(uint32_t frame_width, uint32_t frame_height, uint32_t x, uint32_t y, uint32_t& depth_x, uint32_t& depth_y) const {
        const double u = (double(x) + 0.5) / double(frame_width);
        const double v = (double(y) + 0.5) / double(frame_height);
        const double px = u * double(width);
        const double py = v * double(height);
        depth_x = px < 0.0 ? 0u : (px >= double(width) ? width - 1 : uint32_t(px));
        depth_y = py < 0.0 ? 0u : (py >= double(height) ? height - 1 : uint32_t(py));
    }

protected:
    // Raw value at a depth map pixel.
    float raw_at(const uint8_t* base, uint32_t x, uint32_t y) const {
        uint16_t half;
        std::memcpy(&half, base + size_t(y) * stride + size_t(x) * 2, 2);
        return half_to_float(half);
    }
};

// Owning copy of a frame. Reuse one instance across calls to avoid allocations.
struct DepthFrame : DepthInfo {
    std::vector<uint8_t> data;

    // Distance in meters in front of the camera. Infinity for sky.
    float meters_at(uint32_t x, uint32_t y) const {
        const float raw = raw_at(data.data(), x, y);
        return raw <= kSkyValue + 1.0f ? std::numeric_limits<float>::infinity() : -raw;
    }
};

// Non-owning view straight into shared memory. Keeps the mapping alive, but the
// writer may overwrite the slot after kSlotCount-1 newer frames: call valid()
// after you are done reading and discard the result if it returns false.
class DepthView : public DepthInfo {
public:
    const uint8_t* data = nullptr;

    float meters_at(uint32_t x, uint32_t y) const {
        const float raw = raw_at(data, x, y);
        return raw <= kSkyValue + 1.0f ? std::numeric_limits<float>::infinity() : -raw;
    }
    bool valid() const {
        if (!slot_) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot_->seq.load(std::memory_order_relaxed) == seq_;
    }

private:
    friend class DepthReader;
    std::shared_ptr<void> keepalive_;
    const SlotHeader* slot_ = nullptr;
    uint64_t seq_ = 0;
};

struct ReaderOptions {
    uint32_t spin_us = 0;
};

struct DepthStats {
    uint64_t latest_frame = 0;
    uint64_t frames_published = 0;
    uint64_t frames_dropped = 0;
    uint32_t generation = 0;
    bool publishing = false;
    uint32_t writer_pid = 0;
};

class DepthReader {
public:
    explicit DepthReader(ReaderOptions options = {}) : options_(options) {
        connect();
    }
    ~DepthReader() {
        disconnect();
    }
    DepthReader(const DepthReader&) = delete;
    DepthReader& operator=(const DepthReader&) = delete;

    bool connected() const {
        return ctl_ != nullptr;
    }
    const std::string& last_error() const {
        return lastError_;
    }

    // Blocks until a frame newer than the previous one returned by this reader
    // exists (or timeout) and copies it into `out`.
    bool get_frame(DepthFrame& out, uint32_t timeout_ms = 1000) {
        return acquire(&out, nullptr, true, timeout_ms);
    }

    // Non-blocking: newest frame available right now, copied into `out`.
    bool get_latest_frame(DepthFrame& out) {
        return acquire(&out, nullptr, false, 0);
    }

    // Blocks until a frame newer than the previous one returned by this reader
    // exists (or timeout) and returns a view into shared memory. The caller must
    // call valid() on the returned view after it is done reading to ensure that
    // the writer did not overwrite the slot in the meantime.
    bool get_frame_view(DepthView& out, uint32_t timeout_ms = 1000) {
        return acquire(nullptr, &out, true, timeout_ms);
    }

    // Non-blocking: newest frame available right now, returns a view into shared
    // memory. The caller must call valid() on the returned view after it is done
    // reading to ensure that the writer did not overwrite the slot in the meantime.
    bool get_latest_frame_view(DepthView& out) {
        return acquire(nullptr, &out, false, 0);
    }

    // Returns the depth frame that belongs to an ets2la_capture frame: the published
    // frame whose timestamp_qpc is closest to `timestamp_qpc` and at most tolerance_ms
    // away. Both plugins stamp the QPC time of the game's Present() call, so matching
    // frames differ by well under one frame time. Waits up to timeout_ms for the
    // frame to be published. Returns false if no frame within the tolerance exists.
    bool get_frame_near(DepthFrame& out, uint64_t timestamp_qpc, double tolerance_ms = 10.0, uint32_t timeout_ms = 1000) {
        if (!ensure_connected(timeout_ms)) {
            return false;
        }
        heartbeat();
        const uint64_t toleranceTicks = uint64_t(tolerance_ms * double(freq_) / 1000.0);
        const uint64_t deadline = qpc_now() + uint64_t(timeout_ms) * freq_ / 1000;
        while (true) {
            const uint64_t latest = ctl_->latest_frame.load(std::memory_order_acquire);
            if (latest > 0) {
                const uint64_t oldest = latest > kSlotCount - 2 ? latest - (kSlotCount - 2) : 1;
                DepthView best;
                uint64_t bestDelta = ~0ull;
                bool newestIsLater = false;
                for (uint64_t index = latest; index >= oldest && index > 0; --index) {
                    DepthView candidate;
                    if (!snapshot_index(index, candidate)) {
                        continue;
                    }
                    const uint64_t ts = candidate.timestamp_qpc;
                    const uint64_t delta = ts > timestamp_qpc ? ts - timestamp_qpc : timestamp_qpc - ts;
                    if (delta < bestDelta) {
                        bestDelta = delta;
                        best = std::move(candidate);
                    }
                    if (index == latest && ts > timestamp_qpc + toleranceTicks) {
                        newestIsLater = true;
                    }
                }
                if (bestDelta <= toleranceTicks) {
                    if (copy_view(best, out)) {
                        if (out.frame_index > last_index_) last_index_ = out.frame_index;
                        return true;
                    }
                    continue;
                }
                if (newestIsLater && bestDelta != ~0ull) {
                    return false;
                }
            }
            if (qpc_now() >= deadline || !wait_above(latest, deadline)) {
                return false;
            }
        }
    }

    double ticks_to_ms(uint64_t ticks) const {
        return freq_ ? double(ticks) * 1000.0 / double(freq_) : 0.0;
    }
    // Time since the game presented this frame, in milliseconds.
    double age_ms(const DepthInfo& f) const {
        return ticks_to_ms(qpc_now() - f.timestamp_qpc);
    }

    DepthStats stats() const {
        DepthStats s;
        if (!ctl_) {
            return s;
        }
        s.latest_frame = ctl_->latest_frame.load(std::memory_order_relaxed);
        s.frames_published = ctl_->frames_published.load(std::memory_order_relaxed);
        s.frames_dropped = ctl_->frames_dropped.load(std::memory_order_relaxed);
        s.generation = ctl_->generation.load(std::memory_order_relaxed);
        s.publishing = ctl_->publishing.load(std::memory_order_relaxed) != 0;
        s.writer_pid = ctl_->writer_pid;
        return s;
    }

    bool connect() {
        disconnect();
        ctlMap_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kControlName);
        if (!ctlMap_) {
            return false;
        }
        ctl_ = static_cast<ControlBlock*>(MapViewOfFile(ctlMap_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kControlBytes));
        if (!ctl_) {
            return fail("MapViewOfFile(control) failed");
        }
        if (ctl_->magic != kControlMagic) {
            disconnect();
            return fail("depth DLL not ready");
        }
        if (ctl_->version != kProtocolVersion) {
            disconnect();
            return fail("depth DLL protocol version mismatch");
        }
        for (int i = 0; i < 2; ++i) {
            events_[i] = OpenEventW(SYNCHRONIZE, FALSE, kEventNames[i]);
            if (!events_[i]) {
                disconnect();
                return fail("OpenEvent failed");
            }
        }
        freq_ = ctl_->qpc_frequency;
        register_reader();
        last_index_ = ctl_->latest_frame.load(std::memory_order_acquire);
        lastError_.clear();
        return true;
    }

private:
    struct Mapping {
        HANDLE handle = nullptr;
        void* base = nullptr;
        const DataHeader* hdr = nullptr;
        ~Mapping() {
            if (base) {
                UnmapViewOfFile(base);
            }
            if (handle) {
                CloseHandle(handle);
            }
        }
    };

    bool fail(const char* msg) {
        lastError_ = msg;
        return false;
    }

    void disconnect() {
        unregister_reader();
        map_.reset();
        mapGen_ = 0;
        if (ctl_) {
            UnmapViewOfFile(ctl_);
            ctl_ = nullptr;
        }
        if (ctlMap_) {
            CloseHandle(ctlMap_);
            ctlMap_ = nullptr;
        }
        for (auto& e : events_) {
            if (e) {
                CloseHandle(e);
                e = nullptr;
            }
        }
    }

    void register_reader() {
        static std::atomic<uint32_t> counter{0};
        myId_ = (uint64_t(GetCurrentProcessId()) << 32) | (counter.fetch_add(1) + 1);
        const uint64_t now = GetTickCount64();
        const uint32_t start = GetCurrentProcessId() % kMaxReaders;
        entry_ = &ctl_->readers[start];
        for (uint32_t i = 0; i < kMaxReaders; ++i) {
            ReaderEntry* e = &ctl_->readers[(start + i) % kMaxReaders];
            const uint64_t id = e->id.load(std::memory_order_relaxed);
            if (id == 0 || now - e->heartbeat_ms.load(std::memory_order_relaxed) > kReaderTimeoutMs * 2) {
                entry_ = e;
                break;
            }
        }
        heartbeat();
    }
    void heartbeat() {
        if (!entry_) return;
        entry_->id.store(myId_, std::memory_order_relaxed);
        entry_->heartbeat_ms.store(GetTickCount64(), std::memory_order_release);
    }
    void unregister_reader() {
        if (entry_ && ctl_ && entry_->id.load(std::memory_order_relaxed) == myId_) {
            entry_->heartbeat_ms.store(0, std::memory_order_relaxed);
            entry_->id.store(0, std::memory_order_release);
        }
        entry_ = nullptr;
    }

    bool ensure_mapping(uint32_t generation) {
        if (map_ && mapGen_ == generation) {
            return true;
        }
        wchar_t name[96];
        make_data_name(name, 96, generation);
        auto m = std::make_shared<Mapping>();
        m->handle = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!m->handle) {
            return fail("data mapping not found");
        }
        m->base = MapViewOfFile(m->handle, FILE_MAP_READ, 0, 0, 0);
        if (!m->base) {
            return fail("MapViewOfFile(data) failed");
        }
        m->hdr = static_cast<const DataHeader*>(m->base);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m->hdr->magic != kDataMagic ||
            m->hdr->generation != generation ||
            m->hdr->slot_count == 0 ||
            m->hdr->version != kProtocolVersion) {
            return fail("data mapping header invalid");
        }
        map_ = std::move(m);
        mapGen_ = generation;
        return true;
    }

    bool snapshot(uint64_t minExclusive, DepthView& out) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t latest = ctl_->latest_frame.load(std::memory_order_acquire);
            if (latest == 0 || latest <= minExclusive) {
                return false;
            }
            const uint32_t gen = ctl_->generation.load(std::memory_order_acquire);
            if (!ensure_mapping(gen)) {
                return false;
            }

            if (read_slot(latest, out)) {
                return true;
            }
            YieldProcessor();
        }
        return false;
    }

    bool snapshot_index(uint64_t index, DepthView& out) {
        const uint32_t gen = ctl_->generation.load(std::memory_order_acquire);
        if (!ensure_mapping(gen)) {
            return false;
        }
        return read_slot(index, out);
    }

    bool read_slot(uint64_t index, DepthView& out) {
        const DataHeader* h = map_->hdr;
        const uint8_t* base = static_cast<const uint8_t*>(map_->base);
        const uint8_t* slotBase = base + h->slots_offset + (index % h->slot_count) * h->slot_stride;
        const SlotHeader* slot = reinterpret_cast<const SlotHeader*>(slotBase);

        const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
        if ((s1 & 1) != 0 || slot->frame_index != index) {
            return false;
        }

        out.width = h->width;
        out.height = h->height;
        out.stride = h->stride;
        out.format = h->format;
        out.bytes_per_pixel = h->bytes_per_pixel;
        out.frame_index = index;
        out.timestamp_qpc = slot->timestamp_qpc;
        out.publish_qpc = slot->publish_qpc;
        out.data = slotBase + kSlotHeaderBytes;
        out.slot_ = slot;
        out.seq_ = s1;
        out.keepalive_ = map_;

        std::atomic_thread_fence(std::memory_order_acquire);
        return slot->seq.load(std::memory_order_relaxed) == s1;
    }

    bool wait_for_newer(uint64_t deadline) {
        return wait_above(last_index_, deadline);
    }

    bool wait_above(uint64_t index, uint64_t deadline) {
        if (options_.spin_us) {
            const uint64_t spinEnd = qpc_now() + uint64_t(options_.spin_us) * freq_ / 1000000ull;
            while (qpc_now() < spinEnd) {
                if (ctl_->latest_frame.load(std::memory_order_acquire) > index) {
                    return true;
                }
                YieldProcessor();
            }
        }
        while (true) {
            if (ctl_->latest_frame.load(std::memory_order_acquire) > index) {
                return true;
            }
            const uint64_t now = qpc_now();
            if (now >= deadline) {
                return false;
            }
            const uint64_t remainingMs = ((deadline - now) * 1000 + freq_ - 1) / freq_;
            const DWORD slice = DWORD(remainingMs < 5 ? remainingMs : 5);
            WaitForSingleObject(events_[(index + 1) & 1], slice ? slice : 1);
            heartbeat();
        }
    }

    static bool copy_view(const DepthView& v, DepthFrame& out) {
        static_cast<DepthInfo&>(out) = v;
        out.data.resize(v.size_bytes());
        std::memcpy(out.data.data(), v.data, v.size_bytes());
        return v.valid();
    }

    bool ensure_connected(uint32_t timeoutMs) {
        if (ctl_) {
            return true;
        }
        const DWORD now = GetTickCount();
        if (now - lastReconnectTick_ < 250) {
            Sleep(timeoutMs < 50 ? timeoutMs : 50);
            return false;
        }
        lastReconnectTick_ = now;
        if (!connect()) {
            Sleep(timeoutMs < 50 ? timeoutMs : 50);
            return false;
        }
        return true;
    }

    bool acquire(DepthFrame* copyOut, DepthView* viewOut, bool onlyNew, uint32_t timeoutMs) {
        if (!ensure_connected(timeoutMs)) {
            return false;
        }
        heartbeat();
        const uint64_t t0 = qpc_now();
        const uint64_t deadline = t0 + uint64_t(onlyNew ? timeoutMs : 5) * freq_ / 1000;
        while (true) {
            if (onlyNew && !wait_for_newer(deadline)) {
                return false;
            }
            DepthView v;
            if (snapshot(onlyNew ? last_index_ : 0, v)) {
                if (viewOut) {
                    last_index_ = v.frame_index;
                    *viewOut = std::move(v);
                    return true;
                }
                if (copy_view(v, *copyOut)) {
                    last_index_ = v.frame_index;
                    return true;
                }
            }
            if (qpc_now() >= deadline) {
                return false;
            }
            YieldProcessor();
        }
    }

    ReaderOptions options_;
    std::string lastError_;
    HANDLE ctlMap_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};
    ReaderEntry* entry_ = nullptr;
    uint64_t myId_ = 0;
    uint64_t freq_ = 0;
    uint64_t last_index_ = 0;
    std::shared_ptr<Mapping> map_;
    uint32_t mapGen_ = 0;
    DWORD lastReconnectTick_ = 0;
};

}