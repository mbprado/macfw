#include "macfw/amdtp_transmit_ring.h"
#include <cmath>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <utility>

namespace macfw {
struct AmdtpTransmitRing::StorageSlot {
    UInt32 status = 0;
    UInt32 timestamp = 0;
    std::uint8_t payload[am824::kPlayback48kDataPacketBytes];
};

AmdtpTransmitRing::~AmdtpTransmitRing() { reset(); }
AmdtpTransmitRing::AmdtpTransmitRing(AmdtpTransmitRing&& other) noexcept { moveFrom(std::move(other)); }
AmdtpTransmitRing& AmdtpTransmitRing::operator=(AmdtpTransmitRing&& other) noexcept {
    if (this != &other) { reset(); moveFrom(std::move(other)); }
    return *this;
}

void AmdtpTransmitRing::moveFrom(AmdtpTransmitRing&& other) noexcept {
    device_ = other.device_; storage_ = other.storage_; slots_ = other.slots_;
    packetCount_ = other.packetCount_; mappedBytes_ = other.mappedBytes_;
    firstCycle_ = other.firstCycle_; pool_ = other.pool_; localPort_ = other.localPort_;
    other.device_ = nullptr; other.storage_ = nullptr; other.slots_ = nullptr;
    other.packetCount_ = 0; other.mappedBytes_ = 0; other.firstCycle_ = 0;
    other.pool_ = nullptr; other.localPort_ = nullptr;
}

AmdtpTransmitRing AmdtpTransmitRing::createSilence48k(FireWireDevice& device,
                                                       UInt32 firstCycle,
                                                       std::size_t packetCount) {
    return create48k(device, firstCycle, 0, 0.0, 0.0, packetCount);
}

AmdtpTransmitRing AmdtpTransmitRing::createTone48k(FireWireDevice& device,
                                                    UInt32 firstCycle,
                                                    std::size_t pcmPosition,
                                                    double frequencyHz,
                                                    double amplitude,
                                                    std::size_t packetCount) {
    if (pcmPosition < 1 || pcmPosition > am824::kPlayback48kPcmPositions ||
        frequencyHz <= 0.0 || amplitude < 0.0 || amplitude > 8388607.0)
        return {};
    return create48k(device, firstCycle, pcmPosition, frequencyHz, amplitude, packetCount);
}

AmdtpTransmitRing AmdtpTransmitRing::create48k(FireWireDevice& device,
                                                UInt32 firstCycle,
                                                std::size_t tonePcmPosition,
                                                double frequencyHz,
                                                double amplitude,
                                                std::size_t packetCount) {
    AmdtpTransmitRing ring;
    if (!device.nativeHandle() || packetCount == 0) return ring;

    ring.device_ = &device;
    ring.packetCount_ = packetCount;
    ring.firstCycle_ = firstCycle & 0x1fffu;
    ring.mappedBytes_ = sizeof(StorageSlot) * packetCount;

    ring.storage_ = static_cast<StorageSlot*>(mmap(nullptr, ring.mappedBytes_,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
    if (ring.storage_ == MAP_FAILED) { ring.storage_ = nullptr; ring.reset(); return ring; }
    std::memset(ring.storage_, 0, ring.mappedBytes_);

    ring.slots_ = new (std::nothrow) PacketSlot[packetCount];
    if (!ring.slots_) { ring.reset(); return ring; }

    am824::Playback48kState state{};
    std::uint64_t audioFrame = 0;
    constexpr double kPi = 3.14159265358979323846;

    for (std::size_t i = 0; i < packetCount; ++i) {
        const UInt32 cycle = (ring.firstCycle_ + static_cast<UInt32>(i)) & 0x1fffu;
        auto packet = am824::buildPlayback48kSilence(cycle, state);

        if (packet.dataBearing && tonePcmPosition != 0) {
            const std::size_t channel = tonePcmPosition - 1;
            for (std::size_t event = 0; event < am824::kPlayback48kEventsPerDataPacket; ++event) {
                const double phase = 2.0 * kPi * frequencyHz *
                    static_cast<double>(audioFrame + event) / 48000.0;
                const std::int32_t sample = static_cast<std::int32_t>(std::sin(phase) * amplitude);
                const std::uint32_t mbla =
                    0x40000000u | (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                const std::size_t byteOffset =
                    8 + (event * am824::kPlayback48kPositions + channel) * 4;
                am824::putBe32Playback(packet.bytes.data() + byteOffset, mbla);
            }
        }

        if (packet.dataBearing)
            audioFrame += am824::kPlayback48kEventsPerDataPacket;

        std::memcpy(ring.storage_[i].payload, packet.bytes.data(), packet.length);
        ring.slots_[i] = {ring.storage_[i].payload, packet.length, cycle,
                          packet.dbc, packet.syt, packet.dataBearing};
        if (packet.dataBearing) state.dbc = static_cast<std::uint8_t>(state.dbc + 8u);
        state.phase = static_cast<std::uint8_t>((state.phase + 1u) & 3u);
    }

    auto native = device.nativeHandle();
    ring.pool_ = (*native)->CreateNuDCLPool(native, static_cast<UInt32>(packetCount),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!ring.pool_) { ring.reset(); return ring; }

    (*ring.pool_)->SetCurrentTagAndSync(ring.pool_, 1, 0);

    NuDCLRef first = nullptr;
    NuDCLRef last = nullptr;
    for (std::size_t i = 0; i < packetCount; ++i) {
        IOVirtualRange range = {
            reinterpret_cast<IOVirtualAddress>(ring.storage_[i].payload),
            static_cast<IOByteCount>(ring.slots_[i].length)
        };
        auto dcl = (*ring.pool_)->AllocateSendPacket(ring.pool_, nullptr, 1, &range);
        if (!dcl) { ring.reset(); return ring; }
        const NuDCLRef ref = reinterpret_cast<NuDCLRef>(dcl);
        if (!first) first = ref;
        last = ref;
    }

    if (!first || !last ||
        (*ring.pool_)->SetDCLBranch(last, first) != kIOReturnSuccess) {
        ring.reset();
        return ring;
    }

    DCLCommand* program = (*ring.pool_)->GetProgram(ring.pool_);
    if (!program) { ring.reset(); return ring; }

    IOVirtualRange mapped = {
        reinterpret_cast<IOVirtualAddress>(ring.storage_),
        static_cast<IOByteCount>(ring.mappedBytes_)
    };
    ring.localPort_ = (*native)->CreateLocalIsochPort(native, true, program,
        kFWDCLCycleEvent, ring.firstCycle_, 0x1fffu,
        nullptr, 0, &mapped, 1,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!ring.localPort_) { ring.reset(); return ring; }
    return ring;
}

const AmdtpTransmitRing::PacketSlot& AmdtpTransmitRing::slot(std::size_t index) const {
    static const PacketSlot empty{};
    return (!slots_ || index >= packetCount_) ? empty : slots_[index];
}

void AmdtpTransmitRing::reset() {
    if (localPort_) { (*localPort_)->Release(localPort_); localPort_ = nullptr; }
    if (pool_) { (*pool_)->Release(pool_); pool_ = nullptr; }
    delete[] slots_; slots_ = nullptr;
    if (storage_) { munmap(storage_, mappedBytes_); storage_ = nullptr; }
    device_ = nullptr; packetCount_ = 0; mappedBytes_ = 0; firstCycle_ = 0;
}
} // namespace macfw
