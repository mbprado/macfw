#include "macfw/amdtp_receive_ring.h"
#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <utility>

namespace macfw {

struct AmdtpReceiveRing::RawSlot {
    UInt32 isoHeader;
    UInt32 status;
    UInt32 timestamp;
};

AmdtpReceiveRing::~AmdtpReceiveRing() { reset(); }

AmdtpReceiveRing::AmdtpReceiveRing(AmdtpReceiveRing&& other) noexcept { moveFrom(std::move(other)); }
AmdtpReceiveRing& AmdtpReceiveRing::operator=(AmdtpReceiveRing&& other) noexcept {
    if (this != &other) { reset(); moveFrom(std::move(other)); }
    return *this;
}

void AmdtpReceiveRing::moveFrom(AmdtpReceiveRing&& other) noexcept {
    device_ = other.device_; storage_ = other.storage_; slots_ = other.slots_;
    packetCount_ = other.packetCount_; packetCapacity_ = other.packetCapacity_;
    rawSlotBytes_ = other.rawSlotBytes_; storageBytes_ = other.storageBytes_;
    completed_ = other.completed_; pool_ = other.pool_; localPort_ = other.localPort_;
    other.device_ = nullptr; other.storage_ = nullptr; other.slots_ = nullptr;
    other.packetCount_ = 0; other.packetCapacity_ = 0; other.rawSlotBytes_ = 0;
    other.storageBytes_ = 0; other.completed_ = false; other.pool_ = nullptr; other.localPort_ = nullptr;
}

AmdtpReceiveRing AmdtpReceiveRing::create(FireWireDevice& device, std::size_t packetCount,
                                          std::size_t packetCapacity) {
    AmdtpReceiveRing ring;
    if (!device.nativeHandle() || packetCount == 0 || packetCapacity < 8) return ring;
    ring.device_ = &device; ring.packetCount_ = packetCount; ring.packetCapacity_ = packetCapacity;
    ring.rawSlotBytes_ = sizeof(RawSlot) + packetCapacity;
    ring.storageBytes_ = ring.rawSlotBytes_ * packetCount;

    // Match the known-good probe: every receive slot lives in one contiguous
    // DMA mapping as [isoHeader,status,timestamp,payload].
    ring.storage_ = static_cast<std::uint8_t*>(mmap(nullptr, ring.storageBytes_,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
    if (ring.storage_ == MAP_FAILED) { ring.storage_ = nullptr; ring.reset(); return ring; }
    std::memset(ring.storage_, 0, ring.storageBytes_);
    ring.slots_ = new (std::nothrow) PacketSlot[packetCount];
    if (!ring.slots_) { ring.reset(); return ring; }

    for (std::size_t i = 0; i < packetCount; ++i) {
        auto* raw = reinterpret_cast<RawSlot*>(ring.storage_ + i * ring.rawSlotBytes_);
        ring.slots_[i].payload = reinterpret_cast<std::uint8_t*>(raw) + sizeof(RawSlot);
        ring.slots_[i].capacity = packetCapacity;
    }

    auto native = device.nativeHandle();
    ring.pool_ = (*native)->CreateNuDCLPool(native, static_cast<UInt32>(packetCount),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!ring.pool_) { ring.reset(); return ring; }

    // Receive NuDCL metadata (iso header/status/timestamp) is only guaranteed
    // current after an update has run. Publishing the entire 256-slot ring only
    // at its final DCL made capture arrive to userspace in ~32 ms bursts and
    // left a narrow window where early payload slots could already be reused by
    // the next revolution while userspace was still consuming the old batch.
    //
    // Publish smaller 32-cycle groups instead. At the 8 kHz FireWire cycle rate
    // this exposes a completed receive batch roughly every 4 ms, while leaving
    // the 256-slot DMA ring and its cyclic branch unchanged.
    constexpr std::size_t kUpdateChunkSlots = 32;

    NuDCLRef first = nullptr, last = nullptr;
    CFMutableSetRef updateSet = nullptr;
    for (std::size_t i = 0; i < packetCount; ++i) {
        if ((i % kUpdateChunkSlots) == 0) {
            updateSet = CFSetCreateMutable(kCFAllocatorDefault, 0, nullptr);
            if (!updateSet) { ring.reset(); return ring; }
        }

        auto* raw = reinterpret_cast<RawSlot*>(ring.storage_ + i * ring.rawSlotBytes_);
        auto* payload = reinterpret_cast<std::uint8_t*>(raw) + sizeof(RawSlot);
        IOVirtualRange ranges[2] = {
            {reinterpret_cast<IOVirtualAddress>(&raw->isoHeader), static_cast<IOByteCount>(sizeof(raw->isoHeader))},
            {reinterpret_cast<IOVirtualAddress>(payload), static_cast<IOByteCount>(packetCapacity)}
        };
        auto dcl = (*ring.pool_)->AllocateReceivePacket(ring.pool_, updateSet, 4, 2, ranges);
        if (!dcl) {
            if (updateSet) CFRelease(updateSet);
            ring.reset();
            return ring;
        }
        const NuDCLRef ref = reinterpret_cast<NuDCLRef>(dcl);
        if (!first) first = ref;
        last = ref;
        (*ring.pool_)->SetDCLStatusPtr(ref, &raw->status);
        (*ring.pool_)->SetDCLTimeStampPtr(ref, &raw->timestamp);

        const bool endOfChunk = ((i + 1) % kUpdateChunkSlots) == 0 || (i + 1) == packetCount;
        if (endOfChunk) {
            const IOReturn updateResult = (*ring.pool_)->SetDCLUpdateList(ref, updateSet);
            CFRelease(updateSet);
            updateSet = nullptr;
            if (updateResult != kIOReturnSuccess) {
                ring.reset();
                return ring;
            }
        }
    }

    if (updateSet) CFRelease(updateSet);

    if (!first || !last ||
        (*ring.pool_)->SetDCLBranch(last, first) != kIOReturnSuccess) {
        ring.reset(); return ring;
    }

    DCLCommand* program = (*ring.pool_)->GetProgram(ring.pool_);
    if (!program) { ring.reset(); return ring; }
    IOVirtualRange mappedRange = {reinterpret_cast<IOVirtualAddress>(ring.storage_),
        static_cast<IOByteCount>(ring.storageBytes_)};
    ring.localPort_ = (*native)->CreateLocalIsochPort(native, false, program,
        0, 0, 0, nullptr, 0, &mappedRange, 1,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!ring.localPort_) { ring.reset(); return ring; }
    return ring;
}

void AmdtpReceiveRing::syncSlot(std::size_t index) const {
    if (!slots_ || !storage_ || index >= packetCount_) return;
    const auto* raw = reinterpret_cast<const RawSlot*>(storage_ + index * rawSlotBytes_);
    slots_[index].isoHeader = raw->isoHeader; slots_[index].status = raw->status;
    slots_[index].timestamp = raw->timestamp;
}
const AmdtpReceiveRing::PacketSlot& AmdtpReceiveRing::slot(std::size_t index) const {
    static const PacketSlot empty{}; if (!slots_ || index >= packetCount_) return empty;
    syncSlot(index); return slots_[index];
}
std::size_t AmdtpReceiveRing::touchedCount() const {
    std::size_t count = 0; for (std::size_t i = 0; i < packetCount_; ++i) {
        syncSlot(i); if (slots_[i].touched()) ++count;
    } return count;
}
void AmdtpReceiveRing::reset() {
    if (localPort_) { (*localPort_)->Release(localPort_); localPort_ = nullptr; }
    if (pool_) { (*pool_)->Release(pool_); pool_ = nullptr; }
    delete[] slots_; slots_ = nullptr;
    if (storage_) { munmap(storage_, storageBytes_); storage_ = nullptr; }
    device_ = nullptr; packetCount_ = 0; packetCapacity_ = 0; rawSlotBytes_ = 0;
    storageBytes_ = 0; completed_ = false;
}

} // namespace macfw
