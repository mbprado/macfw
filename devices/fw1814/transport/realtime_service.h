#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
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
        mach_timebase_info_data_t timebase{};
        if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0)
            return;
        const long double ticks = static_cast<long double>(nanoseconds) *
                                  static_cast<long double>(timebase.denom) /
                                  static_cast<long double>(timebase.numer);
        intervalTicks_ = static_cast<std::uint64_t>(ticks);
        if (intervalTicks_ == 0) intervalTicks_ = 1;
        nextWake_ = mach_absolute_time();
    }

    bool valid() const { return intervalTicks_ != 0; }

    void wait() {
        if (!valid()) return;
        nextWake_ += intervalTicks_;
        mach_wait_until(nextWake_);
        const std::uint64_t now = mach_absolute_time();
        if (now > nextWake_ + intervalTicks_ * 4)
            nextWake_ = now;
    }

private:
    std::uint64_t intervalTicks_ = 0;
    std::uint64_t nextWake_ = 0;
};

} // namespace macfw::fw1814::transport
