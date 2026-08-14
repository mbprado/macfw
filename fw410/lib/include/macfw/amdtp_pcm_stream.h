#pragma once

#include "macfw/amdtp_transmit_ring.h"
#include "macfw/pcm_ring_buffer.h"

#include <cstddef>
#include <cstdint>

namespace macfw {

// Cycle-driven live PCM refill scheduler for the FW410 48 kHz blocking stream.
//
// This class deliberately does not own the FireWire channel or poll the bus.
// The caller supplies observed 1394 cycle numbers (0..7999). The scheduler
// tracks progress from the pre-stream observation point, detects completed
// half-rings, and refills only the half DMA has just left behind.
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

    // Fill the complete TX ring before the isochronous channel starts.
    bool prime();

    // Supply the latest 1394 cycle number (0..7999). Any fully consumed
    // half-rings since the previous call are refilled from the PCM ring.
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

} // namespace macfw
