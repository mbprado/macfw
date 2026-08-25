# Installing macfw for the M-Audio FireWire 410

This guide covers the first macfw FW410 alpha runtime for Intel macOS.

> **Alpha software:** this driver has been hardware-tested on the development Intel Mac, but it is not yet a signed/notarized public production release. Back up important work before testing it on another system.

## Requirements

- Intel Mac.
- macOS Sonoma or newer is the current project target.
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

4. The installer validates the connected interface, installs the CoreAudio HAL plug-in and transport runtime, loads the launchd service, and restarts `coreaudiod`.
5. A reboot is normally **not required**. Hardware validation confirmed that the FW410 can become usable immediately after package installation.
6. Open Audio MIDI Setup and select **M-Audio FireWire 410**. Native 44.1 kHz and 48 kHz are currently supported.

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

Do not run the compilation itself with `sudo`. `make install` intentionally installs already-built artifacts rather than compiling as root.

The source installer uses the same supported-device gate as the package installation.

## Building a package locally

From the repository root:

```bash
make package
```

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
/Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
/Library/Application Support/macfw/fw410/
/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist
/Library/Logs/macfw-fw410-transport.log
```

The launchd service is:

```text
com.mbprado.macfw.fw410.transport
```

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

## Uninstalling a source installation

From the repository root:

```bash
sudo make uninstall
```

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

### 44.1 kHz startup

44.1 kHz requires a device-specific post-stream AV/C rate reassertion. This is handled automatically. Earlier development builds occasionally produced broken 44.1 kHz audio; after the clean-stop rate lifecycle was corrected, this became practically absent in subsequent hardware testing. It remains documented as an alpha observation.

See [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) before reporting a problem.
