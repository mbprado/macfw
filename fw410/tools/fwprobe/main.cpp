#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>

static std::string cfString(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        return {};
    }

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
    if (!keyString) {
        return;
    }

    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, keyString, kCFAllocatorDefault, 0);
    CFRelease(keyString);

    if (!value) {
        return;
    }

    std::string text = cfString(value);
    if (!text.empty()) {
        std::cout << "    " << key << ": " << text << '\n';
    } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long number = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number);
        std::cout << "    " << key << ": 0x" << std::hex << number << std::dec << '\n';
    }

    CFRelease(value);
}

static void indent(unsigned depth) {
    for (unsigned i = 0; i < depth; ++i) {
        std::cout << "    ";
    }
}

static const char *configTypeName(IOConfigKeyType type) {
    switch (type) {
        case kConfigImmediateKeyType:
            return "immediate";
        case kConfigOffsetKeyType:
            return "offset";
        case kConfigLeafKeyType:
            return "leaf";
        case kConfigDirectoryKeyType:
            return "directory";
        case kInvalidConfigROMEntryType:
            return "invalid";
        default:
            return "unknown";
    }
}

static void printDataPreview(CFDataRef data, unsigned depth) {
    if (!data) {
        return;
    }

    const CFIndex length = CFDataGetLength(data);
    const UInt8 *bytes = CFDataGetBytePtr(data);

    indent(depth);
    std::cout << "data: " << length << " byte" << (length == 1 ? "" : "s");

    if (bytes && length > 0) {
        constexpr CFIndex kPreviewBytes = 32;
        const CFIndex preview = std::min(length, kPreviewBytes);
        std::cout << " [";
        for (CFIndex i = 0; i < preview; ++i) {
            if (i != 0) {
                std::cout << ' ';
            }
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(bytes[i]);
        }
        std::cout << std::dec << std::setfill(' ');
        if (length > preview) {
            std::cout << " ...";
        }
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
        std::cout << "directory type: 0x" << std::hex << directoryType << std::dec << '\n';
    }

    int entryCount = 0;
    kr = (*directory)->GetNumEntries(directory, &entryCount);
    if (kr != kIOReturnSuccess) {
        indent(depth);
        std::cout << "GetNumEntries failed: 0x" << std::hex << kr << std::dec << '\n';
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

        if (keyResult == kIOReturnSuccess) {
            std::cout << " key=0x" << std::hex << key << std::dec;
        } else {
            std::cout << " key=<error 0x" << std::hex << keyResult << std::dec << '>';
        }

        if (typeResult == kIOReturnSuccess) {
            std::cout << " type=" << configTypeName(type)
                      << '(' << static_cast<unsigned>(type) << ')';
        } else {
            std::cout << " type=<error 0x" << std::hex << typeResult << std::dec << '>';
        }

        if (rawResult == kIOReturnSuccess) {
            std::cout << " raw=0x" << std::hex << rawEntry << std::dec;
        }

        std::cout << '\n';

        if (typeResult != kIOReturnSuccess) {
            continue;
        }

        switch (type) {
            case kConfigImmediateKeyType: {
                UInt32 value = 0;
                kr = (*directory)->GetIndexValue_UInt32(directory, index, &value);
                indent(depth + 1);
                if (kr == kIOReturnSuccess) {
                    std::cout << "value: 0x" << std::hex << value << std::dec << '\n';
                } else {
                    std::cout << "value: <error 0x" << std::hex << kr << std::dec << ">\n";
                }
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
                    if (offsetResult == kIOReturnSuccess) {
                        std::cout << "offset: 0x" << std::hex << offset << std::dec << '\n';
                    } else {
                        std::cout << "offset: <error 0x" << std::hex << kr << std::dec << ">\n";
                    }
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

                if ((stringResult != kIOReturnSuccess || !stringValue) &&
                    (dataResult != kIOReturnSuccess || !dataValue)) {
                    indent(depth + 1);
                    std::cout << "leaf value unavailable\n";
                }
                break;
            }

            case kConfigDirectoryKeyType: {
                IOFireWireLibConfigDirectoryRef child = nullptr;
                kr = (*directory)->GetIndexValue_ConfigDirectory(
                    directory,
                    index,
                    &child,
                    CFUUIDGetUUIDBytes(kIOFireWireConfigDirectoryInterfaceID));

                if (kr == kIOReturnSuccess && child) {
                    indent(depth + 1);
                    std::cout << "subdirectory:\n";
                    dumpConfigDirectory(child, depth + 2);
                    (*child)->Release(child);
                } else {
                    indent(depth + 1);
                    std::cout << "subdirectory: <error 0x"
                              << std::hex << kr << std::dec << ">\n";
                }
                break;
            }

            case kInvalidConfigROMEntryType:
            default:
                break;
        }
    }
}

static void dumpConfigROM(IOFireWireLibDeviceRef device) {
    std::cout << "    Configuration ROM:\n";

    IOFireWireLibConfigDirectoryRef directory = (*device)->GetConfigDirectory(
        device,
        CFUUIDGetUUIDBytes(kIOFireWireConfigDirectoryInterfaceID));

    if (!directory) {
        std::cout << "        GetConfigDirectory failed\n";
        return;
    }

    dumpConfigDirectory(directory, 2);
    (*directory)->Release(directory);
}

static void printUsage(const char *program) {
    std::cout << "Usage: " << program << " [--rom]\n"
              << "  --rom   Read and recursively display the remote configuration ROM\n";
}

int main(int argc, char **argv) {
    bool dumpROM = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rom") {
            dumpROM = true;
        } else if (arg == "--help" || arg == "-h") {
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
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS) {
        std::cerr << "IOServiceGetMatchingServices failed: 0x"
                  << std::hex << kr << std::dec << '\n';
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
        kr = IOCreatePlugInInterfaceForService(
            service,
            kIOFireWireLibTypeID,
            kIOCFPlugInInterfaceID,
            &plugin,
            &score);

        if (kr != KERN_SUCCESS || !plugin) {
            std::cout << "    IOFireWireLib interface: unavailable (0x"
                      << std::hex << kr << std::dec << ")\n\n";
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        HRESULT hr = (*plugin)->QueryInterface(
            plugin,
            CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID *>(&device));

        if (hr != 0 || !device) {
            std::cout << "    IOFireWireDeviceInterface: unavailable (0x"
                      << std::hex << static_cast<unsigned long>(hr) << std::dec << ")\n\n";
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        std::cout << "    IOFireWireDeviceInterface: acquired\n";
        std::cout << "    interface revision: " << (*device)->revision << '\n';
        std::cout << "    interface version:  " << (*device)->version << '\n';

        UInt32 generation = 0;
        kr = (*device)->GetBusGeneration(device, &generation);
        if (kr == KERN_SUCCESS) {
            std::cout << "    bus generation:     " << generation << '\n';
        } else {
            std::cout << "    bus generation:     unavailable (0x"
                      << std::hex << kr << std::dec << ")\n";
        }

        UInt16 nodeID = 0;
        kr = (*device)->GetRemoteNodeID(device, generation, &nodeID);
        if (kr == KERN_SUCCESS) {
            std::cout << "    remote node ID:     0x"
                      << std::hex << nodeID << std::dec << '\n';
        } else {
            std::cout << "    remote node ID:     unavailable (0x"
                      << std::hex << kr << std::dec << ")\n";
        }

        if (dumpROM) {
            dumpConfigROM(device);
        }

        (*device)->Release(device);
        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
        std::cout << '\n';
    }

    IOObjectRelease(iterator);

    if (count == 0) {
        std::cout << "No IOFireWireUnit services were found.\n";
        std::cout << "Check that the FireWire controller is visible to macOS "
                     "and that the FW410 is connected.\n";
    }

    return 0;
}
