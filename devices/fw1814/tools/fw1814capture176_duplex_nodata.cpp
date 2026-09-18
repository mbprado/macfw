#include "../transport/blocking_pcm_tx176.h"
#include "../transport/realtime_service.h"
#include "macfw/amdtp_receive_ring.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"
#include "macfw/pcm_ring_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/firewire/IOFireWireLib.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char* kProduct = "FW 1814";
constexpr UInt16 kAddressHi = 0xffff;
constexpr UInt32 kInfoLo = 0xc8020000;
constexpr UInt32 kFcpCommandLo = 0xf0000b00;
constexpr UInt32 kFcpResponseLo = 0xf0000d00;
constexpr UInt32 kFcpResponseSize = 0x200;
constexpr double kFcpTimeoutSeconds = 1.0;

constexpr unsigned kBaselineRate = 48000;
// BeBoB CIP_BLOCKING at 176.4 kHz uses 32 events and the Linux S/PDIF
// formation changes to two capture PCM and four playback PCM positions.
constexpr UInt32 kCaptureMaxPayload = 392;  // 8 + 32 * (2 PCM + 1 MIDI) * 4.
constexpr UInt32 kPlaybackMaxPayload = 648; // 8 + 32 * (4 PCM + 1 MIDI) * 4.
constexpr std::uint8_t kPlaybackDbs = 5;
constexpr std::uint8_t kPlaybackPcmChannels = 4;
constexpr std::size_t kCapturePackets = 64;
constexpr std::size_t kTxPackets = 1280;
constexpr UInt32 kCyclesPerSecond = 8000;
constexpr UInt32 kTxCycleLead = 4096;
constexpr std::size_t kToneStartFrames = 264600; // 1.5 seconds at 176.4 kHz.
constexpr std::size_t kToneFrames = 529200;      // Three seconds.

struct ResponseContext {
    UInt16 expectedNode = 0;
    bool received = false;
    UInt32 length = 0;
    std::array<UInt8, kFcpResponseSize> bytes{};
};

UInt32 le32(const UInt8* p) {
    return static_cast<UInt32>(p[0]) |
           (static_cast<UInt32>(p[1]) << 8) |
           (static_cast<UInt32>(p[2]) << 16) |
           (static_cast<UInt32>(p[3]) << 24);
}

std::string asciiField(const UInt8* p, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len && p[i]; ++i)
        out.push_back(static_cast<char>(p[i]));
    return out;
}

void printBytes(const UInt8* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

bool validateInfo(macfw::FireWireDevice& device) {
    std::array<UInt8, 0x68> info{};
    UInt32 size = static_cast<UInt32>(info.size());
    if (device.read(kAddressHi, kInfoLo, info.data(), size) != kIOReturnSuccess ||
        size != info.size())
        return false;

    return asciiField(info.data() + 0x00, 8) == "bridgeCo" &&
           le32(info.data() + 0x08) == 1 &&
           le32(info.data() + 0x0c) == 0 &&
           le32(info.data() + 0x18) == 0x83 &&
           le32(info.data() + 0x1c) == 1 &&
           asciiField(info.data() + 0x20, 8) == "20070713" &&
           le32(info.data() + 0x30) == 0 &&
           le32(info.data() + 0x38) == 0x20080000 &&
           le32(info.data() + 0x3c) == 0x00180000;
}

UInt32 responseHandler(IOFireWireLibPseudoAddressSpaceRef space,
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

bool transaction(macfw::FireWireDevice& device,
                 ResponseContext& ctx,
                 const UInt8* command,
                 UInt32 length,
                 bool raw) {
    ctx.received = false;
    ctx.length = 0;
    ctx.bytes.fill(0);

    if (raw) {
        std::cout << "        command:  ";
        printBytes(command, length);
        std::cout << '\n';
    }

    UInt32 size = length;
    const IOReturn kr = device.write(kAddressHi, kFcpCommandLo, command, size);
    if (kr != kIOReturnSuccess) {
        std::cout << "        FCP write failed: 0x" << std::hex << kr
                  << std::dec << '\n';
        return false;
    }

    const CFAbsoluteTime deadline =
        CFAbsoluteTimeGetCurrent() + kFcpTimeoutSeconds;
    while (CFAbsoluteTimeGetCurrent() < deadline) {
        if (!ctx.received) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            continue;
        }

        if (raw) {
            std::cout << "        response: ";
            printBytes(ctx.bytes.data(), ctx.length);
            std::cout << '\n';
        }
        // FCP can send an INTERIM reply and later a final reply, even after
        // the next command has started. Match the subunit, opcode, direction
        // and requested rate before consuming a response. In particular,
        // 0x0f is INTERIM; it cannot confirm that CONTROL completed.
        const bool matching = ctx.length >= 8 &&
            ctx.bytes[1] == command[1] &&
            ctx.bytes[2] == command[2] &&
            ctx.bytes[3] == command[3] &&
            ctx.bytes[4] == command[4] &&
            (command[0] == 0x01 ||
             (ctx.bytes[5] & 0x07) == (command[5] & 0x07));
        const bool finalResponse = command[0] == 0x01
            ? (ctx.bytes[0] == 0x0c || ctx.bytes[0] == 0x0d)
            : (ctx.bytes[0] == 0x09 || ctx.bytes[0] == 0x0c ||
               ctx.bytes[0] == 0x0d);
        if (matching && finalResponse)
            return true;

        if (raw)
            std::cout << "        ignoring "
                      << (matching ? "interim/unexpected" : "unrelated")
                      << " FCP reply\n";
        ctx.received = false;
        ctx.length = 0;
    }

    std::cout << "        matching final FCP response timeout\n";
    return false;
}

bool validControlResponse(UInt8 response) {
    return response == 0x09 || response == 0x0c || response == 0x0d;
}

bool setSignalRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   UInt8 opcode,
                   UInt8 rateCode,
                   bool raw) {
    const UInt8 command[8] = {
        0x00, 0xff, opcode, 0x00, 0x90, rateCode, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
        return false;
    if (ctx.length < 8 || !validControlResponse(ctx.bytes[0]))
        return false;
    return ctx.bytes[1] == 0xff &&
           ctx.bytes[2] == opcode &&
           ctx.bytes[3] == 0x00 &&
           ctx.bytes[4] == 0x90 &&
           (ctx.bytes[5] & 0x07) == rateCode;
}

bool readInputRate(macfw::FireWireDevice& device,
                   ResponseContext& ctx,
                   unsigned& rate,
                   bool raw) {
    const UInt8 command[8] = {
        0x01, 0xff, 0x19, 0x00, 0x90, 0xff, 0xff, 0xff
    };
    if (!transaction(device, ctx, command,
                     static_cast<UInt32>(sizeof(command)), raw))
        return false;
    if (ctx.length < 8 ||
        (ctx.bytes[0] != 0x0c && ctx.bytes[0] != 0x0d) ||
        ctx.bytes[1] != 0xff || ctx.bytes[2] != 0x19 ||
        ctx.bytes[3] != 0x00 || ctx.bytes[4] != 0x90)
        return false;

    static constexpr unsigned rates[] = {
        32000, 44100, 48000, 88200, 96000, 176400, 192000, 0
    };
    rate = rates[ctx.bytes[5] & 0x07];
    return rate != 0;
}

bool specialStreamKick(macfw::FireWireDevice& device,
                       ResponseContext& ctx,
                       bool raw) {
    std::cout << "FW1814 special stream kick with BOTH ISO directions running:\n";
    std::cout << "    set OUTPUT 176400 Hz...\n";
    const bool outputOk = setSignalRate(device, ctx, 0x18, 0x05, raw);
    std::cout << "        result: " << (outputOk ? "PASS" : "FAIL") << '\n';
    if (!outputOk) return false;

    std::cout << "    waiting 100 ms before INPUT rate CONTROL...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "    set INPUT 176400 Hz...\n";
    const bool inputOk = setSignalRate(device, ctx, 0x19, 0x05, raw);
    std::cout << "        result: " << (inputOk ? "PASS" : "FAIL") << '\n';
    if (!inputOk) return false;

    std::cout << "    immediate STATUS readback: intentionally skipped\n";
    return true;
}

bool dumpReceive(const macfw::AmdtpReceiveRing& ring, bool raw) {
    const std::size_t touched = ring.touchedCount();
    std::size_t matchingData = 0;
    std::size_t noData = 0;
    std::size_t other = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        const auto packet = slot.packet();
        if (!packet.hasCip()) { ++other; continue; }
        const auto h = packet.cip();
        if (packet.isNoData()) ++noData;
        else if (h.dbs == 3 && h.fmt == 0x10 && h.fdf == 0x05 &&
                 packet.dataLength() == 32 * 3 * 4) ++matchingData;
        else ++other;
    }
    std::cout << "capture result:\n"
              << "    touched slots: " << touched
              << " / " << ring.packetCount() << '\n'
              << "    176.4 kHz 32-event packets: " << matchingData << '\n'
              << "    NODATA packets: " << noData << '\n'
              << "    other packets: " << other << '\n';

    std::size_t shown = 0;
    for (std::size_t i = 0; i < ring.packetCount() && shown < 16; ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;

        std::cout << "    packet " << i << ": len=" << slot.packetLength();
        const auto packet = slot.packet();
        if (packet.hasCip()) {
            const auto cip = packet.cip();
            std::cout << " CIP{sid=" << static_cast<unsigned>(cip.sid)
                      << " dbs=" << static_cast<unsigned>(cip.dbs)
                      << " dbc=" << static_cast<unsigned>(cip.dbc)
                      << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                      << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                      << " syt=0x" << cip.syt << std::dec << "}";
        }
        std::cout << '\n';

        if (raw && slot.packetLength() && slot.packetLength() <= slot.capacity) {
            const std::size_t n = std::min<std::size_t>(slot.packetLength(), 96);
            std::cout << "        raw: ";
            printBytes(slot.payload, n);
            if (n < slot.packetLength()) std::cout << " ...";
            std::cout << '\n';
        }
        ++shown;
    }
    return matchingData > 0;
}

bool run(bool execute, bool raw, bool tone, unsigned position) {
    if (execute && access("/tmp/macfw-fw1814-control.sock", F_OK) == 0) {
        std::cout << "stop the FW1814 transport service before this diagnostic\n";
        return false;
    }
    auto device = macfw::FireWireDevice::findByProductName(kProduct);
    if (!device) {
        std::cout << "No operational FW 1814 unit found.\n";
        return false;
    }
    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    const UInt32 initialGeneration = device.generation();
    std::cout << "matched operational unit:\n"
              << "    product: " << kProduct << '\n'
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    const bool fingerprintOk = validateInfo(device);
    std::cout << "    BeBoB operational fingerprint: "
              << (fingerprintOk ? "PASS" : "FAIL") << '\n';
    if (!fingerprintOk) {
        device.close();
        return false;
    }

    auto native = device.nativeHandle();
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool responseNotification = false;
    bool opcrConnected = false;
    bool ipcrConnected = false;
    bool playbackStarted = false;
    bool captureStarted = false;
    bool success = false;
    bool kickOk = false;
    bool rateAttempted = false;
    bool restoreOk = true;
    std::uint32_t opcr0 = 0;
    std::uint32_t ipcr0 = 0;

    ResponseContext ctx;
    ctx.expectedNode = device.nodeID();
    IOFireWireLibPseudoAddressSpaceRef responseSpace = nullptr;

    if ((*native)->AddCallbackDispatcherToRunLoop(
            native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
        std::cout << "callback dispatcher setup failed\n";
        device.close();
        return false;
    }
    callbackDispatcher = true;

    responseSpace = (*native)->CreateInitialUnitsPseudoAddressSpace(
        native, kFcpResponseLo, kFcpResponseSize, &ctx, 1024, nullptr,
        kFWAddressSpaceNoReadAccess | kFWAddressSpaceShareIfExists,
        CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!responseSpace) {
        std::cout << "FCP response address-space setup failed\n";
        goto cleanup;
    }
    (*responseSpace)->SetWriteHandler(responseSpace, responseHandler);
    if (!(*responseSpace)->TurnOnNotification(responseSpace)) {
        std::cout << "FCP response notification setup failed\n";
        goto cleanup;
    }
    responseNotification = true;

    {
        unsigned currentRate = 0;
        const bool rateOk = readInputRate(device, ctx, currentRate, raw) &&
                            currentRate == kBaselineRate;
        std::cout << "    authoritative INPUT rate: " << currentRate
                  << " Hz -> " << (rateOk ? "PASS" : "FAIL") << '\n';
        if (!rateOk) goto cleanup;
    }

    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess ||
        macfw::cmp::readIpcr0(device, ipcr0) != kIOReturnSuccess) {
        std::cout << "PCR read failed\n";
        goto cleanup;
    }

    {
        const auto op = macfw::cmp::decodePcr(opcr0);
        const auto ip = macfw::cmp::decodePcr(ipcr0);
        std::cout << "duplex-blocking-silence preflight:\n"
                  << "    oPCR[0]: 0x" << std::hex << opcr0 << std::dec
                  << " online=" << (op.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(op.p2pConnections) << '\n'
                  << "    iPCR[0]: 0x" << std::hex << ipcr0 << std::dec
                  << " online=" << (ip.online ? "yes" : "no")
                  << " p2p=" << static_cast<unsigned>(ip.p2pConnections) << '\n'
                  << "    capture reservation:  " << kCaptureMaxPayload << " bytes\n"
                  << "    playback reservation: " << kPlaybackMaxPayload << " bytes\n"
                  << "    playback blocking: DBS=" << static_cast<unsigned>(kPlaybackDbs)
                  << " PCM=" << static_cast<unsigned>(kPlaybackPcmChannels)
                  << " variable 32-event blocking cadence\n";
        if (!macfw::cmp::ready(op) || !macfw::cmp::ready(ip)) {
            std::cout << "status: REFUSED - PCR0 offline or already connected\n";
            goto cleanup;
        }
    }

    if (!execute) {
        std::cout << "status: PASS - dry run only; no ISO/CMP/rate changes made\n";
        success = true;
        goto cleanup;
    }

    {
        UInt32 cycleTime = 0;
        auto receiveRing = macfw::AmdtpReceiveRing::create(
            device, kCapturePackets, kCaptureMaxPayload);
        // Preload past the TX lead, rate kick and observation window so the
        // DMA refill never relies on CoreAudio or a dynamic PCM producer.
        constexpr std::size_t kPreloadFrames = 970200; // 5.5 seconds.
        macfw::PcmRingBuffer silentPcm(1048576, kPlaybackPcmChannels);
        std::vector<std::int32_t> silence(
            kPreloadFrames * kPlaybackPcmChannels, 0);
        if (tone) {
            constexpr double kTwoPi = 6.2831853071795864769;
            constexpr double kPeak = 529285.0; // -24 dBFS of 24-bit PCM.
            for (std::size_t frame = 0; frame < kToneFrames; ++frame) {
                const double phase = kTwoPi * 440.0 * frame / 176400.0;
                silence[(kToneStartFrames + frame) * kPlaybackPcmChannels + position] =
                    static_cast<std::int32_t>(std::lround(kPeak * std::sin(phase)));
            }
            std::cout << "tone: 440 Hz at -24 dBFS, PCM position " << position
                      << ", after 1.5 s silence for 3 s\n";
        }
        const std::size_t preloadWritten = silentPcm.write(
            silence.data(), kPreloadFrames);
        // Take the cycle anchor only after the expensive PCM preparation.
        // Otherwise it consumes the fixed TX lead before ISO can start.
        const bool cycleReady = (*native)->GetCycleTime(native, &cycleTime) ==
            kIOReturnSuccess;
        const UInt32 currentCycle = (cycleTime >> 12) & 0x1fffu;
        const UInt32 firstTxCycle = (currentCycle + kTxCycleLead) % kCyclesPerSecond;
        auto transmitRing = macfw::fw1814::transport::BlockingPcmTransmitRing176400::create(
            device, firstTxCycle, kTxPackets);
        macfw::fw1814::transport::BlockingPcmStream176400 streamer(
            transmitRing, silentPcm, currentCycle, firstTxCycle, kTxPackets / 2);
        std::atomic<bool> stopTx{false};
        std::atomic<bool> txHealthy{true};
        std::thread txWorker;
        auto capture = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::DeviceToHost,
            kCaptureMaxPayload);
        auto playback = macfw::IsochAllocation::create(
            device, macfw::IsochAllocation::Direction::HostToDevice,
            kPlaybackMaxPayload);
        IOFireWireLibIsochChannelRef captureChannel = nullptr;
        IOFireWireLibIsochChannelRef playbackChannel = nullptr;

        if (!cycleReady || !receiveRing || !transmitRing || !capture || !playback ||
            !silentPcm.valid() || preloadWritten != kPreloadFrames ||
            !streamer.valid() || !streamer.prime()) {
            std::cout << "ISO resource creation failed\n";
            goto cleanup_stream;
        }

        captureChannel = capture.nativeChannel();
        playbackChannel = playback.nativeChannel();

        {
            const IOReturn kr = (*captureChannel)->AddListener(
                captureChannel,
                reinterpret_cast<IOFireWireLibIsochPortRef>(
                    receiveRing.nativeLocalPort()));
            if (kr != kIOReturnSuccess) {
                std::cout << "capture AddListener failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        {
            const IOReturn kr = playback.bindHostToDeviceTalkerFirst(
                transmitRing.nativeLocalPort());
            if (kr != kIOReturnSuccess) {
                std::cout << "playback SetTalker failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        if ((*native)->AddIsochCallbackDispatcherToRunLoop(
                native, CFRunLoopGetCurrent()) != kIOReturnSuccess) {
            std::cout << "isoch callback dispatcher setup failed\n";
            goto cleanup_stream;
        }
        isochDispatcher = true;
        if ((*native)->TurnOnNotification(native)) notifications = true;

        {
            const IOReturn kr = playback.allocate();
            if (kr != kIOReturnSuccess) {
                std::cout << "playback allocation failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        {
            const IOReturn kr = capture.allocate();
            if (kr != kIOReturnSuccess) {
                std::cout << "capture allocation failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }

        std::cout << "ISO resources:\n"
                  << "    current cycle:  " << currentCycle << '\n'
                  << "    first TX cycle: " << firstTxCycle
                  << " (lead " << kTxCycleLead << ")\n"
                  << "    playback: channel=" << playback.channel()
                  << " speed=" << static_cast<unsigned>(playback.speed()) << '\n'
                  << "    capture:  channel=" << capture.channel()
                  << " speed=" << static_cast<unsigned>(capture.speed()) << '\n';

        {
            const IOReturn kr = macfw::cmp::connectOpcr0(
                device, opcr0, capture.channel(), capture.speed());
            if (kr != kIOReturnSuccess) {
                std::cout << "connect oPCR[0] failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        opcrConnected = true;

        {
            const IOReturn kr = macfw::cmp::connectIpcr0(
                device, ipcr0, playback.channel());
            if (kr != kIOReturnSuccess) {
                std::cout << "connect iPCR[0] failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        ipcrConnected = true;
        std::cout << "CMP: BOTH oPCR[0] and iPCR[0] connected\n";

        {
            const IOReturn kr = (*playbackChannel)->Start(playbackChannel);
            if (kr != kIOReturnSuccess) {
                std::cout << "playback channel start failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        playbackStarted = true;
        std::cout << "host->device blocking silent AMDTP DMA: started\n";

        {
            const IOReturn kr = (*captureChannel)->Start(captureChannel);
            if (kr != kIOReturnSuccess) {
                std::cout << "capture channel start failed: 0x" << std::hex
                          << kr << std::dec << '\n';
                goto cleanup_stream;
            }
        }
        captureStarted = true;
        std::cout << "device->host receive DMA: started\n";

        txWorker = std::thread([&] {
            macfw::fw1814::transport::requestInteractiveQos(
                "FW1814 176.4 TX service thread");
            macfw::fw1814::transport::requestAudioTimeConstraint();
            while (!stopTx.load(std::memory_order_acquire)) {
                UInt32 serviceTime = 0;
                if ((*native)->GetCycleTime(native, &serviceTime) != kIOReturnSuccess) {
                    txHealthy.store(false, std::memory_order_release);
                    return;
                }
                streamer.service((serviceTime >> 12) & 0x1fffu);
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        });

        std::cout << "ISO settle: 550 ms through scheduled TX start\n";
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.55, false);

        rateAttempted = true;
        kickOk = specialStreamKick(device, ctx, raw);
        if (!kickOk) {
            std::cout << "status: FAIL - special stream kick CONTROL failed\n";
            goto cleanup_stream;
        }

        std::cout << "capture: waiting up to " << (tone ? 4 : 2)
                  << " s for packets" << (tone ? " and tone playback" : "") << '\n';
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, tone ? 4.0 : 2.0, false);
        success = dumpReceive(receiveRing, raw);

        std::cout << (tone ? "duplex-blocking-tone" : "duplex-blocking-silence")
                  << " experiment: "
                  << (success ? "176.4 kHz PACKETS RECEIVED" : "NO VALID 176.4 kHz PACKETS") << '\n';

cleanup_stream:
        stopTx.store(true, std::memory_order_release);
        if (txWorker.joinable()) txWorker.join();
        const auto& txStats = streamer.stats();
        std::cout << "176.4 TX: halves=" << txStats.halvesRefilled
                  << " frames=" << txStats.framesFromBuffer
                  << " nonzero=" << txStats.nonzeroFrames
                  << " underrun=" << txStats.framesSilenced
                  << " late=" << txStats.lateCyclePolls << '\n';
        success = success && txHealthy.load(std::memory_order_acquire) &&
            txStats.halvesRefilled > 0 && txStats.framesSilenced == 0 &&
            (!tone || txStats.nonzeroFrames > 0);
        if (captureStarted && captureChannel) {
            (*captureChannel)->Stop(captureChannel);
            captureStarted = false;
        }
        if (playbackStarted && playbackChannel) {
            (*playbackChannel)->Stop(playbackChannel);
            playbackStarted = false;
        }

        UInt32 cleanupGeneration = 0;
        const bool sameGeneration =
            (*native)->GetBusGeneration(native, &cleanupGeneration) == kIOReturnSuccess &&
            cleanupGeneration == initialGeneration;
        std::cout << "bus generation after stream: " << cleanupGeneration
                  << (sameGeneration ? " -> unchanged\n" : " -> CHANGED; stale writes skipped\n");
        if (!sameGeneration) success = false;

        if (ipcrConnected && sameGeneration) {
            const IOReturn kr = macfw::cmp::restore(
                device, macfw::cmp::kIpcr0AddressLo, ipcr0);
            std::cout << "restore iPCR[0]: "
                  << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
            if (kr != kIOReturnSuccess) success = false;
            ipcrConnected = false;
        }
        if (opcrConnected && sameGeneration) {
            const IOReturn kr = macfw::cmp::restore(
                device, macfw::cmp::kOpcr0AddressLo, opcr0);
            std::cout << "restore oPCR[0]: "
                  << (kr == kIOReturnSuccess ? "success" : "failed") << '\n';
            if (kr != kIOReturnSuccess) success = false;
            opcrConnected = false;
        }

        capture.release();
        playback.release();
    }

    if (notifications) {
        (*native)->TurnOffNotification(native);
        notifications = false;
    }
    if (isochDispatcher) {
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
        isochDispatcher = false;
    }

    if (execute && rateAttempted) {
        UInt32 restoreGeneration = 0;
        const bool canRestore =
            (*native)->GetBusGeneration(native, &restoreGeneration) == kIOReturnSuccess &&
            restoreGeneration == initialGeneration;
        if (canRestore) {
            std::cout << "restoring OUTPUT and INPUT to " << kBaselineRate << " Hz...\n";
            const bool outputOk = setSignalRate(device, ctx, 0x18, 0x02, raw);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const bool inputOk = setSignalRate(device, ctx, 0x19, 0x02, raw);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            unsigned restoredRate = 0;
            const bool readOk = readInputRate(device, ctx, restoredRate, raw);
            restoreOk = outputOk && inputOk && readOk &&
                restoredRate == kBaselineRate;
            std::cout << "restored INPUT readback: "
                      << (readOk ? std::to_string(restoredRate) + " Hz" : "unavailable")
                      << '\n';
        } else {
            restoreOk = false;
            std::cout << "bus generation changed; rate restoration requires a fresh device handle\n";
        }
        std::cout << "rate restore: " << (restoreOk ? "PASS" : "FAIL") << '\n';
        success = success && restoreOk;
    }

    if (execute) {
        std::uint32_t opAfter = 0;
        std::uint32_t ipAfter = 0;
        UInt32 postGeneration = 0;
        const bool readable =
            (*native)->GetBusGeneration(native, &postGeneration) == kIOReturnSuccess &&
            postGeneration == initialGeneration &&
            macfw::cmp::readOpcr0(device, opAfter) == kIOReturnSuccess &&
            macfw::cmp::readIpcr0(device, ipAfter) == kIOReturnSuccess;
        const bool restored = readable && opAfter == opcr0 && ipAfter == ipcr0;
        std::cout << "post-test PCR restore: "
                  << (restored ? "PASS" : "FAIL") << '\n';
        success = success && restored && kickOk;

        unsigned postRate = 0;
        const bool postRateOk = readable && readInputRate(device, ctx, postRate, raw);
        std::cout << "post-disconnect INPUT rate readback: ";
        if (postRateOk)
            std::cout << postRate << " Hz"
                      << (postRate == kBaselineRate ? " -> PASS" : " -> CHANGED");
        else
            std::cout << "unavailable";
        std::cout << '\n';
        success = success && postRateOk && postRate == kBaselineRate;
    }

cleanup:
    if (responseNotification && responseSpace)
        (*responseSpace)->TurnOffNotification(responseSpace);
    if (responseSpace) (*responseSpace)->Release(responseSpace);
    if (callbackDispatcher)
        (*native)->RemoveCallbackDispatcherFromRunLoop(native);
    device.close();
    return success;
}

void usage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " [--execute --experimental-high-rate --experimental-quad-rate] [--raw]"
                 " [--tone-440 --position 0..3]\n";
}

} // namespace

int main(int argc, char** argv) {
    bool execute = false;
    bool raw = false;
    bool experimentalHighRate = false;
    bool experimentalQuadRate = false;
    bool tone = false;
    unsigned position = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--experimental-high-rate") experimentalHighRate = true;
        else if (arg == "--experimental-quad-rate") experimentalQuadRate = true;
        else if (arg == "--tone-440") tone = true;
        else if (arg == "--position" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.size() != 1 || value[0] < '0' || value[0] > '3') {
                usage(argv[0]);
                return 64;
            }
            position = static_cast<unsigned>(value[0] - '0');
        }
        else if (arg == "--raw") raw = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 64;
        }
    }

    if (execute && (!experimentalHighRate || !experimentalQuadRate)) {
        std::cerr << "176.4 kHz duplex execution requires both --experimental-high-rate"
                     " and --experimental-quad-rate\n";
        return 64;
    }

    std::cout << "macfw fw1814capture176-duplex-blocking — experimental quad-rate duplex diagnostic\n\n";
    return run(execute, raw, tone, position) ? 0 : 1;
}
