# FW1814 high-rate development

## Reference and current boundary

The Linux [M-Audio special-firmware stream table](https://github.com/torvalds/linux/blob/master/sound/firewire/bebob/bebob_maudio.c)
lists the following stream PCM slots for S/PDIF digital mode. Every formation
also has one MIDI position, which macfw does not currently expose.

| Rate | Device to host | Host to device | Status |
|---|---:|---:|---|
| 44.1/48 kHz | 10 PCM | 6 PCM | macfw analog audio validated |
| 88.2 kHz | 10 PCM | 6 PCM | Rate CONTROL/readback validated; streaming untested |
| 96 kHz | 10 PCM | 6 PCM | Duplex silent packet capture validated; audio mapping untested |
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

On the FW1814 test Mac, both high-rate CONTROL tests passed from an initial
48 kHz INPUT rate. OUTPUT and INPUT CONTROL accepted 88.2 kHz (rate code
`0x03`) and 96 kHz (`0x04`), respectively; authoritative INPUT STATUS returned
the requested rate. Both tests restored OUTPUT and INPUT to 48 kHz and
confirmed the restored INPUT STATUS. The reported FireWire generation remained
63 throughout both tests. These results establish rate negotiation and
restoration, not audio stream operation.

The next bounded duplex diagnostic must measure actual packet data blocks,
DBC, SYT, channel labels, startup behavior and bus-generation changes at each
rate. Linux's [AMDTP blocking packet implementation](https://github.com/torvalds/linux/blob/master/sound/firewire/amdtp-stream.c)
uses a 16-event SYT interval at 88.2/96 kHz, versus eight events at 44.1/48
kHz. With the Linux S/PDIF formation's 11 capture slots and seven playback
slots, the corresponding maximum data packet sizes are 712 and 456 bytes
respectively (including the eight-byte CIP header). The existing 48 kHz
diagnostic reserves only 360 and 232 bytes and emits eight-event packets;
its transmitter and reservations must not be reused unchanged for high rates.
Only after streaming measurements can we derive a rate-specific transmitter,
capture mapping and bandwidth reservations and consider a CoreAudio rate
option.

## Experimental 96 kHz duplex packet probe

The standalone `fw1814capture96_duplex_blocking` diagnostic uses the Linux
reference 16-event blocking pattern (16/16/16/NODATA at 96 kHz), 11 capture
slots and seven playback slots. It transmits silence only. It requires an
operational FW1814 at 48 kHz with both PCR0 plugs available, and refuses to
execute while the production FW1814 control socket is present. It connects
both streams, attempts OUTPUT then INPUT 96 kHz CONTROL, observes capture for
two seconds, disconnects the streams and attempts to restore 48 kHz. It checks
the bus generation before PCR and rate restoration; if the generation changes,
it skips stale writes and reports a failure. It does not modify the HAL or
production transport.

On the test Mac, with the FW1814 service stopped:

```sh
make -C devices/fw1814/tools duplex96-tool
devices/fw1814/tools/fw1814capture96_duplex_blocking
devices/fw1814/tools/fw1814capture96_duplex_blocking --execute --experimental-high-rate --raw
```

Keep the service stopped if the probe reports failed rate/PCR restoration or
a changed bus generation. Record the full output, especially capture packet
counts, CIP headers, generation, and restoration results.

The first Mac duplex run at 96 kHz reached the stream kick with both ISO
directions active. OUTPUT returned `0x0f` (INTERIM); subsequent INPUT and
restore calls received replies to earlier commands. The first probe consumed
the first FCP reply for each write without matching the command, so it exited
before measuring any capture packets and could not establish the final INPUT
rate. The bus generation remained 65 and both PCRs restored. A follow-up
probe waits for a matching, final FCP reply and independently reads the
restored INPUT rate.

The second Mac run at 96 kHz reached the capture observation. Both CONTROL
commands received matching final responses; INPUT also produced an INTERIM
response first. The receive ring contained 48 full 712-byte data packets with
`DBS=11`, `FMT=0x10`, `FDF=0x04`, and 16 eight-byte NODATA packets, with zero
unexpected packet shapes. The displayed data DBCs advanced by 16, including
across NODATA cycles. Bus generation remained 66; both PCRs restored; OUTPUT
and INPUT restoration succeeded and INPUT STATUS read 48 kHz twice after the
stream stopped. This validates the initial silent duplex packet exchange at
96 kHz, but not PCM playback, physical channel mapping, uninterrupted
long-duration streaming or the CoreAudio engine.

At 88.2 kHz the 16-event blocking cadence is variable. Linux's 44.1-kHz
family SYT scheduler advances approximately 1386 ticks per interval modulo
the 3072-tick bus cycle; a fixed 16/16/16/NODATA loop is specific to 96 kHz.
The current transmit DCL ring repeats after 128 cycles, so an 88.2-kHz probe
must preserve the scheduler phase and DBC across the ring boundary rather
than repeat the 96-kHz pattern. A calculation using the Linux scheduler's
initial phase (`67`) shows the phase, last SYT offset and 8-bit DBC all return
to their initial values after 10,240 cycles (7056 data packets). That is much
larger than the current 128-packet static transmit ring. Choose a bounded,
phase-correct transmit strategy and check its resource cost before trying
88.2-kHz duplex on hardware.

## First 96 kHz listening test

`fw1814tone96` reuses the proven 96 kHz duplex probe and its restoration
checks. It sends 500 Hz at -24 dBFS peak in exactly one of six playback PCM
positions for three seconds; all other PCM positions are silent. The 128-cycle
static transmit ring contains 1536 samples at 96 kHz, an exact eight periods
of 500 Hz. This first test establishes audible routing without a tone seam at
the ring boundary; it cannot validate arbitrary frequencies or AIFF playback.
The mapping observed at 48 kHz was PCM position 2 to physical Analog Output 1,
position 3 to Output 2, position 0 to Output 3 and position 1 to Output 4.
It must be confirmed at 96 kHz, not assumed.

With the production FW1814 transport stopped and monitor volume low:

```sh
make -C devices/fw1814/tools tone96-tool
devices/fw1814/tools/fw1814tone96 --position 2
devices/fw1814/tools/fw1814tone96 --position 2 --execute --experimental-high-rate
```

Try one position at a time, and record which physical output sounds, whether
the tone is clean, the capture packet counts, and all restoration results.
If the analog routing was reset, use the already documented
`make -C devices/fw1814/tools route-analog` before repeating the listening
test. Do not resume the service after a failed PCR or rate restoration.

The test Mac reported clean 96-kHz 500 Hz playback at each of the four
physical analog outputs. The observed PCM map matched the 48-kHz map:

| Raw playback PCM position | Audible output |
|---:|---|
| 0 | Analog Output 3 |
| 1 | Analog Output 4 |
| 2 | Analog Output 1 |
| 3 | Analog Output 2 |

The next audible test must use a live-refilled transmitter for 440 Hz and a
second non-loop-aligned frequency (the 44.1-kHz history used 523.25 Hz),
then real file playback. The current 96-kHz 500 Hz test is deliberately
phase-aligned to the static 128-cycle DCL ring; it cannot expose the boundary
failure that once distorted 44.1/48-kHz program audio. Keep the 96-kHz
production HAL hidden until arbitrary-frequency playback and rate restore
are both verified.

`fw1814tone96_live` is an experimental next gate. It adapts the proven
48-kHz PCM refill to 16-event, 96-kHz packets, holds a five-second tone in
memory, and refills two transmit halves as the bus cycle advances. Its FCP
reply waits also service the PCM transmitter. It runs for three seconds and
reports frames read, silence fill, late cycle polls and the capture packet
summary, then checks rate and PCR restoration. Test it first at 440 Hz and
then at 523.25 Hz on a known physical output. With the FW1814 service
stopped and low monitor volume:

```sh
make -C devices/fw1814/tools tone96-live-tool
devices/fw1814/tools/fw1814tone96_live --position 2 --frequency 440
devices/fw1814/tools/fw1814tone96_live --position 2 --frequency 440 --execute --experimental-high-rate
devices/fw1814/tools/fw1814tone96_live --position 2 --frequency 523.25 --execute --experimental-high-rate
```

The second execution should only follow a clean first run and successful
restoration. Both 440 Hz and 523.25 Hz were reported clean on the test Mac.
The 523.25 Hz run refilled 42 halves, read 322560 PCM frames with zero
silence fill, captured 48 correctly formed data packets and 16 NODATA packets,
reported no unexpected packet shapes, kept generation 81, restored both PCRs
and read back 48 kHz after rate restoration.

For a real-file test, the same standalone probe can read up to three seconds
of a local mono/stereo AIFF through AudioToolbox. It converts the source to
96 kHz float PCM, mixes stereo to one selected analog output with -12 dB
gain, and pads the rest of its preloaded buffer with silence. Use an absolute
path; for the stock macOS alert as a short first sample:

```sh
make -C devices/fw1814/tools tone96-live-tool
devices/fw1814/tools/fw1814tone96_live --position 2 --file /System/Library/Sounds/Glass.aiff
devices/fw1814/tools/fw1814tone96_live --position 2 --file /System/Library/Sounds/Glass.aiff --execute --experimental-high-rate
```

Then use a longer local AIFF to hear a sustained excerpt. Keep the FW1814
transport stopped, check rate/PCR restoration after each run, and report
whether the file sounds clean. This test exercises file decode and conversion
within the diagnostic; it does not expose 96 kHz through CoreAudio's HAL.

The first file tests were silent despite successful CONTROL, ISO capture,
and restoration. The stock `Glass.aiff` was reported as 48 kHz stereo and
converted to 158408 frames at 96 kHz, while the live transmitter reported
322560 frames read from its PCM ring with no underflow and 74 late cycle
polls. These counters did not measure whether the converted or transmitted
samples were nonzero; the file's waveform and stereo cancellation were not
established. The file path now reports left/right source peaks, selected
channel peak/RMS and nonzero frames, and the transmitter reports the number
and peak of nonzero frames actually copied to AMDTP packets. It uses the
left channel by default and normalizes its peak to the same -24 dBFS level
as the clean test tones; `--file-channel right` or `mix` can be selected
explicitly. Test a single file first and compare decoder and TX counters
before drawing a conclusion about the device or PCM engine.

The subsequent short-file run still produced only the activation click, but
reported 154961 nonzero transmit frames at the expected 529285 peak, no
silence fill, and two late cycle polls. The device-side ISO packet snapshot
remained valid. As a duration check, the file probe now offers `--repeat 4`:
it leaves 250 ms of silence at the beginning, repeats the decoded excerpt up
to four times in an eight-second PCM buffer, and keeps the 96-kHz stream open
for up to seven seconds. This distinguishes a too-short sample from a playback
path that stays silent even with sustained nonzero PCM:

```sh
devices/fw1814/tools/fw1814tone96_live --position 2 \
  --file /System/Library/Sounds/Glass.aiff --repeat 4 \
  --execute --experimental-high-rate
```

Record the converted/copied/nonzero frame counts, active duration, dynamic
TX counts, restoration and what is actually audible. A valid capture stream
alone does not prove that the FW1814 analog output used the PCM payload.

The four-repeat Glass.aiff run audibly played three chimes; the first one was
missing. This demonstrates sustained 96-kHz program audio, while suggesting
the first part of playback is lost during stream startup. The exact time at
which the analog output begins consuming host PCM is still unknown. To measure
whether startup time explains the missing first chime, the probe accepts a
configurable silent lead-in for repeated files:

```sh
devices/fw1814/tools/fw1814tone96_live --position 2 \
  --file /System/Library/Sounds/Glass.aiff --repeat 4 --lead-ms 1750 \
  --execute --experimental-high-rate
```

This schedules approximately 1.75 seconds of silence before the first chime
and holds the stream open for up to 9.25 seconds. The PCM ring contains ten
seconds of preloaded audio/silence. If all four chimes become audible, compare
shorter lead-ins to bound the startup interval before integrating 96 kHz into
the CoreAudio transport.
