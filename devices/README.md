# macfw devices

`devices/` contains model-specific macfw profiles and implementations.

The long-term layout is:

```text
devices/
├── fw410/
│   ├── profile.h
│   └── ... device-specific transport/control/HAL/GUI code as it is migrated
└── fw1814/
    ├── profile.h
    ├── analysis/
    └── ... FW1814-specific transport/control/HAL/GUI code
```

The existing released FW410 implementation remains under `fw410/` during the transition. It is the known-good regression baseline and will be migrated incrementally rather than moved wholesale.

A device profile records macfw-visible identity and the initial supported sample-rate scope. A profile being present does not automatically make the device installer-supported; experimental profiles must remain outside production matching until their real hardware identity and runtime path are validated.
