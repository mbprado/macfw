#pragma once

#include <array>
#include <cstdint>

namespace macfw::fw1814::transport {

// Linux maudio/special.rs: bytes 1 and 2 of the 84-byte meter block are
// transitions for the two headphone encoders (1 = up, 2 = down).
class HeadphoneRotaries {
public:
    void reset() { previous_ = {{0, 0}}; initialized_ = false; }

    std::array<int, 2> observe(const std::array<std::uint8_t, 84>& frame) {
        const std::array<std::uint8_t, 2> current{{frame[1], frame[2]}};
        std::array<int, 2> steps{{0, 0}};
        if (initialized_) {
            for (unsigned i = 0; i < 2; ++i) {
                if (current[i] != previous_[i])
                    steps[i] = current[i] == 1 ? 1 : current[i] == 2 ? -1 : 0;
            }
        }
        previous_ = current;
        initialized_ = true;
        return steps;
    }

private:
    std::array<std::uint8_t, 2> previous_{{0, 0}};
    bool initialized_ = false;
};

// Linux uses a 0x400 rotary step in the signed 0x8000..0 range, which
// corresponds to four whole dB in macfw's 0x100-per-dB gain representation.
inline int rotaryGain(int current, int step) {
    const int next = current + step * 0x400;
    return next < -32768 ? -32768 : next > 0 ? 0 : next;
}

} // namespace macfw::fw1814::transport
