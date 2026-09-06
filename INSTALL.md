# Installing macfw for the M-Audio FireWire 410

This guide covers the current macfw FW410 alpha runtime for Intel macOS.

> **Alpha software:** this driver is hardware-tested but is not yet a signed/notarized public production release. Back up important work before testing it on another system.

## Requirements

- Intel Mac.
- A hardware-tested macOS release. Current cumulative validation includes Monterey 12.7.6, Ventura 13.7.8, Sonoma 14.8.9 and Sequoia 15.x.
- M-Audio FireWire 410 connected through a working FireWire path.
- Administrator access.

The installer deliberately requires a supported interface to be physically present. The hardware gate accepts the FW410 in either its operational or known bootloader personality.

Apple Silicon is not currently supported.

## Recommended installation: `.pkg`

1. Connect and power on the M-Audio FireWire 410.
2. Obtain the `.pkg` for the release you want to test.
3. Install the package normally in macOS, or from Terminal:

   ```bash
   sudo installer -pkg macfw-fw410-0.03.000-<build>.pkg -target /
   ```

4. The installer validates the connected interface and installs:
   - the CoreAudio HAL plug-in;
   - the transport/control runtime;
   - the persistent control-state helper;
   - exact runtime version/build metadata;
   - the launchd service;
   - **macfw FW410 Control.app** in `/Applications`.
5. The installer loads the launchd service and restarts `coreaudiod`.
6. A reboot is normally **not required**.
7. Open Audio MIDI Setup and select **M-Audio FireWire 410**. Native 44.1 kHz and 48 kHz are supported.
8. Open `/Applications/macfw FW410 Control.app` for the validated hardware controls and diagnostics.

The installed runtime is managed automatically by launchd. You do not need to run `haltransport` manually.

## Installation from source

Clone the repository and build as a normal user:

```bash
git clone https://github.com/mbprado/macfw.git
cd macfw
make
```

Then install the already-built artifacts as root:

```bash
sudo make install
```

Do not run compilation itself with `sudo`. The install targets intentionally verify that the artifacts already exist instead of compiling them as root.

The source installer uses the same supported-device gate as the package installation.

## Build targets

From the repository root:

```bash
make             # HAL + release runtime + GUI
make hal         # HAL only
make runtime     # installed runtime/control binaries only
make gui         # native control-panel application only
make all-tools   # all development/reverse-engineering tools
make package     # fresh release build + complete .pkg installer
make clean
```

The `runtime` target is intentionally narrow. It builds only the binaries used by the installed service/control path instead of compiling all historical probes and experiments.

For GUI-only development:

```bash
make gui
open "fw410/control-panel/build/macfw-fw410-control.app"
```

The internal build bundle deliberately uses a space-free name for reliable GNU make behavior. Installation/package staging renames it to the user-facing application name:

```text
/Applications/macfw FW410 Control.app
```

For transport-only development without replacing the HAL or GUI:

```bash
make runtime
sudo bash fw410/service/install-service.sh
```

## Building a package locally

From the repository root:

```bash
make package
```

`make package` performs a clean rebuild of the release artifacts before packaging them so the embedded build identities match the package commit.

The generated installer is placed under:

```text
package/dist/
```

For example:

```text
package/dist/macfw-fw410-0.03.000-<git-sha>.pkg
```

The package disables bundle relocation so the control application is installed at its authoritative `/Applications/macfw FW410 Control.app` path even when another development copy exists elsewhere on the Mac.

## Installed components

The current installation includes:

```text
/Applications/macfw FW410 Control.app
/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
/Library/Application Support/macfw/fw410/
/Library/Application Support/macfw/fw410/runtime-build.conf
/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist
/Library/Logs/macfw-fw410-transport.log
/Library/Logs/macfw_install.log
```

The launchd service is:

```text
com.mbprado.macfw.fw410.transport
```

The installed control tools include:

```text
/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl
/Library/Application Support/macfw/fw410/tools/control/fw410state/fw410state
```

Persistent control state is stored in:

```text
/Library/Application Support/macfw/fw410/control-state.conf
```

## Control panel

The current control panel provides:

- **Mixer** — 7-source x 5-bus main-mixer routing;
- **Outputs** — Mixer/AUX source, independent L/R levels and stereo link;
- **Headphones** — source, independent L/R level and five mixer-output pair enables;
- **AUX** — software-return/AUX output levels;
- **Inputs** — live Analog In 1/2 and S/PDIF L/R meters;
- **Device** — connection state, active/requested rate, engine PID, CoreAudio buffer state and 44.1/48 kHz selection;
- **Info** — component/runtime build identity, transport diagnostics, Copy Diagnostics and Open Transport Log.

Sample-rate changes from the Device tab use the standard CoreAudio nominal-sample-rate property. The GUI does not call FireWire rate-control probes directly.

## Control architecture and persistence

The GUI and CLI do not open FireWire directly. Both use the transport-owned control socket:

```text
macfw FW410 Control.app / fw410ctl
        |
/tmp/macfw-fw410-control.sock
        |
active transport process
        |
FW410 AV/C
```

This allows hardware controls to coexist with active playback/capture without competing for the FireWire device.

Successful user-facing writable control changes are recorded by `fw410state`. On engine startup/reconnect, saved state is restored after low-level engine readiness. Main-mixer routes are restored first through the validated full 35-cell baseline path, then saved differential routes and other controls are replayed.

The control panel's **Reset Defaults** action applies and records the documented macfw baseline. These are macfw defaults, not a claim about undocumented M-Audio factory state.

See [`fw410/analysis/control-state-persistence.md`](fw410/analysis/control-state-persistence.md) for the detailed restore lifecycle and persisted control set.

## Checking status

For a source checkout:

```bash
fw410/tools/transport/transportstatus/transportstatus
```

Watch transitions continuously with:

```bash
fw410/tools/transport/transportstatus/transportstatus --watch
```

Normal operation reports `ONLINE`. During a physical disconnect or transport recovery it may temporarily report `OFFLINE` or `RECOVERING`.

To inspect the installed launchd service:

```bash
sudo launchctl print system/com.mbprado.macfw.fw410.transport
```

Transport logs:

```text
/Library/Logs/macfw-fw410-transport.log
```

Package post-install diagnostics:

```text
/Library/Logs/macfw_install.log
```

The control panel's **Copy Diagnostics** action is the preferred first support snapshot.

## Sample-rate switching

44.1 kHz and 48 kHz can be selected from either Audio MIDI Setup or the Device tab.

The FW410 needs extra device-specific startup work at 44.1 kHz, including a larger ISO start lead and post-start AV/C rate reassertion. Therefore **48 -> 44.1 kHz normally takes longer than 44.1 -> 48 kHz**. The slower direction is hardware-validated as reliable in the current release candidate.

During the longer 44.1 startup interval the Inputs meters can briefly show unavailable and then return when the engine reports READY. This is intentional.

## Disconnect/reconnect behavior

The CoreAudio device intentionally remains registered if the physical FW410 is disconnected. Applications can keep the same selected audio device while the transport recovers.

While the interface is unavailable:

- playback is accepted by CoreAudio and discarded;
- capture returns silence;
- the CoreAudio endpoint remains present.

When the FW410 returns, the transport supervisor reacquires it, restores saved writable controls and playback/capture resume without requiring the application to reselect the device.

## Main mixer initialization note

The FW410 main-mixer ASIC is not treated like an ordinary read/write register matrix. Hardware testing showed that an isolated mixer CONTROL write against an unknown state can disrupt playback. The production control server therefore establishes a complete known 35-cell mixer baseline on first main-mixer access, caches it, and applies later route changes differentially. Mixer STATUS polling is deliberately avoided.

The GUI presents software-return rows in CoreAudio/Logic order even though the FW410's raw AV/C software-return identities are rotated relative to macfw's AMDTP ordering.

See [`fw410/analysis/original-control-panel-mixer-model.md`](fw410/analysis/original-control-panel-mixer-model.md) for the validated model.

## Uninstalling a source installation

From the repository root:

```bash
sudo make uninstall
```

This removes the launchd runtime, the installed control-panel application and the HAL bundle.

Do not manually remove individual runtime files while the launchd service is active.

## Troubleshooting

### Installer says no supported device is connected

Connect and power on the FW410 and retry. Installation is intentionally blocked when no supported macfw interface is detected.

### Package installation fails

Inspect:

```bash
cat /Library/Logs/macfw_install.log
```

For Installer-level failures that happen before the macfw postinstall script starts:

```bash
tail -n 200 /var/log/install.log
```

### Device is present but audio is unavailable

Check:

```bash
fw410/tools/transport/transportstatus/transportstatus

tail -n 100 /Library/Logs/macfw-fw410-transport.log
```

The launchd supervisor is designed to survive boot without the interface and recover when the FW410 is connected later.

### Control panel is present but controls do not respond

Confirm the transport is `ONLINE`, then test the installed CLI through the same control path:

```bash
"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl" mixer get
```

If the CLI also fails, inspect the transport log/socket path rather than opening the FireWire device with a standalone probe while the transport is active.

### Persistent controls do not return

Inspect:

```bash
"/Library/Application Support/macfw/fw410/tools/control/fw410state/fw410state" show

tail -n 100 /Library/Logs/macfw-fw410-transport.log
```

Do not edit `control-state.conf` manually while diagnosing a live system; use `fw410state`/`fw410ctl` so the normal validation and mixer-safety paths remain in effect.

### Rate switch appears stuck

A normal 48 -> 44.1 transition is slower than the reverse direction, but it should complete reliably. If it enters repeated recovery instead, use **Copy Diagnostics** and inspect the transport log. Native engines are hardened against local socket-client `SIGPIPE` failures in `0.03.000`.

See [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) before reporting a problem.
