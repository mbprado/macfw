# macfw 0.04.003 — Alpha

`0.04.003` updates the unified macfw alpha release for the **M-Audio FireWire
410** and **M-Audio FireWire 1814**. The first unified release was `0.04.000`.

Both interfaces now share one project version and one primary installer while
retaining independent drivers, transport services, control state and native
control-panel applications.

## Highlights

- FW1814 physical headphone encoders now change the matching headphone volume
  and save the level for transport restart.
- The FW1814 control panel follows encoder changes while open.

## Build and installation

Build both interfaces, then install the connected model from source:

```bash
make
sudo make install
```

If both models are connected, the default install installs both. To install both
stacks regardless of hardware presence, use:

```bash
sudo make install-force
```

Compilation runs as the normal user. Each detected-device install validates its
matching artifacts; forced installation validates both build trees before
installing either one.

Build the package set from the repository root:

```bash
make package
make fw410-package
make fw1814-package
```

The output is:

```text
package/dist/macfw-0.04.003-<build>.pkg
package/dist/macfw-fw410-0.04.003-<build>.pkg
package/dist/macfw-fw1814-0.04.003-<build>.pkg
```

The combined hardware gate accepts either a connected FW410 or FW1814. Its
postinstall probe then installs only the matching namespaced device stack(s).
Focused installers remain available via `make fw410-package` and
`make fw1814-package`; their gates still require the matching interface.

For a clean package test, remove any previous installation first. An individual
package contains only its named device; an already-installed other device is not
removed automatically.

Installed user-facing applications are:

```text
/Applications/macfw FW410 Control.app
/Applications/macfw FW1814 Control.app
```

See [`INSTALL.md`](INSTALL.md) for complete build, installation, status,
troubleshooting and uninstall instructions.

## Compatibility

The current architecture targets Intel Macs using Apple's legacy FireWire
stack. Cumulative hardware validation includes Monterey 12.7.6, Ventura
13.7.8, Sonoma 14.8.9 and Sequoia 15.x.

Apple Silicon is not currently supported. macOS Tahoe 26 is unsupported because
Apple removed the built-in FireWire stack used by macfw. See
[`COMPATIBILITY.md`](COMPATIBILITY.md) and
[`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md).

## Signing and notarization

`0.04.003` remains an alpha release. Unless explicitly stated otherwise on the
GitHub Release, the package is **unsigned and unnotarized**.

## Diagnostics for testers

Use **Copy Diagnostics** in the appropriate control panel first. Transport logs
are stored at:

```text
/Library/Logs/macfw-fw410-transport.log
/Library/Logs/macfw-fw1814-transport.log
```

Package postinstall activity is recorded in:

```text
/Library/Logs/macfw_install.log
```

When reporting a problem, include the interface model, Mac model, macOS
version, FireWire adapters, sample rate and the event that preceded the failure.

## Detailed changes

See [`CHANGELOG.md`](CHANGELOG.md).
