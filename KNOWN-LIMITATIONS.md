# Known limitations

This file records limitations and open items for the current macfw M-Audio FireWire 410 alpha release.

## Platform scope

- **Intel Macs only.** Apple Silicon is not currently a project target.
- Hardware validation includes **macOS Monterey 12.7.6**, **Ventura 13.7.8**, **Sonoma 14.8.9**, and **Sequoia 15.x**.
- Hardware validation is still concentrated on a small number of Mac/FW410 combinations. A broader Mac / FireWire-adapter / firmware matrix is still needed.
- See [`COMPATIBILITY.md`](COMPATIBILITY.md) for the cumulative tested matrix.

## Supported hardware

- The only supported audio interface is the **M-Audio FireWire 410**.
- The installer recognizes the known FW410 operational and bootloader personalities.
- Installation currently requires a supported interface to be connected. This is intentional and provides the foundation for future device-specific installers.

## Audio formats

- Native **44.1 kHz** and **48 kHz** are supported.
- Higher sample rates are not currently exposed/supported.
- Playback exposes Analog Out 1-8 plus S/PDIF L/R.
- Capture exposes Analog In 1-2 plus S/PDIF L/R.

## Latency

Capture now uses a hardware-validated **256-frame prefill**, approximately 5.8 ms at 44.1 kHz and 5.3 ms at 48 kHz. Physical loopback with Logic software monitoring showed excellent subjective round-trip latency on the development system.

This prefill is only one internal buffering layer and must not be confused with complete CoreAudio or end-to-end latency.

The HAL does not yet publish calibrated `kAudioDevicePropertyLatency`, `kAudioDevicePropertySafetyOffset`, or stream-latency values. Those internal properties currently remain zero placeholders, and the control panel deliberately displays **Not reported by HAL** instead of presenting them as real measurements.

## 44.1 kHz lifecycle and rate-switch timing

The FW410 requires a device-specific 44.1 kHz startup sequence, including a larger rate-specific ISO start lead and an AV/C rate reassertion after duplex streaming starts.

The current 44.1 path is hardware-validated as stable and reliable in normal launchd-managed operation. However, **48 -> 44.1 kHz switching takes noticeably longer than 44.1 -> 48 kHz** because the slow direction performs additional settle/readback, startup-lead and post-start reassert work before reporting READY.

The slower direction is accepted for the current release because repeated switching from both Audio MIDI Setup and the macfw Device tab completes reliably. Timing optimization is deferred until it can be instrumented without disturbing the stable 44.1 sequence.

A release-candidate failure where 44.1 could fall into repeated recovery was traced to a local Unix-socket `SIGPIPE` race: a GUI/control client could time out and disappear before the native engine replied. The transport now ignores `SIGPIPE`, and the 44.1 meter listener is not exposed until the startup/reassert window is complete. Repeated hardware testing after that fix showed reliable switching.

## Sleep/wake coverage

Sleep/wake has been hardware-validated on Ventura 13.7.8, Sonoma 14.8.9 and Sequoia 15.x. Playback and capture resumed normally after wake in the validated tests.

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

A substantial part of the FW410 control surface is implemented and hardware-validated:

- headphone source/level and five-pair mixer routing;
- AUX levels;
- physical output Mixer/AUX selection and L/R levels;
- 7-source x 5-bus main-mixer route assignments;
- native AppKit GUI with Mixer, Outputs, Headphones, AUX, Inputs, Device and Info/Diagnostics tabs;
- live Analog In 1/2 and S/PDIF L/R meters;
- 44.1/48 kHz selection through the standard CoreAudio/HAL lifecycle;
- persistence of currently writable production controls across reboot and interface reconnect;
- Reset Defaults to the documented macfw baseline.

The full original mixer strip feature set is **not yet complete**. Remaining work includes strip level, pan/balance, mute/solo and AUX-send behavior where those semantics are confirmed.

The main mixer also has a device-specific state-management limitation: isolated route writes against an unknown matrix are unsafe. macfw must first establish a complete known 35-cell baseline, cache it in the transport process and then apply later route changes differentially. Mixer STATUS polling is intentionally avoided.

Persistent state follows the same safety model: saved main-mixer routes are restored through the full known baseline before differential route replay. The persisted reset baseline is a macfw-defined default, not a claim about undocumented M-Audio factory defaults.

The GUI currently invokes `fw410ctl` subprocesses as its proven backend boundary for established hardware-control actions. This is functional and hardware-validated, but direct socket IPC may be a future efficiency cleanup.

## S/PDIF control coverage

The S/PDIF output/control backend is decoded and represented in the GUI, but some S/PDIF-specific physical-output validation remains less extensive than the eight analog output-channel tests. Broader digital-I/O testing is still useful.

## MIDI

MIDI transport is not yet validated as a supported user-facing feature. The FireWire stream topology includes a MIDI position, but full MIDI byte transport/integration remains future work.

## Signing and notarization

The alpha packaging work is focused on reproducible installation and hardware validation. Public distribution signing/notarization remains a separate release-hardening step. Release notes must clearly identify whether a particular package is signed/notarized.

## Package and compatibility testing

The `.pkg` installer has been validated for immediate operation after installation without reboot. A completely fresh **Monterey 12.7.6** installation worked as expected with the packaged driver. Ventura 13.7.8, Sonoma 14.8.9 and Sequoia 15.x are also functionally validated in the cumulative compatibility matrix.

The `0.03.000` release-candidate source/install/package path was revalidated on the development system after the low-latency scheduler, Device/Inputs/Diagnostics GUI work and 44.1 rate-switch hardening. `make`, `sudo make install`, and `make package` all completed correctly, and the final functional regression did not expose regressions in the previously validated control/audio paths.

The package/source install paths include the native control-panel application, persistent state helper, HAL, launchd/runtime components and runtime build metadata.

Reboot recovery, delayed hardware attachment, sample-rate switching, physical disconnect/reconnect, sleep/wake on the recorded systems, and launchd process restart have all been validated during development testing.

This does not constitute a broad compatibility guarantee across all Intel Mac models, FireWire adapters, macOS versions, or FW410 hardware revisions.

## Diagnostics

The control panel's **Copy Diagnostics** action is the preferred first support snapshot for the current release. It includes the visible Info state, full transport status and recent transport log output.

Transport diagnostics and reverse-engineering tools are primarily developer/tester interfaces. Their output and command-line contracts may change during the 0.x development series.

Package postinstall diagnostics are written to `/Library/Logs/macfw_install.log`; Installer-level failures that occur before the macfw postinstall script starts may still require `/var/log/install.log`.

## Reporting issues

Useful reports should include:

- macOS version;
- exact Intel Mac model;
- FireWire connection/adapters used;
- FW410 behavior at 44.1 or 48 kHz;
- whether the problem occurs after install, boot, rate switch, disconnect/reconnect, sleep/wake, normal streaming, or a control change;
- whether persistent controls or Reset Defaults are involved;
- Copy Diagnostics output where available;
- relevant `/Library/Logs/macfw-fw410-transport.log` output;
- `/Library/Logs/macfw_install.log` for package-install problems;
- transport status output where available.
