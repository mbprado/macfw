#include "macfw/am824_playback.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <vector>

namespace {
constexpr UInt32 kCaptureMaxPacket = 168;
constexpr UInt32 kPlaybackMaxPacket = 360;
constexpr std::size_t kCaptureSlots = 256;
constexpr std::size_t kPlaybackSlots = 4096;
constexpr std::size_t kWarmupCycles = 512;
constexpr UInt32 kCycleLead = 2048;
constexpr double kObservationSeconds = 0.35;
constexpr std::int32_t kToneAmplitude = 131072;
constexpr double kToneHz = 1000.0;

struct WarmupTxRing {
    struct StorageSlot {
        std::uint8_t payload[macfw::am824::kPlayback44100DataPacketBytes];
    };
    struct Meta {
        std::uint8_t* payload = nullptr;
        std::size_t length = 0;
        UInt32 cycle = 0;
        std::uint8_t dbc = 0;
        std::uint16_t syt = 0xffff;
        bool data = false;
        bool warmup = false;
    };

    ~WarmupTxRing() { reset(); }
    WarmupTxRing(const WarmupTxRing&) = delete;
    WarmupTxRing& operator=(const WarmupTxRing&) = delete;

    static std::unique_ptr<WarmupTxRing> create(macfw::FireWireDevice& device, UInt32 firstCycle) {
        auto out = std::unique_ptr<WarmupTxRing>(new WarmupTxRing());
        out->device = &device;
        out->firstCycle = firstCycle & 0x1fffu;
        out->mappedBytes = sizeof(StorageSlot) * kPlaybackSlots;
        out->storage = static_cast<StorageSlot*>(mmap(nullptr, out->mappedBytes,
            PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0));
        if (out->storage == MAP_FAILED) {
            out->storage = nullptr;
            return {};
        }
        std::memset(out->storage, 0, out->mappedBytes);
        out->meta.resize(kPlaybackSlots);

        macfw::am824::Playback44100State state{};
        std::uint64_t audioFrame = 0;
        constexpr double pi = 3.14159265358979323846;

        for (std::size_t i = 0; i < kPlaybackSlots; ++i) {
            const UInt32 cycle = (out->firstCycle + static_cast<UInt32>(i)) & 0x1fffu;
            auto packet = macfw::am824::buildPlayback44100Silence(cycle, state);
            const bool warmup = i < kWarmupCycles;

            if (warmup) {
                macfw::am824::putBe32Playback(packet.bytes.data(),
                    (static_cast<std::uint32_t>(macfw::am824::kPlayback44100Positions) << 16));
                macfw::am824::putBe32Playback(packet.bytes.data() + 4, 0x9001ffffu);
                packet.length = 8;
                packet.dataBearing = false;
                packet.dbc = 0;
                packet.syt = 0xffff;
                state.dbc = 0;
            } else if (packet.dataBearing) {
                for (std::size_t event = 0; event < macfw::am824::kPlayback44100EventsPerDataPacket; ++event) {
                    for (std::size_t ch = 0; ch < macfw::am824::kPlayback44100PcmPositions; ++ch) {
                        std::int32_t sample = 0;
                        if (ch == 1) {
                            const double phase = 2.0 * pi * kToneHz *
                                static_cast<double>(audioFrame + event) / 44100.0;
                            sample = static_cast<std::int32_t>(std::sin(phase) * kToneAmplitude);
                        }
                        const std::uint32_t mbla = 0x40000000u |
                            (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                        const std::size_t off = 8 +
                            (event * macfw::am824::kPlayback44100Positions + ch) * 4;
                        macfw::am824::putBe32Playback(packet.bytes.data() + off, mbla);
                    }
                }
                audioFrame += macfw::am824::kPlayback44100EventsPerDataPacket;
            }

            std::memcpy(out->storage[i].payload, packet.bytes.data(), packet.length);
            out->meta[i] = {out->storage[i].payload, packet.length, cycle,
                            packet.dbc, packet.syt, packet.dataBearing, warmup};
        }

        auto native = device.nativeHandle();
        out->pool = (*native)->CreateNuDCLPool(native, static_cast<UInt32>(kPlaybackSlots),
            CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
        if (!out->pool) return {};
        (*out->pool)->SetCurrentTagAndSync(out->pool, 1, 0);

        NuDCLRef first = nullptr;
        NuDCLRef last = nullptr;
        for (std::size_t i = 0; i < kPlaybackSlots; ++i) {
            IOVirtualRange range = {
                reinterpret_cast<IOVirtualAddress>(out->meta[i].payload),
                static_cast<IOByteCount>(out->meta[i].length)
            };
            auto dcl = (*out->pool)->AllocateSendPacket(out->pool, nullptr, 1, &range);
            if (!dcl) return {};
            const NuDCLRef ref = reinterpret_cast<NuDCLRef>(dcl);
            if (!first) first = ref;
            last = ref;
        }
        if (!first || !last || (*out->pool)->SetDCLBranch(last, first) != kIOReturnSuccess)
            return {};

        DCLCommand* program = (*out->pool)->GetProgram(out->pool);
        if (!program) return {};
        IOVirtualRange mapped = {
            reinterpret_cast<IOVirtualAddress>(out->storage),
            static_cast<IOByteCount>(out->mappedBytes)
        };
        out->localPort = (*native)->CreateLocalIsochPort(native, true, program,
            kFWDCLCycleEvent, out->firstCycle, 0x1fffu,
            nullptr, 0, &mapped, 1,
            CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
        if (!out->localPort) return {};
        return out;
    }

    void reset() {
        if (localPort) { (*localPort)->Release(localPort); localPort = nullptr; }
        if (pool) { (*pool)->Release(pool); pool = nullptr; }
        if (storage) { munmap(storage, mappedBytes); storage = nullptr; }
        mappedBytes = 0;
        meta.clear();
    }

    macfw::FireWireDevice* device = nullptr;
    StorageSlot* storage = nullptr;
    std::size_t mappedBytes = 0;
    UInt32 firstCycle = 0;
    IOFireWireLibNuDCLPoolRef pool = nullptr;
    IOFireWireLibLocalIsochPortRef localPort = nullptr;
    std::vector<Meta> meta;

private:
    WarmupTxRing() = default;
};

void dumpCapture(const macfw::AmdtpReceiveRing& ring) {
    std::size_t touched = 0, data = 0, fdf = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        ++touched;
        const auto p = slot.packet();
        if (p.length > 8) ++data;
        if (p.hasCip() && p.cip().fdf == 0x01) ++fdf;
    }
    std::cout << "capture summary:\n"
              << "    touched slots:      " << touched << " / " << ring.packetCount() << '\n'
              << "    data-bearing slots: " << data << '\n'
              << "    FDF=0x01 slots:     " << fdf << '\n'
              << "result: " << (data ? "PASS - FW410 accepted 44.1 data after NODATA warm-up"
                                      : "FAIL - capture remained NODATA") << '\n';
}

bool run(bool execute) {
    auto device = macfw::FireWireDevice::findByProductName("FW 410");
    if (!device) { std::cout << "No operational FW 410 unit found.\n"; return false; }
    std::cout << "FW410 operational unit:\n"
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID() << std::dec << '\n';
    if (device.open() != kIOReturnSuccess) return false;

    std::uint32_t opcr0 = 0, ipcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) return false;
    if (!macfw::cmp::ready(macfw::cmp::decodePcr(opcr0)) ||
        !macfw::cmp::ready(macfw::cmp::decodePcr(ipcr0))) {
        std::cout << "status: REFUSED - PCR0 offline or already connected\n";
        return false;
    }

    auto native = device.nativeHandle();
    UInt32 cycleTime = 0;
    if ((*native)->GetCycleTime(native, &cycleTime) != kIOReturnSuccess) return false;
    const UInt32 now = (cycleTime >> 12) & 0x1fffu;
    const UInt32 firstCycle = (now + kCycleLead) & 0x1fffu;

    std::cout << "44.1 kHz warm-up experiment:\n"
              << "    warm-up:            " << kWarmupCycles << " cycles (64 ms) true NODATA\n"
              << "    after warm-up:      native blocking 44.1 cadence + 1 kHz tone on Output 1\n"
              << "    total TX program:   " << kPlaybackSlots << " cycles\n"
              << "    start lead:         " << kCycleLead << " cycles\n";
    if (!execute) {
        std::cout << "status: PASS - dry run only\n"
                  << "to execute safely: ./run44100warmup.sh\n";
        return true;
    }

    auto rx = macfw::AmdtpReceiveRing::create(device, kCaptureSlots, kCaptureMaxPacket);
    auto tx = WarmupTxRing::create(device, firstCycle);
    auto capture = macfw::IsochAllocation::create(device,
        macfw::IsochAllocation::Direction::DeviceToHost, kCaptureMaxPacket);
    auto playback = macfw::IsochAllocation::create(device,
        macfw::IsochAllocation::Direction::HostToDevice, kPlaybackMaxPacket);
    if (!rx || !tx || !capture || !playback) {
        std::cout << "transport object creation failed\n";
        return false;
    }

    (*capture.nativeChannel())->AddListener(capture.nativeChannel(),
        reinterpret_cast<IOFireWireLibIsochPortRef>(rx.nativeLocalPort()));
    if (playback.bindHostToDeviceTalkerFirst(tx->localPort) != kIOReturnSuccess) return false;

    bool cb = false, iso = false, notif = false, capStart = false, playStart = false;
    bool opConn = false, ipConn = false;
    UInt32 startCycleTime = 0;
    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) cb = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess) iso = true;
    if ((*native)->TurnOnNotification(native)) notif = true;

    if (capture.allocate() != kIOReturnSuccess || playback.allocate() != kIOReturnSuccess) goto cleanup;
    std::cout << "ISO resources:\n"
              << "    capture channel:  " << capture.channel() << '\n'
              << "    playback channel: " << playback.channel() << '\n';
    if (macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed()) != kIOReturnSuccess) goto cleanup;
    opConn = true;
    if (macfw::cmp::connectIpcr0(device, ipcr0, playback.channel()) != kIOReturnSuccess) goto cleanup;
    ipConn = true;

    if ((*native)->GetCycleTime(native, &startCycleTime) == kIOReturnSuccess) {
        const UInt32 startNow = (startCycleTime >> 12) & 0x1fffu;
        const UInt32 forward = (firstCycle - startNow) & 0x1fffu;
        std::cout << "TX start timing:\n"
                  << "    scheduled cycle: " << firstCycle << '\n'
                  << "    current cycle:   " << startNow << '\n'
                  << "    forward delta:   " << forward << " cycles\n";
        if (forward > 4096u) { std::cout << "status: REFUSED - scheduled cycle missed\n"; goto cleanup; }
    }

    if ((*playback.nativeChannel())->Start(playback.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    playStart = true;
    if ((*capture.nativeChannel())->Start(capture.nativeChannel()) != kIOReturnSuccess) goto cleanup;
    capStart = true;

    std::cout << "duplex ISO: started\n"
              << "listen: tone should begin after ~64 ms on Analog Out 1\n"
              << "observation window: " << kObservationSeconds << " s\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, kObservationSeconds, false);
    dumpCapture(rx);

cleanup:
    if (playStart) (*playback.nativeChannel())->Stop(playback.nativeChannel());
    if (capStart) (*capture.nativeChannel())->Stop(capture.nativeChannel());
    if (ipConn) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kIpcr0AddressLo, ipcr0);
        std::cout << "restore iPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    if (opConn) {
        const auto kr = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: " << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    playback.release(); capture.release();
    if (notif) (*native)->TurnOffNotification(native);
    if (iso) (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (cb) (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else { std::cerr << "usage: ./pcm44100warmup [--execute]\n"; return 64; }
    }
    std::cout << "macfw pcm44100warmup — 44.1 kHz NODATA warm-up A/B test\n\n";
    return run(execute) ? 0 : 1;
}
