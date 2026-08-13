#!/usr/bin/env python3
"""Generate isoplayback.cpp from the proven isoduplex transport scaffold.

The generated experiment keeps the working duplex/CMP/IRM/capture plumbing but
replaces callback-driven TX mutation with a completely prebuilt 64-cycle AM824
silence ring.  The local playback port is armed with kFWDCLCycleEvent so the SYT
values are computed for the same cycle at which DMA is told to start.
"""
from pathlib import Path
import sys

src_path = Path(__file__).resolve().parent.parent / "isoduplex" / "main.cpp"
out_path = Path(__file__).resolve().parent / "generated.cpp"
s = src_path.read_text()

# A global is sufficient for this small experimental tool and avoids perturbing
# the well-tested run()/device-discovery signatures inherited from isoduplex.
needle = "namespace {\n"
s = s.replace(needle, needle + "\nstatic UInt32 gCycleLead = 256;\nstatic UInt32 gPlaybackStartCycle = 0;\n", 1)

s = s.replace(
    'std::cout << "preflight (48 kHz timed-silence duplex):\\n";',
    'std::cout << "preflight (48 kHz callback-free prebuilt playback):\\n";', 1)
s = s.replace(
    '        << "    playback startup: first 64 cycles NODATA, then timed AM824 silence\\n";',
    '        << "    playback: prebuilt 64-cycle AM824 silence ring\\n"\n'
    '        << "    start lead: " << gCycleLead << " cycles\\n";', 1)

# Show a real bus-relative packet plan even in dry-run mode.
dry = '''    if (!execute) {
        std::cout
            << "status: PASS - no resources or packets used\\n";
        std::cout
            << "to execute: ./isoduplex --execute [--raw]\\n";
        (*device)->Close(device);
        return true;
    }
'''
dry_new = '''    if (!execute) {
        UInt32 ct = 0;
        if ((*device)->GetCycleTime(device, &ct) != kIOReturnSuccess) {
            std::cout << "status: FAIL - GetCycleTime failed\\n";
            (*device)->Close(device);
            return false;
        }
        const UInt32 now = (ct >> 12) & 0x1fffu;
        const UInt32 start = (now + gCycleLead) & 0x1fffu;
        std::cout << "TX plan (dry run):\\n";
        std::cout << "    cycle timer: 0x" << std::hex << ct << std::dec << '\\n';
        std::cout << "    current cycle: " << now << '\\n';
        std::cout << "    planned first TX cycle: " << start << '\\n';
        macfw::am824::Playback48kState state{};
        for (std::size_t i = 0; i < 16; ++i) {
            const UInt32 cycle = (start + static_cast<UInt32>(i)) & 0x1fffu;
            const auto p = macfw::am824::buildPlayback48kSilence(cycle, state);
            std::cout << "    packet " << i << ": cycle=" << cycle
                      << " len=" << p.length << " dbc="
                      << static_cast<unsigned>(p.dbc)
                      << " syt=0x" << std::hex << p.syt << std::dec
                      << (p.dataBearing ? " SILENCE" : " NODATA") << '\\n';
            if (p.dataBearing)
                state.dbc = static_cast<std::uint8_t>(state.dbc + 8u);
            state.phase = static_cast<std::uint8_t>((state.phase + 1u) & 3u);
        }
        std::cout << "status: PASS - dry run only; nothing transmitted\\n";
        std::cout << "to execute: ./isoplayback --execute [--raw] [--cycle-lead N]\\n";
        (*device)->Close(device);
        return true;
    }
'''
if dry not in s:
    sys.exit("isoduplex dry-run block changed; update isoplayback/generate.py")
s = s.replace(dry, dry_new, 1)

# Prebuild the complete ring from a fresh cycle-timer read.  The port start
# event below gates DMA on exactly gPlaybackStartCycle (13-bit cycle counter).
old_init = '''    std::memset(capture, 0, captureBytes);
    std::memset(playback, 0, playbackBytes);
    for (size_t i = 0; i < kPlaybackSlots; ++i)
        makePlaybackNoData(playback[i]);
'''
new_init = '''    std::memset(capture, 0, captureBytes);
    std::memset(playback, 0, playbackBytes);

    UInt32 cycleTime = 0;
    if ((*device)->GetCycleTime(device, &cycleTime) != kIOReturnSuccess)
        goto cleanup_early;
    {
        const UInt32 now = (cycleTime >> 12) & 0x1fffu;
        gPlaybackStartCycle = (now + gCycleLead) & 0x1fffu;
        macfw::am824::Playback48kState state{};
        for (size_t i = 0; i < kPlaybackSlots; ++i) {
            const UInt32 cycle =
                (gPlaybackStartCycle + static_cast<UInt32>(i)) & 0x1fffu;
            const auto packet =
                macfw::am824::buildPlayback48kSilence(cycle, state);
            std::memcpy(playback[i].payload,
                        packet.bytes.data(), packet.length);
            if (packet.dataBearing)
                state.dbc = static_cast<std::uint8_t>(state.dbc + 8u);
            state.phase = static_cast<std::uint8_t>((state.phase + 1u) & 3u);
        }
        std::cout << "TX schedule:\\n";
        std::cout << "    cycle timer: 0x" << std::hex << cycleTime
                  << std::dec << '\\n';
        std::cout << "    first TX cycle: " << gPlaybackStartCycle << '\\n';
    }
'''
if old_init not in s:
    sys.exit("isoduplex playback initialization changed; update generator")
s = s.replace(old_init, new_init, 1)

# The generated ring already contains final packet lengths and contents.
s = s.replace(
    '''        // First ring: proven NODATA behavior.  Each DCL callback then
        // rewrites this same packet for the next ring using its actual
        // FireWire completion timestamp.
        IOVirtualRange range = {
            reinterpret_cast<IOVirtualAddress>(
                playback[i].payload),
            kPlaybackNoDataBytes
        };
''',
    '''        const IOByteCount packetBytes =
            static_cast<IOByteCount>((i & 3u) == 3u
                ? kPlaybackNoDataBytes : kPlaybackBufferBytes);
        IOVirtualRange range = {
            reinterpret_cast<IOVirtualAddress>(playback[i].payload),
            packetBytes
        };
''', 1)

# No callbacks, no dynamic mutation, no timestamp dependency.
callback_block = '''        txContexts[i].pool = txPool;
        txContexts[i].localPort = &localPlayback;
        txContexts[i].slot = &playback[i];
        txContexts[i].slotIndex = i;
        txContexts[i].nextDbc = playbackDbcForSlot(i);

        (*txPool)->SetDCLStatusPtr(
            ref, &playback[i].status);
        (*txPool)->SetDCLTimeStampPtr(
            ref, &playback[i].timestamp);
        (*txPool)->SetDCLCallback(
            ref, playbackDclComplete);
        (*txPool)->SetDCLRefcon(
            ref, &txContexts[i]);
        (*txPool)->SetDCLFlags(
            ref, kNuDCLDynamic | kNuDCLUpdateBeforeCallback);
'''
s = s.replace(callback_block, '', 1)

# Arm the talker for the cycle used when computing packet-0 SYT.
old_port = '''                device, true, program,
                0, 0, 0,
                nullptr, 0,
'''
new_port = '''                device, true, program,
                kFWDCLCycleEvent,
                gPlaybackStartCycle,
                0x1fffu,
                nullptr, 0,
'''
if old_port not in s:
    sys.exit("isoduplex local playback port creation changed; update generator")
s = s.replace(old_port, new_port, 1)

s = s.replace(
    '           "(first TX ring NODATA; later rings timed PCM silence)\\n";',
    '           "(prebuilt timed PCM silence; no TX callbacks)\\n";', 1)

# Remove the now-meaningless callback statistics section.
start = s.find('    {\n        std::uint64_t callbacks = 0;')
end = s.find('\n    ok = true;', start)
if start != -1 and end != -1:
    s = s[:start] + '''    std::cout << "\\nplayback TX mode:\\n";
    std::cout << "    callback dependency: none\\n";
    std::cout << "    dynamic updates:     none\\n";
    std::cout << "    ring packets:        " << kPlaybackSlots << '\\n';
''' + s[end:]
else:
    sys.exit("isoduplex callback statistics block changed; update generator")

# The early GetCycleTime failure occurs before the normal cleanup-owned objects
# exist.  Reuse the already allocated mmap cleanup directly.
s = s.replace(
    '\n    std::array<TxDclContext, kPlaybackSlots> txContexts{};\n',
    '\ncleanup_early:\n    if (cycleTime == 0) {\n'
    '        munmap(playback, playbackBytes);\n'
    '        munmap(capture, captureBytes);\n'
    '        (*device)->Close(device);\n'
    '        return false;\n'
    '    }\n\n'
    '    std::array<TxDclContext, kPlaybackSlots> txContexts{};\n', 1)

# CLI and banner.
s = s.replace(
    '''        else if (arg == "--raw")
            raw = true;
        else {
''',
    '''        else if (arg == "--raw")
            raw = true;
        else if (arg == "--cycle-lead" && i + 1 < argc) {
            try {
                gCycleLead = static_cast<UInt32>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "invalid --cycle-lead value\\n";
                return 64;
            }
            if (gCycleLead == 0 || gCycleLead >= 8192) {
                std::cerr << "--cycle-lead must be 1..8191\\n";
                return 64;
            }
        } else {
''', 1)
s = s.replace(
    '                << " [--execute] [--raw]\\n";',
    '                << " [--execute] [--raw] [--cycle-lead N]\\n";', 1)
s = s.replace(
    '        << "macfw isoduplex — guarded FW410 "\n           "duplex AMDTP transport test\\n\\n";',
    '        << "macfw isoplayback — callback-free FW410 "\n           "prebuilt AMDTP silence test\\n\\n";', 1)

# The generated source lives one directory deeper than isoduplex/main.cpp, but
# the common headers are siblings of both tool directories.
s = s.replace('#include "../common/', '#include "../common/')

out_path.write_text(s)
print(f"generated {out_path}")
