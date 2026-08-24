#pragma once

#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cstdint>
#include <iostream>

namespace macfw::transport::duplex {

// Shared device/CMP/ISO lifecycle used by both native full-duplex engines.
// Rate-specific packet generation and the FW410 44.1 kHz AV/C reassertion
// deliberately remain outside this class.
class FireWireDuplexLifecycle {
public:
    FireWireDuplexLifecycle() = default;
    ~FireWireDuplexLifecycle() { stop(); }

    FireWireDuplexLifecycle(const FireWireDuplexLifecycle&) = delete;
    FireWireDuplexLifecycle& operator=(const FireWireDuplexLifecycle&) = delete;

    bool prepare(macfw::FireWireDevice& device,
                 macfw::AmdtpReceiveRing& rx,
                 IOFireWireLibLocalIsochPortRef playbackPort,
                 UInt32 captureMaxPacket,
                 UInt32 playbackMaxPacket) {
        device_ = &device;
        native_ = device.nativeHandle();
        if (!native_) return false;

        if (macfw::cmp::readOpcr0(device, opcr0_) != kIOReturnSuccess ||
            macfw::cmp::readIpcr0(device, ipcr0_) != kIOReturnSuccess)
            return false;
        if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0_)) ||
            !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0_))) {
            std::cerr << "PCR0 offline or already connected\n";
            return false;
        }

        capture_ = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::DeviceToHost,
            captureMaxPacket);
        playback_ = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::HostToDevice,
            playbackMaxPacket);
        if (!capture_ || !playback_) return false;

        (*capture_.nativeChannel())->AddListener(
            capture_.nativeChannel(),
            reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
        if (playback_.bindHostToDeviceTalkerFirst(playbackPort) != kIOReturnSuccess)
            return false;

        return true;
    }

    bool addCallbackDispatcher() {
        if (!native_) return false;
        if ((*native_)->AddCallbackDispatcherToRunLoop(
                native_, CFRunLoopGetCurrent()) != kIOReturnSuccess)
            return false;
        callbackDispatcher_ = true;
        return true;
    }

    bool startIsoch() {
        if (!device_ || !native_ || !capture_ || !playback_) return false;

        if ((*native_)->AddIsochCallbackDispatcherToRunLoop(
                native_, CFRunLoopGetCurrent()) != kIOReturnSuccess)
            return false;
        isochDispatcher_ = true;

        if (!(*native_)->TurnOnNotification(native_)) return false;
        notifications_ = true;

        if (capture_.allocate() != kIOReturnSuccess ||
            playback_.allocate() != kIOReturnSuccess)
            return false;

        if (macfw::cmp::connectOpcr0(*device_, opcr0_, capture_.channel(),
                                     capture_.speed()) != kIOReturnSuccess)
            return false;
        opConnected_ = true;

        if (macfw::cmp::connectIpcr0(*device_, ipcr0_, playback_.channel()) !=
            kIOReturnSuccess)
            return false;
        ipConnected_ = true;

        // Preserve the proven start order: playback/talker first, capture second.
        if ((*playback_.nativeChannel())->Start(playback_.nativeChannel()) !=
            kIOReturnSuccess)
            return false;
        playbackStarted_ = true;

        if ((*capture_.nativeChannel())->Start(capture_.nativeChannel()) !=
            kIOReturnSuccess)
            return false;
        captureStarted_ = true;

        return true;
    }

    void stop() {
        if (!device_) return;

        // Preserve the proven stop/restore order from both bridge engines.
        if (playbackStarted_)
            (*playback_.nativeChannel())->Stop(playback_.nativeChannel());
        if (captureStarted_)
            (*capture_.nativeChannel())->Stop(capture_.nativeChannel());

        if (ipConnected_)
            macfw::cmp::restore(*device_, macfw::cmp::kIpcr0AddressLo, ipcr0_);
        if (opConnected_)
            macfw::cmp::restore(*device_, macfw::cmp::kOpcr0AddressLo, opcr0_);

        playback_.release();
        capture_.release();

        if (notifications_) (*native_)->TurnOffNotification(native_);
        if (isochDispatcher_)
            (*native_)->RemoveIsochCallbackDispatcherFromRunLoop(native_);
        if (callbackDispatcher_)
            (*native_)->RemoveCallbackDispatcherFromRunLoop(native_);

        captureStarted_ = false;
        playbackStarted_ = false;
        ipConnected_ = false;
        opConnected_ = false;
        notifications_ = false;
        isochDispatcher_ = false;
        callbackDispatcher_ = false;
        native_ = nullptr;
        device_ = nullptr;
    }

    IOFireWireLibDeviceRef native() const { return native_; }

private:
    macfw::FireWireDevice* device_ = nullptr;
    IOFireWireLibDeviceRef native_ = nullptr;
    macfw::IsochAllocation capture_;
    macfw::IsochAllocation playback_;
    std::uint32_t opcr0_ = 0;
    std::uint32_t ipcr0_ = 0;
    bool callbackDispatcher_ = false;
    bool isochDispatcher_ = false;
    bool notifications_ = false;
    bool opConnected_ = false;
    bool ipConnected_ = false;
    bool playbackStarted_ = false;
    bool captureStarted_ = false;
};

} // namespace macfw::transport::duplex
