# HAL integration status

Active since 2026-08-17.

DriverKit/AudioDriverKit remains a future backend, but local development is blocked by DriverKit entitlement/provisioning requirements with a Personal Team on both current and legacy Xcode projects.

The active CoreAudio-facing path is therefore `fw410/hal`, a dependency-free AudioServerPlugIn.

Current milestone: enumerate a synthetic `M-Audio FireWire 410` in Audio MIDI Setup with a stereo output stream supporting 44.1 kHz and 48 kHz. The first implementation discards output samples intentionally.

After enumeration is confirmed, add a companion user-space transport service and connect HAL output to it using Mach control IPC plus a shared-memory PCM ring. The service will reuse the already-proven `IOFireWireLib`, AV/C/CMP, NuDCL, `AmdtpPcmStream44100`, `AmdtpPcmStream48k`, and the FW410-specific post-start 44.1 kHz rate-reassertion behavior.

The HAL real-time I/O callback must never perform blocking FireWire operations, control transactions, filesystem I/O, allocation, or logging.
