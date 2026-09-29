#pragma once
#include "../hal/include/macfw_fw1814_impulse_trace.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace macfw::fw1814::diagnostic {
class Session {
public:
    ~Session() {
        if (shared_ && shared_->requestCookie.load()==cookie_) {
            auto expected=id_;
            shared_->requestId.compare_exchange_strong(expected,0);
        }
    }
    bool arm(std::uint32_t id) {
        if (!id || !mapping_.open()) return false;
        shared_=mapping_.get();
        cookie_=shared_->engineCookie.load();
        if (!cookie_ || !shared_->halPid.load() ||
            kill(static_cast<pid_t>(shared_->enginePid.load()),0) != 0 ||
            kill(static_cast<pid_t>(shared_->halPid.load()),0) != 0) return false;
        auto previous=shared_->requestId.load();
        if (previous && shared_->expiry.load() < mach_absolute_time())
            shared_->requestId.compare_exchange_strong(previous,0);
        std::uint64_t empty=0;
        if (!shared_->requestId.compare_exchange_strong(empty,UINT64_MAX)) return false;
        id_=id;
        for (auto& point: shared_->points) point.id.store(0);
        shared_->requestCookie.store(cookie_);
        const auto begin=mach_absolute_time();
        shared_->expiry.store(begin+nsTicks(10000000000ull));
        shared_->requestId.store(id_,std::memory_order_release);
        for (unsigned i=0; i<50; ++i) {
            if (shared_->engineCookie.load()!=cookie_) return false;
            if (shared_->counterTick.load(std::memory_order_acquire)>begin) {
                printCounters("before");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false; // old engine, stalled thread, or missing instrumentation
    }
    Shared* shared() const { return shared_; }
    void finish() const {
        std::cout << "trace-session={\"id\":" << id_ << ",\"engine_cookie\":" << cookie_
                  << ",\"engine_cookie_after\":" << shared_->engineCookie.load()
                  << ",\"generation\":" << shared_->generation.load()
                  << ",\"engine_pid\":" << shared_->enginePid.load()
                  << ",\"hal_pid\":" << shared_->halPid.load()
                  << ",\"initial_cycle\":" << shared_->initialCycle.load()
                  << ",\"first_cycle\":" << shared_->firstCycle.load()
                  << ",\"tx_packets\":" << shared_->txPackets.load()
                  << ",\"rolling\":" << shared_->rolling.load()
                  << ",\"lead\":" << shared_->lead.load()
                  << ",\"guard\":" << shared_->guard.load()
                  << ",\"service_ns\":" << shared_->serviceNs.load() << "}\n";
        for (unsigned i=0; i<kStages; ++i) {
            const auto& s=shared_->points[i];
            std::cout << "trace-stage={\"stage\":\"" << kStageNames[i] << "\",\"id\":" << s.id.load(std::memory_order_acquire);
            if (s.id.load()==id_) {
                std::cout << ",\"tick\":" << s.tick.load() << ",\"verified_tick\":" << s.verifiedTick.load()
                          << ",\"sample_host\":" << s.sampleHost.load() << ",\"frame\":" << s.frame.load()
                          << ",\"queue\":" << s.queue.load() << ",\"pcm_queue\":" << s.pcmQueue.load()
                          << ",\"callback_frames\":" << s.callbackFrames.load() << ",\"offset\":" << s.offset.load()
                          << ",\"a\":" << s.a.load() << ",\"b\":" << s.b.load()
                          << ",\"c\":" << s.c.load() << ",\"d\":" << s.d.load()
                          << ",\"cycle_timer\":" << s.cycleTimer.load() << ",\"cycle_host\":" << s.cycleHost.load()
                          << ",\"cycle_uncertainty\":" << s.cycleUncertainty.load()
                          << ",\"last_a\":" << s.lastA.load() << ",\"last_tick\":" << s.lastTick.load();
            } else std::cout << ",\"status\":\"missing_match\"";
            std::cout << "}\n";
        }
        printCounters("after");
    }
private:
    void printCounters(const char* suffix) const {
        std::uint64_t tick=0;
        std::array<std::uint64_t,kCounters> counters{};
        bool stable=false;
        for (unsigned retry=0; retry<100; ++retry) {
            const auto sequence=shared_->counterSequence.load();
            if (sequence & 1u) { std::this_thread::yield(); continue; }
            tick=shared_->counterTick.load();
            for (unsigned i=0; i<kCounters; ++i) counters[i]=shared_->counters[i].load();
            if (shared_->counterSequence.load()==sequence) { stable=true; break; }
        }
        std::cout << "trace-counters-" << suffix << "={\"tick\":" << tick << ",\"coherent\":" << (stable?"true":"false");
        for (unsigned i=0; i<kCounters; ++i) std::cout << ",\"" << kCounterNames[i] << "\":" << counters[i];
        std::cout << "}\n";
    }
    Mapping mapping_;
    Shared* shared_=nullptr;
    std::uint64_t id_=0, cookie_=0;
};
} // namespace macfw::fw1814::diagnostic
