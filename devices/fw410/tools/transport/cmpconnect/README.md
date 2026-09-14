# cmpconnect

Guarded IEC 61883 CMP connection experiment for the operational M-Audio FireWire 410.

This tool is deliberately separate from the read-only `cmpprobe`.

## Default mode

```bash
./cmpconnect
```

Default mode is a dry run. It opens the FW410, reads `oPCR[0]` and `iPCR[0]`, verifies that both plugs are online and unused, and prints the 48 kHz AMDTP payload sizes that would be reserved.

No isochronous resources are allocated and no PCR is modified.

## Execute mode

```bash
./cmpconnect --execute
```

The first implementation is intentionally restricted to the currently confirmed 48 kHz formation:

- device OUTPUT / host capture: 5 AMDTP positions = 4 PCM + MIDI, max payload 128 bytes per cycle including the 8-byte CIP header
- device INPUT / host playback: 11 AMDTP positions = 10 PCM + MIDI, max payload 272 bytes per cycle including the 8-byte CIP header

Execution performs this sequence:

1. Re-read and validate both PCR0 registers.
2. Create two `IOFireWireIsochChannel` objects with automatic IRM allocation enabled (`doIrm=true`).
3. Reserve one FireWire isochronous channel/bandwidth allocation for capture and one for playback at S400.
4. Atomically establish the FW410's `oPCR[0]` and `iPCR[0]` point-to-point CMP connections using compare/swap.
5. Read both PCRs back and print the assigned FireWire channels.
6. **Do not call `Start()` and do not transmit any AMDTP packets.**
7. After a short hold, clear the point-to-point connection bits.
8. Release both IRM allocations and print the final PCR state.

The FW410 is known from Linux `snd-bebob` to require both CMP directions for real streaming, so this experiment establishes both together.

## Safety / current scope

- `--execute` modifies FireWire bus resource allocation and the FW410's PCR connection state temporarily.
- It refuses to proceed if either PCR0 is offline or already connected.
- PCR updates use FireWire compare/swap so they fail rather than overwrite a concurrently changed register.
- Cleanup attempts to clear any connection established by this process and release both IRM allocations even if a later step fails.
- No audio packet transmission occurs in this version.
- This initial version assumes the already-confirmed 48 kHz stream formation. Do not use `--execute` after changing the device sample rate; add rate-aware payload selection first.
