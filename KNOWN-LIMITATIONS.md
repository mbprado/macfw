# Known limitations

This file records limitations and open items for the current macfw M-Audio FireWire 410 alpha release.

## Platform scope

- **Intel Macs only.** Apple Silicon is not currently a project target.
- Hardware validation includes **macOS Monterey 12.7.6**, **Ventura 13.7.8**, and **Sonoma 14.8.9**.
- Hardware validation is still concentrated on a small number of Mac/FW410 combinations. A broader Mac / FireWire-adapter / firmware matrix is still needed.
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

Sleep/wake has been hardware-validated on Ventura 13.7.8 and Sonoma 14.8.9. Playback and capture resumed normally after wake in the validated tests.

On Sonoma, the FW410 remained in its operational personality rather than falling back to its bootloader personality during the validated sleep/wake test.

Broader testing across different Macs, sleep durations and FireWire adapter chains is still needed before treating this behavior as universally guaranteed.

## Offline behavior

The CoreAudio endpoint intentionally remains present when the physical FW410 is disconnected or the transport is recovering.

During that period:

- playback is discarded;
- capture returns silence;
- an application such as Logic can continue its timeline/recording;
- normal audio resumes when transport returns `ONLINE`.

This is intentional behavior, not an indication that macOS still sees the physical FireWire device.

## Controls and mixer

A substantial part of the FW410 control surface is now implemented and hardware-validated:

- headphone source/level and five-pair mixer routing;
- AUX levels;
- physical output Mixer/AUX selection and L/R levels;
- 7-source x 5-bus main-mixer route assignments;
- native AppKit GUI for Mixer, Outputs, Headphones, AUX and Info.

The full original mixer strip feature set is **not yet complete**. Remaining work includes controls such as strip level, pan/balance, mute/solo and AUX-send behavior where those semantics are confirmed.

The main mixer also has a device-specific state-management limitation: isolated route writes against an unknown matrix are unsafe. macfw must first establish a complete known 35-cell baseline, cache it in the transport process and then apply later route changes differentially. Mixer STATUS polling is intentionally avoided.

The GUI currently invokes `fw410ctl` subprocesses as its proven backend boundary. This is functional but not the final efficiency target; mixer refresh can later be optimized to fetch the cached matrix in one operation.

## S/PDIF control coverage

The S/PDIF output/control backend is decoded and represented in the GUI, but some S/PDIF-specific physical-output validation remains less extensive than the eight analog output-channel tests. Broader digital-I/O testing is still useful.

## MIDI

MIDI transport is not yet validated as a supported user-facing feature. The FireWire stream topology includes a MIDI position, but full MIDI byte transport/integration remains future work.

## Signing and notarization

The initial alpha packaging work is focused on reproducible installation and hardware validation. Public distribution signing/notarization is a separate release-hardening step. Release notes must clearly identify whether a particular package is signed/notarized.

## Package and compatibility testing

The `.pkg` installer has been validated for immediate operation after installation without reboot. A completely fresh **Monterey 12.7.6** installation worked as expected with the packaged driver. Ventura 13.7.8 and Sonoma 14.8.9 are also functionally validated.

The package/source install paths now include the native control-panel application in addition to the HAL and launchd/runtime components.

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
- whether the problem occurs after install, boot, rate switch, disconnect/reconnect, sleep/wake, normal streaming, or a control change;
- relevant `/Library/Logs/macfw-fw410-transport.log` output;
- transport status output where available.
