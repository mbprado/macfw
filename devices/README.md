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

The root `make`, `sudo make install`, uninstall and package targets operate on
both devices. Namespaced targets remain available for focused development and
individual installers. Use `make fw410-package` or `make fw1814-package` for a
device-specific installer; `make package` builds the unified installer.
