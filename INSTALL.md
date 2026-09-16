# Installing macfw

This guide covers the M-Audio FireWire 410 and FireWire 1814 alpha drivers, control panels, unified source/package installation and device-specific installer packages for Intel macOS.

> **Alpha software:** this driver is hardware-tested but is not yet a signed/notarized public production release. Back up important work before testing it on another system.

## Requirements

- Intel Mac.
- A hardware-tested macOS release. Current cumulative validation includes Monterey 12.7.6, Ventura 13.7.8, Sonoma 14.8.9 and Sequoia 15.x.
- A supported M-Audio FireWire 410 or FireWire 1814 connected through a working FireWire path.
- Administrator access.

The combined package requires at least one supported interface to be physically present and installs only the connected device stack(s). If both interfaces are connected, both stacks are installed. Each device-specific package requires its matching interface. The hardware gates accept the operational personality and known M-Audio bootloader identities; each runtime retains its stronger model-specific guarded boot procedure.

Apple Silicon is not currently supported.

## Recommended installation: `.pkg`

1. Connect and power on either supported M-Audio interface.
2. Obtain the unified `.pkg` and install it normally, or from Terminal:

   ```bash
   sudo installer -pkg macfw-0.04.003-<build>.pkg -target /
   ```

3. The installer validates the connected interface(s) and installs only the
   matching CoreAudio HAL plug-in(s), transport/control runtime(s), persistent
   state helpers, build metadata, launchd service(s) and native control panel(s).
4. The installer starts the matching device-specific service(s) and restarts
   `coreaudiod`. A reboot is normally **not required**.
5. Select the interface in Audio MIDI Setup and open the corresponding app:

   ```text
   /Applications/macfw FW410 Control.app
   /Applications/macfw FW1814 Control.app
   ```

Both installed runtimes are device-namespaced and can coexist. They are managed
automatically by separate launchd services.

## Installation from source
For a source checkout, install Xcode Command Line Tools if not installed:

```bash
xcode-select --install
```

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

`sudo make install` detects the connected supported interface and installs its
matching stack. If both models are connected, it installs both. To install both
stacks without requiring hardware detection, use:

```bash
sudo make install-force
```

The forced target validates both build trees before installing either stack.
The namespaced `fw410-install` and `fw1814-install` targets retain their
matching-device gates.

## Build targets

From the repository root:

```bash
make             # both devices: HAL + release runtime + GUI
make hal         # both HAL plug-ins
make runtime     # both installed runtime/control sets
make gui         # both native control-panel applications
make all-tools   # both development/reverse-engineering tool sets
sudo make install       # install the detected interface(s)
sudo make install-force # install both stacks without hardware detection
make package     # fresh build + unified two-device .pkg installer
make clean
```

The `runtime` target is intentionally narrow. It builds only the binaries used by the installed service/control path instead of compiling all historical probes and experiments.

Namespaced targets remain available for focused device development:

```bash
make fw410             # FW410 HAL + runtime + native control panel
make fw410-hal
make fw410-runtime
make fw410-gui
make fw410-tools
make fw410-package
sudo make fw410-install
sudo make fw410-uninstall
make fw410-clean

make fw1814             # FW1814 HAL + runtime + native control panel
make fw1814-hal         # FW1814 HAL only
make fw1814-runtime     # FW1814 installed service/control binaries
make fw1814-gui         # FW1814 native control-panel application
make fw1814-tools       # all FW1814 development and diagnostic tools
make fw1814-package     # clean FW1814 build + device-specific .pkg
sudo make fw1814-install
sudo make fw1814-uninstall
make fw1814-clean
```

For GUI-only development:

```bash
make fw410-gui
open "devices/fw410/control-panel/build/macfw-fw410-control.app"

make fw1814-gui
open "devices/fw1814/control-panel/build/macfw-fw1814-control.app"
```

The internal build bundle deliberately uses a space-free name for reliable GNU make behavior. Installation/package staging renames it to the user-facing application name:

```text
/Applications/macfw FW410 Control.app
```

For transport-only development without replacing the HAL or GUI:

```bash
make runtime
sudo bash devices/fw410/service/install-service.sh
```

## Device-specific source installation

The default build covers both interfaces, while the default install selects the
connected model. To work on only one explicitly, use its namespaced targets.
For example:

```bash
make fw1814
```

Then install the already-built FW1814 HAL, supervised runtime and control panel as root:

```bash
sudo make fw1814-install
```

The current FW1814 scope exposes Analog Outputs 1-4 and Analog Inputs 1-8 at 44.1 and 48 kHz. Rate switching, reconnect restoration, persistent analog routing/mixer controls and the native AppKit control panel are hardware-validated. S/PDIF, ADAT, higher rates and MIDI remain under development.

The first routing-control API is available through the transport-owned socket. It reports the exact write-only routing baseline cached by the active engine without issuing new FireWire writes:

```bash
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" routing get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" capabilities get
"/Library/Application Support/macfw/fw1814/bin/fw1814ctl" engine get
```

Do not run standalone FireWire probes while the supervised engine is active; the transport must remain the sole FireWire owner.

## Building a package locally

From the repository root:

```bash
make package          # unified two-device package
make package-all      # alias for the unified two-device package
make fw410-package    # optional FW410-only package
make fw1814-package   # optional FW1814-only package
```

Each package target performs a clean rebuild before packaging, so embedded
build identities match the package commit. The individual-package builder also
accepts `bash package/build-pkg.sh fw410|fw1814` or the `MACFW_DEVICE`
environment variable.

The generated installer is placed under:

```text
package/dist/
```

For example:

```text
package/dist/macfw-0.04.003-<git-sha>.pkg
package/dist/macfw-fw410-0.04.003-<git-sha>.pkg
package/dist/macfw-fw1814-0.04.003-<git-sha>.pkg
```

Packages disable bundle relocation so each selected control application is installed at its authoritative `/Applications` path even when development copies exist elsewhere on the Mac.

## Installed components

Depending on which interface(s) were connected, the installation includes the
corresponding FW410 and/or FW1814 paths below:

```text
/Applications/macfw FW410 Control.app
/Applications/macfw FW1814 Control.app
/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
/Library/Audio/Plug-Ins/HAL/macfw-fw1814.driver
/Library/Application Support/macfw/fw410/
/Library/Application Support/macfw/fw1814/
/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist
/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist
/Library/Logs/macfw-fw410-transport.log
/Library/Logs/macfw-fw1814-transport.log
/Library/Logs/macfw_install.log
```

The launchd services are:

```text
com.mbprado.macfw.fw410.transport
com.mbprado.macfw.fw1814.transport
```

The installed control tools include:

```text
/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl
/Library/Application Support/macfw/fw410/tools/control/fw410state/fw410state
/Library/Application Support/macfw/fw1814/bin/fw1814ctl
/Library/Application Support/macfw/fw1814/bin/fw1814state
```

Persistent control state is stored in:

```text
/Library/Application Support/macfw/fw410/control-state.conf
/Library/Application Support/macfw/fw1814/control-state.conf
```

## Control panel

The **FW410** control panel provides:

- **Mixer** — 7-source x 5-bus main-mixer routing;
- **Outputs** — Mixer/AUX source, independent L/R levels and stereo link;
- **Headphones** — source, independent L/R level and five mixer-output pair enables;
- **AUX** — software-return/AUX output levels;
- **Inputs** — live Analog In 1/2 and S/PDIF L/R meters;
- **Device** — connection state, active/requested rate, engine PID, CoreAudio buffer state and 44.1/48 kHz selection;
- **Info** — component/runtime build identity, transport diagnostics, Copy Diagnostics and Open Transport Log.

The **FW1814** panel instead covers its two software-return pairs, four
analog input pairs, two analog output pairs, both headphone outputs and the
AUX bus. Its physical headphone encoders adjust saved volume and the panel
updates the sliders while open. Both panels use the standard CoreAudio
nominal-sample-rate property for Device-tab rate changes rather than calling
FireWire rate-control probes directly.

## Control architecture and persistence

The GUI and CLI do not open FireWire directly. Each interface uses its own
transport-owned control socket. For FW410:

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

For FW1814, `fw1814ctl` and its control panel use
`/tmp/macfw-fw1814-control.sock`; `fw1814state` saves validated controls,
including gain changes made by the physical headphone encoders. For FW410,
successful user-facing writable control changes are recorded by `fw410state`.
On engine startup/reconnect, each service restores its own saved state after
low-level engine readiness. FW410 main-mixer routes are restored first through
the validated full 35-cell baseline path, then saved differential routes and
other controls are replayed.

The control panel's **Reset Defaults** action applies and records the documented macfw baseline. These are macfw defaults, not a claim about undocumented M-Audio factory state.

See [`devices/fw410/analysis/control-state-persistence.md`](devices/fw410/analysis/control-state-persistence.md) for the detailed restore lifecycle and persisted control set.

## Checking status

For an FW410 source checkout:

```bash
devices/fw410/tools/transport/transportstatus/transportstatus
```

Watch transitions continuously with:

```bash
devices/fw410/tools/transport/transportstatus/transportstatus --watch
```

Normal FW410 operation reports `ONLINE`. During a physical disconnect or transport recovery it may temporarily report `OFFLINE` or `RECOVERING`.

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

The appropriate interface's control-panel diagnostics and transport log are
the preferred first support snapshot. The FW1814 service label is
`com.mbprado.macfw.fw1814.transport` and its log is
`/Library/Logs/macfw-fw1814-transport.log`.

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

See [`devices/fw410/analysis/original-control-panel-mixer-model.md`](devices/fw410/analysis/original-control-panel-mixer-model.md) for the validated model.

## Uninstalling a source installation

From the repository root:

```bash
sudo make uninstall          # remove both interface stacks
sudo make fw410-uninstall
sudo make fw1814-uninstall
```

Each device-specific target removes only that device's launchd runtime,
control-panel application and HAL bundle, so the other interface remains installed.

Do not manually remove individual runtime files while the launchd service is active.

## Troubleshooting

### Installer says no supported device is connected

Connect and power on the interface matching the selected package and retry. Installation is intentionally blocked when that package cannot detect a supported target.

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
devices/fw410/tools/transport/transportstatus/transportstatus

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
