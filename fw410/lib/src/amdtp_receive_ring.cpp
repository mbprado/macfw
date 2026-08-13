#include "macfw/amdtp_receive_ring.h"
#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <utility>

namespace macfw {

struct AmdtpReceiveRing::RawSlot {
    UInt32 isoHeader = 0;
    UInt32 status = 0;
    UInt32 timestamp = 0;
};

struct AmdtpReceiveRing::CallbackState {
    AmdtpReceiveRing* owner = nullptr;
};

AmdtpReceiveRing::~AmdtpReceiveRing() { reset(); }

AmdtpReceiveRing::AmdtpReceiveRing(AmdtpReceiveRing&& other) noexcept {
    moveFrom(std::move(other));
}

AmdtpReceiveRing& AmdtpReceiveRing::operator=(AmdtpReceiveRing&& other) noexcept {
    if (this != &other) {
        reset();
        moveFrom(std::move(other));
    }
    return *this;
}

void AmdtpReceiveRing::moveFrom(AmdtpReceiveRing&& other) noexcept {
    device_ = other.device_;
    rawSlots_ = other.rawSlots_;
    payloadBase_ = other.payloadBase_;
    slots_ = other.slots_;
    callbackState_ = other.callbackState_;
    packetCount_ = other.packetCount_;
    packetCapacity_ = other.packetCapacity_;
    metadataBytes_ = other.metadataBytes_;
    payloadBytes_ = other.payloadBytes_;
    completed_ = other.completed_;
    pool_ = other.pool_;
    localPort_ = other.localPort_;
    if (callbackState_) callbackState_->owner = this;

    other.device_ = nullptr;
    other.rawSlots_ = nullptr;
    other.payloadBase_ = nullptr;
    other.slots_ = nullptr;
    other.callbackState_ = nullptr;
    other.packetCount_ = 0;
    other.packetCapacity_ = 0;
    other.metadataBytes_ = 0;
    other.payloadBytes_ = 0;
    other.completed_ = false;
    other.pool_ = nullptr;
    other.localPort_ = nullptr;
}

void AmdtpReceiveRing::onComplete(CallbackState* state, NuDCLRef) {
    if (state && state->owner) state->owner->completed_ = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

AmdtpReceiveRing AmdtpReceiveRing::create(FireWireDevice& device,
                                          std::size_t packetCount,
                                          std::size_t packetCapacity) {
    AmdtpReceiveRing ring;
    if (!device.nativeHandle() || packetCount == 0 || packetCapacity < 8)
        return ring;

    ring.device_ = &device;
    ring.packetCount_ = packetCount;
    ring.packetCapacity_ = packetCapacity;
    ring.metadataBytes_ = sizeof(RawSlot) * packetCount;
    ring.payloadBytes_ = packetCapacity * packetCount;

    ring.rawSlots_ = static_cast<RawSlot*>(mmap(nullptr, ring.metadataBytes_,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
    if (ring.rawSlots_ == MAP_FAILED) {
        ring.rawSlots_ = nullptr;
        ring.reset();
        return ring;
    }

    ring.payloadBase_ = static_cast<std::uint8_t*>(mmap(nullptr, ring.payloadBytes_,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
    if (ring.payloadBase_ == MAP_FAILED) {
        ring.payloadBase_ = nullptr;
        ring.reset();
        return ring;
    }

    std::memset(ring.rawSlots_, 0, ring.metadataBytes_);
    std::memset(ring.payloadBase_, 0, ring.payloadBytes_);
    ring.slots_ = new (std::nothrow) PacketSlot[packetCount];
    ring.callbackState_ = new (std::nothrow) CallbackState{&ring};
    if (!ring.slots_ || !ring.callbackState_) {
        ring.reset();
        return ring;
    }

    for (std::size_t i = 0; i < packetCount; ++i) {
        ring.slots_[i].payload = ring.payloadBase_ + i * packetCapacity;
        ring.slots_[i].capacity = packetCapacity;
    }

    auto native = device.nativeHandle();
    ring.pool_ = (*native)->CreateNuDCLPool(native, static_cast<UInt32>(packetCount),
        CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    if (!ring.pool_) {
        ring.reset();
        return ring;
    }

    NuDCLRef last = nullptr;
    for (std::size_t i = 0; i < packetCount; ++i) {
        IOVirtualRange ranges[2] = {
            {reinterpret_cast<IOVirtualAddress>(&ring.rawSlots_[i].isoHeader),
             sizeof(ring.rawSlots_[i].isoHeader)},
            {reinterpret_cast<IOVirtualAddress>(ring.payloadBase_ + i * packetCapacity),
             static_cast<IOByteCount>(packetCapacity)}
        };
        NuDCLReceivePacketRef dcl = (*ring.pool_)->AllocateReceivePacket(
            ring.pool_, nullptr, 4, 2, ranges);
        if (!dcl) {
            ring.reset();
            return ring;
        }
        last = reinterpret_cast<NuDCLRef>(dcl);
        (*ring.pool_)->SetDCLStatusPtr(last, &ring.rawSlots_[i].status);
        (*ring.pool_)->SetDCLTimeStampPtr(last, &ring.rawSlots_[i].timestamp);
    }

    (*ring.pool_)->SetDCLRefcon(last, ring.callbackState_);
    (*ring.pool_)->SetDCLCallback(last,
        reinterpret_cast<NuDCLCallback>(onComplete));

    DCLCommand* program = (*ring.pool_)->GetProgram(ring.pool_);
    if (!program) {
        ring.reset();
        return ring;
    }

    IOVirtualRange mappedRanges[2] = {
        {reinterpret_cast<IOVirtualAddress>(ring.rawSlots_),
         static_cast<IOByteCount>(ring.metadataBytes_)},
        {reinterpret_cast<IOVirtualAddress>(ring.payloadBase_),
         static_cast<IOByteCount>(ring.payloadBytes_)}
    };

    ring.localPort_ = (*native)->CreateLocalIsochPort(native, false, program,
        kFWDCLSyBitsEvent, 0, 0, nullptr, 0, mappedRanges, 2,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!ring.localPort_) {
        ring.reset();
        return ring;
    }

    return ring;
}

void AmdtpReceiveRing::syncSlot(std::size_t index) const {
    if (!slots_ || !rawSlots_ || index >= packetCount_) return;
    slots_[index].isoHeader = rawSlots_[index].isoHeader;
    slots_[index].status = rawSlots_[index].status;
    slots_[index].timestamp = rawSlots_[index].timestamp;
}

const AmdtpReceiveRing::PacketSlot& AmdtpReceiveRing::slot(std::size_t index) const {
    static const PacketSlot empty{};
    if (!slots_ || index >= packetCount_) return empty;
    syncSlot(index);
    return slots_[index];
}

std::size_t AmdtpReceiveRing::touchedCount() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < packetCount_; ++i) {
        syncSlot(i);
        if (slots_[i].touched()) ++count;
    }
    return count;
}

void AmdtpReceiveRing::reset() {
    if (localPort_) { (*localPort_)->Release(localPort_); localPort_ = nullptr; }
    if (pool_) { (*pool_)->Release(pool_); pool_ = nullptr; }
    delete callbackState_; callbackState_ = nullptr;
    delete[] slots_; slots_ = nullptr;
    if (payloadBase_) { munmap(payloadBase_, payloadBytes_); payloadBase_ = nullptr; }
    if (rawSlots_) { munmap(rawSlots_, metadataBytes_); rawSlots_ = nullptr; }
    device_ = nullptr;
    packetCount_ = 0;
    packetCapacity_ = 0;
    metadataBytes_ = 0;
    payloadBytes_ = 0;
    completed_ = false;
}

} // namespace macfw
