#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <iostream>
#include <string>

namespace {

std::string stringProperty(io_registry_entry_t service, const char* key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                               kCFStringEncodingUTF8);
    if (!k) return {};
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, k,
                                                       kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!value) return {};

    std::string result;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[1024] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                               sizeof(buffer), kCFStringEncodingUTF8))
            result = buffer;
    }
    CFRelease(value);
    return result;
}

void usage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " --product <FireWire Product Name> [--execute]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string product;
    bool execute = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--product" && i + 1 < argc)
            product = argv[++i];
        else if (arg == "--execute")
            execute = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    if (product.empty()) {
        usage(argv[0]);
        return 64;
    }

    std::cout << "macfw firewirebusreset — guarded product-scoped FireWire bus reset\n"
              << "target product: " << product << "\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) return 1;

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching,
                                     &iterator) != KERN_SUCCESS)
        return 1;

    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        const std::string foundProduct =
            stringProperty(service, "FireWire Product Name");
        if (foundProduct != product) {
            IOObjectRelease(service);
            continue;
        }

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        const kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        IOObjectRelease(service);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(iterator);
            return 2;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        IODestroyPlugInInterface(plugin);
        if (hr != 0 || !device) {
            IOObjectRelease(iterator);
            return 2;
        }

        UInt32 generation = 0;
        UInt16 node = 0;
        const IOReturn genKr = (*device)->GetBusGeneration(device, &generation);
        const IOReturn nodeKr = genKr == kIOReturnSuccess
            ? (*device)->GetRemoteNodeID(device, generation, &node)
            : genKr;

        std::cout << "matched unit:\n"
                  << "    product: " << foundProduct << '\n';
        if (genKr == kIOReturnSuccess && nodeKr == kIOReturnSuccess) {
            std::cout << "    generation: " << generation << '\n'
                      << "    remote node: 0x" << std::hex << node
                      << std::dec << '\n';
        }

        if (!execute) {
            std::cout << "\nstatus: PASS - dry run only; no bus reset requested\n"
                      << "to execute: " << argv[0] << " --product \""
                      << product << "\" --execute\n";
            (*device)->Release(device);
            IOObjectRelease(iterator);
            return 0;
        }

        std::cout << "\nrequesting FireWire bus reset...\n";
        const IOReturn resetKr = (*device)->BusReset(device);
        std::cout << "BusReset result: 0x" << std::hex << resetKr
                  << std::dec << '\n';
        if (resetKr == kIOReturnSuccess)
            std::cout << "status: PASS - bus re-enumeration is expected now\n";
        else
            std::cout << "status: FAIL\n";

        (*device)->Release(device);
        IOObjectRelease(iterator);
        return resetKr == kIOReturnSuccess ? 0 : 3;
    }

    IOObjectRelease(iterator);
    std::cout << "No matching FireWire unit found.\n";
    return 2;
}
