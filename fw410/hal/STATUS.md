# HAL integration status

Active since 2026-08-17.

DriverKit/AudioDriverKit remains a future backend, but local development is blocked by DriverKit entitlement/provisioning requirements with a Personal Team on both current and legacy Xcode projects.

The active CoreAudio-facing path is therefore `fw410/hal`, a dependency-free AudioServerPlugIn.

## Milestone reached: CoreAudio enumeration

The synthetic `M-Audio FireWire 410` now enumerates successfully in macOS as a real CoreAudio output device. It is visible in Audio MIDI Setup and in the macOS audio-device selector, can be selected as the default/system output, and exposes selectable 44.1 kHz and 48 kHz formats. CoreAudio starts and stops its I/O thread cleanly for the device.

The breakthrough required correcting the HAL factory registration so the symbol exported by the bundle matched `CFPlugInFactories`; after that Monterey created the remote Core Audio Driver Service and activated the device UID `com.mbprado.macfw.fw410.device`.

The visible milestone is recorded in `fw410/HISTORY.md` with `fw410/pictures/screenshot1.png`.

A remaining non-blocking HAL query for selector `srnd` on output scope is visible in logs and should be cleaned up, but it does not prevent enumeration or I/O thread startup.

## In progress: hardware-backed 44.1 kHz playback

The first end-to-end bridge implementation is now present:

1. `include/macfw_hal_shm.h` defines a versioned lock-free stereo Float32 shared-memory ring.
2. `src/FW410HALBridge.cpp` preserves the working HAL device model and copies `WriteMix` PCM into the pre-mapped ring without FireWire work, allocation, filesystem I/O, or logging in the real-time callback.
3. `tools/transport/halbridge44100` maps the shared ring, converts stereo Float32 into the established 10-channel FW410 PCM layout, and feeds the proven `PcmRingBuffer` + `AmdtpPcmStream44100` scheduler.
4. The physical output mapping follows the already-tested native 44.1 bridge: left to FW410 PCM slot 1 and right to slot 6.
5. AV/C/CMP, NuDCL, duplex ISO startup, and the required post-start 44.1 kHz rate reassertion remain in the companion transport process.

This revision is at the compile-validation checkpoint on the Monterey test Mac. Do not treat it as production-safe yet; the next runtime revision will add the same guarded initial-rate setup/restoration and clean termination behavior used by the proven 44.1 tools before the first end-to-end playback test.

After native 44.1 HAL playback is stable, add the native 48 kHz scheduler path and then capture/input.

The HAL real-time I/O callback must never perform blocking FireWire operations, control transactions, filesystem I/O, allocation, or logging.
