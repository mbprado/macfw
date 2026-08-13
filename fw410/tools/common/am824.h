#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>

namespace macfw::am824 {

constexpr std::size_t kCapturePcmChannels = 4;
constexpr std::size_t kCapturePositions = 5;
constexpr std::size_t kMidiPosition = 4;
constexpr std::uint8_t kMbla24Label = 0x40;

inline std::uint32_t be32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

inline std::int32_t signExtend24(std::uint32_t value) {
    value &= 0x00ffffffu;
    if (value & 0x00800000u)
        value |= 0xff000000u;
    return static_cast<std::int32_t>(value);
}

inline bool decodeMbla24(std::uint32_t word, std::int32_t& sample) {
    if (static_cast<std::uint8_t>(word >> 24) != kMbla24Label)
        return false;
    sample = signExtend24(word);
    return true;
}

struct ChannelStats {
    std::uint64_t samples = 0;
    std::uint64_t invalidLabels = 0;
    std::int32_t minimum = std::numeric_limits<std::int32_t>::max();
    std::int32_t maximum = std::numeric_limits<std::int32_t>::min();
    std::uint32_t peak = 0;
    long double sumSquares = 0.0L;

    void add(std::uint32_t word) {
        std::int32_t sample = 0;
        if (!decodeMbla24(word, sample)) {
            ++invalidLabels;
            return;
        }

        ++samples;
        if (sample < minimum) minimum = sample;
        if (sample > maximum) maximum = sample;

        const std::int64_t wide = sample;
        const std::uint32_t magnitude = static_cast<std::uint32_t>(
            wide < 0 ? -wide : wide);
        if (magnitude > peak) peak = magnitude;

        const long double f = static_cast<long double>(sample);
        sumSquares += f * f;
    }

    double rms() const {
        if (!samples) return 0.0;
        return std::sqrt(static_cast<double>(
            sumSquares / static_cast<long double>(samples)));
    }
};

struct CaptureStats {
    std::array<ChannelStats, kCapturePcmChannels> channel{};
    std::uint64_t events = 0;
    std::uint64_t midiWords = 0;
    std::uint64_t malformedPackets = 0;
};

// Decode one FW410 host-capture AMDTP payload. `payload` begins at the
// 8-byte CIP header and `length` is the complete CIP+AM824 payload length.
// At 48 kHz the expected data formation is DBS=5:
//   pos 0 S/PDIF 1, pos 1 Line 1, pos 2 S/PDIF 2,
//   pos 3 Line 2, pos 4 MIDI.
inline void accumulateCapture48k(
    const std::uint8_t *payload,
    std::size_t length,
    CaptureStats& stats) {
    if (!payload || length <= 8)
        return; // NODATA is valid and contributes no samples.

    const std::uint32_t q0 = be32(payload);
    const std::uint32_t q1 = be32(payload + 4);
    const std::uint8_t dbs = static_cast<std::uint8_t>((q0 >> 16) & 0xff);
    const std::uint8_t fmt = static_cast<std::uint8_t>((q1 >> 24) & 0x3f);
    const std::uint8_t fdf = static_cast<std::uint8_t>((q1 >> 16) & 0xff);

    if (dbs != kCapturePositions || fmt != 0x10 || fdf != 0x02) {
        ++stats.malformedPackets;
        return;
    }

    const std::size_t dataBytes = length - 8;
    const std::size_t eventBytes = kCapturePositions * 4;
    if (dataBytes % eventBytes != 0) {
        ++stats.malformedPackets;
        return;
    }

    const std::size_t eventCount = dataBytes / eventBytes;
    const std::uint8_t *p = payload + 8;

    for (std::size_t event = 0; event < eventCount; ++event) {
        for (std::size_t ch = 0; ch < kCapturePcmChannels; ++ch)
            stats.channel[ch].add(be32(p + ch * 4));

        // Count the MIDI position regardless of whether it currently carries
        // MIDI bytes or an AM824 MIDI no-data word.
        (void)be32(p + kMidiPosition * 4);
        ++stats.midiWords;
        ++stats.events;
        p += eventBytes;
    }
}

inline void printCaptureStats(const CaptureStats& stats, std::ostream& os) {
    static constexpr const char *names[kCapturePcmChannels] = {
        "S/PDIF 1", "Line 1", "S/PDIF 2", "Line 2"
    };

    os << "PCM capture statistics:\n";
    os << "    decoded events: " << stats.events << '\n';
    os << "    malformed packets: " << stats.malformedPackets << '\n';

    for (std::size_t ch = 0; ch < kCapturePcmChannels; ++ch) {
        const auto& s = stats.channel[ch];
        os << "    ch " << (ch + 1) << " (" << names[ch] << "): ";
        if (!s.samples) {
            os << "no valid MBLA samples";
        } else {
            os << "samples=" << s.samples
               << " min=" << s.minimum
               << " max=" << s.maximum
               << " peak=" << s.peak
               << " rms=" << s.rms();
        }
        if (s.invalidLabels)
            os << " invalid-labels=" << s.invalidLabels;
        os << '\n';
    }
}

} // namespace macfw::am824
