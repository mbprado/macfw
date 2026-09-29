#pragma once

// Optional diagnostic sidecar. Existing playback/capture shared-memory ABIs and
// audio scheduling are unchanged. One isolated, physically tagged impulse at a
// time; never infer an ID from a lifetime loud marker.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace macfw::fw1814::diagnostic {
constexpr const char* kName = "/macfw_fw1814_impulse_trace_v1";
constexpr std::uint64_t kMagic = 0x1814485452414345ull;
constexpr unsigned kTagFrames = 512; // impulse + 31 zeros + 40 twelve-frame bits
constexpr unsigned kStages = 7;
enum Stage { ToolSubmit, HalSubmit, PcmRead, TxPrepare, CaptureDecode, HalDelivery, ToolDelivery };
constexpr const char* kStageNames[kStages] = {
    "tool_submit", "hal_submit", "pcm_read", "tx_prepare", "capture_decode", "hal_delivery", "tool_delivery"
};
constexpr const char* kCounterNames[] = {
    "tx_late", "tx_roll_miss", "tx_audio", "tx_silence", "pcm_underrun",
    "dbc_gap", "rx_stale", "capture_drop", "hal_output_drop", "hal_input_underrun", "hal_input_zero"
};
constexpr unsigned kCounters = sizeof(kCounterNames) / sizeof(kCounterNames[0]);

inline bool bit(std::uint32_t id, unsigned index) {
    return index < 8 ? ((0xd3u >> (7-index)) & 1u) : ((id >> (39-index)) & 1u);
}
inline float waveform(std::uint32_t id, unsigned frame) {
    if (frame == 0) return .8f;
    if (frame < 32 || frame >= kTagFrames) return 0;
    return bit(id, (frame-32)/12) ? .25f : -.25f;
}
inline std::uint64_t nsTicks(std::uint64_t ns) {
    static const auto tb = [] { mach_timebase_info_data_t value{}; mach_timebase_info(&value); return value; }();
    return ns * tb.denom / tb.numer;
}

struct Point {
    std::uint64_t tick = 0, sampleHost = 0, frame = 0;
    std::uint64_t queue = UINT64_MAX, pcmQueue = UINT64_MAX, callbackFrames = 0, offset = 0;
    // TX absolute packet / observed progress / intended host time / slot;
    // capture raw DCL timestamp / raw CIP SYT / DBC / decode packet count.
    std::uint64_t a = 0, b = 0, c = 0, d = 0, cycleTimer = 0, cycleHost = 0, cycleUncertainty = 0;
};
struct SharedPoint {
    std::atomic<std::uint64_t> id{0}, verifiedTick{0};
    std::atomic<std::uint64_t> tick{0}, sampleHost{0}, frame{0};
    std::atomic<std::uint64_t> queue{0}, pcmQueue{0}, callbackFrames{0}, offset{0};
    std::atomic<std::uint64_t> a{0}, b{0}, c{0}, d{0}, cycleTimer{0}, cycleHost{0}, lastA{0}, lastTick{0}, cycleUncertainty{0};
};
struct Shared {
    std::uint64_t magic = kMagic;
    std::atomic<std::uint64_t> halPid{0}, engineCookie{0}, enginePid{0}, generation{0};
    std::atomic<std::uint64_t> requestId{0}, requestCookie{0}, expiry{0};
    std::atomic<std::uint64_t> counterTick{0}, counterSequence{0};
    std::atomic<std::uint64_t> firstCycle{0}, initialCycle{0}, txPackets{0}, rolling{0}, lead{0}, guard{0}, serviceNs{0};
    std::atomic<std::uint64_t> counters[kCounters]{};
    SharedPoint points[kStages];
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "trace requires lock-free atomics");

class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { if (p_) munmap(p_, sizeof(Shared)); if (fd_ >= 0) close(fd_); }
    bool open(bool create = false) {
        if (p_) return true;
        bool made = false;
        if (create) {
            fd_ = shm_open(kName, O_CREAT | O_EXCL | O_RDWR, 0666);
            made = fd_ >= 0;
        }
        if (fd_ < 0) fd_ = shm_open(kName, O_RDWR, 0);
        if (fd_ < 0) return false;
        if (made && ftruncate(fd_, sizeof(Shared)) != 0) { close(fd_); fd_=-1; shm_unlink(kName); return false; }
        struct stat info{};
        if (fstat(fd_, &info) || info.st_size < static_cast<off_t>(sizeof(Shared))) { close(fd_); fd_=-1; return false; }
        void* mapped = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) { close(fd_); fd_=-1; return false; }
        p_ = static_cast<Shared*>(mapped);
        if (made) new (p_) Shared{};
        if (p_->magic != kMagic) { munmap(p_,sizeof(Shared)); p_=nullptr; close(fd_); fd_=-1; return false; }
        return true;
    }
    Shared* get() const { return p_; }
private:
    int fd_ = -1;
    Shared* p_ = nullptr;
};

// Validate all 32 ID bits and a preamble through the physical analog path.
// Timestamp the first threshold crossing, not the later tag verification.
// No allocation, locks, logging or syscalls in the sample scan.
class Detector {
public:
    void reset(std::uint32_t id) { id_=id; filled_=0; done_=false; }
    bool feed(float sample, const Point& point) {
        if (done_ || id_ == 0) return false;
        if (filled_ == 0) {
            if (!std::isfinite(sample) || std::fabs(sample) <= .15f) return false;
            onset_ = point;
            polarity_ = sample >= 0 ? 1 : -1;
        }
        buffer_[filled_++] = sample;
        if (filled_ != kTagFrames) return false;
        filled_ = 0;
        for (unsigned i=0; i<40; ++i) {
            const float middle = polarity_ * (buffer_[32+i*12+5] + buffer_[32+i*12+6]);
            if (!std::isfinite(middle) || (bit(id_,i) ? middle <= .04f : middle >= -.04f)) return false;
        }
        done_ = true;
        return true;
    }
    const Point& onset() const { return onset_; }
private:
    std::uint32_t id_ = 0;
    unsigned filled_ = 0;
    bool done_ = false;
    float polarity_ = 1;
    Point onset_{};
    std::array<float,kTagFrames> buffer_{};
};

inline void publish(Shared& shared, Stage stage, std::uint64_t id, const Point& p) {
    if (shared.requestId.load(std::memory_order_acquire) != id) return;
    auto& s = shared.points[stage];
    s.tick.store(p.tick); s.sampleHost.store(p.sampleHost); s.frame.store(p.frame);
    s.queue.store(p.queue); s.pcmQueue.store(p.pcmQueue);
    s.callbackFrames.store(p.callbackFrames); s.offset.store(p.offset);
    s.a.store(p.a); s.b.store(p.b); s.c.store(p.c); s.d.store(p.d);
    s.cycleTimer.store(p.cycleTimer); s.cycleHost.store(p.cycleHost); s.cycleUncertainty.store(p.cycleUncertainty);
    s.verifiedTick.store(mach_absolute_time());
    s.id.store(id, std::memory_order_release);
}

class Endpoint {
public:
    void attach(Shared* shared, Stage stage) { shared_=shared; stage_=stage; }
    bool armed() const {
        if (!shared_ || shared_->requestId.load(std::memory_order_acquire)==0 || shared_->requestId.load()>UINT32_MAX) return false;
        return shared_->requestCookie.load() == shared_->engineCookie.load() &&
               shared_->expiry.load() > mach_absolute_time();
    }
    void block(const float* audio, std::size_t frames, std::size_t channels, Point base,
               std::uint64_t ticksPerSample = 0, std::size_t scanChannels = 1) {
        if (!armed()) return;
        for (std::size_t i=0; i<frames; ++i) {
            Point p=base; p.frame+=i; p.offset=i;
            if (p.sampleHost) p.sampleHost+=i*ticksPerSample;
            for (std::size_t ch=0; ch<scanChannels && ch<channels && ch<detectors_.size(); ++ch)
                feed(audio[i*channels+ch], p, ch);
        }
    }
    void feed(float sample, const Point& point, std::size_t channel = 0) {
        if (!shared_) return;
        if (channel >= detectors_.size()) return;
        const auto id=shared_->requestId.load(std::memory_order_acquire);
        if (!id || id>UINT32_MAX || shared_->requestCookie.load()!=shared_->engineCookie.load() ||
            shared_->expiry.load() <= point.tick) return;
        if (id!=id_) {
            id_=id;
            for (auto& detector: detectors_) detector.reset(static_cast<std::uint32_t>(id));
        }
        if (shared_->points[stage_].id.load(std::memory_order_acquire)==id) return;
        if (detectors_[channel].feed(sample,point)) {
            shared_->points[stage_].lastA.store(point.a);
            shared_->points[stage_].lastTick.store(point.tick);
            publish(*shared_,stage_,id,detectors_[channel].onset());
        }
    }
private:
    Shared* shared_ = nullptr;
    Stage stage_ = ToolSubmit;
    std::uint64_t id_ = 0;
    std::array<Detector,2> detectors_{};
};
} // namespace macfw::fw1814::diagnostic
