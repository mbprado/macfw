# isocapture

`isocapture` is the first user-space FireWire isochronous receive experiment for the M-Audio FireWire 410.

It is intentionally restricted to the already-confirmed 48 kHz operational formation.

## Safety model

By default the tool is a dry run:

```sh
./isocapture
```

It only checks that both FW410 PCR0 plugs are online and unused.

The actual experiment requires:

```sh
./isocapture --execute
```

Optional raw payload display:

```sh
./isocapture --execute --raw
```

## What `--execute` does

1. Reads and saves the exact original oPCR0/iPCR0 values.
2. Creates a finite NuDCL receive program for 64 capture packets.
3. Creates a local macOS isochronous receive port.
4. Allocates IRM resources for both FW410 directions, matching the FW410 requirement that both connections exist.
5. Establishes CMP on device OUTPUT plug 0 (host capture) and device INPUT plug 0 (host playback).
6. Starts only the host capture isochronous channel.
7. Captures a short burst of FW410 -> Mac packets.
8. Prints packet length/tag/sy plus decoded CIP fields (`SID`, `DBS`, `DBC`, `FMT`, `FDF`, `SYT`).
9. Stops capture, restores both PCRs to their exact pre-test values, and releases both IRM allocations.

No CoreAudio device is created, no playback AMDTP packets are transmitted, and no mixer/control state is changed.

## Expected 48 kHz capture formation

From earlier BridgeCo discovery, the FW410 -> Mac stream contains five AMDTP positions:

1. S/PDIF L
2. Line L
3. S/PDIF R
4. Line R
5. MIDI

The maximum payload used for IRM allocation is 128 bytes:

`5 positions * 6 frames * 4 bytes + 8-byte CIP header`

The first successful packet capture should let us validate the FW410's live CIP/AMDTP framing before implementing a persistent streaming engine or CoreAudio integration.
