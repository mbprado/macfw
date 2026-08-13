#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kOmprLo = 0xf0000900;
constexpr UInt32 kOpcr0Lo = 0xf0000904;
constexpr UInt32 kImprLo = 0xf0000980;
constexpr UInt32 kIpcr0Lo = 0xf0000984;

static bool isOperationalFw410(io_registry_entry_t service) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, CFSTR("FireWire Product Name"), kCFAllocatorDefault, 0);
    if (!value) return false;
    const bool ok = CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare(static_cast<CFStringRef>(value), CFSTR("FW 410"), 0) == kCFCompareEqualTo;
    CFRelease(value);
    return ok;
}

static uint32_t be32(const std::array<UInt8,4>& b) {
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) |
           static_cast<uint32_t>(b[3]);
}

static bool readReg(IOFireWireLibDeviceRef device, UInt32 generation,
                    UInt16 nodeID, UInt32 lo, uint32_t& value) {
    FWAddress address = {};
    address.nodeID = nodeID;
    address.addressHi = kAddressHi;
    address.addressLo = lo;
    std::array<UInt8,4> bytes{};
    UInt32 size = bytes.size();
    const IOReturn kr = (*device)->Read(device, 0, &address, bytes.data(), &size,
                                       true, generation);
    if (kr != kIOReturnSuccess || size != 4) {
        std::cout << "        read failed at 0xffff" << std::hex << lo
                  << " (0x" << kr << ")" << std::dec << '\n';
        return false;
    }
    value = be32(bytes);
    return true;
}

static void printMpr(const char* name, uint32_t v) {
    const unsigned speed = (v >> 30) & 0x3;
    const unsigned xspeed = (v >> 5) & 0x3;
    const unsigned plugs = v & 0x1f;
    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << v << std::dec << std::setfill(' ') << '\n';
    std::cout << "        base speed code: " << speed << '\n';
    std::cout << "        extended speed:  " << xspeed << '\n';
    std::cout << "        plug count:      " << plugs << '\n';
}

static void printPcr(const char* name, uint32_t v, bool output) {
    const bool online = (v & 0x80000000u) != 0;
    const bool broadcast = (v & 0x40000000u) != 0;
    const unsigned p2p = (v >> 24) & 0x3f;
    const unsigned channel = (v >> 16) & 0x3f;

    std::cout << "    " << name << ": 0x" << std::hex << std::setw(8)
              << std::setfill('0') << v << std::dec << std::setfill(' ') << '\n';
    std::cout << "        online:          " << (online ? "yes" : "no") << '\n';
    std::cout << "        broadcast conn:  " << (broadcast ? "yes" : "no") << '\n';
    std::cout << "        p2p connections: " << p2p << '\n';
    std::cout << "        channel:         " << channel << '\n';
    std::cout << "        in use:          " << ((broadcast || p2p) ? "yes" : "no") << '\n';

    if (output) {
        const unsigned xspeed = (v >> 22) & 0x3;
        const unsigned speed = (v >> 14) & 0x3;
        const unsigned overhead = (v >> 10) & 0xf;
        std::cout << "        output speed:    " << speed << '\n';
        std::cout << "        output x-speed:  " << xspeed << '\n';
        std::cout << "        overhead ID:     " << overhead << '\n';
    }
}

static void runProbe(IOFireWireLibDeviceRef device, UInt32 generation, UInt16 nodeID) {
    const IOReturn openResult = (*device)->Open(device);
    if (openResult != kIOReturnSuccess) {
        std::cout << "    open failed: 0x" << std::hex << openResult << std::dec << '\n';
        return;
    }

    uint32_t ompr=0, opcr0=0, impr=0, ipcr0=0;
    const bool ok = readReg(device, generation, nodeID, kOmprLo, ompr) &&
                    readReg(device, generation, nodeID, kOpcr0Lo, opcr0) &&
                    readReg(device, generation, nodeID, kImprLo, impr) &&
                    readReg(device, generation, nodeID, kIpcr0Lo, ipcr0);

    if (ok) {
        std::cout << "    CMP registers (device-relative):\n";
        printMpr("oMPR", ompr);
        printPcr("oPCR[0] (device OUTPUT / host capture)", opcr0, true);
        printMpr("iMPR", impr);
        printPcr("iPCR[0] (device INPUT / host playback)", ipcr0, false);
    }

    (*device)->Close(device);
}

} // namespace

int main() {
    std::cout << "macfw cmpprobe — read-only IEC 61883 CMP register probe\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return 1;

    bool found = false;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (!isOperationalFw410(service)) {
            IOObjectRelease(service);
            continue;
        }
        found = true;

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        if (hr == 0 && device) {
            UInt32 generation = 0;
            UInt16 nodeID = 0;
            if ((*device)->GetBusGeneration(device, &generation) == kIOReturnSuccess &&
                (*device)->GetRemoteNodeID(device, generation, &nodeID) == kIOReturnSuccess) {
                std::cout << "FW410 operational unit:\n";
                std::cout << "    generation: " << generation << '\n';
                std::cout << "    remote node: 0x" << std::hex << nodeID << std::dec << '\n';
                runProbe(device, generation, nodeID);
            }
            (*device)->Release(device);
        }

        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);
    if (!found) {
        std::cout << "No operational FW 410 unit found.\n";
        return 2;
    }
    return 0;
}
