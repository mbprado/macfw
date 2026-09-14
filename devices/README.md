# macfw devices

`devices/` contains each interface's complete model-specific implementation.

```text
devices/
├── fw410/    released regression baseline
└── fw1814/   experimental multi-rate/control development
```

Each device directory owns its profile, protocol behavior, HAL, transport,
service, controls, diagnostics and device-specific documentation. Reusable
FireWire, AMDTP and CoreAudio components move to `common/` only after both
hardware implementations demonstrate that the abstraction is genuinely shared.

The root Makefile provides namespaced targets for both devices while preserving
FW410 as the default released build, install and package target.
