#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace macfw::fw1814::transport {

class IsochCallbackRunLoopThread {
public:
    IsochCallbackRunLoopThread() = default;
    ~IsochCallbackRunLoopThread() { stop(); }

    IsochCallbackRunLoopThread(const IsochCallbackRunLoopThread&) = delete;
    IsochCallbackRunLoopThread& operator=(const IsochCallbackRunLoopThread&) = delete;

    bool prepare() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (worker_.joinable()) return false;
            ready_ = false;
            pumping_ = false;
            stopRequested_ = false;
            runLoop_ = nullptr;
        }

        worker_ = std::thread([this] {
            const int qosRc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            if (qosRc == 0)
                std::cout << "FW1814 isoch callback thread QoS: user-interactive\n";
            else
                std::cout << "FW1814 isoch callback thread QoS: request failed ("
                          << qosRc << ")\n";

            CFRunLoopRef loop = CFRunLoopGetCurrent();
            CFRetain(loop);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                runLoop_ = loop;
                ready_ = true;
            }
            cv_.notify_all();

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return pumping_ || stopRequested_; });
                if (stopRequested_) {
                    runLoop_ = nullptr;
                    lock.unlock();
                    CFRelease(loop);
                    return;
                }
            }

            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopRequested_) break;
                }
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.050, false);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                runLoop_ = nullptr;
            }
            CFRelease(loop);
        });

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return ready_; });
        return runLoop_ != nullptr;
    }

    CFRunLoopRef runLoop() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return runLoop_;
    }

    void startPumping() {
        CFRunLoopRef loop = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pumping_ = true;
            loop = runLoop_;
        }
        cv_.notify_all();
        if (loop) CFRunLoopWakeUp(loop);
    }

    void stop() {
        CFRunLoopRef loop = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable()) return;
            stopRequested_ = true;
            pumping_ = true;
            loop = runLoop_;
        }
        cv_.notify_all();
        if (loop) {
            CFRunLoopStop(loop);
            CFRunLoopWakeUp(loop);
        }
        worker_.join();

        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = false;
        pumping_ = false;
        stopRequested_ = false;
        runLoop_ = nullptr;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    CFRunLoopRef runLoop_ = nullptr;
    bool ready_ = false;
    bool pumping_ = false;
    bool stopRequested_ = false;
};

inline void requestInteractiveQos(const char* label) {
    const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc == 0)
        std::cout << label << " QoS: user-interactive\n";
    else
        std::cout << label << " QoS: request failed (" << rc << ")\n";
}

inline std::uint32_t machTicksForNanoseconds(std::uint64_t nanoseconds) {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) return 0;
    const long double ticks = static_cast<long double>(nanoseconds) *
                              static_cast<long double>(timebase.denom) /
                              static_cast<long double>(timebase.numer);
    return static_cast<std::uint32_t>(ticks);
}

inline std::uint64_t configuredAudioServicePeriodNs(
    std::uint64_t fallbackNanoseconds) {
    const char* value = std::getenv("MACFW_AUDIO_SERVICE_PERIOD_US");
    if (!value || value[0] == '\0')
        return fallbackNanoseconds;

    char* end = nullptr;
    const auto microseconds = std::strtoull(value, &end, 10);
    // Keep the experiment well inside the 6 ms rolling deadline guard.
    if (!end || *end != '\0' || microseconds < 250 || microseconds > 2000)
        return fallbackNanoseconds;
    return microseconds * 1000;
}

inline bool hasAudioServicePeriodEnvironmentOverride() {
    const char* value = std::getenv("MACFW_AUDIO_SERVICE_PERIOD_US");
    if (!value || value[0] == '\0') return false;
    char* end = nullptr;
    const auto microseconds = std::strtoull(value, &end, 10);
    return end && *end == '\0' && microseconds >= 250 &&
           microseconds <= 2000;
}

enum class AudioPerformanceProfile : unsigned {
    Aggressive = 0,
    Balanced = 1,
    Conservative = 2,
};

inline std::uint64_t audioPerformanceProfilePeriodNs(
    AudioPerformanceProfile profile) {
    switch (profile) {
        case AudioPerformanceProfile::Aggressive: return 250000;
        case AudioPerformanceProfile::Balanced: return 375000;
        case AudioPerformanceProfile::Conservative: return 500000;
    }
    return 375000;
}

inline const char* audioPerformanceProfileName(
    AudioPerformanceProfile profile) {
    switch (profile) {
        case AudioPerformanceProfile::Aggressive: return "aggressive";
        case AudioPerformanceProfile::Balanced: return "balanced";
        case AudioPerformanceProfile::Conservative: return "conservative";
    }
    return "balanced";
}

inline bool parseAudioPerformanceProfile(
    const std::string& name,
    AudioPerformanceProfile& profile) {
    if (name == "aggressive") {
        profile = AudioPerformanceProfile::Aggressive;
        return true;
    }
    if (name == "balanced") {
        profile = AudioPerformanceProfile::Balanced;
        return true;
    }
    if (name == "conservative") {
        profile = AudioPerformanceProfile::Conservative;
        return true;
    }
    return false;
}

class AudioServicePeriodControl {
public:
    explicit AudioServicePeriodControl(std::uint64_t fallbackNanoseconds)
        : periodNs_(configuredAudioServicePeriodNs(fallbackNanoseconds)),
          environmentOverride_(hasAudioServicePeriodEnvironmentOverride()) {
        const auto period = periodNs_.load(std::memory_order_relaxed);
        if (period == audioPerformanceProfilePeriodNs(
                          AudioPerformanceProfile::Aggressive))
            profile_.store(static_cast<unsigned>(
                               AudioPerformanceProfile::Aggressive),
                           std::memory_order_relaxed);
        else if (period == audioPerformanceProfilePeriodNs(
                               AudioPerformanceProfile::Conservative))
            profile_.store(static_cast<unsigned>(
                               AudioPerformanceProfile::Conservative),
                           std::memory_order_relaxed);
        else
            profile_.store(static_cast<unsigned>(
                               AudioPerformanceProfile::Balanced),
                           std::memory_order_relaxed);
    }

    std::uint64_t periodNs() const {
        return periodNs_.load(std::memory_order_acquire);
    }

    AudioPerformanceProfile profile() const {
        return static_cast<AudioPerformanceProfile>(
            profile_.load(std::memory_order_acquire));
    }

    bool environmentOverride() const { return environmentOverride_; }

    // A profile selected while an environment override is active is still
    // remembered by the persistent control state, but the explicit launchd
    // value remains authoritative until it is removed.
    void setProfile(AudioPerformanceProfile profile) {
        profile_.store(static_cast<unsigned>(profile),
                       std::memory_order_release);
        if (!environmentOverride_)
            periodNs_.store(audioPerformanceProfilePeriodNs(profile),
                            std::memory_order_release);
    }

private:
    std::atomic<std::uint64_t> periodNs_;
    std::atomic<unsigned> profile_{static_cast<unsigned>(
        AudioPerformanceProfile::Balanced)};
    bool environmentOverride_ = false;
};

inline bool requestAudioTimeConstraint() {
    constexpr std::uint64_t kPeriodNs = 2000000;
    constexpr std::uint64_t kComputationNs = 500000;
    constexpr std::uint64_t kConstraintNs = 2000000;

    thread_time_constraint_policy_data_t policy{};
    policy.period = machTicksForNanoseconds(kPeriodNs);
    policy.computation = machTicksForNanoseconds(kComputationNs);
    policy.constraint = machTicksForNanoseconds(kConstraintNs);
    policy.preemptible = TRUE;
    if (policy.period == 0 || policy.computation == 0 || policy.constraint == 0) {
        std::cout << "FW1814 audio service thread time-constraint: unavailable (timebase)\n";
        return false;
    }

    const thread_port_t threadPort = pthread_mach_thread_np(pthread_self());
    const kern_return_t kr = thread_policy_set(
        threadPort,
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) {
        std::cout << "FW1814 audio service thread time-constraint: "
                     "period=2000 us computation=500 us constraint=2000 us\n";
        return true;
    }

    std::cout << "FW1814 audio service thread time-constraint: request failed ("
              << kr << "); continuing with QoS only\n";
    return false;
}

class MachPacer {
public:
    explicit MachPacer(std::uint64_t nanoseconds) {
        setIntervalNanoseconds(nanoseconds);
    }

    bool setIntervalNanoseconds(std::uint64_t nanoseconds) {
        mach_timebase_info_data_t timebase{};
        if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
            timebase.numer == 0)
            return false;
        const long double ticks = static_cast<long double>(nanoseconds) *
                                  static_cast<long double>(timebase.denom) /
                                  static_cast<long double>(timebase.numer);
        intervalTicks_ = static_cast<std::uint64_t>(ticks);
        if (intervalTicks_ == 0) intervalTicks_ = 1;
        intervalNanoseconds_ = nanoseconds;
        nextWake_ = mach_absolute_time();
        return true;
    }

    bool valid() const { return intervalTicks_ != 0; }
    std::uint64_t intervalNanoseconds() const {
        return intervalNanoseconds_;
    }

    std::uint64_t wait() {
        if (!valid()) return 0;
        nextWake_ += intervalTicks_;
        mach_wait_until(nextWake_);
        const std::uint64_t now = mach_absolute_time();
        const std::uint64_t lateness = now > nextWake_ ? now - nextWake_ : 0;
        if (now > nextWake_ + intervalTicks_ * 4)
            nextWake_ = now;
        return lateness;
    }

private:
    std::uint64_t intervalTicks_ = 0;
    std::uint64_t intervalNanoseconds_ = 0;
    std::uint64_t nextWake_ = 0;
};

// Lightweight, verbose-only profiling accumulator for the deadline-sensitive
// audio loop. Callers take the mach timestamps so stages can be measured
// without callbacks, allocation, locks or logging in the hot path.
class AudioLoopTimingStats {
public:
    void observe(std::uint64_t wakeLateTicks,
                 std::uint64_t captureTicks,
                 std::uint64_t playbackTicks,
                 std::uint64_t txTicks,
                 std::uint64_t loopTicks) {
        ++loops_;
        captureTicks_ += captureTicks;
        playbackTicks_ += playbackTicks;
        txTicks_ += txTicks;
        loopTicks_ += loopTicks;
        maxWakeLateTicks_ = std::max(maxWakeLateTicks_, wakeLateTicks);
        maxCaptureTicks_ = std::max(maxCaptureTicks_, captureTicks);
        maxPlaybackTicks_ = std::max(maxPlaybackTicks_, playbackTicks);
        maxTxTicks_ = std::max(maxTxTicks_, txTicks);
        maxLoopTicks_ = std::max(maxLoopTicks_, loopTicks);
    }

    void append(std::ostream& out) const {
        if (loops_ == 0) return;
        out << " rt-loop-us=" << averageMicroseconds(loopTicks_)
            << '/' << microseconds(maxLoopTicks_)
            << " rt-cap-us=" << averageMicroseconds(captureTicks_)
            << '/' << microseconds(maxCaptureTicks_)
            << " rt-pb-us=" << averageMicroseconds(playbackTicks_)
            << '/' << microseconds(maxPlaybackTicks_)
            << " rt-tx-us=" << averageMicroseconds(txTicks_)
            << '/' << microseconds(maxTxTicks_)
            << " rt-wake-max-us=" << microseconds(maxWakeLateTicks_);
    }

    void reset() {
        loops_ = 0;
        captureTicks_ = 0;
        playbackTicks_ = 0;
        txTicks_ = 0;
        loopTicks_ = 0;
        maxWakeLateTicks_ = 0;
        maxCaptureTicks_ = 0;
        maxPlaybackTicks_ = 0;
        maxTxTicks_ = 0;
        maxLoopTicks_ = 0;
    }

private:
    static double microseconds(std::uint64_t ticks) {
        mach_timebase_info_data_t timebase{};
        if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
            timebase.denom == 0)
            return 0.0;
        return static_cast<double>(ticks) *
               static_cast<double>(timebase.numer) /
               static_cast<double>(timebase.denom) / 1000.0;
    }

    double averageMicroseconds(std::uint64_t ticks) const {
        return loops_ == 0 ? 0.0 : microseconds(ticks) /
            static_cast<double>(loops_);
    }

    std::uint64_t loops_ = 0;
    std::uint64_t captureTicks_ = 0;
    std::uint64_t playbackTicks_ = 0;
    std::uint64_t txTicks_ = 0;
    std::uint64_t loopTicks_ = 0;
    std::uint64_t maxWakeLateTicks_ = 0;
    std::uint64_t maxCaptureTicks_ = 0;
    std::uint64_t maxPlaybackTicks_ = 0;
    std::uint64_t maxTxTicks_ = 0;
    std::uint64_t maxLoopTicks_ = 0;
};

} // namespace macfw::fw1814::transport
