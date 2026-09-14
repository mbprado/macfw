#!/usr/bin/env python3
"""Post-process generated isoplayback source for the static 128-cycle TX ring.

This keeps generate.py focused on reusing the proven isoduplex transport scaffold,
while this file owns playback-only policy: exact ring closure and optional test tone.
"""
from pathlib import Path
import sys

path = Path(__file__).resolve().parent / "generated.cpp"
if not path.exists():
    sys.exit("generated.cpp not found; run generate.py first")

s = path.read_text()

# 128 cycles closes both state machines exactly:
#   96 DATA packets * 8 frames = 768 frames = 0 mod 256 DBC
#   128 cycles = 0 mod 16 SYT cycle phase
old_slots = "constexpr size_t kPlaybackSlots = 64;"
new_slots = "constexpr size_t kPlaybackSlots = 128;"

if old_slots in s:
    s = s.replace(old_slots, new_slots, 1)
elif new_slots not in s:
    sys.exit("playback slot declaration not found")

# Tone synthesis uses std::sin only while prebuilding the static ring.
if "#include <cmath>" not in s:
    s = s.replace("#include <cstdint>\n", "#include <cstdint>\n#include <cmath>\n", 1)

# Add playback test-mode state beside the generator globals.
needle = "static UInt32 gPlaybackStartCycle = 0;\n"
if needle not in s:
    sys.exit("playback globals not found")
s = s.replace(needle, needle + "static UInt32 gToneChannel = 0; // 0=silence, 1..10=PCM position\n", 1)

# Normalize stale text inherited from whichever isoduplex probe revision is current.
s = s.replace(
    'std::cout << "preflight (48 kHz TX callback probe):\\n";',
    'std::cout << "preflight (48 kHz callback-free prebuilt playback):\\n";')
s = s.replace(
    'std::cout << "preflight (48 kHz timed-silence duplex):\\n";',
    'std::cout << "preflight (48 kHz callback-free prebuilt playback):\\n";')
s = s.replace(
    '        << "    playback stream: NODATA only; ring-tail callback/timestamp probe\\n";',
    '        << "    playback: prebuilt 128-cycle AM824 ring\\n"\n'
    '        << "    start lead: " << gCycleLead << " cycles\\n";')
s = s.replace(
    '        << "    playback startup: first 64 cycles NODATA, then timed AM824 silence\\n";',
    '        << "    playback: prebuilt 128-cycle AM824 ring\\n"\n'
    '        << "    start lead: " << gCycleLead << " cycles\\n";')
s = s.replace(
    '        << "    playback: prebuilt 64-cycle AM824 silence ring\\n"',
    '        << "    playback: prebuilt 128-cycle AM824 ring\\n"')

# Dry-run diagnostics: clearly distinguish silence from the explicit tone mode.
dry_marker = '        std::cout << "    planned first TX cycle: " << start << \'\\n\';\n'
if dry_marker in s:
    s = s.replace(
        dry_marker,
        dry_marker +
        '        if (gToneChannel)\n'
        '            std::cout << "    mode: 1 kHz tone, PCM position " << gToneChannel << " (~-36 dBFS)\\n";\n'
        '        else\n'
        '            std::cout << "    mode: digital silence\\n";\n',
        1)

s = s.replace(
    '                      << (p.dataBearing ? " SILENCE" : " NODATA") << \'\\n\';',
    '                      << (p.dataBearing ? (gToneChannel ? " TONE" : " SILENCE") : " NODATA") << \'\\n\';',
    1)

# Prebuild: start with the proven silence packet, then overwrite one PCM position
# with a low-level 1 kHz sine. The 128-cycle ring has 768 audio frames, exactly
# 16 periods at 48 kHz, so packet 127 -> packet 0 is waveform-phase continuous.
old = '''        macfw::am824::Playback48kState state{};
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
'''
new = '''        macfw::am824::Playback48kState state{};
        std::uint64_t audioFrame = 0;
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kToneAmplitude = 131072.0; // about -36 dBFS vs signed 24-bit full scale
        for (size_t i = 0; i < kPlaybackSlots; ++i) {
            const UInt32 cycle =
                (gPlaybackStartCycle + static_cast<UInt32>(i)) & 0x1fffu;
            auto packet =
                macfw::am824::buildPlayback48kSilence(cycle, state);

            if (packet.dataBearing && gToneChannel >= 1 && gToneChannel <= 10) {
                const std::size_t channel = static_cast<std::size_t>(gToneChannel - 1);
                for (std::size_t event = 0; event < 8; ++event) {
                    const double phase =
                        2.0 * kPi * 1000.0 * static_cast<double>(audioFrame + event) / 48000.0;
                    const std::int32_t sample =
                        static_cast<std::int32_t>(std::sin(phase) * kToneAmplitude);
                    const std::uint32_t mbla =
                        0x40000000u | (static_cast<std::uint32_t>(sample) & 0x00ffffffu);
                    const std::size_t byteOffset =
                        8 + (event * 11 + channel) * 4;
                    putBe32(packet.bytes.data() + byteOffset, mbla);
                }
                audioFrame += 8;
            } else if (packet.dataBearing) {
                audioFrame += 8;
            }

            std::memcpy(playback[i].payload,
                        packet.bytes.data(), packet.length);
            if (packet.dataBearing)
                state.dbc = static_cast<std::uint8_t>(state.dbc + 8u);
            state.phase = static_cast<std::uint8_t>((state.phase + 1u) & 3u);
        }
'''
if old not in s:
    sys.exit("static playback prebuild block changed; update postprocess.py")
s = s.replace(old, new, 1)

schedule_marker = '        std::cout << "    first TX cycle: " << gPlaybackStartCycle << \'\\n\';\n'
if schedule_marker in s:
    s = s.replace(
        schedule_marker,
        schedule_marker +
        '        if (gToneChannel)\n'
        '            std::cout << "    TX content: 1 kHz tone on PCM position " << gToneChannel << " (~-36 dBFS)\\n";\n'
        '        else\n'
        '            std::cout << "    TX content: digital silence\\n";\n',
        1)

# CLI: silence remains default; tone requires an explicit channel selection.
cli = '''        else if (arg == "--raw")
            raw = true;
'''
cli_new = '''        else if (arg == "--raw")
            raw = true;
        else if (arg == "--tone-channel" && i + 1 < argc) {
            try {
                gToneChannel = static_cast<UInt32>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "invalid --tone-channel value\\n";
                return 64;
            }
            if (gToneChannel < 1 || gToneChannel > 10) {
                std::cerr << "--tone-channel must be 1..10\\n";
                return 64;
            }
        }
'''
if cli not in s:
    sys.exit("CLI raw option block changed; update postprocess.py")
s = s.replace(cli, cli_new, 1)

s = s.replace(
    ' [--execute] [--raw] [--cycle-lead N]\\n";',
    ' [--execute] [--raw] [--cycle-lead N] [--tone-channel 1..10]\\n";')
s = s.replace(
    'to execute: ./isoplayback --execute [--raw] [--cycle-lead N]\\n',
    'to execute: ./isoplayback --execute [--raw] [--cycle-lead N] [--tone-channel 1..10]\\n')

# The generated experiment must be callback-free even if isoduplex currently
# contains an abandoned callback probe. Remove known callback setup fragments.
callback_fragment = '''        (*txPool)->SetDCLCallback(
            ref, playbackDclComplete);
'''
s = s.replace(callback_fragment, "")

s = s.replace(
    '           "(NODATA playback + ring-tail callback probe)\\n";',
    '           "(prebuilt static playback; no TX callbacks)\\n";')
s = s.replace(
    '           "(first TX ring NODATA; later rings timed PCM silence)\\n";',
    '           "(prebuilt static playback; no TX callbacks)\\n";')

path.write_text(s)
print(f"post-processed {path}")
