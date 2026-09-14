# FW410 protocol research

This directory contains the reconstructed protocol specification for the M-Audio FireWire 410.

The protocol should be documented independently of any particular driver implementation.

## Areas

- `firewire.md` — IEEE 1394 transport behavior
- `bebob.md` — BeBoB-specific behavior
- `firmware.md` — firmware detection, loading and startup
- `audio.md` — audio streaming, sample rates, clocking and channel layout

## Evidence model

Each protocol statement should be classified as:

- **Confirmed** — verified by hardware, packet capture, or unambiguous code analysis.
- **Observed** — captured or observed but not yet fully explained.
- **Inferred** — supported by multiple independent implementations or evidence.
- **Unknown** — requires further investigation.

## Sources

Primary sources should include:

1. Original M-Audio driver analysis
2. Hardware traffic captures
3. Linux FireWire / BeBoB implementation
4. FFADO
5. Other BeBoB device implementations where relevant

Do not assume that behavior from another BeBoB device is identical to the FW410 without validation.
