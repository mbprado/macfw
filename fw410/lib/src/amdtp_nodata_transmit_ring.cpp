#include "macfw/amdtp_nodata_transmit_ring.h"
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <utility>

namespace macfw {
struct AmdtpNoDataTransmitRing::StorageSlot {
    std::uint8_t payload[8];
};

namespace {
inline void putBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
    p[3] = static_cast<std::uint8_t>(v & 0xffu);
}
}

AmdtpNoDataTransmitRing::~AmdtpNoDataTransmitRing() { reset(); }
AmdtpNoDataTransmitRing::AmdtpNoDataTransmitRing(AmdtpNoDataTransmitRing&& other) noexcept {
    moveFrom(std::move(other));
}
AmdtpNoDataTransmitRing& AmdtpNoDataTransmitRing::operator=(AmdtpNoDataTransmitRing&& other) noexcept {
    if (this != &other) { reset(); moveFrom(std::move(other)); }
    return *this;
}

void AmdtpNoDataTransmitRing::moveFrom(AmdtpNoDataTransmitRing&& other) noexcept {
    storage_ = other.storage_;
    packetCount_ = other.packetCount_;
    mappedBytes_ = other.mappedBytes_;
    firstCycle_ = other.firstCycle_;
    pool_ = other.pool_;
    localPort_ = other.localPort_;
    other.storage_ = nullptr;
    other.packetCount_ = 0;
    other.mappedBytes_ = 0;
    other.firstCycle_ = 0;
    other.pool_ = nullptr;
    other.localPort_ = nullptr;
}

AmdtpNoDataTransmitRing AmdtpNoDataTransmitRing::create(FireWireDevice& device,
                                                         UInt32 firstCycle,
                                                         std::uint8_t fdf,
                                                         std::uint8_t dbs,
                                                         std::size_t packetCount) {
    AmdtpNoDataTransmitRing ring;
    auto native = device.nativeHandle();
    if (!native || packetCount == 0) return ring;

    ring.packetCount_ = packetCount;
    ring.firstCycle_ = firstCycle & 0x1fffu;
    ring.mappedBytes_ = sizeof(StorageSlot) * packetCount;
    ring.storage_ = static_cast<StorageSlot*>(mmap(nullptr, ring.mappedBytes_,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
    if (ring.storage_ == MAP_FAILED) { ring.storage_ = nullptr; ring.reset(); return ring; }
    std::memset(ring.storage_, 0, ring.mappedBytes_);

    for (std::size_t i = 0; i < packetCount; ++i) {
        putBe32(ring.storage_[i].payload,
                (static_cast<std::uint32_t>(dbs) << 16));
        putBe32(ring.storage_[i].payload + 4,
                0x90000000u | (static_cast<std::uint32_t>(fdf) << 16) | 0xffffu);
    }

    ring.pool_ = (*native)->CreateNuDCLPool(native, static_cast<UInt32>(packetCount),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!ring.pool_) { ring.reset(); return ring; }
    (*ring.pool_)->SetCurrentTagAndSync(ring.pool_, 1, 0);

    NuDCLRef first = nullptr;
    NuDCLRef last = nullptr;
    for (std::size_t i = 0; i < packetCount; ++i) {
        IOVirtualRange range = {
            reinterpret_cast<IOVirtualAddress>(ring.storage_[i].payload), 8
        };
        auto dcl = (*ring.pool_)->AllocateSendPacket(ring.pool_, nullptr, 1, &range);
        if (!dcl) { ring.reset(); return ring; }
        const NuDCLRef ref = reinterpret_cast<NuDCLRef>(dcl);
        if (!first) first = ref;
        last = ref;
    }
    if (!first || !last || (*ring.pool_)->SetDCLBranch(last, first) != kIOReturnSuccess) {
        ring.reset(); return ring;
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

void AmdtpNoDataTransmitRing::reset() {
    if (localPort_) { (*localPort_)->Release(localPort_); localPort_ = nullptr; }
    if (pool_) { (*pool_)->Release(pool_); pool_ = nullptr; }
    if (storage_) { munmap(storage_, mappedBytes_); storage_ = nullptr; }
    packetCount_ = 0;
    mappedBytes_ = 0;
    firstCycle_ = 0;
}
}
