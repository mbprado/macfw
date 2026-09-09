# macfw FW1814 HAL

Experimental AudioServerPlugIn for the M-Audio FireWire 1814.

## Current scope

The current HAL milestone is rate-switchable and analog-only:

- 4 output channels: physical Analog Outputs 1-4;
- 8 input channels: physical Analog Inputs 1-8;
- 44.1 and 48 kHz nominal sample rates;
- Float32 interleaved CoreAudio streams;
- no S/PDIF exposure yet;
- no MIDI exposure yet;
- no explicit headphone control yet;
- automatic transport selection and reconnect recovery through
  `fw1814supervisor`.

The HAL never opens the FireWire device. It exchanges PCM with the selected
`fw1814analog44` or `fw1814analog48` engine through the FW1814-specific
shared-memory ABI. The supervisor remains the lifecycle owner and starts the
engine matching the CoreAudio rate request.

## Build

```bash
make fw1814
```

## Install

```bash
sudo make fw1814-install
```

The consolidated install target copies `macfw-fw1814.driver` to `/Library/Audio/Plug-Ins/HAL/`, installs the complete supervised runtime and `fw1814ctl`, loads the launchd service and restarts `coreaudiod`.

## Hardware test order

1. Verify `M-Audio FireWire 1814` appears in Audio MIDI Setup.
2. Verify both 44100 and 48000 Hz are offered.
3. Verify the device reports 4 output channels and 8 input channels.
4. Change to 44100 Hz and wait for `FW1814 44.1 analog engine ONLINE` in
   `/Library/Logs/macfw-fw1814-transport.log`.
5. Validate playback from a normal CoreAudio application.
6. Change to 48000 Hz, wait for the 48 kHz engine to come online, and repeat.
7. Disconnect/reconnect the interface and confirm that the supervisor restores
   the selected rate and clean audio.

The selected transport remains the sole FireWire owner. Do not run direct
transport probes concurrently with the service. Do not combine the SHM tone
producer with active CoreAudio playback because the playback ring is SPSC.

## Uninstall

```bash
sudo make fw1814-uninstall
```
