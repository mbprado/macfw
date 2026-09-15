#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEVICE="${1:-${MACFW_DEVICE:-fw410}}"

usage() {
    echo "usage: $0 [fw410|fw1814]"
    echo "       MACFW_DEVICE=fw410|fw1814 $0"
}

case "$DEVICE" in
fw410)
    DEV_DIR="$REPO_DIR/devices/fw410"
    VERSION_HEADER="$DEV_DIR/version.h"
    DISPLAY_NAME="M-Audio FireWire 410"
    CONTROL_DISPLAY="macfw FW410 Control"
    SLUG="fw410"
    IDENTIFIER="com.mbprado.macfw.fw410"
    HAL_BUNDLE="$DEV_DIR/hal/build/macfw-fw410.driver"
    HAL_BINARY="$HAL_BUNDLE/Contents/MacOS/macfw-fw410"
    CONTROL_APP="$DEV_DIR/control-panel/build/macfw-fw410-control.app"
    CONTROL_BINARY="$CONTROL_APP/Contents/MacOS/macfw-fw410-control"
    CONTROL_APP_INSTALL_NAME="macfw FW410 Control.app"
    PLIST="$DEV_DIR/service/com.mbprado.macfw.fw410.transport.plist"
    PROBE="$DEV_DIR/tools/device/deviceprobe/deviceprobe"
    PROBE_NAME="deviceprobe"
    PREINSTALL="$SCRIPT_DIR/scripts/preinstall"
    POSTINSTALL="$SCRIPT_DIR/scripts/postinstall"
    WELCOME_SCOPE="Native 44.1/48 kHz audio, recovery, persistent controls and the FW410 control panel"
    runtime=(
        "$DEV_DIR/tools/transport/haltransport/haltransport|tools/transport/haltransport/haltransport"
        "$DEV_DIR/tools/transport/halbridge44100/halbridge44100|tools/transport/halbridge44100/halbridge44100"
        "$DEV_DIR/tools/transport/halbridge48000/halbridge48000|tools/transport/halbridge48000/halbridge48000"
        "$DEV_DIR/tools/transport/transportstatus/transportstatus|tools/transport/transportstatus/transportstatus"
        "$DEV_DIR/tools/control/rateprobe/rateprobe|tools/control/rateprobe/rateprobe"
        "$DEV_DIR/tools/control/fw410ctl/fw410ctl|tools/control/fw410ctl/fw410ctl"
        "$DEV_DIR/tools/control/fw410state/fw410state|tools/control/fw410state/fw410state"
        "$DEV_DIR/tools/device/fwboot/fwboot|tools/device/fwboot/fwboot"
        "$DEV_DIR/tools/device/deviceprobe/deviceprobe|tools/device/deviceprobe/deviceprobe"
    )
    ;;
fw1814)
    DEV_DIR="$REPO_DIR/devices/fw1814"
    VERSION_HEADER="$DEV_DIR/version.h"
    DISPLAY_NAME="M-Audio FireWire 1814"
    CONTROL_DISPLAY="macfw FW1814 Control"
    SLUG="fw1814"
    IDENTIFIER="com.mbprado.macfw.fw1814"
    HAL_BUNDLE="$DEV_DIR/hal/build/macfw-fw1814.driver"
    HAL_BINARY="$HAL_BUNDLE/Contents/MacOS/macfw-fw1814"
    CONTROL_APP="$DEV_DIR/control-panel/build/macfw-fw1814-control.app"
    CONTROL_BINARY="$CONTROL_APP/Contents/MacOS/macfw-fw1814-control"
    CONTROL_APP_INSTALL_NAME="macfw FW1814 Control.app"
    PLIST="$DEV_DIR/service/com.mbprado.macfw.fw1814.transport.plist"
    PROBE="$DEV_DIR/tools/fw1814deviceprobe"
    PROBE_NAME="fw1814deviceprobe"
    PREINSTALL="$SCRIPT_DIR/scripts/fw1814-preinstall"
    POSTINSTALL="$SCRIPT_DIR/scripts/fw1814-postinstall"
    WELCOME_SCOPE="Hardware-validated 44.1/48 kHz analog audio, recovery, persistent controls and the FW1814 control panel"
    runtime=(
        "$DEV_DIR/transport/fw1814supervisor|bin/fw1814supervisor"
        "$DEV_DIR/transport/fw1814analog48|bin/fw1814analog48"
        "$DEV_DIR/transport/fw1814analog44|bin/fw1814analog44"
        "$DEV_DIR/tools/fw1814init|bin/fw1814init"
        "$DEV_DIR/tools/fwboot1814|bin/fwboot1814"
        "$REPO_DIR/common/tools/firewirebusreset/firewirebusreset|bin/firewirebusreset"
        "$DEV_DIR/tools/control/fw1814ctl/fw1814ctl|bin/fw1814ctl"
        "$DEV_DIR/tools/control/fw1814state/fw1814state|bin/fw1814state"
        "$DEV_DIR/tools/fw1814deviceprobe|bin/fw1814deviceprobe"
    )
    ;;
-h|--help)
    usage
    exit 0
    ;;
*)
    echo "error: unsupported package device '$DEVICE'" >&2
    usage >&2
    exit 64
    ;;
esac

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

WORK="$SCRIPT_DIR/build"
ROOT="$WORK/root"
SCRIPTS="$WORK/scripts"
STAGE="$WORK/stage"
OUTPUT="$SCRIPT_DIR/dist"
PKG="$OUTPUT/macfw-${SLUG}-${VERSION}-${GIT_SHA}.pkg"
RUNTIME_ROOT="$ROOT/Library/Application Support/macfw/$SLUG"

required=("$HAL_BINARY" "$CONTROL_BINARY" "$PLIST" "$PROBE" "$PREINSTALL" "$POSTINSTALL")
for entry in "${runtime[@]}"; do
    IFS='|' read -r source destination <<< "$entry"
    required+=("$source")
done
for file in "${required[@]}"; do
    if [[ ! -e "$file" ]]; then
        echo "error: required $SLUG artifact is missing: $file" >&2
        echo "run 'make ${SLUG}' from the repository root first" >&2
        exit 1
    fi
done

rm -rf "$WORK"
mkdir -p "$ROOT/Applications" "$ROOT/Library/Audio/Plug-Ins/HAL" \
    "$RUNTIME_ROOT" "$ROOT/Library/LaunchDaemons" "$SCRIPTS" "$STAGE" "$OUTPUT"

cp -R "$HAL_BUNDLE" "$ROOT/Library/Audio/Plug-Ins/HAL/"
cp -R "$CONTROL_APP" "$ROOT/Applications/$CONTROL_APP_INSTALL_NAME"
printf 'version=%s\nbuild=%s\n' "$VERSION" "$GIT_SHA" > "$RUNTIME_ROOT/runtime-build.conf"

for entry in "${runtime[@]}"; do
    IFS='|' read -r source destination <<< "$entry"
    mkdir -p "$RUNTIME_ROOT/$(dirname "$destination")"
    cp "$source" "$RUNTIME_ROOT/$destination"
done
cp "$PLIST" "$ROOT/Library/LaunchDaemons/"
cp "$POSTINSTALL" "$SCRIPTS/postinstall"
chmod 0755 "$SCRIPTS/postinstall"

GATE_ROOT="$STAGE/probe-root/tmp/macfw-${SLUG}-package"
mkdir -p "$GATE_ROOT" "$STAGE/probe-scripts"
cp "$PROBE" "$GATE_ROOT/$PROBE_NAME"
cp "$PREINSTALL" "$STAGE/probe-scripts/postinstall"
chmod 0755 "$STAGE/probe-scripts/postinstall"

pkgbuild --root "$STAGE/probe-root" --scripts "$STAGE/probe-scripts" \
    --identifier "${IDENTIFIER}.hardware-gate" --version "$VERSION" \
    --install-location / "$STAGE/hardware-gate.pkg"

COMPONENT_PLIST="$STAGE/components.plist"
pkgbuild --analyze --root "$ROOT" "$COMPONENT_PLIST"
for ((index=0; ; ++index)); do
    if ! /usr/libexec/PlistBuddy -c "Print :${index}:RootRelativeBundlePath" "$COMPONENT_PLIST" >/dev/null 2>&1; then
        break
    fi
    if /usr/libexec/PlistBuddy -c "Print :${index}:BundleIsRelocatable" "$COMPONENT_PLIST" >/dev/null 2>&1; then
        /usr/libexec/PlistBuddy -c "Set :${index}:BundleIsRelocatable false" "$COMPONENT_PLIST"
    else
        /usr/libexec/PlistBuddy -c "Add :${index}:BundleIsRelocatable bool false" "$COMPONENT_PLIST"
    fi
done

PAYLOAD_PKG="$STAGE/${SLUG}.pkg"
pkgbuild --root "$ROOT" --scripts "$SCRIPTS" --component-plist "$COMPONENT_PLIST" \
    --identifier "$IDENTIFIER" --version "$VERSION" --install-location / "$PAYLOAD_PKG"

cat > "$STAGE/welcome.html" <<EOF
<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
body { font-family: -apple-system, sans-serif; font-size: 13px; line-height: 1.45; }
h2 { font-size: 15px; margin-top: 18px; } ul { margin-top: 6px; }
</style></head><body>
<p>You will be guided through the installation of macfw ${VERSION} for the ${DISPLAY_NAME}.</p>
<h2>Included components</h2><ul>
<li>${DISPLAY_NAME} CoreAudio driver and transport service</li>
<li>${CONTROL_DISPLAY} application</li>
<li>Persistent control-state restore across restart and reconnect</li>
</ul><h2>Current scope</h2><p>${WELCOME_SCOPE}.</p>
<p>The installer checks for supported FireWire hardware before installing.</p>
</body></html>
EOF

cat > "$STAGE/Distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>macfw ${DISPLAY_NAME} ${VERSION}</title>
    <organization>${IDENTIFIER}</organization>
    <welcome file="welcome.html" mime-type="text/html"/>
    <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
    <options customize="never" require-scripts="true"/>
    <choices-outline><line choice="hardware"/><line choice="driver"/></choices-outline>
    <choice id="hardware" visible="false"><pkg-ref id="${IDENTIFIER}.hardware-gate"/></choice>
    <choice id="driver" visible="false"><pkg-ref id="${IDENTIFIER}"/></choice>
    <pkg-ref id="${IDENTIFIER}.hardware-gate" version="${VERSION}">hardware-gate.pkg</pkg-ref>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}">${SLUG}.pkg</pkg-ref>
</installer-gui-script>
EOF

productbuild --distribution "$STAGE/Distribution.xml" --package-path "$STAGE" \
    --resources "$STAGE" "$PKG"

echo "built: $PKG"
echo "device: $DISPLAY_NAME"
echo "version: $VERSION"
echo "build: $GIT_SHA"
