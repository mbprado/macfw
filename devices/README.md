# macfw devices

`devices/` contains each interface's complete model-specific implementation.

```text
devices/
├── fw410/    released regression baseline
└── fw1814/   initial 44.1/48 kHz analog release surface
```

Each device directory owns its profile, protocol behavior, HAL, transport,
service, control panel, diagnostics and device-specific documentation. Reusable
FireWire, AMDTP and CoreAudio components move to `common/` only after both
hardware implementations demonstrate that the abstraction is genuinely shared.

The root Makefile provides namespaced build, install, uninstall and package
targets for both devices while preserving FW410 as the default compatibility
path. Use `make fw1814-package` for the FW1814 installer or
`make package-all` to build both device packages.
