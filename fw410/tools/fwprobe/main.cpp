#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

static std::string cfString(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    char buffer[1024] = {};
    if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                           sizeof(buffer), kCFStringEncodingUTF8)) {
        return buffer;
    }
    return {};
}

static void printProperty(io_registry_entry_t service, const char *key) {
    CFStringRef keyString = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!keyString) return;

    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);
    if (!value) return;

    const std::string text = cfString(value);
    if (!text.empty()) {
        std::cout << "    " << key << ": " << text << '\n';
    } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long number = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(value),
                         kCFNumberLongLongType, &number);
        std::cout << "    " << key << ": 0x" << std::hex << number
                  << std::dec << '\n';
    }
    CFRelease(value);
}

static void indent(unsigned depth) {
    while (depth--) std::cout << "    ";
}

static const char *configTypeName(IOConfigKeyType type) {
    switch (type) {
        case kConfigImmediateKeyType: return "immediate";
        case kConfigOffsetKeyType: return "offset";
        case kConfigLeafKeyType: return "leaf";
        case kConfigDirectoryKeyType: return "directory";
        case kInvalidConfigROMEntryType: return "invalid";
        default: return "unknown";
    }
}

static void printDataPreview(CFDataRef data, unsigned depth) {
    if (!data) return;
    const CFIndex length = CFDataGetLength(data);
    const UInt8 *bytes = CFDataGetBytePtr(data);
    indent(depth);
    std::cout << "data: " << length << " byte" << (length == 1 ? "" : "s");
    if (bytes && length > 0) {
        const CFIndex preview = std::min<CFIndex>(length, 32);
        std::cout << " [";
        for (CFIndex i = 0; i < preview; ++i) {
            if (i) std::cout << ' ';
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(bytes[i]);
        }
        std::cout << std::dec << std::setfill(' ');
        if (length > preview) std::cout << " ...";
        std::cout << ']';
    }
    std::cout << '\n';
}

static void dumpConfigDirectory(IOFireWireLibConfigDirectoryRef directory,
                                unsigned depth = 0) {
    constexpr unsigned kMaxDepth = 16;
    if (!directory) {
        indent(depth);
        std::cout << "<null configuration directory>\n";
        return;
    }
    if (depth > kMaxDepth) {
        indent(depth);
        std::cout << "<maximum directory depth reached>\n";
        return;
    }

    int directoryType = 0;
    IOReturn kr = (*directory)->GetType(directory, &directoryType);
    if (kr == kIOReturnSuccess) {
        indent(depth);
        std::cout << "directory type: 0x" << std::hex << directoryType
                  << std::dec << '\n';
    }

    int entryCount = 0;
    kr = (*directory)->GetNumEntries(directory, &entryCount);
    if (kr != kIOReturnSuccess) {
        indent(depth);
        std::cout << "GetNumEntries failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return;
    }

    indent(depth);
    std::cout << "entries: " << entryCount << '\n';

    for (int index = 0; index < entryCount; ++index) {
        int key = -1;
        IOConfigKeyType type = kInvalidConfigROMEntryType;
        UInt32 rawEntry = 0;
        const IOReturn keyResult = (*directory)->GetIndexKey(directory, index, &key);
        const IOReturn typeResult = (*directory)->GetIndexType(directory, index, &type);
        const IOReturn rawResult = (*directory)->GetIndexEntry(directory, index, &rawEntry);

        indent(depth);
        std::cout << '[' << index << ']';
        if (keyResult == kIOReturnSuccess)
            std::cout << " key=0x" << std::hex << key << std::dec;
        else
            std::cout << " key=<error 0x" << std::hex << keyResult << std::dec << '>';

        if (typeResult == kIOReturnSuccess)
            std::cout << " type=" << configTypeName(type)
                      << '(' << static_cast<unsigned>(type) << ')';
        else
            std::cout << " type=<error 0x" << std::hex << typeResult << std::dec << '>';

        if (rawResult == kIOReturnSuccess)
            std::cout << " raw=0x" << std::hex << rawEntry << std::dec;
        std::cout << '\n';

        if (typeResult != kIOReturnSuccess) continue;

        switch (type) {
            case kConfigImmediateKeyType: {
                UInt32 value = 0;
                kr = (*directory)->GetIndexValue_UInt32(directory, index, &value);
                indent(depth + 1);
                if (kr == kIOReturnSuccess)
                    std::cout << "value: 0x" << std::hex << value << std::dec << '\n';
                else
                    std::cout << "value: <error 0x" << std::hex << kr << std::dec << ">\n";
                break;
            }
            case kConfigOffsetKeyType: {
                FWAddress address = {};
                kr = (*directory)->GetIndexOffset_FWAddress(directory, index, &address);
                indent(depth + 1);
                if (kr == kIOReturnSuccess) {
                    std::cout << "address: node=0x" << std::hex << address.nodeID
                              << " hi=0x" << address.addressHi
                              << " lo=0x" << address.addressLo << std::dec << '\n';
                } else {
                    UInt32 offset = 0;
                    const IOReturn offsetResult =
                        (*directory)->GetIndexOffset_UInt32(directory, index, &offset);
                    if (offsetResult == kIOReturnSuccess)
                        std::cout << "offset: 0x" << std::hex << offset << std::dec << '\n';
                    else
                        std::cout << "offset: <error 0x" << std::hex << kr << std::dec << ">\n";
                }
                break;
            }
            case kConfigLeafKeyType: {
                CFStringRef stringValue = nullptr;
                const IOReturn stringResult =
                    (*directory)->GetIndexValue_String(directory, index, &stringValue);
                if (stringResult == kIOReturnSuccess && stringValue) {
                    indent(depth + 1);
                    std::cout << "string: " << cfString(stringValue) << '\n';
                    CFRelease(stringValue);
                }
                CFDataRef dataValue = nullptr;
                const IOReturn dataResult =
                    (*directory)->GetIndexValue_Data(directory, index, &dataValue);
                if (dataResult == kIOReturnSuccess && dataValue) {
                    printDataPreview(dataValue, depth + 1);
                    CFRelease(dataValue);
                }
                break;
            }
            case kConfigDirectoryKeyType: {
                IOFireWireLibConfigDirectoryRef child = nullptr;
                kr = (*directory)->GetIndexValue_ConfigDirectory(
                    directory, index, &child,
                    CFUUIDGetUUIDBytes(kIOFireWireConfigDirectoryInterfaceID));
                if (kr == kIOReturnSuccess && child) {
                    indent(depth + 1);
                    std::cout << "subdirectory:\n";
                    dumpConfigDirectory(child, depth + 2);
                    (*child)->Release(child);
                } else {
                    indent(depth + 1);
                    std::cout << "subdirectory: <error 0x" << std::hex << kr
                              << std::dec << ">\n";
                }
                break;
            }
            default:
                break;
        }
    }
}

static void dumpConfigROM(IOFireWireLibDeviceRef device) {
    std::cout << "    Configuration ROM:\n";
    IOFireWireLibConfigDirectoryRef directory = (*device)->GetConfigDirectory(
        device, CFUUIDGetUUIDBytes(kIOFireWireConfigDirectoryInterfaceID));
    if (!directory) {
        std::cout << "        GetConfigDirectory failed\n";
        return;
    }
    dumpConfigDirectory(directory, 2);
    (*directory)->Release(directory);
}

static IOReturn readAbsolute(IOFireWireLibDeviceRef device,
                             UInt32 generation,
                             UInt16 remoteNodeID,
                             UInt16 addressHi,
                             UInt32 addressLo,
                             void *buffer,
                             UInt32 &size) {
    FWAddress address = {};
    address.nodeID = remoteNodeID;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device)->Read(device, 0, &address, buffer, &size, true, generation);
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

static UInt32 be32(const UInt8 *p) {
    return (static_cast<UInt32>(p[0]) << 24) |
           (static_cast<UInt32>(p[1]) << 16) |
           (static_cast<UInt32>(p[2]) << 8) |
           static_cast<UInt32>(p[3]);
}

static UInt64 be64(const UInt8 *p) {
    return (static_cast<UInt64>(be32(p)) << 32) | be32(p + 4);
}

static void printHex32Field(const char *label, const UInt8 *p) {
    std::cout << "        " << std::left << std::setw(22) << label
              << "0x" << std::right << std::hex << std::setw(8)
              << std::setfill('0') << be32(p)
              << std::dec << std::setfill(' ') << std::left << '\n';
}

static void printAsciiField(const char *label, const UInt8 *p, size_t len) {
    std::cout << "        " << std::left << std::setw(22) << label
              << asciiField(p, len) << '\n';
}

static bool openForTransaction(IOFireWireLibDeviceRef device, const char *prefix) {
    const IOReturn kr = (*device)->Open(device);
    if (kr != kIOReturnSuccess) {
        std::cout << prefix << "open:    failed (0x" << std::hex << kr
                  << std::dec << ")\n";
        return false;
    }
    std::cout << prefix << "open:    success\n";
    return true;
}

static void readInfoDate(IOFireWireLibDeviceRef device,
                         UInt32 generation,
                         UInt16 remoteNodeID) {
    constexpr UInt16 kAddressHi = 0xffff;
    constexpr UInt32 kAddressLo = 0xc8020020;
    constexpr UInt32 kLength = 8;
    UInt8 buffer[kLength] = {};
    UInt32 size = kLength;

    std::cout << "    BeBoB software build date probe:\n"
              << "        address: 0xffffc8020020\n"
              << "        node:    0x" << std::hex << remoteNodeID << std::dec << '\n'
              << "        length:  8 bytes\n";

    if (!openForTransaction(device, "        ")) return;
    const IOReturn kr = readAbsolute(device, generation, remoteNodeID,
                                     kAddressHi, kAddressLo, buffer, size);
    (*device)->Close(device);

    if (kr != kIOReturnSuccess) {
        std::cout << "        read:    failed (0x" << std::hex << kr
                  << std::dec << ")\n";
        return;
    }

    std::cout << "        read:    success (" << size << " bytes)\n"
              << "        raw:     ";
    for (UInt32 i = 0; i < size; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(buffer[i]);
    }
    std::cout << std::dec << std::setfill(' ') << "\n"
              << "        ASCII:   " << asciiField(buffer, size) << '\n';
}

static void readInfoRegisters(IOFireWireLibDeviceRef device,
                              UInt32 generation,
                              UInt16 remoteNodeID) {
    constexpr UInt16 kAddressHi = 0xffff;
    constexpr UInt32 kAddressLo = 0xc8020000;
    constexpr UInt32 kLength = 0x68; // FFADO info_register_t, offsets 0x00..0x64.
    UInt8 buffer[kLength] = {};
    UInt32 size = kLength;

    std::cout << "    BeBoB information registers:\n"
              << "        address: 0xffffc8020000\n"
              << "        node:    0x" << std::hex << remoteNodeID << std::dec << '\n'
              << "        length:  " << kLength << " bytes\n";

    if (!openForTransaction(device, "        ")) return;
    const IOReturn kr = readAbsolute(device, generation, remoteNodeID,
                                     kAddressHi, kAddressLo, buffer, size);
    (*device)->Close(device);

    if (kr != kIOReturnSuccess) {
        std::cout << "        read:    failed (0x" << std::hex << kr
                  << std::dec << ")\n";
        return;
    }
    if (size < kLength) {
        std::cout << "        read:    short read (" << size << "/" << kLength
                  << " bytes)\n";
        return;
    }

    std::cout << "        read:    success (" << size << " bytes)\n";
    printAsciiField("manufacturer:", buffer + 0x00, 8);
    printHex32Field("protocol version:", buffer + 0x08);
    printHex32Field("bootloader version:", buffer + 0x0c);
    std::cout << "        " << std::left << std::setw(22) << "GUID:"
              << "0x" << std::right << std::hex << std::setw(16)
              << std::setfill('0') << be64(buffer + 0x10)
              << std::dec << std::setfill(' ') << std::left << '\n';
    printHex32Field("hardware model ID:", buffer + 0x18);
    printHex32Field("hardware revision:", buffer + 0x1c);
    printAsciiField("software date:", buffer + 0x20, 8);
    printAsciiField("software time:", buffer + 0x28, 8);
    printHex32Field("software ID:", buffer + 0x30);
    printHex32Field("software version:", buffer + 0x34);
    printHex32Field("base address:", buffer + 0x38);
    printHex32Field("max image length:", buffer + 0x3c);
    printAsciiField("bootloader date:", buffer + 0x40, 8);
    printAsciiField("bootloader time:", buffer + 0x48, 8);
    printAsciiField("debugger date:", buffer + 0x50, 8);
    printAsciiField("debugger time:", buffer + 0x58, 8);
    printHex32Field("debugger ID:", buffer + 0x60);
    printHex32Field("debugger version:", buffer + 0x64);
}

static void printUsage(const char *program) {
    std::cout << "Usage: " << program << " [--rom] [--info-date] [--info]\n"
              << "  --rom        Read and recursively display the remote configuration ROM\n"
              << "  --info-date  Read the 8-byte software date at 0xffffc8020020\n"
              << "  --info       Read the documented 104-byte BeBoB information block\n";
}

int main(int argc, char **argv) {
    bool dumpROM = false;
    bool infoDate = false;
    bool info = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rom") dumpROM = true;
        else if (arg == "--info-date") infoDate = true;
        else if (arg == "--info") info = true;
        else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            printUsage(argv[0]);
            return 2;
        }
    }

    std::cout << "macfw fwprobe — M1 user-space FireWire probe\n\n";
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) {
        std::cerr << "Unable to create IOFireWireUnit matching dictionary\n";
        return 1;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    matching, &iterator);
    if (kr != KERN_SUCCESS) {
        std::cerr << "IOServiceGetMatchingServices failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return 1;
    }

    unsigned count = 0;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        ++count;
        std::cout << "FireWire unit #" << count << '\n';
        printProperty(service, "FireWire Product Name");
        printProperty(service, "Vendor_ID");
        printProperty(service, "GUID");
        printProperty(service, "Unit_Spec_ID");
        printProperty(service, "Unit_SW_Version");
        printProperty(service, "Unit_Type");

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            std::cout << "    IOFireWireLib interface: unavailable (0x"
                      << std::hex << kr << std::dec << ")\n\n";
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID *>(&device));
        if (hr != 0 || !device) {
            std::cout << "    IOFireWireDeviceInterface: unavailable (0x"
                      << std::hex << static_cast<unsigned long>(hr)
                      << std::dec << ")\n\n";
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        std::cout << "    IOFireWireDeviceInterface: acquired\n"
                  << "    interface revision: " << (*device)->revision << '\n'
                  << "    interface version:  " << (*device)->version << '\n';

        UInt32 generation = 0;
        const IOReturn generationResult = (*device)->GetBusGeneration(device,
                                                                      &generation);
        if (generationResult == kIOReturnSuccess)
            std::cout << "    bus generation:     " << generation << '\n';
        else
            std::cout << "    bus generation:     unavailable (0x" << std::hex
                      << generationResult << std::dec << ")\n";

        UInt16 nodeID = 0;
        IOReturn nodeResult = kIOReturnError;
        if (generationResult == kIOReturnSuccess)
            nodeResult = (*device)->GetRemoteNodeID(device, generation, &nodeID);
        if (nodeResult == kIOReturnSuccess)
            std::cout << "    remote node ID:     0x" << std::hex << nodeID
                      << std::dec << '\n';
        else
            std::cout << "    remote node ID:     unavailable (0x" << std::hex
                      << nodeResult << std::dec << ")\n";

        if (dumpROM) dumpConfigROM(device);

        const bool canTransact = generationResult == kIOReturnSuccess &&
                                 nodeResult == kIOReturnSuccess;
        if (infoDate) {
            if (canTransact) readInfoDate(device, generation, nodeID);
            else std::cout << "    BeBoB software build date probe skipped: valid generation/node ID unavailable\n";
        }
        if (info) {
            if (canTransact) readInfoRegisters(device, generation, nodeID);
            else std::cout << "    BeBoB information-register probe skipped: valid generation/node ID unavailable\n";
        }

        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        std::cout << '\n';
    }

    IOObjectRelease(iterator);
    if (count == 0) {
        std::cout << "No IOFireWireUnit services were found.\n"
                  << "Check that the FireWire controller is visible to macOS and that the FW410 is connected.\n";
    }
    return 0;
}
