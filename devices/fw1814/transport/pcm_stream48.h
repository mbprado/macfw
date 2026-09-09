#pragma once

#include "blocking_pcm_tx.h"
#include "macfw/pcm_ring_buffer.h"

#include <cstddef>
#include <cstdint>

namespace macfw::fw1814::transport {

class BlockingPcmStream48k {
public:
    struct Stats {
        std::uint64_t halvesRefilled = 0;
        std::uint64_t dataPacketsRefilled = 0;
        std::uint64_t framesFromBuffer = 0;
        std::uint64_t framesSilenced = 0;
        std::uint64_t lateCyclePolls = 0;
    };

    BlockingPcmStream48k(BlockingPcmTransmitRing48k& tx,
                         macfw::PcmRingBuffer& pcm,
                         UInt32 observedCycleBeforeStart,
                         UInt32 firstTxCycle,
                         std::size_t halfPackets = 64)
        : tx_(&tx), pcm_(&pcm),
          initialCycle_(observedCycleBeforeStart % kCyclesPerSecond),
          firstTxCycle_(firstTxCycle % kCyclesPerSecond),
          leadCycles_(cycleDelta(firstTxCycle_, initialCycle_)),
          lastCycle_(initialCycle_), halfPackets_(halfPackets) {}

    bool valid() const {
        return tx_ && pcm_ && static_cast<bool>(*tx_) && pcm_->valid() &&
               pcm_->channelCount() == BlockingPcmTransmitRing48k::pcmChannels() &&
               tx_->packetCount() != 0 && halfPackets_ != 0 &&
               tx_->packetCount() == halfPackets_ * 2;
    }

    bool prime() {
        if (!valid()) return false;
        const auto first = tx_->refill(*pcm_, 0, halfPackets_);
        const auto second = tx_->refill(*pcm_, halfPackets_, halfPackets_);
        if (first.dataPacketsRefilled == 0 || second.dataPacketsRefilled == 0)
            return false;
        accumulate(first);
        accumulate(second);
        primed_ = true;
        return true;
    }

    void service(UInt32 currentCycle) {
        if (!valid() || !primed_) return;
        currentCycle %= kCyclesPerSecond;
        const UInt32 delta = cycleDelta(currentCycle, lastCycle_);
        lastCycle_ = currentCycle;
        if (delta > 32) ++stats_.lateCyclePolls;
        cyclesObserved_ += delta;

        if (!streamReached_) {
            if (cyclesObserved_ < leadCycles_) return;
            streamReached_ = true;
        }

        const std::uint64_t sinceStart = cyclesObserved_ - leadCycles_;
        const std::uint64_t halfNumber = sinceStart / halfPackets_;
        while (lastHalfNumber_ < halfNumber) {
            const std::size_t consumedHalf =
                static_cast<std::size_t>(lastHalfNumber_ & 1u);
            accumulate(tx_->refill(*pcm_, consumedHalf * halfPackets_, halfPackets_));
            ++lastHalfNumber_;
        }
    }

    const Stats& stats() const { return stats_; }
    bool streamReached() const { return streamReached_; }

private:
    static constexpr UInt32 kCyclesPerSecond = 8000;

    static UInt32 cycleDelta(UInt32 newer, UInt32 older) {
        return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
    }

    void accumulate(const BlockingPcmTransmitRing48k::RefillResult& refill) {
        ++stats_.halvesRefilled;
        stats_.dataPacketsRefilled += refill.dataPacketsRefilled;
        stats_.framesFromBuffer += refill.framesFromBuffer;
        stats_.framesSilenced += refill.framesSilenced;
    }

    BlockingPcmTransmitRing48k* tx_ = nullptr;
    macfw::PcmRingBuffer* pcm_ = nullptr;
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

} // namespace macfw::fw1814::transport
