#pragma once

#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cstdint>
#include <iostream>

namespace macfw::fw1814::transport {

class DuplexLifecycle {
public:
    DuplexLifecycle() = default;
    ~DuplexLifecycle() { stop(); }
    DuplexLifecycle(const DuplexLifecycle&) = delete;
    DuplexLifecycle& operator=(const DuplexLifecycle&) = delete;

    bool prepare(macfw::FireWireDevice& device,
                 macfw::AmdtpReceiveRing& rx,
                 IOFireWireLibLocalIsochPortRef playbackPort,
                 UInt32 captureMaxPacket,
                 UInt32 playbackMaxPacket) {
        device_ = &device;
        native_ = device.nativeHandle();
        initialGeneration_ = device.generation();
        if (!native_) return false;

        if (macfw::cmp::readOpcr0(device, opcr0_) != kIOReturnSuccess ||
            macfw::cmp::readIpcr0(device, ipcr0_) != kIOReturnSuccess)
            return false;
        if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0_)) ||
            !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0_))) {
            std::cerr << "FW1814 PCR0 offline or already connected\n";
            return false;
        }

        capture_ = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::DeviceToHost,
            captureMaxPacket);
        playback_ = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::HostToDevice,
            playbackMaxPacket);
        if (!capture_ || !playback_) return false;

        const IOReturn listener = (*capture_.nativeChannel())->AddListener(
            capture_.nativeChannel(),
            reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
        if (listener != kIOReturnSuccess) return false;
        if (playback_.bindHostToDeviceTalkerFirst(playbackPort) != kIOReturnSuccess)
            return false;
        return true;
    }

    bool addCallbackDispatcher(CFRunLoopRef runLoop = nullptr) {
        if (!native_) return false;
        CFRunLoopRef loop = runLoop ? runLoop : CFRunLoopGetCurrent();
        if ((*native_)->AddCallbackDispatcherToRunLoop(native_, loop) !=
            kIOReturnSuccess)
            return false;
        callbackDispatcher_ = true;
        callbackRunLoop_ = loop;
        return true;
    }

    bool startIsoch(CFRunLoopRef runLoop = nullptr) {
        if (!device_ || !native_ || !capture_ || !playback_) return false;
        CFRunLoopRef loop = runLoop ? runLoop : CFRunLoopGetCurrent();
        if ((*native_)->AddIsochCallbackDispatcherToRunLoop(native_, loop) !=
            kIOReturnSuccess)
            return false;
        isochDispatcher_ = true;
        isochRunLoop_ = loop;

        if (!(*native_)->TurnOnNotification(native_)) return false;
        notifications_ = true;

        if (playback_.allocate() != kIOReturnSuccess ||
            capture_.allocate() != kIOReturnSuccess)
            return false;

        if (macfw::cmp::connectOpcr0(*device_, opcr0_, capture_.channel(),
                                     capture_.speed()) != kIOReturnSuccess)
            return false;
        opConnected_ = true;

        if (macfw::cmp::connectIpcr0(*device_, ipcr0_, playback_.channel()) !=
            kIOReturnSuccess)
            return false;
        ipConnected_ = true;

        // Hardware-proven FW1814 start order: host->device playback first,
        // device->host capture second.
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

    bool generationStillValid() const {
        if (!native_ || initialGeneration_ == 0) return false;
        UInt32 current = 0;
        return (*native_)->GetBusGeneration(native_, &current) == kIOReturnSuccess &&
               current == initialGeneration_;
    }

    bool stopIsochAndRestoreCmp() {
        if (!device_) return true;

        // Stop local DMA regardless of bus generation. Never issue a remote
        // write until the current generation is known to match the one used
        // when the CMP connections were established.
        if (captureStarted_ && capture_.nativeChannel())
            (*capture_.nativeChannel())->Stop(capture_.nativeChannel());
        if (playbackStarted_ && playback_.nativeChannel())
            (*playback_.nativeChannel())->Stop(playback_.nativeChannel());
        captureStarted_ = false;
        playbackStarted_ = false;

        bool restoreOk = true;
        if (ipConnected_ || opConnected_) {
            if (generationStillValid()) {
                if (ipConnected_) {
                    const IOReturn kr = macfw::cmp::restore(
                        *device_, macfw::cmp::kIpcr0AddressLo, ipcr0_);
                    restoreOk = restoreOk && kr == kIOReturnSuccess;
                }
                if (opConnected_) {
                    const IOReturn kr = macfw::cmp::restore(
                        *device_, macfw::cmp::kOpcr0AddressLo, opcr0_);
                    restoreOk = restoreOk && kr == kIOReturnSuccess;
                }
            } else {
                std::cerr << "FW1814 bus generation changed/unavailable; "
                             "skipping stale-generation PCR restore writes\n";
                restoreOk = false;
            }
        }
        ipConnected_ = false;
        opConnected_ = false;

        playback_.release();
        capture_.release();
        return restoreOk;
    }

    void removeDispatchers() {
        if (!native_) return;
        if (notifications_) (*native_)->TurnOffNotification(native_);
        if (isochDispatcher_)
            (*native_)->RemoveIsochCallbackDispatcherFromRunLoop(native_);
        if (callbackDispatcher_)
            (*native_)->RemoveCallbackDispatcherFromRunLoop(native_);
        notifications_ = false;
        isochDispatcher_ = false;
        callbackDispatcher_ = false;
        callbackRunLoop_ = nullptr;
        isochRunLoop_ = nullptr;
    }

    void stop() {
        if (!device_) return;
        stopIsochAndRestoreCmp();
        removeDispatchers();

        // Clear every object/pointer that could reach the FireWire client so a
        // destructor or explicit second stop() is a strict no-op.
        playback_ = macfw::IsochAllocation{};
        capture_ = macfw::IsochAllocation{};
        native_ = nullptr;
        device_ = nullptr;
        initialGeneration_ = 0;
        opcr0_ = 0;
        ipcr0_ = 0;
    }

    IOFireWireLibDeviceRef native() const { return native_; }
    UInt32 initialGeneration() const { return initialGeneration_; }
    UInt32 playbackChannel() const { return playback_.channel(); }
    UInt32 captureChannel() const { return capture_.channel(); }

private:
    macfw::FireWireDevice* device_ = nullptr;
    IOFireWireLibDeviceRef native_ = nullptr;
    macfw::IsochAllocation capture_;
    macfw::IsochAllocation playback_;
    std::uint32_t opcr0_ = 0;
    std::uint32_t ipcr0_ = 0;
    UInt32 initialGeneration_ = 0;
    CFRunLoopRef callbackRunLoop_ = nullptr;
    CFRunLoopRef isochRunLoop_ = nullptr;
    bool callbackDispatcher_ = false;
    bool isochDispatcher_ = false;
    bool notifications_ = false;
    bool opConnected_ = false;
    bool ipConnected_ = false;
    bool playbackStarted_ = false;
    bool captureStarted_ = false;
};

} // namespace macfw::fw1814::transport
