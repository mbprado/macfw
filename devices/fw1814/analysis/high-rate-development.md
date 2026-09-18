# FW1814 high-rate development

## Reference and current boundary

The Linux [M-Audio special-firmware stream table](https://github.com/torvalds/linux/blob/master/sound/firewire/bebob/bebob_maudio.c)
lists the following stream PCM slots for S/PDIF digital mode. Every formation
also has one MIDI position, which macfw does not currently expose.

| Rate | Device to host | Host to device | Status |
|---|---:|---:|---|
| 44.1/48 kHz | 10 PCM | 6 PCM | macfw analog audio validated |
| 88.2 kHz | 10 PCM | 6 PCM | Clear 440-Hz playback with 1280-slot TX; continuous analog capture validated in the guarded probe; CoreAudio trial pending |
| 96 kHz | 10 PCM | 6 PCM | Experimental CoreAudio playback and analog capture tested; hardware opt-in required |
| 176.4/192 kHz | 2 PCM | 4 PCM | Linux reference; macfw hardware untested |

Linux's [FW1814 clock protocol](https://github.com/alsa-project/snd-firewire-ctl-services/blob/master/protocols/bebob/src/maudio/special.rs)
lists all six rates. This is evidence of a supported rate-control code, not
proof that the existing macfw blocking packet cadence, bandwidth, channel map
or CoreAudio engine works at the higher rates. The default installation
exposes only 44.1/48 kHz; higher rates require explicit experimental opt-in.

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
The static 96-kHz probe's 128-cycle transmit ring cannot use that cadence.
The experimental 88.2-kHz probe instead uses a 640-packet NuDCL ring with
320-packet live refills: packet *lengths* repeat every 640 cycles (441 data
packets), while DBC and SYT continue across each wrap. An offline check follows
the full scheduler phase for 10,240 cycles (7056 data packets) and confirms
that every repeated NuDCL slot keeps its original transfer length. This is a
packet-schedule check, not proof of stream behavior on hardware.

Build and run the guarded first hardware probe with the FW1814 transport
stopped and the interface operational at 48 kHz:

```sh
make -C devices/fw1814/tools schedule88-check
make -C devices/fw1814/tools duplex88-tool
devices/fw1814/tools/fw1814capture88_duplex_blocking
devices/fw1814/tools/fw1814capture88_duplex_blocking --execute --experimental-high-rate --raw
```

The read-only invocation checks device identity, INPUT rate, and free PCRs.
The explicit execution sends silent 6-PCM/1-MIDI variable-cadence packets,
attempts the OUTPUT-then-INPUT rate kick with both ISO directions running,
and checks for 712-byte, 16-event capture packets with FDF `0x03` and
8-byte NODATA packets. It then stops both streams, restores both PCRs and the
original 48-kHz rate, and checks the final INPUT readback. Audio playback,
capture channel mapping, and CoreAudio 88.2-kHz support remain unverified.

The first silent run on the test FW1814 passed: 44 full packets and 20
NODATA packets in the 64-slot snapshot, zero unexpected packet shapes or
TX underruns, and both PCRs and the 48-kHz INPUT rate restored. An optional
bounded listening test is now available on the same guarded executable:

```sh
make -C devices/fw1814/tools duplex88-tool
devices/fw1814/tools/fw1814capture88_duplex_blocking \
  --tone-440 --position 2 --execute --experimental-high-rate
```

With the transport stopped and the interface at 48 kHz, it sends 1.5 seconds
of silence, three seconds of 440 Hz at -24 dBFS on playback PCM position 0-5,
and a silent tail. It prints the nonzero frame count and checks the same
capture packets and rate/PCR restoration. Audibility and the physical output
mapping still require testing on the interface.

The first 88.2-kHz 440-Hz test on PCM position 2 was audible but broken,
despite 44 full/20 NODATA packets in the snapshot, 259198 nonzero TX frames,
zero PCM underruns and successful PCR/rate restoration. A valid capture
snapshot and queued TX samples do not prove continuous or correctly timed
host-to-device audio. As a bounded A/B diagnostic, `--extended-tx-ring` uses
1280 NuDCL packets and 640-packet refill halves instead of the original
640/320. The variable data/NODATA packet lengths repeat every 640 cycles;
both ring sizes pass the offline schedule check. Try the same listening test
with the larger ring:

```sh
devices/fw1814/tools/fw1814capture88_duplex_blocking \
  --tone-440 --position 2 --extended-tx-ring \
  --execute --experimental-high-rate
```

The comparison tests whether the shorter refill window contributes to the
distortion. Even a clean result would still require a sustained capture and
playback validation before enabling 88.2 kHz in the installed driver.

The 1280/640 run sounded clear on the test FW1814, with a 44-data/20-NODATA
capture snapshot. The probe now defaults to that longer TX ring; the previous
640/320 ring remains selectable using `--short-tx-ring` for comparison.
The clear playback result does not by itself validate sustained capture.

The next diagnostic continuously decodes the RX ring while streaming, prints
half-second capture-rate windows and analog input peaks, and freezes RX DMA
before taking the final 64-packet snapshot. The existing guarded tone command
also performs this check. To validate Analog Input 1, inject a known 440-Hz
signal into its physical input during the tone run and compare its peak with
the other inputs. The packet counts and steady windows should approach
88,200 decoded frames per second, with no malformed packets or DBC gaps.

The first continuous decode found Analog Input 1 at about -25.7 dBFS with no
malformed packets, invalid labels or frame drops. Its later half-second
windows reported up to 4,122 total data/NODATA packets, which exceeds the
4,000 FireWire cycles available in half a second. Reported 89-90 kHz rates
and 105 DBC gaps therefore cannot yet be treated as actual device capture
behavior. The standalone 88.2-kHz decoder now deduplicates by each RX slot's
cycle timestamp, in addition to its chunk completion signature, and prints
duplicate, reorder, timestamp-regression and metadata-swap counters. Repeat
the guarded test before drawing conclusions about sustained capture.

The repeated run after RX-slot deduplication reported 307488 decoded frames,
19218 data packets, 13006 NODATA packets and 416 suppressed duplicates, with
zero DBC gaps, malformed packets, invalid labels, dropped frames, reordered
packets, timestamp regressions and metadata swaps. Between 1.5 and 4.5
seconds, successive half-second windows measured 88161, 88195, 88237, 88200,
88191 and 88168 Hz. Startup windows contain the expected silent settle and
rate-kick period; the short final window is not a steady-state measurement.
These results support a guarded CoreAudio trial, not a default release.

During the CoreAudio trial, waiting for all 32 RX-slot timestamps to advance
removed duplicate slots and made most steady two-second windows decode exactly
176400 frames. The following timestamp-ordered decoder recorded clean audio
until capture stopped: RX groups still completed at about 500 per two seconds,
but every subsequent data packet was marked stale. The receive timestamp's
seconds field rolls over independently of a 32-bit integer comparison. The
88.2-kHz decoder now orders and filters packets using the modulo-8000
FireWire cycle field (bits 12..24), which remains unambiguous across the
32-ms RX ring. Continuous recording across that rollover requires another
Mac test; playback and the completed-group check are unchanged.

## Experimental 88.2 kHz CoreAudio trial

The 88.2 kHz HAL format and supervisor engine are gated by a root-owned
`enable-88-experimental` file. The engine uses the probe's clear-sounding
1280-packet transmit ring with 640-packet refill halves and continuous
receive-slot deduplication. It starts from a validated 48 kHz baseline, kicks
OUTPUT and INPUT to 88.2 kHz with both ISO directions active, then restores
48 kHz when it stops. Its startup silence and monitoring latency have not yet
been measured with CoreAudio. The standard install does not include this
engine or enable the rate.

On the FW1814 Mac, with the device attached:

```sh
git pull --ff-only
make fw1814-experimental88
sudo make fw1814-install-experimental88
```

Select 88.2 kHz in Audio MIDI Setup or Logic and test analog output, input 1,
software monitoring and switching back to 48 kHz. Check
`/Library/Logs/macfw-fw1814-transport.log` for engine READY, steady capture
rate, TX underruns and capture errors. To return to the normal 44.1/48 kHz
installation, run `sudo make fw1814-install` after the ordinary build.
The separate 96 kHz experimental install remains available, and installing
either experimental engine preserves the other if it was already enabled.

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
to four times in a ten-second PCM buffer, and keeps the 96-kHz stream open
for the repeated excerpt. This distinguishes a too-short sample from a playback
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
seconds of preloaded audio/silence. The test Mac audibly played all four
chimes with this lead-in, compared with three of four with the default 250 ms
lead-in. The playback path is therefore working after startup; the exact
minimum settling time remains unknown. Compare shorter lead-ins to bound
the startup interval before integrating 96 kHz into the CoreAudio transport.

Follow-up listening tests found that an 800 ms silent lead-in played all four
chimes cleanly, while 700 ms cut the beginning of the first chime. This is an
observed threshold for this Mac and device state, not a guaranteed device
constant. Use at least 1000 ms of silent PCM as the initial conservative
setting when prototyping the 96-kHz transport, with output activity verified
after the actual rate CONTROL completes. Repeat the startup test after rate
switches and bus reconnection before promoting the setting to a release.

The current production integration still constrains sample rates to 44.1/48
kHz in the HAL's nominal-rate list, shared-ring rate validation, supervisor
selection, and rate-specific 44/48 transport engines. The standalone live
96-kHz probe validates analog output PCM and rate restoration but does not
validate the production capture pump, HAL lifecycle or repeated sample-rate
switching. Integrate those pieces under an experimental opt-in before exposing
96 kHz to all installations. The 88.2-kHz variable transmit schedule remains
a separate evidence gate.

An experimental 96-kHz capture decoder has been added to the standalone live
probe. It keeps the released 48-kHz decoder untouched, accepts `FDF=0x04`
and up to 16 events per packet, and prints decoded frames, malformed packets,
invalid MBLA labels and DBC gaps for the probe's 64 captured slots. A clean
snapshot will validate packet decoding, not sustained production capture or
the physical input map; those still need live input and HAL tests.

The first 96-kHz tone run with this decoder returned 768 decoded frames,
zero malformed packets, zero invalid MBLA labels and zero DBC gaps across
the 64-slot capture snapshot. The stream retained its bus generation and
restored the PCRs and original 48-kHz rate. The probe now also prints a peak
reading for each decoded analog input. Feed a known signal into one analog
input and check that its corresponding peak rises before claiming the 96-kHz
physical input map; these readings cover only the captured snapshot.

With a 440 Hz, 0.2 V signal connected to Analog Input 1, the snapshot showed
Analog1 at -26.84 dBFS while Analog2 through Analog8 were approximately
-93 to -99 dBFS. This strongly supports the Analog Input 1 mapping at 96 kHz.
The same run reported 768 decoded frames and zero decoder errors, but one
capture slot had an impossible 41025-byte length. At that point the diagnostic
still read a cyclic DMA ring while capture was running, and the decoder
silently skipped oversized slots. The probe now stops capture before taking
the snapshot and counts any oversized slot as malformed. Repeat this input
test and check the packet summary and decoder counts before treating the
snapshot as fully clean. Sustained capture and the remaining physical inputs
are still separate checks.

Stopping the capture channel before the snapshot did not eliminate the bad
headers. A repeat with the same 440 Hz input showed Analog1 at -27.56 dBFS,
736 decoded frames, 46 valid data packets, 16 NODATA packets, and two
adjacent slots reporting 41025 bytes (two malformed slots and DBC gaps).
The receive slot capacity is 712 bytes, so these values cannot represent
ordinary audio packets. The cause remains unknown; the diagnostic now logs
the full receive header, status, timestamp, and first 16 payload bytes for
every oversized slot to distinguish a damaged metadata word from a wider
receive/DMA problem. Do not mark the capture path clean until this is
understood and a repeated input test has no unexplained gaps.

Several subsequent runs of the same 440 Hz Analog Input 1 test had clean
snapshots. One recorded 48 data packets, 16 NODATA packets, zero oversized or
other packets, and 768 decoded frames with zero malformed packets, invalid
labels, or DBC gaps. Analog1 peaked at -25.39 dBFS; the other analog inputs
were near the noise floor. The bus generation stayed unchanged and the
original 48-kHz rate and both PCRs restored. This passes the bounded 96-kHz
Analog Input 1 mapping check, but packet integrity across runs remains open.
The earlier intermittent 41025-byte headers have not been explained by the
stop-before-snapshot change; retain the raw metadata diagnostics. The next
capture gate is to resolve those headers and then check continuous decoding
with ring consumption and packet/error counters over the whole run.

A later run reproduced two adjacent oversized slots (0 and 1), each with
`isoHeader=0xa041c802` and `status=0x51840000`, followed by valid-looking
packet metadata. Their payload prefixes began with plausible 96-kHz CIP
headers and DBC values 0 and 16, but that does not prove the complete payload
was received. The run decoded 736 frames and counted two malformed packets;
its decoder DBC gap counter remained zero even though the skipped slots lost
audio frames, so a zero DBC gap counter by itself is insufficient. The probe
now also prints header, status and timestamp for the first four slots on every
run, permitting direct comparison with neighboring valid slots. Do not
reconstruct packet lengths from the CIP prefix or expose this as reliable
capture until the receive metadata anomaly is understood.

Comparison with a clean run identified a consistent byte-order reversal of
all three receive DCL metadata words in the anomalous slots. For example,
the bad `0xa0410800` header, `0x51840000` status and `0x00f0ee0b`
timestamp become an 8-byte header, the normal `0x00008451` status and a
plausible timestamp when individually byte-swapped. A previous 41025-byte
header similarly converts to a 712-byte data-packet header. The payload CIP
prefixes were already in normal byte order. The shared receive reader now
corrects metadata only when the native length exceeds the slot capacity,
the swapped length is bounded and quadlet-aligned, and the swapped status
matches the observed successful completion. The probe reports the number of
corrected slots separately; test on the Mac before concluding all payload
bytes and frame continuity are intact. This conditional correction leaves
normal receive metadata unchanged and preserves unrecognized bad headers
as malformed.

A subsequent excerpt still showed a raw 41025-byte header, but did not
include the new `byte-swapped receive metadata` summary, so it cannot yet
show whether the correction was used. The tools Makefile could treat an
existing shared `libmacfw.a` as up to date without re-entering its own
Makefile. `tone96-live-tool` now updates that archive before checking and
relinking its executable; the startup output includes `receive metadata
byte-swap guard: enabled` so a complete test log identifies the updated
binary. The correction still needs a hardware run.

The next Mac run confirmed the metadata correction: two slots were reported
as byte-swapped, both decoded as ordinary 712-byte data/8-byte NODATA
packets in sequence, and the complete 64-slot snapshot contained 48 data
packets, 16 NODATA packets and 768 decoded frames without malformed packets,
invalid labels, DBC gaps, or oversized headers. Analog Input 1 measured
-29.01 dBFS with the 440 Hz test signal; other inputs remained close to the
noise floor. This passes the bounded corrected-metadata capture test; it does
not establish continuity over seconds.

The live probe now also decodes completed 32-slot capture chunks while the
96-kHz stream runs. After a 30 ms post-kick settling interval, it services
capture alongside dynamic playback for the three-second active test and
advances the diagnostic ring's read cursor so its 32768-frame buffer cannot
fill. The final `experimental 96 kHz capture decode` line now reports totals
over the run; `continuous capture` reports decoded data/NODATA packet counts,
chunk completions, reordering, stale packets, timestamp regressions and
dropped frames. The existing `capture result` still describes only the last
64 DMA slots. Test with the same 440 Hz analog input and verify that frames
grow well beyond 768, the packet count tracks elapsed time, and no malformed,
invalid-label, DBC-gap, or dropped-frame counters increase. Rate and PCR
restoration must still pass. Initial chunk synchronization and the realtime
cost of decoding on the playback service loop require hardware validation.

The first full-run test decoded 247760 frames from 15485 data packets over
742 completed 32-slot chunks (23744 bus cycles), plus 8259 NODATA packets.
The final 64 slots looked healthy at 48 data and 16 NODATA, and malformed,
invalid-label, DBC-gap, reordering, stale, timestamp-regression and dropped
frame counters were all zero. Nevertheless, 247760 frames in approximately
2.968 seconds of completed cycles is only about 83.5 kHz; sustained 96 kHz
would carry about 285k frames over that interval. The final snapshot therefore
cannot represent the entire run. The live probe now reports half-second
capture windows with data packets, NODATA packets and effective frame rate,
and counts metadata byte swaps over every processed chunk. Find whether the
shortfall occurs during startup or persists through the run before using this
path as reliable 96-kHz capture. The cause of the extra NODATA cycles is
unknown from these counters alone.

The half-second windows resolved that question. The first window carried 485
data and 3259 NODATA packets, while every later window carried 3000 data and
1000 NODATA packets, yielding approximately 96 kHz in each steady window.
The device therefore has a startup capture interval dominated by NODATA
after the rate kick, followed by stable 96-kHz packet formation for at least
2.5 seconds in this test. The exact transition time within the first half
second has not been measured. No malformed packets, invalid labels, DBC gaps,
reordering, stale packets, timestamp regressions or dropped frames were
reported; both PCRs and the original 48-kHz rate restored. Analog Input 1
peaked at -25.46 dBFS with a 440 Hz signal, but amplitude alone does not
verify the captured waveform frequency or uninterrupted audio. The production
transport must withhold capture during startup and prime silent playback
before delivering user audio; an experimental end-to-end input signal check
remains useful before exposing 96 kHz in the HAL.

The live probe now counts hysteretic positive crossings in the decoded
Analog Input 1 samples after the first 0.5 seconds. It reports the captured
frame count and an estimated tone frequency using the known 96-kHz sample
rate. With the known 440 Hz input, a result close to 440 Hz would confirm
that the steady input samples contain the expected waveform; this simple
estimate is diagnostic and does not replace an audio quality or long-running
CoreAudio capture test. The threshold is 0.002 full scale, so a very quiet
signal might not produce a useful estimate.

The first crossing-count run reported 240000 steady frames over 2.5 seconds,
but 4923 positive crossings produced an apparent 1969.2 Hz for the injected
440 Hz source. Packet formation and transport restore passed; one timestamp
regression was counted. A threshold-crossing count alone cannot distinguish
the intended tone from additional high-frequency content or glitches. The
probe now retains 8192 decoded Analog Input 1 frames after the first half
second and performs a Hann-windowed spectral scan from 100 to 4000 Hz after
DMA stops. It prints the dominant frequency and amplitude, amplitude near
440 Hz and near the observed 1969 Hz crossing count, and RMS. Compare these
values before judging whether the captured 440 Hz waveform is intact; do
not infer capture fidelity from packet/DBC counters alone.

The earlier source was an audio signal, rather than the pure wave assumed by
the crossing-count test. With a raw 440 Hz waveform connected to Analog Input
1, the test decoded exactly 1100 positive crossings over 240000 steady frames
(2.5 seconds), estimating 440 Hz. The 8192-frame spectrum placed the dominant
peak in the 445.3125 Hz bin nearest 440 Hz, at approximately 0.020 amplitude;
the bin near 1969 Hz measured about 3.5e-7. The measured Analog1 peak was
-32.91 dBFS. Every half-second capture window after startup carried 3000
data and 1000 NODATA packets at approximately 96 kHz. There were no malformed
packets, invalid labels, DBC gaps, reordered or stale packets, dropped frames
or byte-swapped metadata in this run, and both PCRs and 48-kHz rate restored.
One timestamp regression was reported; its cause and placement have not been
verified. The probe now prints the first previous/current receive timestamps
when this counter is nonzero, so a later run can distinguish a timer wrap
from genuine packet reordering. These results validate the steady standalone
96-kHz input waveform
for Analog Input 1, alongside the previously tested analog playback, while
the production HAL and rate-change lifecycle remain untested at 96 kHz.

## Guarded production transport prototype

`make -C devices/fw1814/transport analog96-experimental` builds a separate
`fw1814analog96` executable using the validated 96-kHz blocking TX and capture
decoder. It is excluded from the normal `all`, `runtime` and release package.
The executable compiled on the FW1814 test Mac. Its supervised CoreAudio
start and rate-change lifecycle remain untested on hardware.

The prototype preflights the device at the known 48-kHz baseline, preloads
two seconds of silent PCM, starts the audio service thread before the 4096-cycle
TX lead and FCP kick, sends OUTPUT 96 kHz then INPUT 96 kHz 100 ms later,
withholds capture for 500 ms after the kick, and waits another 500 ms before
publishing playback readiness. It restores 48 kHz after stopping both ISO
directions and restoring PCRs when the bus generation is unchanged. This
startup and restore path has not yet been run on hardware.

## Opt-in CoreAudio integration (experimental)

Build all FW1814 artifacts and the separate engine, then preflight and install
the test stack with the device connected:

```sh
make fw1814-experimental96
sudo make fw1814-install-experimental96
```

The opt-in installer validates hardware and **all** binaries before changing
the HAL or service. It installs the 96-kHz engine and a root-owned, non-writable
marker at `/Library/Application Support/macfw/fw1814/enable-96-experimental`.
Only when that marker exists do the HAL and supervisor accept 96 kHz. CoreAudio
advertises 96 kHz for input and output, while the supervisor first initializes
the known 48-kHz baseline and lets the engine perform the duplex 96-kHz kick.
The usual `sudo make fw1814-install` removes the opt-in marker and engine;
normal installation and release packages continue at 44.1/48 kHz.

Start with no audio clients, then select 96 kHz for macfw FW1814 in Audio MIDI
Setup. Confirm the supervisor log reports the experimental engine ONLINE and
the device is usable for both playback and capture. Switch back to 48 kHz and
verify the rate restores and the 48-kHz engine starts. Test a 440 Hz playback
signal and a 440 Hz analog input at 96 kHz, listening for missing onset,
dropouts or distortion. Record any failures and the service log:

```sh
tail -n 120 /Library/Logs/macfw-fw1814-transport.log
```

To roll back, close any FW1814 audio clients, run `sudo make fw1814-install`
with the same built tree and connected FW1814, and reopen the device. This
replaces the experimental service and HAL, removes the marker and the 96-kHz
engine, and returns the available rates to 44.1/48 kHz. If the unit remains at
96 kHz after an interrupted run, stop the service and use the guarded
`fw1814init 48000 --execute` recovery only after verifying the device is in
its operational personality.

The first integrated 96-kHz playback test remained online and moved 192000
CoreAudio and TX frames per two seconds, with no HAL playback drops, PCM
underruns, TX silence or accumulating TX late polls, but playback sounded
broken. The same Logic-exported 440-Hz recording played cleanly after
switching the device back to 48 kHz; YouTube playback also sounded broken at
96 kHz. This points to the 96-kHz playback path rather than the file. The
capture ring filled while no CoreAudio input reads were reported in the
provided log, so its drop counter cannot explain the playback symptom.
An AIFF exported separately from Logic contained a continuous 440-Hz
waveform; the log and export cannot yet establish identical capture intervals.
The 96-kHz engine now anchors its TX cycle after the large silent preload and
RX allocation, mirroring the standalone tone probe, and reports the setup
delay before ISO servicing. If the setup consumes nearly the 4096-cycle lead,
it refuses the kick. A separate HAL SHM size check now accepts macOS page
rounding to avoid replacing a live shared-memory object. The next FW1814
hardware test confirmed clean 96-kHz playback and monitoring: setup took
23 ms of the 512-ms TX lead; the engine sustained 192000 TX frames per two
seconds with no HAL playback drops, PCM underruns, accumulated late polls,
capture DBC gaps, or capture reorders in that run. This establishes a usable
96-kHz audio path, while latency and longer-run recovery remain untested.

The same clean run held 41856 queued PCM frames at steady state, about 436 ms
before even accounting for the DMA packet ring and CoreAudio buffering. The
user reported noticeable latency. The experimental engine now preloads 1.7
seconds of silence rather than 2 seconds and, before opening HAL playback,
tops up to at least 4096 silent frames if startup consumed its preload.
It reports the PCM backlog at READY and any pre-ready underrun. This is a
bounded initial reduction of roughly 300 ms of excess queued silence based
on the clean run; hardware must confirm onset, uninterrupted audio and the
new measured round-trip latency before further reduction.

The first 1.7-second preload improved perceived latency, but it remained
noticeable. The next experimental step shortens the initial silence to 1.6
seconds while retaining the 4096-frame READY reserve and the already-tested
1280-slot TX packet ring. The 96-kHz engine still waits one second after the
INPUT rate kick before making playback available. Check that its reported
READY backlog approaches the reserve, that any pre-ready silent underrun does
not continue during playback, and that the first sound and sustained audio
remain intact. Measure physical loopback round-trip latency before treating
the setting as settled or touching the released 44.1/48-kHz engines.

At the 1.6-second preload, Logic software monitoring still had noticeable
latency. The next log showed only 2176 playback PCM frames queued (~23 ms),
while 32512 capture frames remained queued (~339 ms). Capture produced and
Logic read 192000 frames per two seconds, so the old backlog could not drain
at equal rates. Earlier time without an input reader had filled the ring and
caused dropped *new* capture frames. The experimental 96-kHz HAL input reader
now suspends capture when more than 4096 frames are queued, discards the whole
stale backlog, and returns silence briefly while the engine primes 512 fresh
frames before resuming. Initial activation also drops a stale full ring before
its first 512-frame prefill. The released 44.1/48-kHz input behavior stays as is.
The next hardware run must verify that `queued` falls near 512 while Logic
monitors, `cap-drop` stops growing after the one-time catch-up, and capture
audio remains continuous once the prefill has completed.

The capture catch-up test brought its steady queue down to 128–512 frames
(~1–5 ms); playback PCM was 320–512 frames (~3–5 ms). Across the reported
interval `cap-drop` and input zero-fill stopped increasing; TX underrun
increased by only 64 frames in one interval, then stabilized. Logic software
monitoring improved markedly, though some latency remained noticeable.
The next experimental change halves the 96-kHz TX packet ring from 1280/640
to 640/320 slots, an 80-ms ring with 40-ms refill halves. The 48-kHz engine
already uses this ring duration; the working standalone 96-kHz probe used
the longer one. This shorter 96-kHz packet schedule requires a fresh Mac
test for first-onset integrity, sustained audio and new TX underruns; the
prior 96-kHz packet geometry remains available in Git for comparison.

The FW1814 Mac test of the 640/320-slot TX ring found Logic software
monitoring usable, though some delay remains audible. During sustained
playback the PCM queue stayed at 512–768 frames (~5–8 ms), and CoreAudio
playback drops and cumulative TX late polls did not grow. Capture initially
showed 3648 queued frames (~38 ms), then recovered to 640 (~7 ms) after a
one-time stale-buffer catch-up. That transition added 4448 capture dropped
frames, 896 input zero-filled frames, 4 DBC gaps and 15 reordered packets;
those counters then stabilized in the provided log. TX silence/PCM underrun
grew by 128 frames once, then also stabilized. These transients warrant
longer-duration and reconnect/rate-change checks; they are not evidence of
sustained loss during the logged steady intervals. Keep this working
experimental packet geometry until physical loopback round-trip latency is
measured and any subsequent change has an isolated hardware comparison.

## Logic rate switching while monitoring (2026-09-17)

The Mac test switched a Logic project from 44.1 to 48 to 96 kHz while
software monitoring. All three transitions completed, and the listener heard
no dropouts or crackles. Perceived monitoring latency was bad at 44.1 kHz,
best at 48 kHz, and good at 96 kHz.

| Rate | Steady playback PCM queue | Capture queue in logged interval | Observation |
| --- | --- | --- | --- |
| 44.1 kHz | ~63472–63592 frames (~1.44 s) | 352–11640 frames (~8–264 ms) | Bad latency; capture DBC gaps rose 0 to 130 and one reorder appeared. |
| 48 kHz | 1664 frames (~35 ms) | 512 frames (~11 ms) | Best perceived latency; capture gap and reorder counters stayed at zero. |
| 96 kHz | 6528 frames (~68 ms) | 512–640 frames (~5–7 ms) | Good perceived latency; capture gap and reorder counters stayed at zero. |

The large 44.1-kHz playback queue is consistent with its required
88200-frame silent startup preload draining through the ordinary service
loop. It is a strong explanation for the rate's relative monitoring delay,
but this log is not a physical round-trip measurement. The earlier 44.1-kHz
hardware A/B tests in `dynamic-rate-switching-success.md` found that reducing
or resetting the preload and gating READY distorted playback. Preserve that
validated startup sequence until an isolated hardware experiment demonstrates
clean audio with less queued silence. Investigate the growing 44.1-kHz capture
queue and DBC gaps independently; the log does not establish whether they
contribute audibly to monitoring latency. Keep the measured 48/96-kHz paths
as reference points for any subsequent latency changes.

The 44.1-kHz verbose service line now also reports `pb-read`,
`pcm-underrun`, `cap-drop`, `cap-active`, `hal-in-reads`, `hal-in-frames`,
`hal-in-zero` and `hal-in-underrun`. During the same Logic S1/SW3 monitoring
test, compare consecutive two-second lines: `pb-read` should rise by about
88200 frames if HAL audio is entering playback, and `hal-in-frames` should
rise by about 88200 if Logic is continuously consuming capture. A steady
~63480-frame PCM queue despite those increments points to persistent playback
buffering; a rising capture queue with fewer HAL input reads or unexpected
decoded frames points to a separate input timing problem. These counters do
not change stream behavior and avoid repeating the previously distorted
44.1-kHz warm-up variants before identifying which side dominates the delay.

In a subsequent 44.1-kHz monitoring run, the playback queue stayed near
61968–61984 frames (~1.4 s) with zero PCM underruns while both `pb-read` and
`tx-audio` advanced by 88200 frames per two seconds. The capture queue filled
to ~32528 frames; decoded capture briefly jumped by 124984 frames in two
seconds (the expected amount is 88200), DBC gaps rose by hundreds, and capture
drops increased. That points to two distinct issues: persistent playback
buffering and a receive-side overcount/replay or publication fault. The 44.1
decoder now accepts a 32-slot completion only when its terminal timestamp
changes. A header change with the previous timestamp no longer replays the
entire chunk. `repeat-ts` counts such header changes, and `ts-regress` reports
packet timestamp regressions. Confirm on the Mac that `capture (delta ...)`
returns to ~88200 per two seconds, `chunks` advances by ~500, the capture
queue stops filling, and DBC gaps stabilize before considering a reduction
of the 44.1-kHz playback preload. If those numbers still diverge, the extra
logging distinguishes this specific replay hypothesis from other capture
timing faults.

The post-reboot comparison returned 48/96-kHz monitoring latency to its
previous state. The 44.1-kHz capture stream produced exactly 88200 frames
and 500 completed chunks every two seconds, with a small steady queue and
no new DBC gaps, but 44.1 playback still held ~63600 PCM frames (~1.44 s)
even for playback alone. The next 44.1-kHz experiment retains the proven
88200-frame silence preload and starts the same ordinary audio service loop
immediately after the stream kick. While that preload drains, it discards
CoreAudio playback SHM backlog rather than adding incoming audio behind the
silence. When no more than 8192 silent PCM frames remain, it starts draining
the live SHM into PCM normally. Expect a temporary mute after selecting
44.1 kHz; the log reports its duration and the PCM reserve at handoff.
Hardware must confirm clean first sound, sustained playback, output-only
latency, and S1/SW3 monitoring before this approach can replace the earlier
hardware-validated ordering. Repeated startup distortion blocks release of
this experiment; reducing the 88200-frame initial preload is not a substitute.

Initial Mac feedback on the live-playback handoff: 44.1-kHz monitoring and
output-only playback had very low perceived latency and sounded clean on the
second transport start. The first transport start produced broken audio; its
startup log has not yet been captured, so the cause is unknown. In three
steady two-second lines of the successful run the PCM queue fell from 304 to
296 frames (~7 ms), `tx-silence`/`pcm-underrun` held at 706272 frames,
`hal-drop` stayed zero, and capture advanced by exactly 88200 frames with
no DBC gaps. The high cumulative TX-silence and capture-drop counters were
already present before these steady lines; they cannot establish when the
first-start distortion occurred. Preserve this promising experiment while
collecting both first and second startup logs, including the `live playback
enabled` handoff line and the earliest two-second service statistics, before
changing its timing again.

A later log containing one 48 -> 44.1-kHz transition shows the handoff after
1.27 s with 7056 PCM frames remaining. Its queue then stays around 7528–7560
frames (~171 ms) across five two-second reports, with no new TX silence,
PCM underrun or playback drops; 44.1 capture holds a small queue. In the
previous especially responsive run the queue was only ~300 frames (~7 ms),
after earlier cumulative TX underruns. The change in perceived latency has a
real playback-queue counterpart here; the supplied log does not contain
multiple transitions, so it cannot establish cumulative latency across rate
switches or rule out a Logic buffer contribution at other rates. The next
44.1-kHz experiment lowers the *live handoff reserve* from 8192 to 2048
frames. It still preloads the full 88200 silent frames before ISO startup.
Expect a handoff near one 320-packet TX refill (~1764 frames); test both the
first start and several 48/44.1/96 switches for first-sound distortion and
new TX underruns before accepting the smaller steady playback queue.

The first Mac run with the 2048-frame handoff reserve had near-synchronous
perceived S1/SW3 monitoring at 44.1 kHz. Two steady two-second statistics
reported PCM queues of 1744 and 1736 frames (~40 ms), 88200 capture frames
per interval, `hal-drop=0`, and no increase in TX silence, PCM underruns,
capture drops, or DBC gaps. This is much better than the previous 8192-frame
handoff result, but repeated cold starts and rate switches still need
validation before release.

One supplied 48-kHz restart after a rate change sounded broken. Across six
two-second status lines its PCM queue stayed at 1792 frames (~37 ms), both
TX and HAL advanced by 96000 frames per interval, and `tx-silence=26624`,
`tx-late=1` and `hal-drop=0` did not increase. Its capture was mostly steady,
with four cumulative DBC gaps appearing in one interval. The supervisor also
restored 22 saved control values on that start. These observations do not
identify whether the audible failure came from control routing, software
output, or another device-side condition; do not alter the 48-kHz transport
based only on its steady counters. On a repeat, compare the control-panel
routing and whether both the direct and software-return paths are affected,
and preserve the complete log from the rate transition.
