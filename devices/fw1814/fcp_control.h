#pragma once

#include "macfw/firewire_device.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace macfw::fw1814 {

// FW1814-local AV/C/FCP helper.
//
// This follows the response-correlation behavior used by Linux sound/firewire:
// - a response must match selected bytes of the active command;
// - AV/C CONTROL (ctype 0x00) is deferrable;
// - matching INTERIM (0x0f) responses are non-terminal and the transaction
//   continues waiting for the matching final response.
//
// The caller owns the IOFireWireLib callback dispatcher. Call arm() only after
// AddCallbackDispatcherToRunLoop() succeeds and call reset() before removing it.
class FcpControl {
private:
    // Declare these before the public signatures that use them as template
    // arguments; C++ class scope does not make a later declaration visible in
    // an earlier member function return type.
    static constexpr UInt16 kAddressHi = 0xffff;
    static constexpr UInt32 kFcpCommandLo = 0xf0000b00;
    static constexpr UInt32 kFcpResponseLo = 0xf0000d00;
    static constexpr UInt32 kFcpResponseSize = 0x200;
    static constexpr double kTimeoutSeconds = 1.0;

public:
    FcpControl() = default;
    ~FcpControl() { reset(); }

    FcpControl(const FcpControl&) = delete;
    FcpControl& operator=(const FcpControl&) = delete;

    bool arm(FireWireDevice& device) {
        reset();
        device_ = &device;
        native_ = device.nativeHandle();
        if (!native_) {
            device_ = nullptr;
            return false;
        }

        expectedNode_ = device.nodeID();
        responseSpace_ = (*native_)->CreateInitialUnitsPseudoAddressSpace(
            native_, kFcpResponseLo, kFcpResponseSize, this, 1024, nullptr,
            kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
            CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!responseSpace_) {
            reset();
            return false;
        }

        (*responseSpace_)->SetWriteHandler(responseSpace_, responseHandler);
        if (!(*responseSpace_)->TurnOnNotification(responseSpace_)) {
            reset();
            return false;
        }
        notificationOn_ = true;
        return true;
    }

    void reset() {
        waiting_ = false;
        if (notificationOn_ && responseSpace_)
            (*responseSpace_)->TurnOffNotification(responseSpace_);
        notificationOn_ = false;
        if (responseSpace_)
            (*responseSpace_)->Release(responseSpace_);
        responseSpace_ = nullptr;
        native_ = nullptr;
        device_ = nullptr;
        expectedNode_ = 0;
        clearTransactionState();
    }

    explicit operator bool() const {
        return device_ != nullptr && responseSpace_ != nullptr && notificationOn_;
    }

    bool setSignalRate(unsigned rate, UInt8 opcode, bool raw = false) {
        const int sfc = sfcForRate(rate);
        if (sfc < 0 || (opcode != 0x18 && opcode != 0x19))
            return false;

        const UInt8 command[8] = {
            0x00, 0xff, opcode, 0x00, 0x90,
            static_cast<UInt8>(sfc), 0xff, 0xff
        };

        // Linux avc_general_set_sig_fmt() matches response bytes 1..5.
        if (!transaction(command, sizeof(command), 0x3eu, true, raw))
            return false;
        if (responseLength_ < sizeof(command))
            return false;

        // Mirror Linux's explicit terminal-error handling for this command.
        if (response_[0] == 0x08 || response_[0] == 0x0a)
            return false;
        return true;
    }

    bool readInputRate(unsigned& rate, bool raw = false) {
        rate = 0;
        const UInt8 command[8] = {
            0x01, 0xff, 0x19, 0x00, 0x90, 0xff, 0xff, 0xff
        };

        // Linux avc_general_get_sig_fmt() matches bytes 1..4; byte 5 is the
        // returned sample-frequency code.
        if (!transaction(command, sizeof(command), 0x1eu, false, raw))
            return false;
        if (responseLength_ < sizeof(command))
            return false;
        if (response_[0] != 0x0c && response_[0] != 0x0d)
            return false;

        static constexpr unsigned rates[] = {
            32000, 44100, 48000, 88200, 96000, 176400, 192000, 0
        };
        rate = rates[response_[5] & 0x07u];
        return rate != 0;
    }

    std::uint64_t matchedInterimCount() const { return interimCount_; }
    std::uint64_t ignoredResponseCount() const { return ignoredCount_; }
    const std::array<UInt8, kFcpResponseSize>& response() const { return response_; }
    UInt32 responseLength() const { return responseLength_; }

private:
    static int sfcForRate(unsigned rate) {
        static constexpr unsigned rates[] = {
            32000, 44100, 48000, 88200, 96000, 176400, 192000
        };
        for (int i = 0; i < 7; ++i)
            if (rates[i] == rate) return i;
        return -1;
    }

    static void printBytes(const UInt8* bytes, UInt32 length) {
        for (UInt32 i = 0; i < length; ++i) {
            if (i) std::cout << ' ';
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(bytes[i]);
        }
        std::cout << std::dec << std::setfill(' ');
    }

    bool matchesActiveCommand(const UInt8* bytes, UInt32 length) const {
        if (!waiting_ || !bytes) return false;
        std::uint32_t mask = matchMask_;
        for (UInt32 i = 0; mask != 0; ++i) {
            if (i >= length || i >= commandLength_)
                return false;
            if ((mask & 1u) != 0 && bytes[i] != command_[i])
                return false;
            mask >>= 1u;
        }
        return true;
    }

    bool transaction(const UInt8* command,
                     UInt32 length,
                     std::uint32_t matchMask,
                     bool deferrable,
                     bool raw) {
        if (!device_ || !native_ || !responseSpace_ || !notificationOn_ ||
            !command || length == 0 || length > command_.size())
            return false;

        clearTransactionState();
        std::copy_n(command, length, command_.begin());
        commandLength_ = length;
        matchMask_ = matchMask;
        deferrable_ = deferrable;
        waiting_ = true;

        if (raw) {
            std::cout << "        command:  ";
            printBytes(command, length);
            std::cout << '\n';
        }

        UInt32 size = length;
        const IOReturn kr = device_->write(kAddressHi, kFcpCommandLo,
                                           command, size);
        if (kr != kIOReturnSuccess) {
            waiting_ = false;
            std::cout << "        FCP write failed: 0x" << std::hex << kr
                      << std::dec << '\n';
            return false;
        }

        std::uint64_t observedInterims = interimCount_;
        double deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
        while (!finalReceived_ && CFAbsoluteTimeGetCurrent() < deadline) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            if (interimCount_ != observedInterims) {
                observedInterims = interimCount_;
                // Linux starts another timeout interval after a matching
                // INTERIM response. Do the same here.
                deadline = CFAbsoluteTimeGetCurrent() + kTimeoutSeconds;
                if (raw) {
                    std::cout << "        interim:  ";
                    printBytes(lastInterim_.data(), lastInterimLength_);
                    std::cout << " (waiting for matching final response)\n";
                }
            }
        }
        waiting_ = false;

        if (!finalReceived_) {
            std::cout << "        FCP response timeout";
            if (interimCount_ != 0)
                std::cout << " after " << interimCount_ << " matching INTERIM response(s)";
            if (ignoredCount_ != 0)
                std::cout << "; ignored unrelated responses=" << ignoredCount_;
            std::cout << '\n';
            return false;
        }

        if (raw) {
            std::cout << "        response: ";
            printBytes(response_.data(), responseLength_);
            if (ignoredCount_ != 0)
                std::cout << "  [ignored unrelated=" << ignoredCount_ << ']';
            std::cout << '\n';
        }
        return true;
    }

    static UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space,
                                  FWClientCommandID commandID,
                                  UInt32 packetLen,
                                  void* packet,
                                  UInt16 srcNodeID,
                                  UInt32,
                                  UInt32,
                                  void* refCon) {
        auto* self = static_cast<FcpControl*>(refCon);
        if (self && packet && srcNodeID == self->expectedNode_ && self->waiting_) {
            const auto* bytes = static_cast<const UInt8*>(packet);
            if (self->matchesActiveCommand(bytes, packetLen)) {
                const UInt32 n = std::min<UInt32>(packetLen, kFcpResponseSize);
                if (self->deferrable_ && packetLen != 0 && bytes[0] == 0x0f) {
                    self->lastInterimLength_ = n;
                    std::copy_n(bytes, n, self->lastInterim_.begin());
                    ++self->interimCount_;
                } else {
                    self->responseLength_ = n;
                    std::copy_n(bytes, n, self->response_.begin());
                    self->finalReceived_ = true;
                }
            } else {
                ++self->ignoredCount_;
            }
        }
        (*space)->ClientCommandIsComplete(space, commandID, kIOReturnSuccess);
        return kIOReturnSuccess;
    }

    void clearTransactionState() {
        command_.fill(0);
        response_.fill(0);
        lastInterim_.fill(0);
        commandLength_ = 0;
        responseLength_ = 0;
        lastInterimLength_ = 0;
        matchMask_ = 0;
        deferrable_ = false;
        finalReceived_ = false;
        interimCount_ = 0;
        ignoredCount_ = 0;
    }

    FireWireDevice* device_ = nullptr;
    IOFireWireLibDeviceRef native_ = nullptr;
    IOFireWireLibPseudoAddressSpaceRef responseSpace_ = nullptr;
    UInt16 expectedNode_ = 0;
    bool notificationOn_ = false;
    bool waiting_ = false;
    bool deferrable_ = false;
    bool finalReceived_ = false;
    std::array<UInt8, 8> command_{};
    UInt32 commandLength_ = 0;
    std::uint32_t matchMask_ = 0;
    std::array<UInt8, kFcpResponseSize> response_{};
    UInt32 responseLength_ = 0;
    std::array<UInt8, kFcpResponseSize> lastInterim_{};
    UInt32 lastInterimLength_ = 0;
    std::uint64_t interimCount_ = 0;
    std::uint64_t ignoredCount_ = 0;
};

} // namespace macfw::fw1814
