#pragma once

#include "macfw/am824.h"

#include <cstddef>
#include <cstdint>

namespace macfw::amdtp {

struct CipHeader {
    std::uint8_t sid = 0;
    std::uint8_t dbs = 0;
    std::uint8_t dbc = 0;
    std::uint8_t fmt = 0;
    std::uint8_t fdf = 0;
    std::uint16_t syt = 0;
};

struct PacketView {
    const std::uint8_t* payload = nullptr;
    std::size_t length = 0;

    bool hasCip() const {
        return payload != nullptr && length >= 8;
    }

    CipHeader cip() const {
        CipHeader h{};
        if (!hasCip())
            return h;

        const auto q0 = macfw::am824::be32(payload);
        const auto q1 = macfw::am824::be32(payload + 4);

        h.sid = static_cast<std::uint8_t>((q0 >> 24) & 0x3f);
        h.dbs = static_cast<std::uint8_t>((q0 >> 16) & 0xff);
        h.dbc = static_cast<std::uint8_t>(q0 & 0xff);
        h.fmt = static_cast<std::uint8_t>((q1 >> 24) & 0x3f);
        h.fdf = static_cast<std::uint8_t>((q1 >> 16) & 0xff);
        h.syt = static_cast<std::uint16_t>(q1 & 0xffff);
        return h;
    }

    bool isNoData() const {
        return hasCip() && length == 8 && cip().syt == 0xffff;
    }

    const std::uint8_t* data() const {
        return hasCip() ? payload + 8 : nullptr;
    }

    std::size_t dataLength() const {
        return hasCip() ? length - 8 : 0;
    }
};

inline bool isFw410Capture48k(const PacketView& packet) {
    if (!packet.hasCip())
        return false;

    const auto h = packet.cip();
    return h.dbs == 5 && h.fmt == 0x10 && h.fdf == 0x02;
}

inline bool isFw410Playback48k(const PacketView& packet) {
    if (!packet.hasCip())
        return false;

    const auto h = packet.cip();
    return h.dbs == 11 && h.fmt == 0x10 && h.fdf == 0x02;
}

} // namespace macfw::amdtp
