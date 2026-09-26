#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* kProductName = "FW 1814";
constexpr std::uint64_t kVendorId = 0x00000d6c;
constexpr std::uint64_t kUnitSpecId = 0x0000a02d;
constexpr std::uint64_t kUnitSwVersion = 0x00014001;

constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kInfoSize = 0x68;
constexpr UInt32 kRequestLo = 0xc8021000;
constexpr UInt32 kOpcr0Lo = 0xf0000904;
constexpr UInt32 kIpcr0Lo = 0xf0000984;

constexpr std::uint32_t kPcrOnline = 0x80000000u;
constexpr std::uint32_t kPcrBroadcast = 0x40000000u;
constexpr std::uint32_t kPcrP2PMask = 0x3f000000u;

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

std::uint32_t le32(const UInt8* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t be32(const UInt8* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::string asciiField(const UInt8* p, std::size_t len) {
    std::string result;
    for (std::size_t i = 0; i < len && p[i] != 0; ++i)
        result.push_back(static_cast<char>(p[i]));
    return result;
}

IOReturn readAbsolute(IOFireWireLibDeviceRef device, UInt32 generation,
                      UInt16 node, UInt32 addressLo, void* buffer,
                      UInt32& size) {
    FWAddress address = {};
    address.nodeID = node;
    address.addressHi = kAddressHi;
    address.addressLo = addressLo;
    return (*device)->Read(device, 0, &address, buffer, &size, true, generation);
}

IOReturn writeAbsolute(IOFireWireLibDeviceRef device, UInt32 generation,
                       UInt16 node, UInt32 addressLo, const void* buffer,
                       UInt32& size) {
    FWAddress address = {};
    address.nodeID = node;
    address.addressHi = kAddressHi;
    address.addressLo = addressLo;
    return (*device)->Write(device, 0, &address, buffer, &size, true, generation);
}

bool check(const char* label, bool ok) {
    std::cout << "  " << std::left << std::setw(30) << label
              << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

bool validateOperationalFingerprint(IOFireWireLibDeviceRef device,
                                    UInt32 generation, UInt16 node) {
    std::array<UInt8, kInfoSize> info{};
    UInt32 size = static_cast<UInt32>(info.size());
    if (readAbsolute(device, generation, node, kInfoLo, info.data(), size) !=
            kIOReturnSuccess ||
        size != info.size()) {
        std::cout << "BeBoB information block: FAIL\n";
        return false;
    }

    std::cout << "FW1814 operational fingerprint:\n";
    bool ok = true;
    ok &= check("manufacturer bridgeCo",
                asciiField(info.data() + 0x00, 8) == "bridgeCo");
    ok &= check("protocol version 1", le32(info.data() + 0x08) == 1);
    ok &= check("application personality", le32(info.data() + 0x0c) == 0);
    ok &= check("hardware model 0x83", le32(info.data() + 0x18) == 0x83);
    ok &= check("hardware revision 1", le32(info.data() + 0x1c) == 1);
    ok &= check("software date 20070713",
                asciiField(info.data() + 0x20, 8) == "20070713");
    ok &= check("software ID 0", le32(info.data() + 0x30) == 0);
    ok &= check("image base 0x20080000",
                le32(info.data() + 0x38) == 0x20080000);
    ok &= check("image max 0x00180000",
                le32(info.data() + 0x3c) == 0x00180000);
    return ok;
}

bool readPcr(IOFireWireLibDeviceRef device, UInt32 generation, UInt16 node,
             UInt32 addressLo, std::uint32_t& value) {
    std::array<UInt8, 4> bytes{};
    UInt32 size = static_cast<UInt32>(bytes.size());
    if (readAbsolute(device, generation, node, addressLo, bytes.data(), size) !=
            kIOReturnSuccess ||
        size != bytes.size())
        return false;
    value = be32(bytes.data());
    return true;
}

bool pcrDisconnected(std::uint32_t value) {
    return (value & kPcrOnline) != 0 &&
           (value & kPcrBroadcast) == 0 &&
           (value & kPcrP2PMask) == 0;
}

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0
        << " [--execute --experimental-firmware-reset]\n"
        << "  default  read-only identity, fingerprint and CMP preflight\n"
        << "  execute  send FFADO's documented BeBoB reset-to-bootloader command\n\n"
        << "The FW1814 transport service must be stopped. After a successful\n"
        << "transition, run fwboot1814 --execute to restart the flash-resident\n"
        << "application. No firmware image or persistent configuration is written.\n";
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    bool experimentalReset = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--experimental-firmware-reset")
            experimentalReset = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    if (execute != experimentalReset) {
        std::cerr << "execution requires both --execute and "
                     "--experimental-firmware-reset\n";
        return 64;
    }
    if (execute && access("/tmp/macfw-fw1814-control.sock", F_OK) == 0) {
        std::cerr << "status: REFUSED - stop the FW1814 transport service "
                     "before this diagnostic\n";
        return 4;
    }

    std::cout << "macfw fw1814firmwarereset — guarded BeBoB application reset probe\n\n";

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
        const std::string product =
            stringProperty(service, "FireWire Product Name");

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
        const kern_return_t pluginKr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        IOObjectRelease(service);
        if (pluginKr != KERN_SUCCESS || !plugin) {
            result = 11;
            break;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        IODestroyPlugInInterface(plugin);
        if (hr != 0 || !device) {
            result = 11;
            break;
        }

        UInt32 generation = 0;
        UInt16 node = 0;
        IOReturn io = (*device)->GetBusGeneration(device, &generation);
        if (io == kIOReturnSuccess)
            io = (*device)->GetRemoteNodeID(device, generation, &node);
        if (io != kIOReturnSuccess || (*device)->Open(device) != kIOReturnSuccess) {
            (*device)->Release(device);
            result = 11;
            break;
        }

        std::cout << "generation: " << generation << '\n'
                  << "remote node: 0x" << std::hex << node << std::dec << '\n';

        if (!validateOperationalFingerprint(device, generation, node)) {
            std::cout << "\nstatus: REFUSED - no write performed\n";
            (*device)->Close(device);
            (*device)->Release(device);
            result = 12;
            break;
        }

        std::uint32_t opcr0 = 0;
        std::uint32_t ipcr0 = 0;
        const bool pcrRead =
            readPcr(device, generation, node, kOpcr0Lo, opcr0) &&
            readPcr(device, generation, node, kIpcr0Lo, ipcr0);
        std::cout << "CMP preflight:\n"
                  << "  oPCR[0]: 0x" << std::hex << opcr0 << std::dec << '\n'
                  << "  iPCR[0]: 0x" << std::hex << ipcr0 << std::dec << '\n';
        const bool cmpIdle = pcrRead && pcrDisconnected(opcr0) &&
                             pcrDisconnected(ipcr0);
        check("both PCRs online/disconnected", cmpIdle);
        if (!cmpIdle) {
            std::cout << "\nstatus: REFUSED - no write performed\n";
            (*device)->Close(device);
            (*device)->Release(device);
            result = 13;
            break;
        }

        if (!execute) {
            std::cout << "\nstatus: PASS - dry run only; no write performed\n"
                      << "to execute: " << argv[0]
                      << " --execute --experimental-firmware-reset\n";
            (*device)->Close(device);
            (*device)->Release(device);
            result = 0;
            break;
        }

        // FFADO BeBoB bootloader protocol v1:
        // command 0x02 = Reset, operand 0x01 = start bootloader.
        // This is deliberately separate from the boot-from-flash Go command
        // used by fwboot1814 and never writes firmware or configuration.
        const UInt8 resetCommand[12] = {
            0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x02, 0x01,
            0x01, 0x00, 0x00, 0x00,
        };
        UInt32 commandSize = sizeof(resetCommand);
        std::cout << "\nexecuting documented BeBoB reset-to-bootloader command:\n"
                  << "  address: 0xffffc8021000\n"
                  << "  bytes:   01 00 00 00 00 00 02 01 01 00 00 00\n";
        const IOReturn writeKr = writeAbsolute(
            device, generation, node, kRequestLo, resetCommand, commandSize);
        (*device)->Close(device);
        (*device)->Release(device);

        std::cout << "write result: 0x" << std::hex << writeKr << std::dec
                  << " (" << commandSize << " bytes)\n";
        if (writeKr != kIOReturnSuccess || commandSize != sizeof(resetCommand)) {
            std::cout << "status: FAIL - reset request was not accepted\n";
            result = 14;
            break;
        }

        std::cout << "status: RESET REQUEST ACCEPTED\n"
                  << "expected next personality: FW 1814 Bootloader\n"
                  << "next: wait for re-enumeration, then run "
                     "./fwboot1814 --execute\n";
        result = 0;
        break;
    }

    IOObjectRelease(iterator);
    if (result == 10)
        std::cout << "No confirmed FW1814 operational personality found.\n";
    return result;
}
