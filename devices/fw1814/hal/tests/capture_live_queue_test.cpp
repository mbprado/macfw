#include "../include/macfw_fw1814_capture_shm.h"

#include <cassert>
#include <memory>
#include <vector>

namespace capture = macfw::fw1814::hal::capture;

int main() {
    auto ring = std::make_unique<capture::SharedCaptureRing>();
    capture::initialize(*ring);
    std::vector<float> input(1024 * capture::kInputChannels);
    for (std::size_t frame = 0; frame < 1024; ++frame)
        input[frame * capture::kInputChannels] = static_cast<float>(frame);

    assert(capture::write(*ring, input.data(), 512) == 512);
    ring->active.store(1);
    assert(capture::trimLiveCapture(*ring, 512 + 2 * 192, 512, 10) == 0);
    assert(capture::write(*ring, input.data() + 512 * capture::kInputChannels,
                          512) == 512);
    assert(capture::trimLiveCapture(*ring, 512 + 2 * 192, 512, 20) == 512);
    assert(capture::availableFrames(*ring) == 512);
    assert(ring->halDiscardEvents.load() == 1);
    assert(ring->halDiscardedFrames.load() == 512);
    assert(ring->halLastDiscardReason.load() ==
           static_cast<std::uint32_t>(capture::DiscardReason::liveTrim));
    assert(ring->halLastDiscardTick.load() == 20);

    std::vector<float> output(192 * capture::kInputChannels);
    assert(capture::read(*ring, output.data(), 192) == 192);
    assert(output[0] == 512.0f); // The oldest 512 frames were skipped.
    assert(ring->halUnderrunEvents.load() == 0);

    assert(capture::discardForClientStart(*ring, 30) == 320);
    assert(ring->active.load() == 0);
    assert(capture::availableFrames(*ring) == 0);
    assert(ring->halDiscardEvents.load() == 2);
    assert(ring->halDiscardedFrames.load() == 832);
    assert(ring->halLastDiscardReason.load() ==
           static_cast<std::uint32_t>(capture::DiscardReason::clientStart));

    assert(capture::write(*ring, input.data(), 512) == 512);
    assert(capture::availableFrames(*ring) == 512);
    assert(!capture::suspendStaleCapture(*ring, 4096, 40));
    assert(capture::write(*ring, input.data(), 1024) == 1024);
    assert(capture::trimLiveCapture(*ring, 896, 512, 50) == 1024);
    assert(capture::availableFrames(*ring) == 512);
    assert(ring->halUnderrunEvents.load() == 0);

    ring->active.store(1);
    for (int i = 0; i < 4; ++i)
        assert(capture::write(*ring, input.data(), 1024) == 1024);
    assert(capture::suspendStaleCapture(*ring, 4096, 60));
    assert(ring->active.load() == 0);
    assert(capture::availableFrames(*ring) == 0);
    assert(ring->halLastDiscardReason.load() ==
           static_cast<std::uint32_t>(capture::DiscardReason::staleFlush));
    assert(ring->halLastDiscardQueued.load() == 4608);

    capture::initialize(*ring);
    assert(capture::write(*ring, input.data(), 1024) == 1024);
    assert(capture::write(*ring, input.data(), 1024) == 1024);
    assert(capture::trimLiveCapture(*ring, capture::k48MaxLiveQueuedFrames,
                                    capture::k48CapturePrefillFrames, 70) == 0);
    assert(capture::write(*ring, input.data(), 8) == 8);
    assert(capture::trimLiveCapture(*ring, capture::k48MaxLiveQueuedFrames,
                                    capture::k48CapturePrefillFrames, 80) == 1544);
    assert(capture::availableFrames(*ring) == capture::k48CapturePrefillFrames);
    assert(capture::read(*ring, output.data(), 192) == 192);
    assert(output[0] == 520.0f);
    assert(ring->halUnderrunEvents.load() == 0);
}
