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
│   ├── amdtp_receive_ring.h
│   ├── amdtp_transmit_ring.h
│   ├── channel_map.h
│   ├── cmp.h
│   ├── firewire_device.h
│   ├── isoch_allocation.h
│   ├── pcm_buffer.h
│   └── pcm_ring_buffer.h
├── src/
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

The diagnostic `createTone48k()` factory now generates a temporary PCM buffer
and passes it through this same PCM packetization path, so the tone test also
regresses the generic PCM implementation.

## Continuous-source buffer

`PcmRingBuffer` is the producer/consumer layer that will sit between the future
CoreAudio-facing code and the AMDTP packet engine. It stores interleaved frames,
tracks absolute produced/consumed frame counters, supports wraparound, reports
available/free frames, and zero-fills consumer underruns while counting the
number of silenced frames.

The design follows the same separation used by Linux FireWire AMDTP: PCM buffer
position/accounting is kept independent from the isochronous packet queue, and
the AM824 layer consumes PCM frames as packets are prepared. The macOS transport
still uses the proven callback-free prebuilt NuDCL ring; dynamic packet refill is
the next transport milestone and will be connected only after the PCM ring
semantics are validated independently.

Run:

```bash
cd fw410/lib
make check
```

Then validate the transport with the existing playback tools:

```bash
cd ../tools/transport/isoplayback
make
./isoplayback --execute --tone-output 1
```

or the direct PCM-buffer test:

```bash
cd ../pcmbufferplayback
make
./pcmbufferplayback --execute
```

## Planned extraction order

1. channel maps and AM824 packet helpers
2. FireWire device/session wrapper
3. CMP/IRM connection management
4. BeBoB/AV/C discovery and clock/sample-rate control
5. AMDTP receive/transmit transport
6. FW410-specific boot/application-state handling
7. macOS audio-facing layer

`tools/` remains the diagnostic/regression suite during the migration.
