#pragma once

#include "macfw/amdtp_transmit_ring.h"
#include "macfw/pcm_ring_buffer.h"

#include <cstddef>
#include <cstdint>

namespace macfw {

class AmdtpPcmStream48k {
public:
    struct Stats {
        std::uint64_t halvesRefilled = 0;
        std::uint64_t dataPacketsRefilled = 0;
        std::uint64_t framesFromBuffer = 0;
        std::uint64_t framesSilenced = 0;
        std::uint64_t lateCyclePolls = 0;
    };

    AmdtpPcmStream48k(AmdtpTransmitRing& tx,
                      PcmRingBuffer& pcm,
                      UInt32 observedCycleBeforeStart,
                      UInt32 firstTxCycle,
                      std::size_t halfPackets = 64);

    bool valid() const;
    bool prime();
    void service(UInt32 currentCycle);

    const Stats& stats() const { return stats_; }
    std::uint64_t cyclesObserved() const { return cyclesObserved_; }
    bool streamReached() const { return streamReached_; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;
    static UInt32 cycleDelta(UInt32 newer, UInt32 older);
    void accumulate(const AmdtpTransmitRing::RefillResult& refill);

    AmdtpTransmitRing* tx_ = nullptr;
    PcmRingBuffer* pcm_ = nullptr;
    UInt32 initialCycle_ = 0;
    UInt32 firstTxCycle_ = 0;
    UInt32 leadCycles_ = 0;
    UInt32 lastCycle_ = 0;
    std::size_t halfPackets_ = 0;
    std::uint64_t cyclesObserved_ = 0;
    std::uint64_t lastHalfNumber_ = 0;
    bool streamReached_ = false;
    bool primed_ = false;
    Stats stats_{};
};

// Native 44.1-kHz live scheduler. The transport ring must contain exactly two
// equal halves and should be 640 packets total so the 441/640 data/NODATA
// pattern repeats at the DCL slot boundary. Recycled halves get complete new
// CIP headers as well as PCM payload, keeping DBC/SYT continuous across wraps.
class AmdtpPcmStream44100 {
public:
    using Stats = AmdtpPcmStream48k::Stats;

    AmdtpPcmStream44100(AmdtpTransmitRing& tx,
                        PcmRingBuffer& pcm,
                        UInt32 observedCycleBeforeStart,
                        UInt32 firstTxCycle,
                        std::size_t halfPackets = 320);

    bool valid() const;
    bool prime();
    void service(UInt32 currentCycle);

    const Stats& stats() const { return stats_; }
    std::uint64_t cyclesObserved() const { return cyclesObserved_; }
    bool streamReached() const { return streamReached_; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;
    static UInt32 cycleDelta(UInt32 newer, UInt32 older);
    void accumulate(const AmdtpTransmitRing::RefillResult& refill);

    AmdtpTransmitRing* tx_ = nullptr;
    PcmRingBuffer* pcm_ = nullptr;
    UInt32 initialCycle_ = 0;
    UInt32 firstTxCycle_ = 0;
    UInt32 leadCycles_ = 0;
    UInt32 lastCycle_ = 0;
    UInt32 nextCycle_ = 0;
    std::size_t halfPackets_ = 0;
    std::uint64_t cyclesObserved_ = 0;
    std::uint64_t lastHalfNumber_ = 0;
    bool streamReached_ = false;
    bool primed_ = false;
    am824::Playback44100State nextState_{};
    Stats stats_{};
};

} // namespace macfw
