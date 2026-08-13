#include "macfw/firewire_device.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <libkern/OSByteOrder.h>

#include <array>
#include <utility>

namespace macfw {
namespace {

bool productNameMatches(io_registry_entry_t service, const char* productName) {
    if (!productName)
        return false;

    CFStringRef wanted = CFStringCreateWithCString(
        kCFAllocatorDefault, productName, kCFStringEncodingUTF8);
    if (!wanted)
        return false;

    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, CFSTR("FireWire Product Name"), kCFAllocatorDefault, 0);

    bool matches = false;
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
        matches = CFStringCompare(
            static_cast<CFStringRef>(value), wanted, 0) == kCFCompareEqualTo;
    }

    if (value)
        CFRelease(value);
    CFRelease(wanted);
    return matches;
}

std::uint32_t decodeBe32(const std::array<UInt8, 4>& bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

} // namespace

FireWireDevice::FireWireDevice(IOFireWireLibDeviceRef device,
                               IOCFPlugInInterface** plugin,
                               io_registry_entry_t service)
    : device_(device), plugin_(plugin), service_(service) {}

FireWireDevice::~FireWireDevice() {
    reset();
}

FireWireDevice::FireWireDevice(FireWireDevice&& other) noexcept {
    *this = std::move(other);
}

FireWireDevice& FireWireDevice::operator=(FireWireDevice&& other) noexcept {
    if (this == &other)
        return *this;

    reset();
    device_ = other.device_;
    plugin_ = other.plugin_;
    service_ = other.service_;
    generation_ = other.generation_;
    nodeID_ = other.nodeID_;
    open_ = other.open_;

    other.device_ = nullptr;
    other.plugin_ = nullptr;
    other.service_ = IO_OBJECT_NULL;
    other.generation_ = 0;
    other.nodeID_ = 0;
    other.open_ = false;
    return *this;
}

FireWireDevice FireWireDevice::findByProductName(const char* productName) {
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireUnit");
    if (!matching)
        return {};

    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(
            kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return {};

    FireWireDevice result;
    io_registry_entry_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (!productNameMatches(service, productName)) {
            IOObjectRelease(service);
            continue;
        }

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        const kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            IOObjectRelease(service);
            continue;
        }

        IOFireWireLibDeviceRef device = nullptr;
        const HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
            reinterpret_cast<LPVOID*>(&device));
        if (hr != 0 || !device) {
            IODestroyPlugInInterface(plugin);
            IOObjectRelease(service);
            continue;
        }

        result = FireWireDevice(device, plugin, service);
        if (result.refreshBusAddress() != kIOReturnSuccess)
            result.reset();
        break;
    }

    IOObjectRelease(iterator);
    return result;
}

IOReturn FireWireDevice::open() {
    if (!device_)
        return kIOReturnNoDevice;
    if (open_)
        return kIOReturnSuccess;

    const IOReturn kr = (*device_)->Open(device_);
    if (kr == kIOReturnSuccess)
        open_ = true;
    return kr;
}

void FireWireDevice::close() {
    if (!device_ || !open_)
        return;
    (*device_)->Close(device_);
    open_ = false;
}

IOReturn FireWireDevice::refreshBusAddress() {
    if (!device_)
        return kIOReturnNoDevice;

    UInt32 generation = 0;
    IOReturn kr = (*device_)->GetBusGeneration(device_, &generation);
    if (kr != kIOReturnSuccess)
        return kr;

    UInt16 nodeID = 0;
    kr = (*device_)->GetRemoteNodeID(device_, generation, &nodeID);
    if (kr != kIOReturnSuccess)
        return kr;

    generation_ = generation;
    nodeID_ = nodeID;
    return kIOReturnSuccess;
}

IOReturn FireWireDevice::read(UInt16 addressHi, UInt32 addressLo,
                              void* buffer, UInt32& size) const {
    if (!device_)
        return kIOReturnNoDevice;
    if (!buffer && size != 0)
        return kIOReturnBadArgument;

    FWAddress address = {};
    address.nodeID = nodeID_;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device_)->Read(
        device_, 0, &address, buffer, &size, true, generation_);
}

IOReturn FireWireDevice::write(UInt16 addressHi, UInt32 addressLo,
                               const void* buffer, UInt32& size) const {
    if (!device_)
        return kIOReturnNoDevice;
    if (!buffer && size != 0)
        return kIOReturnBadArgument;

    FWAddress address = {};
    address.nodeID = nodeID_;
    address.addressHi = addressHi;
    address.addressLo = addressLo;
    return (*device_)->Write(
        device_, 0, &address, buffer, &size, true, generation_);
}

IOReturn FireWireDevice::readQuadletBE(UInt16 addressHi, UInt32 addressLo,
                                       std::uint32_t& value) const {
    std::array<UInt8, 4> bytes{};
    UInt32 size = static_cast<UInt32>(bytes.size());
    const IOReturn kr = read(addressHi, addressLo, bytes.data(), size);
    if (kr != kIOReturnSuccess)
        return kr;
    if (size != bytes.size())
        return kIOReturnUnderrun;
    value = decodeBe32(bytes);
    return kIOReturnSuccess;
}

IOReturn FireWireDevice::compareSwapQuadletBE(
    UInt16 addressHi, UInt32 addressLo,
    std::uint32_t expected, std::uint32_t replacement) const {
    if (!device_)
        return kIOReturnNoDevice;

    FWAddress address = {};
    address.nodeID = nodeID_;
    address.addressHi = addressHi;
    address.addressLo = addressLo;

    const UInt32 expectedBus = OSSwapHostToBigInt32(expected);
    const UInt32 replacementBus = OSSwapHostToBigInt32(replacement);
    return (*device_)->CompareSwap(
        device_, 0, &address, expectedBus, replacementBus,
        true, generation_);
}

void FireWireDevice::reset() {
    close();
    if (device_) {
        (*device_)->Release(device_);
        device_ = nullptr;
    }
    if (plugin_) {
        IODestroyPlugInInterface(plugin_);
        plugin_ = nullptr;
    }
    if (service_ != IO_OBJECT_NULL) {
        IOObjectRelease(service_);
        service_ = IO_OBJECT_NULL;
    }
    generation_ = 0;
    nodeID_ = 0;
}

} // namespace macfw
