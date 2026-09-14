#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
constexpr int kExitSuccess = 0;
constexpr int kExitGeneralFailure = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNoBootloader = 10;
constexpr int kExitGuardRefused = 11;
constexpr int kExitCandidateUnavailable = 12;
}

static UInt32 le32(const UInt8 *p) {
    return static_cast<UInt32>(p[0]) |
           (static_cast<UInt32>(p[1]) << 8) |
           (static_cast<UInt32>(p[2]) << 16) |
           (static_cast<UInt32>(p[3]) << 24);
}

static std::string asciiField(const UInt8 *p, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = p[i];
        if (c == 0) break;
        out.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
    }
    return out;
}

static bool allAsciiDigits(const UInt8 *p, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
    }
    return true;
}

static IOReturn readAbsolute(IOFireWireLibDeviceRef device,
                             UInt32 generation,
                             UInt16 nodeID,
                             UInt16 addressHi,
                             UInt32 addressLo,
                             void *buffer,
                             UInt32 &size) {
    FWAddress address = {};
    address.nodeID = nodeID;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device)->Read(device, 0, &address, buffer, &size, true, generation);
}

static IOReturn writeAbsolute(IOFireWireLibDeviceRef device,
                              UInt32 generation,
                              UInt16 nodeID,
                              UInt16 addressHi,
                              UInt32 addressLo,
                              const void *buffer,
                              UInt32 &size) {
    FWAddress address = {};
    address.nodeID = nodeID;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device)->Write(device, 0, &address, buffer, &size, true, generation);
}

static bool getNumberProperty(io_registry_entry_t service,
                              const char *key,
                              UInt64 &valueOut) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return false;

    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return false;

    bool ok = false;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long n = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value),
                             kCFNumberLongLongType, &n)) {
            valueOut = static_cast<UInt64>(n);
            ok = true;
        }
    }
    CFRelease(value);
    return ok;
}

static bool preflight(IOFireWireLibDeviceRef device,
                      UInt32 generation,
                      UInt16 nodeID) {
    constexpr UInt16 kInfoHi = 0xffff;
    constexpr UInt32 kInfoLo = 0xc8020000;
    constexpr UInt32 kInfoSize = 0x68;

    UInt8 info[kInfoSize] = {};
    UInt32 size = kInfoSize;
    IOReturn kr = readAbsolute(device, generation, nodeID,
                               kInfoHi, kInfoLo, info, size);
    if (kr != kIOReturnSuccess || size < kInfoSize) {
        std::cout << "preflight read: FAIL (0x" << std::hex << kr
                  << std::dec << ", " << size << " bytes)\n";
        return false;
    }

    const UInt32 protocol = le32(info + 0x08);
    const UInt32 bootloaderVersion = le32(info + 0x0c);
    const UInt32 softwareId = le32(info + 0x30);
    const std::string softwareDate = asciiField(info + 0x20, 8);

    const bool protocolOk = protocol == 1;
    const bool bootloaderOk = bootloaderVersion != 0;
    const bool dateOk = softwareDate.size() == 8 &&
                        allAsciiDigits(info + 0x20, 8) &&
                        softwareDate >= "20070401";
    const bool appOk = softwareId == 0x00010046;

    std::cout << "preflight:\n"
              << "  protocol v1:       " << (protocolOk ? "PASS" : "FAIL")
              << " (0x" << std::hex << protocol << std::dec << ")\n"
              << "  bootloader active: " << (bootloaderOk ? "PASS" : "FAIL")
              << " (0x" << std::hex << bootloaderVersion << std::dec << ")\n"
              << "  software date:      " << (dateOk ? "PASS" : "FAIL")
              << " (" << softwareDate << ")\n"
              << "  FW410 app ID:       " << (appOk ? "PASS" : "FAIL")
              << " (0x" << std::hex << softwareId << std::dec << ")\n";

    return protocolOk && bootloaderOk && dateOk && appOk;
}

static void printUsage(const char *program) {
    std::cout << "Usage: " << program << " [--execute]\n\n"
              << "Without --execute, performs only the FW410 boot-cue preflight.\n"
              << "With --execute, sends the single documented 12-byte Linux snd-bebob\n"
              << "boot-from-flash cue to 0xffffc8021000 after all preflight checks pass.\n";
}

int main(int argc, char **argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return kExitSuccess;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            printUsage(argv[0]);
            return kExitUsage;
        }
    }

    std::cout << "macfw fwboot — guarded M-Audio FW410 boot-from-flash tool\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) {
        std::cerr << "Unable to create IOFireWireUnit matching dictionary\n";
        return kExitGeneralFailure;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    matching, &iterator);
    if (kr != KERN_SUCCESS) {
        std::cerr << "IOServiceGetMatchingServices failed: 0x"
                  << std::hex << kr << std::dec << '\n';
        return kExitGeneralFailure;
    }

    int matched = 0;
    int refused = 0;
    int unavailable = 0;
    bool preflightPassed = false;
    bool cueIssued = false;

    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        UInt64 specifier = 0, swVersion = 0;
        const bool haveSpecifier = getNumberProperty(service, "Unit_Spec_ID", specifier);
        const bool haveSwVersion = getNumberProperty(service, "Unit_SW_Version", swVersion);

        // Refuse unrelated FireWire units. These are the exact bootloader
        // unit-directory values observed on the tested FW410 and documented
        // by the Linux/FFADO correlation in this repository.
        if (!haveSpecifier || !haveSwVersion ||
            specifier != 0x0000a02d || swVersion != 0x00014001) {
            IOObjectRelease(service);
            continue;
        }

        ++matched;
        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(service,
                                               kIOFireWireLibTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            ++unavailable;
            std::cerr << "IOFireWireLib interface unavailable: 0x"
                      << std::hex << kr << std::dec << '\n';
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID *>(&device));
        if (hr != 0 || !device) {
            ++unavailable;
            std::cerr << "IOFireWireDeviceInterface unavailable: 0x"
                      << std::hex << static_cast<unsigned long>(hr)
                      << std::dec << '\n';
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        UInt32 generation = 0;
        UInt16 nodeID = 0;
        const IOReturn genKr = (*device)->GetBusGeneration(device, &generation);
        const IOReturn nodeKr = genKr == kIOReturnSuccess
            ? (*device)->GetRemoteNodeID(device, generation, &nodeID)
            : genKr;
        if (genKr != kIOReturnSuccess || nodeKr != kIOReturnSuccess) {
            ++unavailable;
            std::cerr << "Unable to obtain stable generation/node ID\n";
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        std::cout << "FW410 bootloader candidate:\n"
                  << "  generation: " << generation << '\n'
                  << "  node:       0x" << std::hex << nodeID << std::dec << '\n';

        const IOReturn openKr = (*device)->Open(device);
        if (openKr != kIOReturnSuccess) {
            ++unavailable;
            std::cerr << "Open failed: 0x" << std::hex << openKr
                      << std::dec << '\n';
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        const bool ok = preflight(device, generation, nodeID);
        if (!ok) {
            ++refused;
            std::cout << "status: REFUSED - preflight did not match the known FW410 loader\n";
            (*device)->Close(device);
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        preflightPassed = true;
        if (!execute) {
            std::cout << "status: PASS - no write performed\n"
                      << "to execute: ./fwboot --execute\n";
            (*device)->Close(device);
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        // Linux snd-bebob: MAUDIO_BOOTLOADER_CUE1/2/3 converted with
        // cpu_to_le32() and written as one 12-byte block to BEBOB_ADDR_REG_REQ.
        const UInt8 cue[12] = {
            0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x11, 0x01,
            0x00, 0x00, 0x00, 0x00
        };
        UInt32 cueSize = sizeof(cue);

        std::cout << "executing one-shot boot cue:\n"
                  << "  address:    0xffffc8021000\n"
                  << "  bytes:      01 00 00 00 00 00 11 01 00 00 00 00\n";

        const IOReturn writeKr = writeAbsolute(device, generation, nodeID,
                                                0xffff, 0xc8021000,
                                                cue, cueSize);
        cueIssued = true;

        // A successful cue is expected to trigger a FireWire bus reset, so the
        // device/generation represented by this interface may become stale now.
        (*device)->Close(device);

        if (writeKr == kIOReturnSuccess) {
            std::cout << "write result: success (" << cueSize << " bytes)\n";
        } else {
            std::cout << "write result: 0x" << std::hex << writeKr << std::dec
                      << " (the boot cue was issued; an immediate reset can invalidate the transaction result)\n";
        }
        std::cout << "next: wait for FireWire re-enumeration, then run ../fwprobe/fwprobe --rom\n";

        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        break;
    }

    IOObjectRelease(iterator);

    if (cueIssued) return kExitSuccess;
    if (!execute && preflightPassed) return kExitSuccess;
    if (matched == 0) {
        std::cout << "No FW410 bootloader unit (specifier 0xa02d / SW 0x14001) found.\n";
        return kExitNoBootloader;
    }
    if (refused != 0) return kExitGuardRefused;
    if (unavailable != 0) return kExitCandidateUnavailable;
    return kExitGeneralFailure;
}
