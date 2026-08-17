# HAL integration status

Active since 2026-08-17.

DriverKit/AudioDriverKit remains a future backend, but local development is blocked by DriverKit entitlement/provisioning requirements with a Personal Team on both current and legacy Xcode projects.

The active CoreAudio-facing path is therefore `fw410/hal`, a dependency-free AudioServerPlugIn.

## Milestone reached: CoreAudio enumeration

The synthetic `M-Audio FireWire 410` enumerates successfully in macOS as a real CoreAudio output device. It is visible in Audio MIDI Setup and in the macOS audio-device selector, can be selected as the default/system output, and exposes selectable 44.1 kHz and 48 kHz formats. CoreAudio starts and stops its I/O thread cleanly for the device.

The breakthrough required correcting the HAL factory registration so the symbol exported by the bundle matched `CFPlugInFactories`; after that Monterey created the remote Core Audio Driver Service and activated the device UID `com.mbprado.macfw.fw410.device`.

The visible milestone is recorded in `fw410/HISTORY.md` with `fw410/pictures/screenshot1.png`.

A remaining non-blocking HAL query for selector `srnd` on output scope is visible in logs and should be cleaned up, but it does not prevent enumeration or I/O thread startup.

## Milestone reached: clean hardware-backed native 44.1 kHz playback

On 2026-08-17 the complete output path was hardware-confirmed with clear stereo audio on physical FW410 Analog Outputs 1/2:

```text
normal macOS application
        |
CoreAudio
        |
macfw AudioServerPlugIn / WriteMix
        |
lock-free shared-memory stereo Float32 ring
        |
halbridge44100 companion transport
        |
10-channel FW410 PCM mapping
        |
PcmRingBuffer
        |
AmdtpPcmStream44100
        |
IOFireWireLib / NuDCL / CMP / duplex ISO
        |
M-Audio FireWire 410 Analog Outputs 1/2
```

The HAL instrumentation confirmed that Monterey continuously calls `WriteMix` with non-null 192-frame playback buffers. The shared-memory ring was then consumed by `halbridge44100`, mapped to the previously verified FW410 output positions, and fed into the native 44.1 kHz AMDTP scheduler.

The transport performs the required FW410 startup ritual: set 44.1 kHz, establish duplex CMP/ISO, begin streaming, then reassert 44.1 kHz on both AV/C directions while the stream is live. Both post-start reassertions were accepted in the successful end-to-end run.

The user confirmed that audio on Analog Outputs 1 and 2 was clear. This validates the architecture, not merely individual components.

The HAL real-time callback remains intentionally narrow: it must never perform blocking FireWire operations, AV/C control transactions, filesystem I/O, allocation, or logging. FireWire ownership remains in the companion transport process.

## Immediate roadmap

Proceed in this order:

1. Add **native 48 kHz HAL playback** using the proven `AmdtpPcmStream48k` path. Do not use sample-rate conversion when CoreAudio and the FW410 are both operating at 48 kHz.
2. Unify 44.1/48 transport selection so the HAL-selected device rate drives the appropriate native scheduler automatically rather than requiring a rate-specific companion executable.
3. Expand the temporary stereo CoreAudio presentation toward the FW410's real playback channel topology.
4. Add FW410 -> macOS capture/input using the already-proven receive/AM824 path.
5. Integrate transport lifecycle so normal users do not manually launch a companion binary.
6. Add persistent-service recovery for FireWire bus resets, generation/node changes, device disconnect/reconnect and rate changes.
7. Return to mixer/routing/headphone, S/PDIF and MIDI integration after basic bidirectional audio is stable.

## Future requirement: bootloader personality / interface boot mode

A production solution must also handle the FW410's boot mode. The interface can enumerate initially as the `FW Bootloader` personality (model `0x00010058`) rather than the operational `FW 410` personality (model `0x00010046`). The repository already contains user-space device/boot work proving that this transition can be performed through the macfw FireWire stack.

This is deliberately not being mixed into the current playback-rate work. When transport lifecycle is consolidated into a persistent service, startup should become a device-state machine that can:

1. detect whether the attached unit is the bootloader or operational personality;
2. perform the proven boot transition when required;
3. wait for/reacquire the operational FireWire unit after re-enumeration;
4. refresh generation/node state rather than retaining stale handles;
5. only then publish/activate transport availability to the HAL side.

Boot handling must therefore be part of eventual automatic transport/device lifecycle, before the software can be considered a user-ready driver package.

## Diagnostic tooling

`fw410/tools/transport/shmprobe` is the shared-memory/HAL diagnostic tool. It reports PCM ring state plus StartIO/StopIO and AudioServerPlugIn I/O-operation counters. It established that `WriteMix` is the actual Monterey playback operation and that valid non-null buffers reach the HAL bridge.

## Rate-control note

`control/rateprobe` uses post-CONTROL STATUS readback as the authoritative result of a requested sample-rate transition. The FW410 can occasionally return a CONTROL response shape that the strict parser does not accept even though subsequent STATUS reports the requested rate on both directions. A successful 44.1 -> 48 kHz STATUS readback must therefore not be reported as a failed restore.
