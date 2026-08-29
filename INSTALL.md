# Installing macfw for the M-Audio FireWire 410

This guide covers the current macfw FW410 alpha runtime for Intel macOS.

> **Alpha software:** this driver has been hardware-tested on the development Intel Mac, but it is not yet a signed/notarized public production release. Back up important work before testing it on another system.

## Requirements

- Intel Mac.
- A hardware-tested macOS release. Current validation includes Monterey 12.7.6, Ventura 13.7.8 and Sonoma 14.8.9.
- M-Audio FireWire 410 connected through a working FireWire path.
- Administrator access.

The current installer deliberately requires a supported interface to be physically present. The hardware gate accepts the FW410 in either its operational or known bootloader personality.

Apple Silicon is not currently supported.

## Recommended installation: `.pkg`

1. Connect and power on the M-Audio FireWire 410.
2. Obtain the `.pkg` for the release you want to test.
3. Install the package normally in macOS, or from Terminal:

   ```bash
   sudo installer -pkg macfw-fw410-0.01.000-<build>.pkg -target /
   ```

4. The installer validates the connected interface and installs:
   - the CoreAudio HAL plug-in;
   - the transport/control runtime;
   - the launchd service;
   - **macfw FW410 Control.app** in `/Applications`.
5. The installer loads the launchd service and restarts `coreaudiod`.
6. A reboot is normally **not required**. Hardware validation confirmed that the FW410 can become usable immediately after installation.
7. Open Audio MIDI Setup and select **M-Audio FireWire 410**. Native 44.1 kHz and 48 kHz are currently supported.
8. Open `/Applications/macfw FW410 Control.app` for the validated hardware controls.

The installed runtime is managed automatically by launchd. You do not need to run `haltransport` manually.

## Installation from source

Clone the repository and build as a normal user:

```bash
git clone https://github.com/mbprado/macfw.git
cd macfw
make
```

The default build now produces the installable HAL bundle, the release runtime/control binaries and the native control-panel application.

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
make package     # complete .pkg installer
make clean
```

The `runtime` target is intentionally narrow. It builds only the binaries used by the installed service/control path instead of compiling all historical probes and experiments.

For GUI-only development:

```bash
make gui
open "fw410/control-panel/build/macfw FW410 Control.app"
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

`make package` builds the complete installable set first and packages the HAL, runtime/service and control panel.

The generated installer is placed under:

```text
package/dist/
```

For example:

```text
package/dist/macfw-fw410-0.01.000-<git-sha>.pkg
```

## Installed components

The current installation includes:

```text
/Applications/macfw FW410 Control.app
/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
/Library/Application Support/macfw/fw410/
/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist
/Library/Logs/macfw-fw410-transport.log
```

The launchd service is:

```text
com.mbprado.macfw.fw410.transport
```

The installed control CLI is:

```text
/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl
```

## Control architecture

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

## Checking status

For a source checkout, the diagnostic status reader is:

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

Transport logs are written to:

```text
/Library/Logs/macfw-fw410-transport.log
```

## Disconnect/reconnect behavior

The CoreAudio device intentionally remains registered if the physical FW410 is disconnected. Applications can keep the same selected audio device while the transport recovers.

While the interface is unavailable:

- playback is accepted by CoreAudio and discarded;
- capture returns silence;
- the CoreAudio endpoint remains present.

When the FW410 returns, the transport supervisor reacquires it and playback/capture resume without requiring the application to reselect the device. This behavior has been hardware-validated in Logic Pro.

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

For packaged alpha builds, use the project-provided uninstall path when one is included with that release. Do not manually remove individual runtime files while the launchd service is active.

## Troubleshooting

### Installer says no supported device is connected

Connect and power on the FW410 and retry. Installation is intentionally blocked when no supported macfw interface is detected.

### Device is present but audio is unavailable

Check the transport state and log:

```bash
fw410/tools/transport/transportstatus/transportstatus

tail -n 100 /Library/Logs/macfw-fw410-transport.log
```

The launchd supervisor is designed to survive boot without the interface and to recover when the FW410 is connected later.

### Control panel is present but controls do not respond

Confirm the transport is `ONLINE`, then test the installed CLI through the same control path, for example:

```bash
"/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl" mixer get
```

If the CLI also fails, inspect the transport log/socket path rather than opening the FireWire device with a standalone probe while the transport is active.

### 44.1 kHz startup

44.1 kHz requires a device-specific post-stream AV/C rate reassertion. This is handled automatically. Earlier development builds occasionally produced broken 44.1 kHz audio; after the clean-stop rate lifecycle was corrected, this became practically absent in subsequent hardware testing. It remains documented as an alpha observation.

See [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) before reporting a problem.
