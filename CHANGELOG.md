# Changelog

All notable user-visible changes to macfw releases are recorded here.

The project uses the `x.yy.zzz` version format described in [`RELEASES.md`](RELEASES.md).

## [Unreleased]

### Release infrastructure

- Prepare tag-driven GitHub release automation.
- Add public release notes and final compatibility checklist for the first alpha.
- Add signing/notarization when Apple distribution credentials are available.

## [0.01.000] — first alpha candidate

### Added

- M-Audio FireWire 410 discovery in both operational and known bootloader personalities.
- Guarded FW410 boot-from-flash recovery.
- User-space FireWire asynchronous, AV/C, CMP, CIP/AMDTP and NuDCL transport.
- Native 44.1 kHz and 48 kHz full-duplex audio engines.
- CoreAudio AudioServerPlugIn exposing the FW410 as a normal macOS audio device.
- 10 CoreAudio playback channels: Analog Out 1-8 and S/PDIF L/R.
- 4 CoreAudio capture channels: Analog In 1-2 and S/PDIF L/R.
- Simultaneous playback, capture and software monitoring.
- Runtime 44.1 <-> 48 kHz sample-rate switching.
- Versioned transport-status ABI with `OFFLINE`, `RECOVERING`, and `ONLINE` states.
- Explicit native-engine READY handshake before transport is published `ONLINE`.
- Persistent CoreAudio endpoint across physical FireWire disconnect/reconnect.
- Offline capture silence and playback discard while preserving application device selection.
- Automatic FireWire generation-change detection and reconnect recovery.
- launchd-managed `haltransport` runtime with automatic process restart.
- Boot without hardware followed by delayed FW410 attachment and automatic recovery.
- `deviceprobe --require-supported` installation hardware gate.
- Version/build metadata embedded in runtime tools and HAL bundle.
- Quiet default transport logging with opt-in verbose/debug runtime diagnostics.
- Root repository build orchestration: `make`, `make install`, `make uninstall`, `make clean`, and `make package`.
- Root-level `.pkg` packaging infrastructure under `package/`.
- `.pkg` installation validated to bring the interface online immediately without reboot on the development system.

### Changed

- 44.1 kHz clean stop now leaves the FW410 at 44.1 kHz rather than unconditionally restoring 48 kHz.
- Redundant initial AV/C sample-rate CONTROL is skipped when the interface already reports the requested rate.
- Release/runtime builds are separated from historical and experimental tool builds.
- Installation no longer compiles as root; artifacts are built as the normal user before `sudo make install`.

### Fixed

- 44.1 kHz startup reliability improved substantially by removing the development-era forced clean-stop 48 kHz restore.
- Persistent transport-status shared-memory handling survives supervisor restart and Darwin page-sized backing objects.
- HAL-first startup creates a compatible persistent offline status object, removing supervisor/HAL startup-order dependency.
- launchd runtime packaging now includes all required rate-control/runtime dependencies.
- Transport restart no longer requires a macOS reboot after the persistent status lifecycle fixes.

### Known limitations

See [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md).
