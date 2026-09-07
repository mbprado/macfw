# macfw FW1814 HAL

Experimental AudioServerPlugIn for the M-Audio FireWire 1814.

## Current scope

This first HAL milestone is intentionally fixed at 48 kHz and analog-only:

- 4 output channels: physical Analog Outputs 1-4;
- 8 input channels: physical Analog Inputs 1-8;
- Float32 interleaved CoreAudio streams;
- no S/PDIF exposure yet;
- no MIDI exposure yet;
- no explicit headphone control yet;
- no automatic FireWire transport supervision yet.

The HAL never opens the FireWire device. It exchanges PCM with the manually-run `devices/fw1814/transport/fw1814analog48` process through the FW1814-specific shared-memory ABI.

## Build

```bash
make -C devices/fw1814/hal clean all
```

## Install

Stop any running `fw1814analog48` process before installing so the HAL can establish the persistent SHM objects first.

```bash
sudo make -C devices/fw1814/hal install
```

The install target copies `macfw-fw1814.driver` to `/Library/Audio/Plug-Ins/HAL/` and restarts `coreaudiod`.

## First hardware test order

1. Verify `M-Audio FireWire 1814` appears in Audio MIDI Setup.
2. Verify the only nominal sample rate is 48000 Hz.
3. Verify the device reports 4 output channels and 8 input channels.
4. Run the guarded special-firmware initializer:

```bash
make -C devices/fw1814/tools init-48
```

5. Start the manual transport:

```bash
MACFW_VERBOSE=1 ./devices/fw1814/transport/fw1814analog48
```

6. Select the FW1814 in a CoreAudio playback application and validate Analog Outputs 1-4.
7. Select the FW1814 as an input device and validate Analog Inputs 1-8.

The transport remains the sole FireWire owner. Do not run the older direct transport diagnostics concurrently with `fw1814analog48`.

## Uninstall

```bash
sudo make -C devices/fw1814/hal uninstall
```
