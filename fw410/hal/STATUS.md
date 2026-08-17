# HAL integration status

Active since 2026-08-17.

DriverKit/AudioDriverKit remains a future backend, but local development is blocked by DriverKit entitlement/provisioning requirements with a Personal Team on both current and legacy Xcode projects.

The active CoreAudio-facing path is therefore `fw410/hal`, a dependency-free AudioServerPlugIn.

## Milestone reached: CoreAudio enumeration

The synthetic `M-Audio FireWire 410` now enumerates successfully in macOS as a real CoreAudio output device. It is visible in Audio MIDI Setup and in the macOS audio-device selector, can be selected as the default/system output, and exposes selectable 44.1 kHz and 48 kHz formats. CoreAudio starts and stops its I/O thread cleanly for the device.

The current `DoIOOperation` implementation intentionally discards output samples, so silence is expected at this stage.

The breakthrough required correcting the HAL factory registration so the symbol exported by the bundle matched `CFPlugInFactories`; after that Monterey created the remote Core Audio Driver Service and activated the device UID `com.mbprado.macfw.fw410.device`.

A remaining non-blocking HAL query for selector `srnd` on output scope is visible in logs and should be cleaned up, but it does not prevent enumeration or I/O thread startup.

## Next milestone: hardware-backed 44.1 kHz playback

Add a companion user-space transport service and connect HAL output to it using a pre-mapped shared-memory PCM ring. The first hardware-backed path will be deliberately narrow:

1. HAL stereo Float32 output at 44.1 kHz.
2. Non-blocking copy from `DoIOOperation` into shared memory.
3. Companion transport process maps the shared ring into the existing 10-channel FW410 PCM layout.
4. Reuse the proven `IOFireWireLib`, AV/C/CMP, NuDCL, `PcmRingBuffer`, `AmdtpPcmStream44100`, and the FW410-specific post-start 44.1 kHz AV/C rate reassertion.
5. No SRC and no capture in this first hardware-backed version.

After native 44.1 HAL playback is stable, add the native 48 kHz scheduler path and then capture/input.

The HAL real-time I/O callback must never perform blocking FireWire operations, control transactions, filesystem I/O, allocation, or logging.
