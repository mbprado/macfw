#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

namespace {

constexpr const char* kProductName = "FW 1814 Bootloader";
constexpr std::uint64_t kVendorId = 0x00000d6c;
constexpr std::uint64_t kUnitSpecId = 0x0000a02d;
constexpr std::uint64_t kUnitSwVersion = 0x00014001;

constexpr UInt16 kInfoHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kInfoSize = 0x68;

constexpr UInt16 kCueHi = 0xffff;
constexpr UInt32 kCueLo = 0xc8021000;

std::string stringProperty(io_registry_entry_t service, const char* key) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return {};
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return {};

    std::string result;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[256] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                               sizeof(buffer), kCFStringEncodingUTF8))
            result = buffer;
    }
    CFRelease(value);
    return result;
}

std::optional<std::uint64_t> numberProperty(io_registry_entry_t service,
                                             const char* key) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return std::nullopt;
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return std::nullopt;

    std::optional<std::uint64_t> result;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long number = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value),
                             kCFNumberLongLongType, &number))
            result = static_cast<std::uint64_t>(number);
    }
    CFRelease(value);
    return result;
}

UInt32 le32(const UInt8* p) {
    return static_cast<UInt32>(p[0]) |
           (static_cast<UInt32>(p[1]) << 8) |
           (static_cast<UInt32>(p[2]) << 16) |
           (static_cast<UInt32>(p[3]) << 24);
}

std::string asciiField(const UInt8* p, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len && p[i] != 0; ++i)
        out.push_back(static_cast<char>(p[i]));
    return out;
}

IOReturn readAbsolute(IOFireWireLibDeviceRef device,
                      UInt32 generation,
                      UInt16 nodeID,
                      UInt16 addressHi,
                      UInt32 addressLo,
                      void* buffer,
                      UInt32& size) {
    FWAddress address = {};
    address.nodeID = nodeID;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device)->Read(device, 0, &address, buffer, &size, true, generation);
}

IOReturn writeAbsolute(IOFireWireLibDeviceRef device,
                       UInt32 generation,
                       UInt16 nodeID,
                       UInt16 addressHi,
                       UInt32 addressLo,
                       const void* buffer,
                       UInt32& size) {
    FWAddress address = {};
    address.nodeID = nodeID;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device)->Write(device, 0, &address, buffer, &size, true, generation);
}

bool check(const char* label, bool ok) {
    std::cout << "  " << std::left << std::setw(24) << label
              << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

bool preflight(IOFireWireLibDeviceRef device,
               UInt32 generation,
               UInt16 nodeID) {
    UInt8 info[kInfoSize] = {};
    UInt32 size = kInfoSize;
    const IOReturn kr = readAbsolute(device, generation, nodeID,
                                     kInfoHi, kInfoLo, info, size);
    if (kr != kIOReturnSuccess || size != kInfoSize) {
        std::cout << "BeBoB info read: FAIL (0x" << std::hex << kr
                  << std::dec << ", " << size << " bytes)\n";
        return false;
    }

    const std::string manufacturer = asciiField(info + 0x00, 8);
    const UInt32 protocol = le32(info + 0x08);
    const UInt32 bootloaderVersion = le32(info + 0x0c);
    const UInt32 hardwareModel = le32(info + 0x18);
    const UInt32 hardwareRevision = le32(info + 0x1c);
    const std::string softwareDate = asciiField(info + 0x20, 8);
    const UInt32 softwareId = le32(info + 0x30);
    const UInt32 baseAddress = le32(info + 0x38);
    const UInt32 maxImageLength = le32(info + 0x3c);
    const std::string bootloaderDate = asciiField(info + 0x40, 8);

    std::cout << "FW1814 bootloader preflight:\n";
    bool ok = true;
    ok &= check("manufacturer bridgeCo", manufacturer == "bridgeCo");
    ok &= check("protocol version 1", protocol == 1);
    ok &= check("bootloader 0x2805", bootloaderVersion == 0x00002805);
    ok &= check("hardware model 0x83", hardwareModel == 0x00000083);
    ok &= check("hardware revision 1", hardwareRevision == 0x00000001);
    ok &= check("software date 20070713", softwareDate == "20070713");
    ok &= check("software ID 0", softwareId == 0x00000000);
    ok &= check("image base 0x20080000", baseAddress == 0x20080000);
    ok &= check("image max 0x00180000", maxImageLength == 0x00180000);
    ok &= check("bootloader date 20040330", bootloaderDate == "20040330");
    return ok;
}

void usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [--execute]\n"
              << "  default     perform FW1814-specific bootloader preflight only\n"
              << "  --execute   after all guards pass, send the documented M-Audio boot-from-flash cue\n";
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    if (argc == 2 && std::string(argv[1]) == "--execute") execute = true;
    else if (argc == 2 && (std::string(argv[1]) == "--help" ||
                           std::string(argv[1]) == "-h")) {
        usage(argv[0]);
        return 0;
    } else if (argc != 1) {
        usage(argv[0]);
        return 2;
    }

    std::cout << "macfw fwboot1814 — guarded FW1814 boot-from-flash tool\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) !=
        KERN_SUCCESS)
        return 1;

    int result = 10;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        const auto vendor = numberProperty(service, "Vendor_ID");
        const auto spec = numberProperty(service, "Unit_Spec_ID");
        const auto sw = numberProperty(service, "Unit_SW_Version");
        const std::string product = stringProperty(service, "FireWire Product Name");

        const bool registryMatch =
            product == kProductName && vendor && *vendor == kVendorId &&
            spec && *spec == kUnitSpecId && sw && *sw == kUnitSwVersion;
        if (!registryMatch) {
            IOObjectRelease(service);
            continue;
        }

        std::cout << "registry identity: PASS\n"
                  << "  product: " << product << '\n'
                  << "  vendor:  0x" << std::hex << *vendor << '\n'
                  << "  spec:    0x" << *spec << '\n'
                  << "  sw:      0x" << *sw << std::dec << "\n\n";

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            result = 11;
            break;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        if (hr != 0 || !device) {
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            result = 11;
            break;
        }

        UInt32 generation = 0;
        UInt16 nodeID = 0;
        IOReturn io = (*device)->GetBusGeneration(device, &generation);
        if (io == kIOReturnSuccess)
            io = (*device)->GetRemoteNodeID(device, generation, &nodeID);
        if (io != kIOReturnSuccess) {
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            result = 11;
            break;
        }

        io = (*device)->Open(device);
        if (io != kIOReturnSuccess) {
            std::cout << "open: FAIL (0x" << std::hex << io << std::dec << ")\n";
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            result = 11;
            break;
        }

        if (!preflight(device, generation, nodeID)) {
            std::cout << "\nstatus: REFUSED - no write performed\n";
            (*device)->Close(device);
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            result = 12;
            break;
        }

        if (!execute) {
            std::cout << "\nstatus: PASS - no write performed\n"
                      << "next: run ./fwboot1814 --execute to boot operational firmware\n";
            (*device)->Close(device);
            (*device)->Release(device);
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            result = 0;
            break;
        }

        const UInt8 cue[12] = {
            0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x11, 0x01,
            0x00, 0x00, 0x00, 0x00,
        };
        UInt32 cueSize = sizeof(cue);

        std::cout << "\nexecuting documented M-Audio boot-from-flash cue:\n"
                  << "  address: 0xffffc8021000\n"
                  << "  bytes:   01 00 00 00 00 00 11 01 00 00 00 00\n";

        const IOReturn writeKr = writeAbsolute(device, generation, nodeID,
                                                kCueHi, kCueLo,
                                                cue, cueSize);
        (*device)->Close(device);

        std::cout << "write result: 0x" << std::hex << writeKr << std::dec
                  << " (" << cueSize << " bytes)\n"
                  << "A FireWire bus reset/re-enumeration is expected now.\n";

        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        result = 0;
        break;
    }

    IOObjectRelease(iterator);
    if (result == 10)
        std::cout << "No confirmed FW1814 bootloader personality found.\n";
    return result;
}
