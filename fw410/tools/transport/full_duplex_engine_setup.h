#pragma once

#include "macfw/amdtp_receive_ring.h"
#include "macfw/firewire_device.h"
#include "macfw/pcm_ring_buffer.h"
#include "../capture_shared.h"
#include "full_duplex_shared.h"

#include <IOKit/firewire/IOFireWireLib.h>

#include <atomic>
#include <cstdint>
#include <iostream>

namespace macfw::transport::duplex {

struct FullDuplexEngineSetup {
    SharedPlaybackReader input;
    macfw::transport::CaptureSharedWriter captureShared;
    macfw::FireWireDevice device;
    IOFireWireLibDeviceRef native = nullptr;
    UInt32 initialCycle = 0;
    UInt32 firstCycle = 0;

    bool prepare(UInt32 sampleRate, UInt32 cycleLead,
                 const char* playbackRingError,
                 const char* rateError) {
        if (!input.open()) {
            std::cerr << playbackRingError << '\n';
            return false;
        }
        if (input.ring()->sampleRate.load(std::memory_order_acquire) != sampleRate) {
            std::cerr << rateError << '\n';
            return false;
        }
        input.discardBacklog();

        if (!captureShared.open(sampleRate)) {
            std::cerr << "capture shared ring setup failed\n";
            return false;
        }

        device = macfw::FireWireDevice::findByProductName("FW 410");
        if (!device) {
            std::cerr << "No operational FW 410 unit found.\n";
            return false;
        }
        if (device.open() != kIOReturnSuccess)
            return false;

        native = device.nativeHandle();
        UInt32 cycleTime = 0;
        if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess)
            return false;

        initialCycle = cycleCount(cycleTime);
        firstCycle = (initialCycle + cycleLead) % kCyclesPerSecond;
        return true;
    }
};

} // namespace macfw::transport::duplex
