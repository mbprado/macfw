#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FW410_DIR="$REPO_DIR/fw410"
VERSION_HEADER="$FW410_DIR/version.h"
WORK="$SCRIPT_DIR/build"
ROOT="$WORK/root"
SCRIPTS="$WORK/scripts"
STAGE="$WORK/stage"
OUTPUT="$SCRIPT_DIR/dist"
IDENTIFIER="com.mbprado.macfw.fw410"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "error: package build requires macOS" >&2
    exit 1
fi

VERSION="$(sed -n 's/^#define MACFW_VERSION "\([^"]*\)"/\1/p' "$VERSION_HEADER" | head -1)"
if [[ -z "$VERSION" ]]; then
    echo "error: unable to read MACFW_VERSION from $VERSION_HEADER" >&2
    exit 1
fi

GIT_SHA="$(git -C "$REPO_DIR" rev-parse --short=12 HEAD)"
PKG="$OUTPUT/macfw-fw410-${VERSION}-${GIT_SHA}.pkg"

HAL_BUNDLE="$FW410_DIR/hal/build/macfw-fw410.driver"
PLIST="$FW410_DIR/service/com.mbprado.macfw.fw410.transport.plist"
DEVICEPROBE="$FW410_DIR/tools/device/deviceprobe/deviceprobe"

required=(
    "$HAL_BUNDLE/Contents/MacOS/macfw-fw410"
    "$FW410_DIR/tools/transport/haltransport/haltransport"
    "$FW410_DIR/tools/transport/halbridge44100/halbridge44100"
    "$FW410_DIR/tools/transport/halbridge48000/halbridge48000"
    "$FW410_DIR/tools/transport/transportstatus/transportstatus"
    "$FW410_DIR/tools/control/rateprobe/rateprobe"
    "$FW410_DIR/tools/device/fwboot/fwboot"
    "$DEVICEPROBE"
    "$PLIST"
)

for file in "${required[@]}"; do
    if [[ ! -e "$file" ]]; then
        echo "error: required built artifact is missing: $file" >&2
        echo "run 'make runtime' from the repository root first" >&2
        exit 1
    fi
done

rm -rf "$WORK"
mkdir -p \
    "$ROOT/Library/Audio/Plug-Ins/HAL" \
    "$ROOT/Library/Application Support/macfw/fw410" \
    "$ROOT/Library/LaunchDaemons" \
    "$SCRIPTS" "$STAGE" "$OUTPUT"

cp -R "$HAL_BUNDLE" "$ROOT/Library/Audio/Plug-Ins/HAL/"

for rel in \
    tools/transport/haltransport/haltransport \
    tools/transport/halbridge44100/halbridge44100 \
    tools/transport/halbridge48000/halbridge48000 \
    tools/transport/transportstatus/transportstatus \
    tools/control/rateprobe/rateprobe \
    tools/device/fwboot/fwboot \
    tools/device/deviceprobe/deviceprobe; do
    mkdir -p "$ROOT/Library/Application Support/macfw/fw410/$(dirname "$rel")"
    cp "$FW410_DIR/$rel" "$ROOT/Library/Application Support/macfw/fw410/$rel"
done

cp "$PLIST" "$ROOT/Library/LaunchDaemons/"
cp "$SCRIPT_DIR/scripts/postinstall" "$SCRIPTS/postinstall"
chmod 0755 "$SCRIPTS/postinstall"

mkdir -p "$STAGE/probe-root/tmp/macfw-fw410-package"
cp "$DEVICEPROBE" "$STAGE/probe-root/tmp/macfw-fw410-package/deviceprobe"
mkdir -p "$STAGE/probe-scripts"
cp "$SCRIPT_DIR/scripts/preinstall" "$STAGE/probe-scripts/postinstall"
chmod 0755 "$STAGE/probe-scripts/postinstall"

pkgbuild \
    --root "$STAGE/probe-root" \
    --scripts "$STAGE/probe-scripts" \
    --identifier "${IDENTIFIER}.hardware-gate" \
    --version "$VERSION" \
    --install-location / \
    "$STAGE/hardware-gate.pkg"

pkgbuild \
    --root "$ROOT" \
    --scripts "$SCRIPTS" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --install-location / \
    "$STAGE/fw410.pkg"

cat > "$STAGE/Distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>macfw FireWire Audio Driver ${VERSION}</title>
    <organization>${IDENTIFIER}</organization>
    <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
    <options customize="never" require-scripts="true"/>
    <choices-outline>
        <line choice="hardware"/>
        <line choice="driver"/>
    </choices-outline>
    <choice id="hardware" visible="false">
        <pkg-ref id="${IDENTIFIER}.hardware-gate"/>
    </choice>
    <choice id="driver" visible="false">
        <pkg-ref id="${IDENTIFIER}"/>
    </choice>
    <pkg-ref id="${IDENTIFIER}.hardware-gate" version="${VERSION}">hardware-gate.pkg</pkg-ref>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}">fw410.pkg</pkg-ref>
</installer-gui-script>
EOF

productbuild \
    --distribution "$STAGE/Distribution.xml" \
    --package-path "$STAGE" \
    "$PKG"

echo "built: $PKG"
echo "version: $VERSION"
echo "build: $GIT_SHA"
