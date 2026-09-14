# Native 44.1 live scheduler isolation test

This tool removes CoreAudio from the equation and exercises `AmdtpPcmStream44100` with an independent generated PCM producer. It alternates 440/880 Hz on FW410 Analog Output 1 while using the same 640-cycle live refill path as the native 44.1 CoreAudio bridge.

Run directly:

```sh
make
./pcmstream44100 --execute
```

The binary performs the guarded initial 44.1 kHz rate setup and restores 48 kHz on normal exit. No shell runner is required.
