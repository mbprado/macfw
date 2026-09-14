#include "macfw/amdtp_pcm_stream.h"

namespace macfw {

UInt32 AmdtpPcmStream48k::cycleDelta(UInt32 newer, UInt32 older) {
    return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
}

AmdtpPcmStream48k::AmdtpPcmStream48k(AmdtpTransmitRing& tx,
                                     PcmRingBuffer& pcm,
                                     UInt32 observedCycleBeforeStart,
                                     UInt32 firstTxCycle,
                                     std::size_t halfPackets)
    : tx_(&tx), pcm_(&pcm),
      initialCycle_(observedCycleBeforeStart % kCyclesPerSecond),
      firstTxCycle_(firstTxCycle % kCyclesPerSecond),
      leadCycles_(cycleDelta(firstTxCycle_, initialCycle_)),
      lastCycle_(initialCycle_), halfPackets_(halfPackets) {}

bool AmdtpPcmStream48k::valid() const {
    return tx_ && pcm_ && static_cast<bool>(*tx_) && pcm_->valid() &&
           tx_->packetCount() != 0 && halfPackets_ != 0 &&
           tx_->packetCount() % halfPackets_ == 0 &&
           tx_->packetCount() / halfPackets_ == 2;
}

void AmdtpPcmStream48k::accumulate(const AmdtpTransmitRing::RefillResult& refill) {
    ++stats_.halvesRefilled;
    stats_.dataPacketsRefilled += refill.dataPacketsRefilled;
    stats_.framesFromBuffer += refill.framesFromBuffer;
    stats_.framesSilenced += refill.framesSilenced;
}

bool AmdtpPcmStream48k::prime() {
    if (!valid()) return false;
    const auto first = tx_->refillPcm48k(*pcm_, 0, halfPackets_);
    const auto second = tx_->refillPcm48k(*pcm_, halfPackets_, halfPackets_);
    if (first.dataPacketsRefilled == 0 || second.dataPacketsRefilled == 0) return false;
    primed_ = true;
    return true;
}

void AmdtpPcmStream48k::service(UInt32 currentCycle) {
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
        const std::size_t consumedHalf = static_cast<std::size_t>(lastHalfNumber_ & 1u);
        accumulate(tx_->refillPcm48k(*pcm_, consumedHalf * halfPackets_, halfPackets_));
        ++lastHalfNumber_;
    }
}

UInt32 AmdtpPcmStream44100::cycleDelta(UInt32 newer, UInt32 older) {
    return (newer + kCyclesPerSecond - older) % kCyclesPerSecond;
}

AmdtpPcmStream44100::AmdtpPcmStream44100(AmdtpTransmitRing& tx,
                                         PcmRingBuffer& pcm,
                                         UInt32 observedCycleBeforeStart,
                                         UInt32 firstTxCycle,
                                         std::size_t halfPackets)
    : tx_(&tx), pcm_(&pcm),
      initialCycle_(observedCycleBeforeStart % kCyclesPerSecond),
      firstTxCycle_(firstTxCycle % kCyclesPerSecond),
      leadCycles_(cycleDelta(firstTxCycle_, initialCycle_)),
      lastCycle_(initialCycle_), nextCycle_(firstTxCycle),
      halfPackets_(halfPackets) {}

bool AmdtpPcmStream44100::valid() const {
    return tx_ && pcm_ && static_cast<bool>(*tx_) && pcm_->valid() &&
           pcm_->channelCount() == am824::kPlayback44100PcmPositions &&
           tx_->packetCount() == 640 && halfPackets_ == 320;
}

void AmdtpPcmStream44100::accumulate(const AmdtpTransmitRing::RefillResult& refill) {
    ++stats_.halvesRefilled;
    stats_.dataPacketsRefilled += refill.dataPacketsRefilled;
    stats_.framesFromBuffer += refill.framesFromBuffer;
    stats_.framesSilenced += refill.framesSilenced;
}

bool AmdtpPcmStream44100::prime() {
    if (!valid()) return false;

    am824::Playback44100State state{};
    UInt32 cycle = firstTxCycle_;
    const auto first = tx_->refillPcm44100(*pcm_, 0, halfPackets_, state, cycle);
    const auto second = tx_->refillPcm44100(*pcm_, halfPackets_, halfPackets_, state, cycle);
    if (first.dataPacketsRefilled == 0 || second.dataPacketsRefilled == 0) return false;

    nextState_ = state;
    nextCycle_ = cycle;
    primed_ = true;
    return true;
}

void AmdtpPcmStream44100::service(UInt32 currentCycle) {
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
        const std::size_t consumedHalf = static_cast<std::size_t>(lastHalfNumber_ & 1u);
        const auto refill = tx_->refillPcm44100(*pcm_, consumedHalf * halfPackets_,
                                                halfPackets_, nextState_, nextCycle_);
        accumulate(refill);
        ++lastHalfNumber_;
    }
}

} // namespace macfw
