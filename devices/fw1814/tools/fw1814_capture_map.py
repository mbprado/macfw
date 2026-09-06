#!/usr/bin/env python3
"""Analyze FW1814 duplex-blocking --raw output by AM824 position.

This intentionally does not control the device. It consumes the existing
hardware-proven diagnostic output and measures the complete AM824 events
present in each printed raw capture packet.
"""

from __future__ import annotations

import math
import re
import sys
from collections import Counter
from dataclasses import dataclass, field

POSITIONS = 11
FULL_SCALE_24 = 0x7FFFFF
RAW_RE = re.compile(r"^\s*raw:\s+([0-9a-fA-F ]+)(?:\s+\.\.\.)?\s*$")
PACKET_RE = re.compile(
    r"^\s*packet\s+\d+:\s+len=(\d+)\s+CIP\{sid=(\d+)\s+dbs=(\d+)\s+"
    r"dbc=(\d+)\s+fmt=0x([0-9a-fA-F]+)\s+fdf=0x([0-9a-fA-F]+)\s+"
    r"syt=0x([0-9a-fA-F]+)\}"
)


@dataclass
class PositionStats:
    words: int = 0
    labels: Counter[int] = field(default_factory=Counter)
    mbla_samples: int = 0
    peak: int = 0
    sum_squares: int = 0
    nonzero_words: int = 0

    def add(self, word: int) -> None:
        self.words += 1
        label = (word >> 24) & 0xFF
        self.labels[label] += 1
        if word & 0x00FFFFFF:
            self.nonzero_words += 1

        if label != 0x40:
            return

        raw = word & 0x00FFFFFF
        sample = raw - 0x1000000 if raw & 0x800000 else raw
        mag = abs(sample)
        self.mbla_samples += 1
        self.peak = max(self.peak, mag)
        self.sum_squares += sample * sample

    @property
    def rms(self) -> float:
        if not self.mbla_samples:
            return 0.0
        return math.sqrt(self.sum_squares / self.mbla_samples)


def dbfs(value: float) -> str:
    if value <= 0:
        return "-inf"
    return f"{20.0 * math.log10(value / FULL_SCALE_24):.1f}"


def parse_word(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def main() -> int:
    stats = [PositionStats() for _ in range(POSITIONS)]
    current_packet = None
    data_packets = 0
    nodata_packets = 0
    malformed_packets = 0
    decoded_events = 0
    saw_transport_success = False
    saw_input_pass = False

    for line in sys.stdin:
        # Preserve the original diagnostic on stderr so failures remain visible.
        sys.stderr.write(line)

        if "duplex-blocking-silence experiment: PACKETS RECEIVED" in line:
            saw_transport_success = True
        if "reassert INPUT 48000 Hz" in line:
            current_packet = current_packet  # no-op; keeps state explicit
        if "result: PASS" in line and "reassert" not in line:
            pass

        match = PACKET_RE.match(line)
        if match:
            length, sid, dbs, dbc, fmt, fdf, syt = match.groups()
            current_packet = {
                "length": int(length),
                "sid": int(sid),
                "dbs": int(dbs),
                "dbc": int(dbc),
                "fmt": int(fmt, 16),
                "fdf": int(fdf, 16),
                "syt": int(syt, 16),
            }
            if current_packet["length"] == 8 and current_packet["syt"] == 0xFFFF:
                nodata_packets += 1
            continue

        # In this diagnostic the line immediately following the INPUT heading is
        # the result line; remember a successful kick without depending on raw
        # response-code details.
        if line.strip() == "result: PASS":
            saw_input_pass = True

        raw_match = RAW_RE.match(line)
        if not raw_match or not current_packet:
            continue

        raw_bytes = bytes(int(x, 16) for x in raw_match.group(1).split())
        packet = current_packet
        current_packet = None

        if packet["length"] == 8 and packet["syt"] == 0xFFFF:
            continue

        # Only analyze the hardware-confirmed data-bearing FW1814 capture form.
        if packet["dbs"] != 11 or packet["fmt"] != 0x10 or packet["fdf"] != 0x02:
            malformed_packets += 1
            continue
        if len(raw_bytes) < 8:
            malformed_packets += 1
            continue

        payload = raw_bytes[8:]
        event_bytes = POSITIONS * 4
        complete_events = len(payload) // event_bytes
        if complete_events == 0:
            malformed_packets += 1
            continue

        data_packets += 1
        for event in range(complete_events):
            base = event * event_bytes
            for pos in range(POSITIONS):
                stats[pos].add(parse_word(payload, base + pos * 4))
            decoded_events += 1

    print("\nFW1814 capture-position activity summary")
    print("    source: printed raw events from duplex-blocking-raw")
    print(f"    analyzed data packets: {data_packets}")
    print(f"    observed NODATA packets: {nodata_packets}")
    print(f"    decoded complete events: {decoded_events}")
    print(f"    malformed/unexpected printed packets: {malformed_packets}")
    print()
    print("    pos  words  dominant-label  MBLA   peak      peak dBFS   RMS       RMS dBFS  nonzero")

    for pos, s in enumerate(stats):
        if s.labels:
            label, count = s.labels.most_common(1)[0]
            dominant = f"0x{label:02x} {count}/{s.words}"
        else:
            dominant = "--"
        print(
            f"    {pos:>2}   {s.words:>5}  {dominant:<14}  {s.mbla_samples:>4}  "
            f"{s.peak:>8}  {dbfs(float(s.peak)):>9}  {s.rms:>8.1f}  "
            f"{dbfs(s.rms):>8}  {s.nonzero_words:>7}"
        )

    ranked = [
        (s.rms, pos)
        for pos, s in enumerate(stats)
        if s.mbla_samples > 0
    ]
    ranked.sort(reverse=True)
    if ranked:
        print("\n    MBLA positions ranked by RMS activity:")
        for rms, pos in ranked:
            print(f"        pos {pos}: RMS={rms:.1f} ({dbfs(rms)} dBFS)")

    print("\nInterpretation:")
    print("    - Do not assign physical names yet; compare runs with one known input driven.")
    print("    - A strong RMS/peak increase identifies the AM824 position for that input.")
    print("    - label 0x40 is MBLA audio; label 0x80 is typically MIDI no-data.")
    print("    - label 0x00 positions are reported but intentionally not guessed.")

    if not saw_transport_success or decoded_events == 0:
        print("status: FAIL - no successful analyzable capture found", file=sys.stderr)
        return 1

    print("status: PASS - capture position analysis completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
