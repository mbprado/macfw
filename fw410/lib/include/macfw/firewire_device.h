#pragma once

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <cstddef>
#include <cstdint>

namespace macfw {

class FireWireDevice {
public:
    FireWireDevice() = default;
    ~FireWireDevice();

    FireWireDevice(const FireWireDevice&) = delete;
    FireWireDevice& operator=(const FireWireDevice&) = delete;

    FireWireDevice(FireWireDevice&& other) noexcept;
    FireWireDevice& operator=(FireWireDevice&& other) noexcept;

    // Find the first IOFireWireUnit whose "FireWire Product Name" matches.
    static FireWireDevice findByProductName(const char* productName);

    explicit operator bool() const { return device_ != nullptr; }
    IOFireWireLibDeviceRef nativeHandle() const { return device_; }

    IOReturn open();
    void close();
    bool isOpen() const { return open_; }

    IOReturn refreshBusAddress();
    UInt32 generation() const { return generation_; }
    UInt16 nodeID() const { return nodeID_; }

    IOReturn read(UInt16 addressHi, UInt32 addressLo,
                  void* buffer, UInt32& size) const;
    IOReturn write(UInt16 addressHi, UInt32 addressLo,
                   const void* buffer, UInt32& size) const;

    IOReturn readQuadletBE(UInt16 addressHi, UInt32 addressLo,
                           std::uint32_t& value) const;

private:
    FireWireDevice(IOFireWireLibDeviceRef device,
                   IOCFPlugInInterface** plugin,
                   io_registry_entry_t service);
    void reset();

    IOFireWireLibDeviceRef device_ = nullptr;
    IOCFPlugInInterface** plugin_ = nullptr;
    io_registry_entry_t service_ = IO_OBJECT_NULL;
    UInt32 generation_ = 0;
    UInt16 nodeID_ = 0;
    bool open_ = false;
};

} // namespace macfw
