#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

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

int main() {
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
