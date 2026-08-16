# macfw release and versioning policy

This document defines the release contract for macfw before the first distributable driver build exists. The GitHub Actions implementation should follow this contract once the AudioDriverKit application/extension has a reproducible signed build.

## Version format

All public releases use:

```text
x.yy.zzz
```

where:

- `x` — major version. Increment for a major architectural generation, compatibility break, or other project-wide major release.
- `yy` — feature/update version. Increment for larger updates and feature additions.
- `zzz` — patch version. Increment for minor fixes, small corrections, packaging changes, and maintenance releases.

Examples:

```text
0.01.000   first development release
0.01.001   patch/fix to 0.01.000
0.02.000   larger update or feature addition
1.00.000   first major/stable generation
```

The numeric fields are displayed zero-padded as shown. `yy` is two digits and `zzz` is three digits. A release tag uses the exact version string, without a mandatory `v` prefix:

```text
0.01.000
```

The release workflow must reject tags that do not match:

```regex
^[0-9]+\.[0-9]{2}\.[0-9]{3}$
```

## Tag-driven releases

A GitHub Actions release workflow will be activated when the project has its first reproducibly buildable piece of distributable software.

The intended release flow is:

1. Finish and validate the release candidate on real FW410 hardware.
2. Update release notes/changelog.
3. Create and push a tag matching `x.yy.zzz`.
4. GitHub Actions validates the tag.
5. The workflow builds the signed/notarized software where required.
6. The workflow assembles the three release packages below.
7. The workflow calculates checksums.
8. The workflow creates the GitHub Release for that tag and attaches all packages/checksums.

A tag must identify the exact source revision used for all three packages.

## Release packages

Every software release will contain three packages built from the same tag.

### 1. Lite

Filename convention:

```text
macfw-x.yy.zzz-lite.tar.gz
```

Purpose: normal users who only want to use the FW410.

Contents will be the minimum runtime installation set required for the working driver. Based on the current architecture this is expected to include:

- the host macOS application/system-extension container if required for installation;
- the AudioDriverKit `.dext`;
- the macfw/FW410 transport service or daemon if the final architecture requires it;
- required runtime configuration/resources;
- install/uninstall instructions or scripts where appropriate.

Diagnostic and reverse-engineering tools are intentionally excluded.

`lite` means **driver/runtime only**, not literally a single binary if macOS requires several cooperating components.

### 2. Full

Filename convention:

```text
macfw-x.yy.zzz-full.tar.gz
```

Purpose: developers, testers, diagnostics, hardware investigation, and advanced users.

Contents:

- everything from `lite`;
- the compiled tools under `fw410/tools/device/`;
- the compiled tools under `fw410/tools/control/`;
- the compiled tools under `fw410/tools/transport/` that are safe/useful to distribute;
- supporting command-line utilities and documentation needed to use those tools.

Experimental/destructive tools should remain clearly marked and should not silently become part of a normal end-user workflow.

### 3. Source

Filename convention:

```text
macfw-x.yy.zzz-source.tar.gz
```

Purpose: exact source snapshot for the release.

This is a `.tar.gz` archive of the repository at the release tag. It must represent committed source only and must not contain local build products, credentials, signing certificates, provisioning profiles, or other machine-specific/private material.

The source archive should be generated from Git rather than by archiving a developer working tree, for example conceptually with `git archive`, so its contents correspond exactly to the tagged revision.

## Checksums

The release should additionally publish SHA-256 checksums for the generated artifacts, preferably in:

```text
SHA256SUMS
```

At minimum the checksum file must cover the `lite`, `full`, and `source` archives.

## Signing and notarization

The release workflow must not embed Apple signing credentials in the repository.

When the DriverKit build reaches distribution stage, required certificates, provisioning information, App Store Connect/notarization credentials, or equivalent secrets must be supplied through GitHub Actions secrets or another appropriate protected secret mechanism.

A release must fail rather than silently publish an unsigned/unnotarized package when signing/notarization is required for the target macOS installation path.

## Reproducibility and provenance

All package names, embedded version metadata, and release notes must derive from the tag that triggered the workflow. The workflow must not independently invent a version number.

The same tag must produce:

```text
macfw-x.yy.zzz-lite.tar.gz
macfw-x.yy.zzz-full.tar.gz
macfw-x.yy.zzz-source.tar.gz
SHA256SUMS
```

The workflow should record the Git commit SHA in its logs and, where practical, in a small build metadata file included with binary packages.

## When to implement the workflow

Do not freeze the GitHub Actions build commands before the Xcode AudioDriverKit project and runtime layout exist. The package contract and version policy in this document are stable now; the exact build/sign/notarize commands should be added as soon as the first working driver target can be built reproducibly from the repository.
