# macfw release and versioning policy

This document defines the release contract for macfw.

## Version format

Public releases use:

```text
x.y.zzz
```

where:

- `x` — major architectural generation or compatibility break;
- `y` — larger feature/update release;
- `zzz` — zero-padded patch, packaging, maintenance or small-fix release.

Examples:

```text
0.4.000   fourth development release
0.4.001   patch/fix to 0.4.000
1.0.000   first major/stable generation
```

Historical releases used a zero-padded middle field (`0.01.000` through
`0.03.000`). Those existing tags remain valid; new unified releases use the
non-padded form beginning with `0.4.000`.

FW410 and FW1814 use one project version. Both
`devices/fw410/version.h` and `devices/fw1814/version.h` must match before a
combined package or numeric release tag can be produced.

Pre-release status such as **alpha** or **beta** is represented by the GitHub
Release state/title rather than changing the numeric version embedded in the
binaries.

## Tag-driven releases

The normal release tag is the unified numeric version:

```text
0.4.000
```

It builds the combined package containing both supported interfaces.
Device-prefixed tags remain available when a device-specific package is needed:

```text
fw410-0.4.000
fw1814-0.4.000
```

The intended release flow is:

1. Validate the candidate on real supported hardware.
2. Update the changelog, release notes, limitations, compatibility and install documentation.
3. Merge the validated release-candidate work into `main`.
4. Confirm both embedded component versions match the intended release.
5. Build/check the combined package from `main`.
6. Create and push the numeric version tag on the release commit.
7. GitHub Actions validates the tag and both component versions.
8. The workflow builds the combined installer and SHA-256 checksum.
9. It creates the prerelease and attaches the package/checksum artifacts.

Release tags must identify the intended `main` release commit, not an
experiment or development branch.

## Primary release artifacts

The primary binary distribution is the combined native macOS installer:

```text
macfw-x.y.zzz-<build>.pkg
```

It contains the FW410 and FW1814 CoreAudio HAL plug-ins, namespaced
transport/control runtimes, persistent-state helpers, build metadata, launchd
definitions and native control panels. Its hardware gate accepts either
supported interface; installing the bundle installs support for both.

Device-specific packages remain available:

```text
macfw-fw410-x.y.zzz-<build>.pkg
macfw-fw1814-x.y.zzz-<build>.pkg
```

Build packages from the repository root with:

```bash
make package          # combined package; same as make package-all
make fw410-package    # FW410-only package
make fw1814-package   # FW1814-only package
```

Artifacts appear under `package/dist/`. Package targets perform a fresh build
before staging so embedded build identities correspond to the package commit.

## Source installation

A tagged release must remain buildable directly from source:

```bash
make
sudo make install
```

Root `make` builds both supported interfaces. Aggregate installation validates
all required FW410 and FW1814 artifacts before installing either interface.
Compilation must run as the normal user; `sudo make install` only installs
already-built artifacts.

## Source archive

GitHub automatically exposes source archives for tags/releases. Any explicit
project source archive must be generated from the tag/commit rather than from a
developer working tree.

## Optional future developer bundle

A separate full developer/diagnostic bundle may be introduced later for
compiled reverse-engineering and diagnostic tools. It is not required for
normal releases and must not block the normal driver package. Experimental or
destructive tools must remain clearly identified and must not silently become
part of a normal end-user installation.

## Checksums

Release automation publishes SHA-256 checksums for distributable binary
artifacts in `SHA256SUMS`. At minimum it must cover the published `.pkg`.

## Signing and notarization

Apple signing credentials must never be committed to the repository.

Current alpha releases separate functional package validation from Apple
distribution signing/notarization. Until those steps are implemented, the
GitHub Release must explicitly state that the package is unsigned and
unnotarized.

## Reproducibility and provenance

Release package names, embedded version metadata, release notes and checksums
must correspond to the triggering tag and commit. The build records the Git
commit SHA in its logs and embedded metadata.

For a unified release, the tag version must equal `MACFW_VERSION` in both device
headers. The release workflow enforces this before building.

## Release documentation

Every release candidate should review/update:

- [`CHANGELOG.md`](CHANGELOG.md);
- [`RELEASE-NOTES.md`](RELEASE-NOTES.md);
- [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md);
- [`COMPATIBILITY.md`](COMPATIBILITY.md);
- [`INSTALL.md`](INSTALL.md);
- [`README.md`](README.md).

## Current `0.4.000` release gate

Before tagging `0.4.000`, verify at minimum:

- clean combined source build with `make`;
- aggregate source installation with `sudo make install`;
- both interface control panels, HAL plug-ins and namespaced services are installed;
- `make package` produces exactly one combined installer;
- `make fw410-package` and `make fw1814-package` still produce individual installers;
- combined installation succeeds with an FW410 connected;
- combined installation succeeds with an FW1814 connected;
- each transport reaches `ONLINE` when its matching hardware is connected;
- FW410 44.1/48 kHz audio, controls, persistence and recovery remain regression-free;
- FW1814 44.1/48 kHz analog audio, controls, persistence and recovery remain regression-free;
- package/source install paths use the authoritative application names;
- package runtime metadata contains the exact version and source commit;
- known limitations and unsigned/unnotarized status are accurately stated;
- the release candidate is merged into `main` before tagging.
