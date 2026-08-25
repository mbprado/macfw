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
1.00.000   first major/stable generation
```

The numeric fields are zero-padded as shown. `yy` is two digits and `zzz` is three digits.

Release tags use the exact version string:

```text
0.01.000
```

A release workflow should reject tags that do not match:

```regex
^[0-9]+\.[0-9]{2}\.[0-9]{3}$
```

Pre-release status such as **alpha** or **beta** is represented by the GitHub Release state/title rather than changing the numeric version embedded in the binaries.

## Tag-driven releases

The intended release flow is:

1. Validate the candidate on real supported hardware.
2. Update `CHANGELOG.md`, `KNOWN-LIMITATIONS.md`, and `RELEASE-NOTES.md`.
3. Confirm the version embedded by the build matches the intended release.
4. Create and push the `x.yy.zzz` tag.
5. GitHub Actions validates the tag and builds on the supported macOS runner.
6. The workflow runs the release build and package path.
7. It calculates SHA-256 checksums.
8. It creates the GitHub Release and attaches the package/checksum artifacts.

The tag identifies the exact source revision used for the package.

## Primary release artifact

The first practical macfw binary distribution is the native macOS installer package:

```text
macfw-fw410-x.yy.zzz-<build>.pkg
```

The build identifier is the Git commit identifier embedded by the existing build system. The package is produced from the repository root with:

```bash
make package
```

and appears under:

```text
package/dist/
```

The package contains the FW410 CoreAudio HAL plug-in, self-contained transport runtime, launchd service definition, and installation scripts. Installation is gated by `deviceprobe --require-supported`, so a known supported interface must be connected in either operational or bootloader personality.

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

A separate full developer/diagnostic bundle may be introduced later for compiled reverse-engineering and diagnostic tools. It is not required for the first alpha release and must not block the normal driver package.

Experimental/destructive tools must remain clearly identified and must not silently become part of a normal end-user installation.

## Checksums

Release automation should publish SHA-256 checksums for distributable binary artifacts, preferably in:

```text
SHA256SUMS
```

At minimum it must cover the published `.pkg`.

## Signing and notarization

Apple signing credentials must never be committed to the repository.

The current first-alpha milestone separates **functional package validation** from Apple distribution signing/notarization. Until signing/notarization is implemented, the GitHub Release must explicitly state the artifact's status and must not imply that an unsigned/unnotarized build is signed.

When enabled, certificates and notarization credentials must be supplied through protected GitHub Actions secrets or another appropriate secret mechanism. The release workflow should then fail rather than silently publish an artifact that was expected to be signed/notarized but was not.

## Reproducibility and provenance

Release package names, embedded version metadata, release notes, and checksums must correspond to the triggering tag and commit.

The build should record the Git commit SHA in its logs and embedded metadata. The existing runtime and HAL build metadata provide the basis for this provenance.

## Release documentation

Every release candidate should review/update:

- [`CHANGELOG.md`](CHANGELOG.md) — accumulated user-visible changes;
- [`RELEASE-NOTES.md`](RELEASE-NOTES.md) — release-facing summary for the current candidate;
- [`KNOWN-LIMITATIONS.md`](KNOWN-LIMITATIONS.md) — current limitations and unsupported behavior;
- [`INSTALL.md`](INSTALL.md) — installation, status, troubleshooting, and source-build instructions.

## Current first-alpha gate

Before tagging `0.01.000`, verify at minimum:

- clean source build with `make`;
- source installation with `sudo make install`;
- root `make package` produces the expected `.pkg`;
- fresh `.pkg` installation succeeds with a supported FW410 attached;
- launchd runtime reaches `ONLINE`;
- 44.1 kHz playback/capture;
- 48 kHz playback/capture;
- runtime 44.1 <-> 48 kHz switching;
- physical disconnect/reconnect recovery;
- reboot recovery;
- boot without interface followed by delayed attachment;
- launchd process restart;
- known limitations and signing status are accurately stated.

The development machine has already hardware-validated these functional behaviors; the final release pass exists to ensure the exact tagged/package artifact preserves them.
