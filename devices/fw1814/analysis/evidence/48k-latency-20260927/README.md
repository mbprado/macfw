# Frozen 48 kHz evidence, 2026-09-27

These are byte-for-byte copies, verified by `manifest.json`. The original files under `samples/benchmarks` remain unchanged.

- Clean retry: all 60 raw probes, CSV, transport log and original software/profile metadata; electrical values roughly 10–14 ms.
- Earlier stopped group: all 12 raw 48 kHz probes, including the 106.437 ms stop and the 100.271 ms transition exclusion; CSV preserves classifications. The source log also includes the preceding 44.1 kHz groups.
- `validation-probe.txt`: original 82.125 ms electrical / 88.0057 ms wall reading, with 192-frame callbacks.
- Combined original report: context for the six-mode benchmark, unchanged.

A later repeat requested after the user identified a background video measured 82.625 ms electrical / 87.9559 ms wall, with 192-frame callbacks. Its output was displayed in the session and was not originally saved as a raw file; this note preserves the observation without inventing a raw acquisition file. The repeat remained slow; the video/client state was not independently captured in a process snapshot.

These runs have no per-impulse transport IDs. Lifetime first-loud markers must not be retrospectively joined to later impulses. They establish that both latency states existed, not where the extra time arose.
