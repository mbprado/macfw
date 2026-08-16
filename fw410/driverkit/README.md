# FW410 AudioDriverKit bootstrap

This directory starts the CoreAudio-facing half of the real FW410 driver.

## First milestone

Publish a synthetic `M-Audio FireWire 410` CoreAudio device that appears in Audio MIDI Setup with:

- stereo output;
- 44.1 kHz and 48 kHz advertised sample rates;
- no FireWire transport dependency yet;
- silence as the initial output implementation.

Once enumeration and ordinary application playback are proven, the output I/O buffer will be bridged to the already hardware-tested `macfw` user-space FireWire transport.

## Why the split is intentional

The existing FW410 stack uses `IOFireWireLib` successfully from an ordinary user-space process. Apple documents AudioDriverKit hardware transports for DriverKit-supported transports such as USB and PCI, but does not expose a FireWireDriverKit transport. For that reason the first architecture is deliberately split:

```text
CoreAudio / HAL
      |
AudioDriverKit dext
      |
shared-memory / IPC boundary   <- next integration milestone
      |
macfw transport service
      |
IOFireWireLib
      |
FW410
```

Do not move the proven FireWire code into the dext until we have evidence that the required APIs are available there.

## Apple project setup

Create a macOS app with an Audio Driver Extension target in Xcode, then use the sources in `src/` as the starting driver/device implementation. The driver target requires the DriverKit and DriverKit Audio Family entitlements. For the simple CoreAudio host connection used by Apple's sample, the driver also uses the allow-any-userclient entitlement during development.

The dext Info.plist needs an `IOKitPersonalities` entry whose `IOUserClass` names `FW410AudioDriver`, plus `IOUserAudioDriverUserClientProperties` with `IOClass=IOUserUserClient` and `IOUserClass=IOUserAudioDriverUserClient`.

The first build is considered successful when the installed system extension shows a device named `M-Audio FireWire 410` in Audio MIDI Setup and an ordinary application can open its stereo output stream without errors.

## Next steps after enumeration

1. Add the AudioDriverKit real-time I/O callback and observe the host output buffer.
2. Add a tiny companion transport daemon that owns `IOFireWireLib` and the proven FW410 startup/stream engine.
3. Define a shared-memory ring/control protocol between dext and daemon.
4. Feed 44.1 kHz stereo CoreAudio output directly to the native `AmdtpPcmStream44100` path.
5. Add 48 kHz, then capture, then expose all FW410 channels.

This directory intentionally does not modify anything under `fw410/lib` or the existing transport probes yet.
