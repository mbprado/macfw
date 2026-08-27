#pragma once

#include "macfw/firewire_device.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace macfw::transport::duplex {

class Fw410FcpControl {
public:
    ~Fw410FcpControl() { reset(); }

    bool arm(macfw::FireWireDevice& device) {
        reset();
        native_ = device.nativeHandle();
        generation_ = device.generation();
        node_ = device.nodeID();
        response_.expectedNode = node_;

        responseSpace_ = (*native_)->CreateInitialUnitsPseudoAddressSpace(
            native_, kFcpResponseLo, kFcpResponseSize, &response_, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!responseSpace_) return false;

        (*responseSpace_)->SetWriteHandler(responseSpace_, responseHandler);
        if (!(*responseSpace_)->TurnOnNotification(responseSpace_)) {
            reset();
            return false;
        }
        notificationOn_ = true;
        return true;
    }

    void reset() {
        if (responseSpace_) {
            if (notificationOn_)
                (*responseSpace_)->TurnOffNotification(responseSpace_);
            (*responseSpace_)->Release(responseSpace_);
        }
        responseSpace_ = nullptr;
        notificationOn_ = false;
        native_ = nullptr;
        generation_ = 0;
        node_ = 0;
        response_ = {};
    }

    bool reassert44100() {
        const bool out = setRate44100(0x18);
        const bool in = setRate44100(0x19);
        std::cout << "post-start AV/C reassert:\n"
                  << "    OUTPUT plug 0 -> 44100: " << (out ? "accepted" : "failed") << '\n'
                  << "    INPUT plug 0  -> 44100: " << (in ? "accepted" : "failed") << '\n';
        return out && in;
    }

    bool readSelector(std::uint8_t functionBlock, std::uint8_t& value) {
        const UInt8 cmd[12] = {
            0x01, 0x08, 0xb8, 0x80, functionBlock, 0x10, 0x02, 0xff,
            0x01, 0x00, 0x00, 0x00
        };
        if (!transaction(cmd, sizeof(cmd))) return false;
        if (response_.length < 9 || response_.bytes[0] != 0x0c) return false;
        value = response_.bytes[7];
        return true;
    }

    bool writeSelector(std::uint8_t functionBlock, std::uint8_t value) {
        const UInt8 cmd[12] = {
            0x00, 0x08, 0xb8, 0x80, functionBlock, 0x10, 0x02, value,
            0x01, 0x00, 0x00, 0x00
        };
        if (!transaction(cmd, sizeof(cmd))) return false;
        return acceptedResponse();
    }

    bool readLevel(std::uint8_t functionBlock,
                   std::uint8_t audioChannel,
                   std::int16_t& value) {
        // AV/C Audio subunit FUNCTION BLOCK / Feature / CURRENT / Volume.
        // This matches the Linux snd-firewire-ctl-services AvcLevelOperation
        // representation: signed 16-bit fixed-point level, 0x0100 per dB.
        const UInt8 cmd[12] = {
            0x01, 0x08, 0xb8, 0x81, functionBlock, 0x10, 0x02, audioChannel,
            0x02, 0x02, 0xff, 0xff
        };
        if (!transaction(cmd, sizeof(cmd))) return false;
        if (response_.length < 12 || response_.bytes[0] != 0x0c) return false;
        value = static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(response_.bytes[10]) << 8) |
            response_.bytes[11]);
        return true;
    }

    bool writeLevel(std::uint8_t functionBlock,
                    std::uint8_t audioChannel,
                    std::int16_t value) {
        const std::uint16_t raw = static_cast<std::uint16_t>(value);
        const UInt8 cmd[12] = {
            0x00, 0x08, 0xb8, 0x81, functionBlock, 0x10, 0x02, audioChannel,
            0x02, 0x02,
            static_cast<UInt8>((raw >> 8) & 0xff),
            static_cast<UInt8>(raw & 0xff)
        };
        if (!transaction(cmd, sizeof(cmd))) return false;
        return acceptedResponse();
    }

private:
    static constexpr UInt16 kFcpAddressHi = 0xffff;
    static constexpr UInt32 kFcpCommandLo = 0xf0000b00;
    static constexpr UInt32 kFcpResponseLo = 0xf0000d00;
    static constexpr UInt32 kFcpResponseSize = 0x200;
    static constexpr double kFcpTimeoutSeconds = 1.0;

    struct ResponseContext {
        UInt16 expectedNode = 0;
        bool received = false;
        UInt32 length = 0;
        std::array<UInt8, kFcpResponseSize> bytes{};
    };

    static UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space,
                                  FWClientCommandID commandID,
                                  UInt32 packetLen,
                                  void* packet,
                                  UInt16 srcNodeID,
                                  UInt32,
                                  UInt32,
                                  void* refCon) {
        auto* ctx = static_cast<ResponseContext*>(refCon);
        if (ctx && packet && srcNodeID == ctx->expectedNode) {
            ctx->length = std::min<UInt32>(packetLen, ctx->bytes.size());
            std::memcpy(ctx->bytes.data(), packet, ctx->length);
            ctx->received = true;
        }
        (*space)->ClientCommandIsComplete(space, commandID, kIOReturnSuccess);
        return kIOReturnSuccess;
    }

    bool transaction(const UInt8* cmd, UInt32 len) {
        if (!native_ || !responseSpace_) return false;
        response_.received = false;
        response_.length = 0;
        response_.bytes.fill(0);

        FWAddress address{};
        address.nodeID = node_;
        address.addressHi = kFcpAddressHi;
        address.addressLo = kFcpCommandLo;
        UInt32 size = len;
        if ((*native_)->Write(native_, 0, &address, cmd, &size, true, generation_) != kIOReturnSuccess)
            return false;

        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kFcpTimeoutSeconds;
        while (!response_.received && CFAbsoluteTimeGetCurrent() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
        return response_.received;
    }

    bool acceptedResponse() const {
        if (response_.length == 0) return false;
        const UInt8 r = response_.bytes[0];
        return r == 0x09 || r == 0x0c || r == 0x0d || r == 0x0f;
    }

    bool setRate44100(UInt8 opcode) {
        const UInt8 cmd[8] = {0x00, 0xff, opcode, 0x00, 0x90, 0x01, 0xff, 0xff};
        if (!transaction(cmd, sizeof(cmd))) return false;
        const UInt8 r = response_.length ? response_.bytes[0] : 0;
        const bool accepted = r == 0x09 || r == 0x0c || r == 0x0d || r == 0x0f;
        return response_.length >= 8 && accepted && response_.bytes[1] == 0xff &&
               response_.bytes[2] == opcode && response_.bytes[3] == 0x00 &&
               response_.bytes[4] == 0x90 && (response_.bytes[5] & 0x07) == 0x01;
    }

    IOFireWireLibDeviceRef native_ = nullptr;
    IOFireWireLibPseudoAddressSpaceRef responseSpace_ = nullptr;
    ResponseContext response_{};
    UInt32 generation_ = 0;
    UInt16 node_ = 0;
    bool notificationOn_ = false;
};

} // namespace macfw::transport::duplex
