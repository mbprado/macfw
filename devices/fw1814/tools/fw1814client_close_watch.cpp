#include "macfw/firewire_device.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

constexpr const char* kProduct = "FW 1814";

bool readGeneration(IOFireWireLibDeviceRef native, UInt32& generation) {
    generation = 0;
    return native &&
           (*native)->GetBusGeneration(native, &generation) == kIOReturnSuccess;
}

bool reportGeneration(IOFireWireLibDeviceRef observer,
                      UInt32 baseline,
                      const char* label,
                      bool& changed,
                      std::string& firstChange) {
    UInt32 observed = 0;
    if (!readGeneration(observer, observed)) {
        std::cout << "    " << label << ": generation read FAILED\n";
        return false;
    }

    std::cout << "    " << label << ": generation " << observed;
    if (observed != baseline) {
        std::cout << "  <-- CHANGED from " << baseline;
        if (!changed) {
            changed = true;
            firstChange = label;
        }
    } else {
        std::cout << "  (stable)";
    }
    std::cout << '\n';
    return true;
}

bool run(bool execute) {
    // Keep a separate observer interface alive while the test client is
    // explicitly closed and destroyed. The observer is never opened and
    // issues no FireWire writes.
    auto observer = macfw::FireWireDevice::findByProductName(kProduct);
    if (!observer) {
        std::cout << "No operational FW 1814 observer handle found.\n";
        return false;
    }

    auto observerNative = observer.nativeHandle();
    UInt32 baseline = 0;
    if (!readGeneration(observerNative, baseline)) {
        std::cout << "observer generation read failed\n";
        return false;
    }

    auto client = macfw::FireWireDevice::findByProductName(kProduct);
    if (!client) {
        std::cout << "No operational FW 1814 test-client handle found.\n";
        return false;
    }

    std::cout << "FW1814 plain-client close/release generation watch:\n"
              << "    baseline generation: " << baseline << '\n'
              << "    observer node: 0x" << std::hex << observer.nodeID()
              << std::dec << '\n'
              << "    FireWire writes: NONE\n"
              << "    ISO/CMP/AV-C activity: NONE\n";

    if (!execute) {
        std::cout << "status: PASS - dry run only\n";
        return true;
    }

    const IOReturn openKr = client.open();
    std::cout << "test client Open: 0x" << std::hex << openKr
              << std::dec << '\n';
    if (openKr != kIOReturnSuccess)
        return false;

    bool changed = false;
    std::string firstChange;

    reportGeneration(observerNative, baseline,
                     "after test client Open", changed, firstChange);

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.10, false);
    reportGeneration(observerNative, baseline,
                     "100 ms with client open", changed, firstChange);

    std::cout << "closing test client...\n";
    client.close();
    reportGeneration(observerNative, baseline,
                     "immediately after Close", changed, firstChange);

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.10, false);
    reportGeneration(observerNative, baseline,
                     "100 ms after Close", changed, firstChange);

    std::cout << "destroying test IOFireWireDeviceInterface/plugin/service...\n";
    client = macfw::FireWireDevice{};
    reportGeneration(observerNative, baseline,
                     "immediately after client interface release",
                     changed, firstChange);

    for (unsigned i = 1; i <= 50; ++i) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        UInt32 observed = 0;
        if (!readGeneration(observerNative, observed)) {
            std::cout << "    observer generation read FAILED at +"
                      << (i * 20) << " ms after release\n";
            break;
        }
        if (observed != baseline) {
            std::cout << "    generation changed by +" << (i * 20)
                      << " ms after release: " << baseline
                      << " -> " << observed << '\n';
            if (!changed) {
                changed = true;
                firstChange = "after client interface release";
            }
            break;
        }
    }

    if (!changed) {
        reportGeneration(observerNative, baseline,
                         "1000 ms after client interface release",
                         changed, firstChange);
    }

    if (changed) {
        std::cout << "result: BUS GENERATION CHANGED first at: "
                  << firstChange << '\n';
    } else {
        std::cout << "result: generation remained stable through plain client close/release\n";
    }

    std::cout << "status: PASS - client lifecycle diagnostic completed\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute")
            execute = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: " << argv[0] << " [--execute]\n";
            return 0;
        } else {
            std::cout << "usage: " << argv[0] << " [--execute]\n";
            return 64;
        }
    }

    std::cout << "macfw fw1814client-close-watch — plain FireWire client lifecycle diagnostic\n\n";
    return run(execute) ? 0 : 1;
}
