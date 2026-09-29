# FW1814 in macfw 0.05.000 — Alpha

The primary [unified release notes](../../RELEASE-NOTES.md) cover packaging, compatibility and installation. This page summarizes the FW1814 scope.

Analog Outputs 1–4 and Inputs 1–8 have hardware-tested full-duplex operation at 44.1, 48, 88.2 and 96 kHz. Analog 176.4 and 192 kHz playback and two-channel capture are available as experimental modes. All six analog engines use guarded rolling TX and accept persistent Aggressive, Balanced and Conservative live service profiles. Firmware boot recovery, rate switching, reconnect and saved analog mixer/headphone control are included.

At 48 kHz, stale capture is discarded when a client starts and active capture is bounded to 2,048 frames. One clean 40-probe run returned 39 impulses at 11.98–16.31 ms; a loaded run returned all 40 at 16.48–27.48 ms. Quad-rate qualification can occasionally retry. Small capture artifacts and DBC gaps were observed under host load. The GUI now waits for engine and restored-control readiness after a rate change before refreshing its socket-backed controls. CoreAudio latency properties are provisional estimates.

Build `make fw1814` then `sudo make fw1814-install`, or use the combined `macfw-0.05.000-<build>.pkg` or focused `macfw-fw1814-0.05.000-<build>.pkg`. The package requires connected supported hardware. The bundled FW1814 control panel installs under `/Applications/macfw FW1814 Control.app`. See [INSTALL.md](../../INSTALL.md) for complete instructions and [high-rate development](analysis/high-rate-development.md) for the experimental startup history.

The package is unsigned and unnotarized. Intel Macs using Apple's legacy FireWire stack are the current target. Apple Silicon and Tahoe 26 are unsupported. S/PDIF, ADAT and MIDI are deferred. The [compatibility matrix](../../COMPATIBILITY.md) is cumulative and does not imply high-rate validation on every macOS version.
