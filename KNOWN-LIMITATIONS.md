# Known limitations

This file records limitations and open items for the first macfw M-Audio FireWire 410 alpha release.

## Platform scope

- **Intel Macs only.** Apple Silicon is not currently a project target.
- Hardware validation now includes clean/package tests on **macOS Monterey 12.7.6** and **macOS Sonoma 14.8.9**.
- **Ventura 13.x remains untested.** Its position between two tested releases is not sufficient evidence to mark it validated.
- Hardware validation is still concentrated on one development Mac/FW410 combination. A broader Mac / FireWire-adapter / firmware matrix is still needed.
- See [`COMPATIBILITY.md`](COMPATIBILITY.md) for the current tested matrix.

## Supported hardware

- The only supported audio interface in the first release is the **M-Audio FireWire 410**.
- The installer recognizes the known FW410 operational and bootloader personalities.
- Installation currently requires a supported interface to be connected. This is intentional and provides the foundation for future device-specific installers.

## Audio formats

- Native **44.1 kHz** and **48 kHz** are supported.
- Higher sample rates are not currently exposed/supported.
- Playback exposes Analog Out 1-8 plus S/PDIF L/R.
- Capture exposes Analog In 1-2 plus S/PDIF L/R.

## Latency

Capture currently uses a deliberately conservative 4,096-frame prefill to prioritize correctness and recovery stability. This produces noticeable software-monitoring latency and has not yet been tuned for low-latency production use.

## 44.1 kHz lifecycle

The FW410 requires a device-specific 44.1 kHz startup sequence, including an AV/C rate reassertion after duplex streaming starts.

Earlier development testing occasionally produced broken 44.1 kHz audio/capture. Removing the unconditional clean-stop reset to 48 kHz reduced this occurrence significantly to practically zero in subsequent testing. The historical failure remains documented until broader hardware testing establishes that the lifecycle is fully robust across machines and interfaces.

## Sleep/wake coverage

Sleep/wake has been hardware-validated on macOS Sonoma 14.8.9: playback and capture resumed normally after wake, and the FW410 remained in its operational personality rather than falling back to its bootloader personality.

This is encouraging and improves on the observed behavior of the original vendor driver on the same hardware, but broader testing across different Macs, sleep durations and FireWire adapter chains is still needed before treating the behavior as universally guaranteed.

## Offline behavior

The CoreAudio endpoint intentionally remains present when the physical FW410 is disconnected or the transport is recovering.

During that period:

- playback is discarded;
- capture returns silence;
- an application such as Logic can continue its timeline/recording;
- normal audio resumes when transport returns `ONLINE`.

This is intentional behavior, not an indication that macOS still sees the physical FireWire device.

## Controls and mixer

The complete FW410 mixer/control surface is not yet implemented. Basic control/mixer probing exists, but the first alpha is focused on reliable audio transport and CoreAudio integration.

## MIDI

MIDI transport is not yet validated as a supported user-facing feature. The FireWire stream topology includes a MIDI position, but full MIDI byte transport/integration remains future work.

## Signing and notarization

The initial alpha packaging work is focused on reproducible installation and hardware validation. Public distribution signing/notarization is a separate release-hardening step. Release notes must clearly identify whether a particular package is signed/notarized.

## Package and compatibility testing

The `.pkg` installer has been validated for immediate operation after installation without reboot. A completely fresh **Monterey 12.7.6** installation worked as expected with the packaged driver. The same installer is also fully functionally validated on **Sonoma 14.8.9**, including playback, capture and sleep/wake recovery.

Reboot recovery, delayed hardware attachment, sample-rate switching, physical disconnect/reconnect, and launchd process restart have also been validated during development testing.

This does not yet constitute a broad compatibility guarantee across all Intel Mac models, FireWire adapters, macOS versions, or FW410 hardware revisions.

## Diagnostics

Transport diagnostics and reverse-engineering tools are primarily developer/tester interfaces. Their output and command-line contracts may change during the 0.x development series.

## Reporting issues

Useful reports should include:

- macOS version;
- exact Intel Mac model;
- FireWire connection/adapters used;
- FW410 behavior at 44.1 or 48 kHz;
- whether the problem occurs after install, boot, rate switch, disconnect/reconnect, sleep/wake, or normal streaming;
- relevant `/Library/Logs/macfw-fw410-transport.log` output;
- transport status output where available.
