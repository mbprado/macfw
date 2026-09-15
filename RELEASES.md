# macfw release and versioning policy

This document defines the release contract for macfw.

## Version format

Public releases use:

```text
x.yy.zzz
```

where:

- `x` — major architectural generation / compatibility break;
- `yy` — larger feature/update release;
- `zzz` — patch, packaging, maintenance, or small-fix release.

Examples:

```text
0.01.000   first development release
0.01.001   patch/fix to 0.01.000
0.02.000   larger update or feature addition
0.03.000   next larger update
1.00.000   first major/stable generation
```

The numeric fields are zero-padded as shown. `yy` is two digits and `zzz` is three digits.

Device releases use a device-prefixed version tag:

```text
fw410-0.03.000
fw1814-0.01.000
```

The original numeric-only FW410 tags remain supported for compatibility:

```text
0.03.000
```

The release workflow resolves the target device from the prefix, validates the
version against `devices/<device>/version.h`, and builds only that device's
package. Versions must match `^[0-9]+\.[0-9]{2}\.[0-9]{3}# macfw release and versioning policy

This document defines the release contract for macfw.

## Version format

Public releases use:

```text
x.yy.zzz
```

where:

- `x` — major architectural generation / compatibility break;
- `yy` — larger feature/update release;
- `zzz` — patch, packaging, maintenance, or small-fix release.

Examples:

```text
0.01.000   first development release
0.01.001   patch/fix to 0.01.000
0.02.000   larger update or feature addition
0.03.000   next larger update
1.00.000   first major/stable generation
```

The numeric fields are zero-padded as shown. `yy` is two digits and `zzz` is three digits.

 after removing
the optional device prefix.

Pre-release status such as **alpha** or **beta** is represented by the GitHub Release state/title rather than changing the numeric version embedded in the binaries.

## Tag-driven releases

The intended release flow is:

1. Validate the candidate on real supported hardware.
2. Update `CHANGELOG.md`, `KNOWN-LIMITATIONS.md`, `COMPATIBILITY.md`, `INSTALL.md`, `RELEASE-NOTES.md`, and the public README where needed.
3. Merge the validated release-candidate work into `main`.
4. Confirm the version embedded by the build on `main` matches the intended release.
5. Build/check the package from `main`.
6. Create and push the device tag (`fw410-x.yy.zzz` or `fw1814-x.yy.zzz`) on the release commit; legacy numeric-only tags remain FW410 aliases.
7. GitHub Actions validates the tag and builds on the supported macOS runner.
8. The workflow runs the release build and package path.
9. It calculates SHA-256 checksums.
10. It creates the GitHub Release and attaches the package/checksum artifacts.

The tag identifies the exact source revision used for the published package. Release tags must be created from the intended `main` release commit, not from an experiment/development branch.

## Primary release artifacts

The primary macfw binary distributions are device-specific native macOS
installer packages:

```text
macfw-fw410-x.yy.zzz-<build>.pkg
macfw-fw1814-x.yy.zzz-<build>.pkg
```

The build identifier is the Git commit embedded by the build system. Packages
are produced from the repository root with:

```bash
make package          # FW410 compatibility alias
make fw410-package
make fw1814-package
make package-all
```

Artifacts appear under `package/dist/`. Each package contains only its
device's CoreAudio HAL plug-in, self-contained transport/control runtime,
persistent state helper, runtime build metadata, launchd definition, native
control panel and installation scripts. Device-specific hardware gates require
the matching supported interface to be connected.

Every package target performs a fresh build of that device before staging, so
embedded build identities correspond to the package commit.

## Source installation

A tagged release must also remain buildable directly from source:

```bash
make
sudo make install
```

Compilation must run as the normal user. `sudo make install` only installs already-built artifacts.

## Source archive

GitHub automatically exposes source archives for tags/releases. If an explicit project source archive is added later, it must be generated from the tag/commit rather than from a developer working tree.

## Optional future developer bundle

A separate full developer/diagnostic bundle may be introduced later for compiled reverse-engineering and diagnostic tools. It is not required for normal releases and must not block the normal driver package.

Experimental/destructive tools must remain clearly identified and must not silently become part of a normal end-user installation.

## Checksums

Release automation publishes SHA-256 checksums for distributable binary artifacts in:

```text
SHA256SUMS
```

At minimum it must cover the published `.pkg`.

## Signing and notarization

Apple signing credentials must never be committed to the repository.

Current alpha releases separate **functional package validation** from Apple distribution signing/notarization. Until signing/notarization is implemented, the GitHub Release must explicitly state the artifact's status and must not imply that an unsigned/unnotarized build is signed.

When enabled, certificates and notarization credentials must be supplied through protected GitHub Actions secrets or another appropriate secret mechanism. The release workflow should then fail rather than silently publish an artifact that was expected to be signed/notarized but was not.

## Reproducibility and provenance

Release package names, embedded version metadata, release notes, and checksums must correspond to the triggering tag and commit.

The build records the Git commit SHA in its logs and embedded metadata. The runtime and HAL build metadata provide the basis for this provenance.

The tag version must equal `MACFW_VERSION` in `devices/fw410/version.h`; the release workflow enforces this before building.

## Release documentation

Every release candidate should review/update:

- [`CHANGELOG.md`](CHANGELOG.md) — accumulated user-visible changes;
- [`RELEASE-NOTES.md`](RELEASE-NOTES.md) — release-facing summary for the current candidate;
- [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) — current limitations and unsupported behavior;
- [`COMPATIBILITY.md`](COMPATIBILITY.md) — cumulative hardware-tested operating-system matrix;
- [`INSTALL.md`](INSTALL.md) — installation, status, troubleshooting, persistence and source-build instructions;
- [`README.md`](README.md) — current public capability/status summary.

## Current `0.03.000` release gate

Before tagging `0.03.000`, verify at minimum:

- clean source build with `make`;
- source installation with `sudo make install`;
- root `make package` produces the expected `.pkg`;
- control application is installed at `/Applications/macfw FW410 Control.app`;
- launchd runtime reaches `ONLINE`;
- 44.1 kHz playback/capture and Logic software-monitoring loopback;
- 48 kHz playback/capture and Logic software-monitoring loopback;
- low-latency 256-frame capture baseline remains stable;
- repeated 44.1 <-> 48 kHz switching from the Device tab;
- repeated switching from Audio MIDI Setup;
- 48 -> 44.1 kHz is slower but consistently completes without entering recovery;
- physical disconnect/reconnect recovery;
- reboot/delayed-attachment/launchd-restart lifecycle remains intact from the validated baseline;
- main-mixer route controls;
- physical output, headphone and AUX controls;
- live Inputs meters at both rates;
- writable control-state persistence across rate changes/restart/reconnect;
- Reset Defaults behavior;
- Info/Diagnostics exact runtime metadata, Copy Diagnostics and Open Transport Log;
- GUI build is free of the previously addressed Makefile/compiler warnings;
- package/source install paths use the corrected internal GUI bundle path;
- package contains runtime build metadata;
- known limitations and unsigned/unnotarized status are accurately stated;
- release candidate is merged into `main` before tagging.

The development machine completed the `0.03.000` release-candidate regression after the real-time scheduler, 256-frame capture prefill, Inputs/Device/Info GUI expansion, build/package cleanup, and 44.1 `SIGPIPE`/meter-startup hardening. No regression was observed in the previously validated audio/control functionality, and repeated rate switching completed reliably.
