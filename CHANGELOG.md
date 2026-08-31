# Changelog

All notable user-visible changes to macfw releases are recorded here.

The project uses the `x.yy.zzz` version format described in [`RELEASES.md`](RELEASES.md).

## [Unreleased]

### Added

- Native AppKit FW410 control panel installed as `/Applications/macfw FW410 Control.app`.
- Outputs tab with five physical stereo output pairs, Mixer/AUX source selection, independent L/R levels and reusable stereo-link behavior.
- Main Mixer tab exposing the FW410 7-source x 5-bus routing matrix.
- Hardware-validated main-mixer routing for Analog In 1/2, S/PDIF input and all five software-return pairs.
- Multiple simultaneous main-mixer destination assignments.
- CoreAudio/Logic-aligned software-return naming in the GUI despite the FW410 raw AV/C return rotation.
- Production `fw410ctl` main-mixer route commands using the transport-owned control socket.
- Persistent writable FW410 control state via `fw410state`, restored across reboot and physical interface reconnect.
- Control-panel **Reset Defaults** action that applies and records the documented macfw control baseline.
- Dedicated package postinstall diagnostics at `/Library/Logs/macfw_install.log`.
- Complete package/source-install inclusion of the native control-panel application and persistent state helper.
- Explicit narrow `runtime` Makefile target for the binaries required by the installed service/control path.

### Changed

- The FW410 main mixer now establishes a complete known 35-cell macfw-compatible baseline before later differential route writes.
- Main-mixer state is cached in the active transport and is not reconstructed with AV/C mixer STATUS polling.
- Saved main-mixer routes are restored through the same safe full-baseline-first path before other persisted controls.
- Control-state restore runs after native-engine low-level readiness and before the transport is published `ONLINE`.
- Root/subdirectory Makefile targets are normalized: default `make` builds HAL + release runtime + GUI; `make runtime` no longer builds every historical transport probe.
- `sudo make install` verifies prebuilt artifacts and installs HAL, runtime/service and GUI without intentionally compiling as root.
- Package builds include the control-panel application and disable macOS bundle relocation so it remains at `/Applications/macfw FW410 Control.app`.
- Documentation records validated Monterey 12.7.6, Ventura 13.7.8 and Sonoma 14.8.9 coverage.
- The complete `.pkg` lifecycle is hardware-validated through installation, service startup, status, persistent controls, reset, reboot and disconnect/reconnect.

### Fixed

- Corrected main-mixer matrix display orientation in `fw410ctl`.
- Corrected GUI software-return row mapping so Logic/CoreAudio 1/2 through 9/10 correspond to the expected GUI labels.
- Removed non-standard Objective-C GNU shorthand ternaries that produced `-Wgnu-conditional-omitted-operand` warnings.
- Prevented Installer from relocating the control-panel bundle to an existing development copy outside `/Applications`.
- Made package component-plist generation robust when `BundleIsRelocatable` is absent from `pkgbuild --analyze` output.

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
