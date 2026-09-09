# FW1814 first integrated analog 48 kHz engine success

Hardware result recorded on 2026-09-07 on the M-Audio FireWire 1814 special firmware.

## Scope

This is the first successful startup of the production-style FW1814 analog-only full-duplex engine under `devices/fw1814/transport/`.

The path is intentionally limited to:

- 48 kHz;
- internal clock;
- S/PDIF digital mode;
- four hardware-confirmed analog playback outputs;
- eight hardware-confirmed analog capture inputs;
- digital PCM positions hidden/deferred;
- MIDI deferred;
- explicit headphone routing deferred.

## Successful startup log

```text
macfw fw1814analog48 — experimental 48 kHz analog full-duplex engine
FW1814 capture SHM recreated: /macfw_fw1814_capture_v1 (1052672 -> 1048712 bytes)
FW1814 capture SHM ready: /macfw_fw1814_capture_v1 (1048712 bytes, 48000 Hz, 8 channels)
FW1814 analog routing: Stream 1/2->Mix 1/2->Analog 1/2, Stream 3/4->Mix 3/4->Analog 3/4
FW1814 duplex ISO started: playback ch=0 capture ch=1
FW1814 ISO settle: 50 ms
FW1814 special stream kick: OUTPUT 48000 Hz
FW1814 special stream kick: wait 100 ms
FW1814 special stream kick: INPUT 48000 Hz
FW1814 stream kick: PASS (matched INTERIMs=0, ignored unrelated=0)
FW1814 analog engine ONLINE
    CoreAudio-facing outputs: Analog 1-4
    CoreAudio-facing inputs:  Analog 1-8
    digital/MIDI/headphone routing: deferred
    Ctrl-C to stop
```

## Interpretation

The following are hardware-proven by this run:

- the dedicated FW1814 playback and capture shared-memory ABIs initialize successfully;
- the documented straight analog mixer routing writes complete without disturbing startup;
- the 48 kHz blocking duplex transport starts with host playback on ISO channel 0 and device capture on channel 1;
- the proven 50 ms transport settle plus OUTPUT -> 100 ms -> INPUT special-firmware rate kick succeeds in the integrated engine;
- the correlated FW1814 FCP helper completes the startup without accepting any unrelated response (`ignored unrelated=0`);
- no matching AV/C `INTERIM` response occurred on this run (`matched INTERIMs=0`);
- the engine remains online after the stream kick and is ready for live shared-memory PCM/capture testing.

This milestone does not yet prove live dynamic PCM refill or capture delivery to the SHM consumer. Those are the next hardware tests.
