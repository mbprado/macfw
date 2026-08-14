# FW410 reusable backend

This directory is the reusable user-space backend being extracted from the
proven FW410 diagnostic tools.

The extraction is deliberately incremental. The working tools remain the
regression suite while reusable pieces move here one subsystem at a time.

## Current layout

```text
lib/
├── include/macfw/
│   ├── am824.h
│   ├── am824_playback.h
│   ├── amdtp_packet.h
│   ├── amdtp_pcm_stream.h
│   ├── amdtp_receive_ring.h
│   ├── amdtp_transmit_ring.h
│   ├── channel_map.h
│   ├── cmp.h
│   ├── firewire_device.h
│   ├── isoch_allocation.h
│   ├── pcm_buffer.h
│   └── pcm_ring_buffer.h
├── src/
│   ├── amdtp_pcm_stream.cpp
│   ├── amdtp_receive_ring.cpp
│   ├── amdtp_transmit_ring.cpp
│   ├── cmp.cpp
│   ├── firewire_device.cpp
│   ├── isoch_allocation.cpp
│   ├── pcm_ring_buffer.cpp
│   └── smoke.cpp
└── Makefile
```

## Extracted so far

- confirmed FW410 48 kHz capture channel map
- physically validated 48 kHz playback/output map
- proven AM824 MBLA24 capture decode/statistics helper
- reusable FireWire device/session, CMP/IRM and AMDTP RX/TX transport
- callback-free prebuilt 48 kHz playback ring
- reusable interleaved PCM-buffer packetization path
- single-producer/single-consumer PCM ring buffer for continuous sources
- reusable cycle-driven live TX half-ring refill scheduler
- standalone compile/smoke check

## PCM playback contract

`PcmBufferView` is a non-owning view over interleaved signed PCM frames. Samples
are stored in `int32_t`, but `AmdtpTransmitRing` clips them to the signed 24-bit
range used by AM824 MBLA (`-8388608..8388607`).

At 48 kHz, channels map directly to the FW410's ten PCM stream positions in
zero-based order: PCM buffer channel 0 is stream position 1, channel 1 is stream
position 2, and so on. A buffer may expose between 1 and 10 channels. Missing
stream channels are transmitted as digital zero. If the buffer runs out of
frames, playback is zero-filled unless `loop` is set, in which case frames wrap.

`AmdtpTransmitRing::createPcm48k()` consumes and copies the source samples while
constructing its prebuilt NuDCL ring; the source buffer does not need to remain
alive afterward.

The diagnostic `createTone48k()` factory generates a temporary PCM buffer and
passes it through this same PCM packetization path, so the tone test also
regresses the generic PCM implementation.

## Continuous PCM streaming

`PcmRingBuffer` is the producer/consumer layer intended to sit between the
future CoreAudio-facing code and the AMDTP packet engine. It stores interleaved
frames, tracks absolute produced/consumed frame counters, supports wraparound,
reports available/free frames, and zero-fills consumer underruns while counting
the number of silenced frames.

`AmdtpPcmStream48k` is the live TX refill scheduler. It does not own the FireWire
channel and does not require a NuDCL completion callback. The caller supplies
observed IEEE 1394 cycle numbers; the scheduler tracks progress from the planned
TX start, determines when a 64-packet half-ring has been consumed, and refills
only the half DMA has just left behind from `PcmRingBuffer`. The NuDCL program,
packet lengths, CIP timing, DBC and SYT remain unchanged while only the mmap-backed
AM824 PCM payload words are replaced.

The separation is inspired by the Linux FireWire AMDTP model—PCM accounting is
kept independent from the isochronous packet queue—but the running refill
mechanism is macOS-specific and uses the behavior validated with IOFireWireLib.

Run:

```bash
cd fw410/lib
make check
```

Then validate the live path:

```bash
cd ../tools/transport/pcmstreamplayback
make
./pcmstreamplayback --execute
```

The test should continuously alternate 440 Hz and 880 Hz on Analog Output 1
while reporting zero PCM underrun/silence frames under normal operation.

## Planned extraction order

1. channel maps and AM824 packet helpers
2. FireWire device/session wrapper
3. CMP/IRM connection management
4. BeBoB/AV/C discovery and clock/sample-rate control
5. AMDTP receive/transmit transport
6. FW410-specific boot/application-state handling
7. macOS audio-facing layer

`tools/` remains the diagnostic/regression suite during the migration.
