#include "macfw/amdtp_transmit_ring.h"
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <utility>

namespace macfw {
struct AmdtpTransmitRing::StorageSlot {
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
    for (std::size_t i = 0; i < packetCount; ++i) {
        const UInt32 cycle = (ring.firstCycle_ + static_cast<UInt32>(i)) & 0x1fffu;
        const auto packet = am824::buildPlayback48kSilence(cycle, state);
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

    for (std::size_t i = 0; i < packetCount; ++i) {
        IOVirtualRange range = {
            reinterpret_cast<IOVirtualAddress>(ring.storage_[i].payload),
            static_cast<IOByteCount>(ring.slots_[i].length)
        };
        auto dcl = (*ring.pool_)->AllocateSendPacket(ring.pool_, nullptr, 1, &range);
        if (!dcl) { ring.reset(); return ring; }
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
