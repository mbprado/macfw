# FW1814 power-management notes

This file records sleep/wake observations separately from validated transport
milestones. A single incident is not treated as a reproducible macfw defect.

## 2026-09-10 — One-off AppleFWOHCI deep-idle panic

The test machine panicked once while entering system sleep. The panic headline
was:

```text
Deep idle: AppleFWOHCI asserted requireMaxBusStall() of 60000 ns remains active, potentially inhibiting deep c-states @xcpm_idle.c:567
```

Observed environment:

- MacBookPro12,1;
- macOS build `21H1123`, Darwin `21.6.0`;
- panic task and thread: `kernel_task`;
- sleep path included `AppleACPIPlatform`, `IOPMrootDomain` and
  `IOCPUSleepKernel`;
- `System shutdown begun: NO` and `Hibernation exit count: 0`;
- non-default boot arguments were present;
- the incident occurred once and has not been reproduced.

The signature is a kernel power-management assertion involving Apple's legacy
`AppleFWOHCI` driver. No macfw userspace code appears in the backtrace. An
active FireWire transport may be relevant to the retained bus-stall assertion,
but the report alone does not establish that macfw caused or triggered it.
This is distinct from the audio, routing and sample-rate behavior validated so
far.

No workaround or transport change is justified by this single observation. If
the panic repeats, record:

- whether the FW1814 transport and a CoreAudio client were active;
- whether playback, capture or both were running;
- whether the interface was powered and connected throughout the transition;
- the complete panic report and the matching `pmset -g log` interval;
- `/Library/Logs/macfw-fw1814-transport.log` around the sleep attempt;
- whether stopping the transport cleanly before sleep prevents recurrence.

A reproducible case would justify adding explicit sleep/wake coordination to
the supervisor: quiesce isochronous traffic and release FireWire resources
before sleep, then re-enumerate and restore the engine after wake. Until then,
this remains an observation rather than a confirmed requirement.
