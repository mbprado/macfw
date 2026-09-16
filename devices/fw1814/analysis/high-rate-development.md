# FW1814 high-rate development

## Reference and current boundary

The Linux [M-Audio special-firmware stream table](https://github.com/torvalds/linux/blob/master/sound/firewire/bebob/bebob_maudio.c)
lists the following stream PCM slots for S/PDIF digital mode. Every formation
also has one MIDI position, which macfw does not currently expose.

| Rate | Device to host | Host to device | Status |
|---|---:|---:|---|
| 44.1/48 kHz | 10 PCM | 6 PCM | macfw analog audio validated |
| 88.2/96 kHz | 10 PCM | 6 PCM | Linux reference; macfw hardware untested |
| 176.4/192 kHz | 2 PCM | 4 PCM | Linux reference; macfw hardware untested |

Linux's [FW1814 clock protocol](https://github.com/alsa-project/snd-firewire-ctl-services/blob/master/protocols/bebob/src/maudio/special.rs)
lists all six rates. This is evidence of a supported rate-control code, not
proof that the existing macfw blocking packet cadence, bandwidth, channel map
or CoreAudio engine works at the higher rates. The released HAL and transport
continue to expose only 44.1/48 kHz.

## First diagnostic: CONTROL and readback only

`fw1814init` now accepts 88.2/96 kHz as a separate, explicitly enabled
diagnostic. It checks the operational firmware fingerprint, requires the
current INPUT rate to be 44.1 or 48 kHz, applies the known internal-clock and
S/PDIF mode, sets OUTPUT then INPUT with the documented 100 ms separation,
reads back INPUT rate and attempts to restore the original rate before exit.
It refuses high-rate CONTROL while the installed FW1814 control socket is
present. It does **not** allocate an ISO stream or claim working audio.

On the FW1814 test Mac, build the diagnostic with
`make -C devices/fw1814/tools init-tool`, then run an identity-only dry run:

```sh
devices/fw1814/tools/fw1814init 88200
devices/fw1814/tools/fw1814init 96000
```

Stop the installed FW1814 service before the CONTROL test (the FW410 service
can remain running), then execute one rate at a time:

```sh
sudo launchctl bootout system/com.mbprado.macfw.fw1814.transport
devices/fw1814/tools/fw1814init 88200 --execute --experimental-high-rate --raw
devices/fw1814/tools/fw1814init 96000 --execute --experimental-high-rate --raw
sudo launchctl bootstrap system "/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist"
```

Record the OUTPUT/INPUT responses, INPUT STATUS readback, restore result, and
whether the unit remains in its operational personality. If restoration fails,
leave the production transport stopped and restore the device to a validated
rate before continuing. Do not expose the high rate in the HAL merely because
the CONTROL response succeeds.

## Next evidence gate

After hardware confirms rate CONTROL and restore, a bounded duplex diagnostic
must measure actual packet data blocks, DBC, SYT, channel labels, startup
behavior and bus-generation changes at each rate. Only then can we derive a
rate-specific transmitter, capture mapping and bandwidth reservations and
consider a CoreAudio rate option.
