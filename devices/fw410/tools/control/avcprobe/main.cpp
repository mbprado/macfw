#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/avc/IOFireWireAVCLib.h>

#include <iomanip>
#include <iostream>

int main() {
    std::cout << "macfw avcprobe — IOFireWireAVCLib attachment probe\n\n";

    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching) {
        std::cerr << "Unable to create IOFireWireUnit matching dictionary\n";
        return 1;
    }

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    matching, &iterator);
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

        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        kr = IOCreatePlugInInterfaceForService(
            service,
            kIOFireWireAVCLibUnitTypeID,
            kIOCFPlugInInterfaceID,
            &plugin,
            &score);

        if (kr != KERN_SUCCESS || !plugin) {
            std::cout << "    AVCLib plugin from IOFireWireUnit: unavailable (0x"
                      << std::hex << kr << std::dec << ")\n";
            IOObjectRelease(service);
            continue;
        }

        std::cout << "    AVCLib plugin from IOFireWireUnit: acquired\n";
        std::cout << "    plugin score: " << score << '\n';

        IOFireWireAVCLibUnitInterface **avc = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin,
            CFUUIDGetUUIDBytes(kIOFireWireAVCLibUnitInterfaceID),
            reinterpret_cast<LPVOID *>(&avc));

        if (hr != 0 || !avc) {
            std::cout << "    IOFireWireAVCLibUnitInterface: unavailable (0x"
                      << std::hex << static_cast<unsigned long>(hr)
                      << std::dec << ")\n";
        } else {
            std::cout << "    IOFireWireAVCLibUnitInterface: acquired\n";
            std::cout << "    interface version:  " << (*avc)->version << '\n';
            std::cout << "    interface revision: " << (*avc)->revision << '\n';
            std::cout << "    AVCCommand available: YES\n";
            std::cout << "    command sent: NO\n";
            (*avc)->Release(avc);
        }

        IODestroyPlugInInterface(plugin);
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    if (count == 0) {
        std::cout << "No IOFireWireUnit services were found.\n";
    }

    return 0;
}
