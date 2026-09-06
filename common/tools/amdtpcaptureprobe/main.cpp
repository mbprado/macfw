#include "macfw/amdtp_receive_ring.h"
#include "macfw/am824.h"
#include "macfw/cmp.h"
#include "macfw/firewire_device.h"
#include "macfw/isoch_allocation.h"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

struct PositionStats {
    std::map<unsigned, std::uint64_t> labels;
    std::uint64_t mblaWords = 0;
    std::int32_t minimum = std::numeric_limits<std::int32_t>::max();
    std::int32_t maximum = std::numeric_limits<std::int32_t>::min();
    std::uint32_t peak = 0;
};

void usage(const char* argv0) {
    std::cout << "usage: " << argv0
              << " --product <name> --dbs <count> --payload <bytes> [--packets <count>] [--execute] [--raw]\n";
}

void printBytes(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (i) std::cout << ' ';
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(p[i]);
    }
    std::cout << std::dec << std::setfill(' ');
}

void accumulatePositions(const macfw::amdtp::PacketView& packet,
                         unsigned expectedDbs,
                         std::vector<PositionStats>& stats,
                         std::uint64_t& events,
                         std::uint64_t& malformed) {
    if (!packet.hasCip() || packet.isNoData()) return;
    const auto cip = packet.cip();
    if (cip.dbs != expectedDbs || cip.fmt != 0x10) {
        ++malformed;
        return;
    }
    const auto bytes = packet.dataLength();
    const auto eventBytes = static_cast<std::size_t>(expectedDbs) * 4;
    if (!eventBytes || bytes % eventBytes != 0) {
        ++malformed;
        return;
    }

    const auto* p = packet.data();
    const auto count = bytes / eventBytes;
    for (std::size_t event = 0; event < count; ++event) {
        for (unsigned pos = 0; pos < expectedDbs; ++pos) {
            const auto word = macfw::am824::be32(p + pos * 4);
            const auto label = static_cast<unsigned>((word >> 24) & 0xff);
            ++stats[pos].labels[label];
            if (label == macfw::am824::kMbla24Label) {
                const auto sample = macfw::am824::signExtend24(word);
                ++stats[pos].mblaWords;
                stats[pos].minimum = std::min(stats[pos].minimum, sample);
                stats[pos].maximum = std::max(stats[pos].maximum, sample);
                const std::int64_t wide = sample;
                const auto mag = static_cast<std::uint32_t>(wide < 0 ? -wide : wide);
                stats[pos].peak = std::max(stats[pos].peak, mag);
            }
        }
        ++events;
        p += eventBytes;
    }
}

bool run(const std::string& product,
         unsigned expectedDbs,
         unsigned payloadBytes,
         std::size_t packetCount,
         bool execute,
         bool raw) {
    auto device = macfw::FireWireDevice::findByProductName(product.c_str());
    if (!device) {
        std::cout << "No matching operational unit found.\n";
        return false;
    }

    std::cout << "matched operational unit:\n"
              << "    product: " << product << '\n'
              << "    generation: " << device.generation() << '\n'
              << "    remote node: 0x" << std::hex << device.nodeID()
              << std::dec << '\n';

    if (device.open() != kIOReturnSuccess) {
        std::cout << "open failed\n";
        return false;
    }

    std::uint32_t opcr0 = 0;
    if (macfw::cmp::readOpcr0(device, opcr0) != kIOReturnSuccess) {
        std::cout << "oPCR[0] read failed\n";
        return false;
    }

    const auto pcr = macfw::cmp::decodePcr(opcr0);
    std::cout << "receive-only preflight:\n"
              << "    oPCR[0]:       0x" << std::hex << opcr0 << std::dec << '\n'
              << "    online:        " << (pcr.online ? "yes" : "no") << '\n'
              << "    p2p:           " << static_cast<unsigned>(pcr.p2pConnections) << '\n'
              << "    expected DBS:  " << expectedDbs << '\n'
              << "    max payload:   " << payloadBytes << " bytes\n"
              << "    packet slots:  " << packetCount << '\n';

    if (!macfw::cmp::ready(pcr)) {
        std::cout << "status: REFUSED - device OUTPUT plug is offline or already connected\n";
        return false;
    }

    if (!execute) {
        std::cout << "status: PASS - dry run only; no CMP connection or ISO stream started\n";
        return true;
    }

    auto ring = macfw::AmdtpReceiveRing::create(device, packetCount, payloadBytes);
    auto capture = macfw::IsochAllocation::create(
        device, macfw::IsochAllocation::Direction::DeviceToHost, payloadBytes);
    if (!ring || !capture) {
        std::cout << "ISO receive resource creation failed\n";
        return false;
    }

    auto channel = capture.nativeChannel();
    const auto listenerKr = (*channel)->AddListener(
        channel, reinterpret_cast<IOFireWireLibIsochPortRef>(ring.nativeLocalPort()));
    if (listenerKr != kIOReturnSuccess) {
        std::cout << "AddListener failed: 0x" << std::hex << listenerKr << std::dec << '\n';
        return false;
    }

    auto native = device.nativeHandle();
    bool callbackDispatcher = false;
    bool isochDispatcher = false;
    bool notifications = false;
    bool started = false;
    bool connected = false;
    bool ok = false;

    if ((*native)->AddCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        callbackDispatcher = true;
    if ((*native)->AddIsochCallbackDispatcherToRunLoop(native, CFRunLoopGetCurrent()) == kIOReturnSuccess)
        isochDispatcher = true;
    if ((*native)->TurnOnNotification(native))
        notifications = true;

    auto kr = capture.allocate();
    if (kr != kIOReturnSuccess) {
        std::cout << "capture ISO resource allocation failed: 0x"
                  << std::hex << kr << std::dec << '\n';
        goto cleanup;
    }

    std::cout << "capture ISO resource: channel=" << capture.channel()
              << " speed=" << static_cast<unsigned>(capture.speed()) << '\n';

    kr = macfw::cmp::connectOpcr0(device, opcr0, capture.channel(), capture.speed());
    if (kr != kIOReturnSuccess) {
        std::cout << "connect oPCR[0] failed: 0x" << std::hex << kr << std::dec << '\n';
        goto cleanup;
    }
    connected = true;
    std::cout << "CMP: device OUTPUT connected to host receive channel\n";

    kr = (*channel)->Start(channel);
    if (kr != kIOReturnSuccess) {
        std::cout << "capture channel start failed: 0x" << std::hex << kr << std::dec << '\n';
        goto cleanup;
    }
    started = true;

    std::cout << "capture: waiting up to 2 s for " << packetCount << " packet slots\n";
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);

    std::cout << "capture: " << (ring.completed() ? "NuDCL burst completed" : "timeout before burst completion") << '\n';
    std::cout << "    touched slots: " << ring.touchedCount() << " / " << ring.packetCount() << '\n';

    std::vector<PositionStats> positions(expectedDbs);
    std::uint64_t events = 0;
    std::uint64_t malformed = 0;
    std::size_t shown = 0;
    for (std::size_t i = 0; i < ring.packetCount(); ++i) {
        const auto& slot = ring.slot(i);
        if (!slot.touched()) continue;
        const auto packet = slot.packet();
        if (shown < 12) {
            const auto cip = packet.cip();
            std::cout << "    packet " << i
                      << ": len=" << slot.packetLength();
            if (packet.hasCip()) {
                std::cout << " CIP{dbs=" << static_cast<unsigned>(cip.dbs)
                          << " dbc=" << static_cast<unsigned>(cip.dbc)
                          << " fmt=0x" << std::hex << static_cast<unsigned>(cip.fmt)
                          << " fdf=0x" << static_cast<unsigned>(cip.fdf)
                          << " syt=0x" << cip.syt << std::dec << "}";
            }
            std::cout << '\n';
            if (raw && slot.packetLength() && slot.packetLength() <= slot.capacity) {
                const auto n = std::min<std::size_t>(slot.packetLength(), 64);
                std::cout << "        raw: ";
                printBytes(slot.payload, n);
                if (n < slot.packetLength()) std::cout << " ...";
                std::cout << '\n';
            }
            ++shown;
        }
        accumulatePositions(packet, expectedDbs, positions, events, malformed);
    }

    std::cout << "AM824 formation summary:\n"
              << "    decoded data-block events: " << events << '\n'
              << "    malformed packets:         " << malformed << '\n';
    for (unsigned pos = 0; pos < expectedDbs; ++pos) {
        const auto& s = positions[pos];
        std::cout << "    pos " << std::setw(2) << pos << ": labels";
        if (s.labels.empty()) {
            std::cout << " <none>";
        } else {
            for (const auto& [label, count] : s.labels) {
                std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0')
                          << label << std::dec << std::setfill(' ')
                          << "(" << count << ")";
            }
        }
        if (s.mblaWords) {
            std::cout << "  MBLA=" << s.mblaWords
                      << " min=" << s.minimum
                      << " max=" << s.maximum
                      << " peak=" << s.peak;
        }
        std::cout << '\n';
    }

    ok = ring.touchedCount() > 0 && events > 0 && malformed == 0;
    std::cout << "formation validation: " << (ok ? "PASS" : "INCOMPLETE") << '\n';

cleanup:
    if (started)
        (*channel)->Stop(channel);
    if (connected) {
        const auto restore = macfw::cmp::restore(device, macfw::cmp::kOpcr0AddressLo, opcr0);
        std::cout << "restore oPCR[0]: "
                  << (restore == kIOReturnSuccess ? "success" : "failed") << '\n';
    }
    capture.release();
    if (notifications)
        (*native)->TurnOffNotification(native);
    if (isochDispatcher)
        (*native)->RemoveIsochCallbackDispatcherFromRunLoop(native);
    if (callbackDispatcher)
        (*native)->RemoveCallbackDispatcherFromRunLoop(native);

    std::uint32_t after = 0;
    if (macfw::cmp::readOpcr0(device, after) == kIOReturnSuccess)
        std::cout << "post-test oPCR restore: " << (after == opcr0 ? "PASS" : "FAIL") << '\n';

    return ok;
}

} // namespace

int main(int argc, char** argv) {
    std::string product;
    unsigned dbs = 0;
    unsigned payload = 0;
    std::size_t packets = 64;
    bool execute = false;
    bool raw = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--product" && i + 1 < argc) product = argv[++i];
        else if (arg == "--dbs" && i + 1 < argc) dbs = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (arg == "--payload" && i + 1 < argc) payload = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (arg == "--packets" && i + 1 < argc) packets = static_cast<std::size_t>(std::stoul(argv[++i]));
        else if (arg == "--execute") execute = true;
        else if (arg == "--raw") raw = true;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 64; }
    }

    if (product.empty() || dbs == 0 || payload < 8 || packets == 0) {
        usage(argv[0]);
        return 64;
    }

    std::cout << "macfw amdtpcaptureprobe — guarded receive-only AMDTP formation probe\n\n";
    return run(product, dbs, payload, packets, execute, raw) ? 0 : 1;
}
