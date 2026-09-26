#include "fw1814_receive_cycle.h"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using macfw::fw1814::experimental::receiveTimestampAfter;
    constexpr std::uint32_t beforeWrap = (7u << 25) | (7998u << 12);
    constexpr std::uint32_t afterWrap = 1u << 12;
    static_assert(receiveTimestampAfter(afterWrap, beforeWrap));
    static_assert(!receiveTimestampAfter(beforeWrap, afterWrap));
    static_assert(!receiveTimestampAfter(afterWrap, afterWrap));

    // A receive group straddling the rollover must retain capture order.
    constexpr std::uint32_t lastCycle = (7u << 25) | (7999u << 12);
    constexpr std::uint32_t firstCycle = 0u;
    assert(receiveTimestampAfter(lastCycle, beforeWrap));
    assert(receiveTimestampAfter(firstCycle, lastCycle));
    assert(receiveTimestampAfter(afterWrap, firstCycle));
    assert(!receiveTimestampAfter(beforeWrap, afterWrap));
    std::cout << "88.2 receive timestamp rollover: PASS\n";
}
