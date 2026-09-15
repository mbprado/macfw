#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "error: package build requires macOS" >&2
    exit 1
fi

FW410_DIR="$REPO_DIR/devices/fw410"
FW1814_DIR="$REPO_DIR/devices/fw1814"

read_version() {
    sed -n 's/^#define MACFW_VERSION "\([^"]*\)"/\1/p' "$1" | head -1
}

FW410_VERSION="$(read_version "$FW410_DIR/version.h")"
FW1814_VERSION="$(read_version "$FW1814_DIR/version.h")"
if [[ -z "$FW410_VERSION" || -z "$FW1814_VERSION" ]]; then
    echo "error: unable to read both device versions" >&2
    exit 1
fi
if [[ "$FW410_VERSION" != "$FW1814_VERSION" ]]; then
    echo "error: combined package requires one shared version" >&2
    echo "FW410:  $FW410_VERSION" >&2
    echo "FW1814: $FW1814_VERSION" >&2
    exit 1
fi

VERSION="$FW410_VERSION"
GIT_SHA="$(git -C "$REPO_DIR" rev-parse --short=12 HEAD)"
IDENTIFIER="com.mbprado.macfw"
WORK="$SCRIPT_DIR/build/all"
ROOT="$WORK/root"
SCRIPTS="$WORK/scripts"
STAGE="$WORK/stage"
OUTPUT="$SCRIPT_DIR/dist"
PKG="$OUTPUT/macfw-${VERSION}-${GIT_SHA}.pkg"
PAYLOAD_ROOT="$ROOT/tmp/macfw-all-package/payload"
FW410_PAYLOAD_ROOT="$PAYLOAD_ROOT/fw410"
FW1814_PAYLOAD_ROOT="$PAYLOAD_ROOT/fw1814"
FW410_RUNTIME_ROOT="$FW410_PAYLOAD_ROOT/Library/Application Support/macfw/fw410"
FW1814_RUNTIME_ROOT="$FW1814_PAYLOAD_ROOT/Library/Application Support/macfw/fw1814"

FW410_HAL="$FW410_DIR/hal/build/macfw-fw410.driver"
FW410_APP="$FW410_DIR/control-panel/build/macfw-fw410-control.app"
FW410_PLIST="$FW410_DIR/service/com.mbprado.macfw.fw410.transport.plist"
FW410_PROBE="$FW410_DIR/tools/device/deviceprobe/deviceprobe"
FW410_RUNTIME=(
    "$FW410_DIR/tools/transport/haltransport/haltransport|tools/transport/haltransport/haltransport"
    "$FW410_DIR/tools/transport/halbridge44100/halbridge44100|tools/transport/halbridge44100/halbridge44100"
    "$FW410_DIR/tools/transport/halbridge48000/halbridge48000|tools/transport/halbridge48000/halbridge48000"
    "$FW410_DIR/tools/transport/transportstatus/transportstatus|tools/transport/transportstatus/transportstatus"
    "$FW410_DIR/tools/control/rateprobe/rateprobe|tools/control/rateprobe/rateprobe"
    "$FW410_DIR/tools/control/fw410ctl/fw410ctl|tools/control/fw410ctl/fw410ctl"
    "$FW410_DIR/tools/control/fw410state/fw410state|tools/control/fw410state/fw410state"
    "$FW410_DIR/tools/device/fwboot/fwboot|tools/device/fwboot/fwboot"
    "$FW410_PROBE|tools/device/deviceprobe/deviceprobe"
)

FW1814_HAL="$FW1814_DIR/hal/build/macfw-fw1814.driver"
FW1814_APP="$FW1814_DIR/control-panel/build/macfw-fw1814-control.app"
FW1814_PLIST="$FW1814_DIR/service/com.mbprado.macfw.fw1814.transport.plist"
FW1814_PROBE="$FW1814_DIR/tools/fw1814deviceprobe"
FW1814_RUNTIME=(
    "$FW1814_DIR/transport/fw1814supervisor|bin/fw1814supervisor"
    "$FW1814_DIR/transport/fw1814analog48|bin/fw1814analog48"
    "$FW1814_DIR/transport/fw1814analog44|bin/fw1814analog44"
    "$FW1814_DIR/tools/fw1814init|bin/fw1814init"
    "$FW1814_DIR/tools/fwboot1814|bin/fwboot1814"
    "$REPO_DIR/common/tools/firewirebusreset/firewirebusreset|bin/firewirebusreset"
    "$FW1814_DIR/tools/control/fw1814ctl/fw1814ctl|bin/fw1814ctl"
    "$FW1814_DIR/tools/control/fw1814state/fw1814state|bin/fw1814state"
    "$FW1814_PROBE|bin/fw1814deviceprobe"
)

required=(
    "$FW410_HAL/Contents/MacOS/macfw-fw410"
    "$FW410_APP/Contents/MacOS/macfw-fw410-control"
    "$FW410_PLIST"
    "$FW410_PROBE"
    "$FW1814_HAL/Contents/MacOS/macfw-fw1814"
    "$FW1814_APP/Contents/MacOS/macfw-fw1814-control"
    "$FW1814_PLIST"
    "$FW1814_PROBE"
    "$SCRIPT_DIR/scripts/all-preinstall"
    "$SCRIPT_DIR/scripts/all-postinstall"
    "$SCRIPT_DIR/scripts/postinstall"
    "$SCRIPT_DIR/scripts/fw1814-postinstall"
)
for entry in "${FW410_RUNTIME[@]}" "${FW1814_RUNTIME[@]}"; do
    IFS='|' read -r source _ <<< "$entry"
    required+=("$source")
done
for file in "${required[@]}"; do
    if [[ ! -e "$file" ]]; then
        echo "error: required combined-package artifact is missing: $file" >&2
        echo "run 'make' from the repository root first" >&2
        exit 1
    fi
done

rm -rf "$WORK"
mkdir -p \
    "$FW410_PAYLOAD_ROOT/Applications" \
    "$FW410_PAYLOAD_ROOT/Library/Audio/Plug-Ins/HAL" \
    "$FW410_RUNTIME_ROOT" \
    "$FW410_PAYLOAD_ROOT/Library/LaunchDaemons" \
    "$FW1814_PAYLOAD_ROOT/Applications" \
    "$FW1814_PAYLOAD_ROOT/Library/Audio/Plug-Ins/HAL" \
    "$FW1814_RUNTIME_ROOT" \
    "$FW1814_PAYLOAD_ROOT/Library/LaunchDaemons" \
    "$SCRIPTS" "$STAGE" "$OUTPUT"

cp -R "$FW410_HAL" "$FW410_PAYLOAD_ROOT/Library/Audio/Plug-Ins/HAL/"
cp -R "$FW1814_HAL" "$FW1814_PAYLOAD_ROOT/Library/Audio/Plug-Ins/HAL/"
cp -R "$FW410_APP" "$FW410_PAYLOAD_ROOT/Applications/macfw FW410 Control.app"
cp -R "$FW1814_APP" "$FW1814_PAYLOAD_ROOT/Applications/macfw FW1814 Control.app"
printf 'version=%s\nbuild=%s\n' "$VERSION" "$GIT_SHA" > "$FW410_RUNTIME_ROOT/runtime-build.conf"
printf 'version=%s\nbuild=%s\n' "$VERSION" "$GIT_SHA" > "$FW1814_RUNTIME_ROOT/runtime-build.conf"

stage_runtime() {
    local runtime_root="$1"
    shift
    local entry source destination
    for entry in "$@"; do
        IFS='|' read -r source destination <<< "$entry"
        mkdir -p "$runtime_root/$(dirname "$destination")"
        cp "$source" "$runtime_root/$destination"
    done
}
stage_runtime "$FW410_RUNTIME_ROOT" "${FW410_RUNTIME[@]}"
stage_runtime "$FW1814_RUNTIME_ROOT" "${FW1814_RUNTIME[@]}"

cp "$FW410_PLIST" "$FW410_PAYLOAD_ROOT/Library/LaunchDaemons/"
cp "$FW1814_PLIST" "$FW1814_PAYLOAD_ROOT/Library/LaunchDaemons/"
cp "$SCRIPT_DIR/scripts/all-postinstall" "$SCRIPTS/postinstall"
cp "$SCRIPT_DIR/scripts/postinstall" "$SCRIPTS/fw410-postinstall"
cp "$SCRIPT_DIR/scripts/fw1814-postinstall" "$SCRIPTS/fw1814-postinstall"
chmod 0755 "$SCRIPTS/postinstall" "$SCRIPTS/fw410-postinstall" "$SCRIPTS/fw1814-postinstall"

GATE_ROOT="$STAGE/probe-root/tmp/macfw-all-package"
mkdir -p "$GATE_ROOT" "$STAGE/probe-scripts"
cp "$FW410_PROBE" "$GATE_ROOT/deviceprobe"
cp "$FW1814_PROBE" "$GATE_ROOT/fw1814deviceprobe"
cp "$SCRIPT_DIR/scripts/all-preinstall" "$STAGE/probe-scripts/postinstall"
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

PAYLOAD_PKG="$STAGE/macfw.pkg"
pkgbuild --root "$ROOT" --scripts "$SCRIPTS" --component-plist "$COMPONENT_PLIST" \
    --identifier "$IDENTIFIER" --version "$VERSION" --install-location / "$PAYLOAD_PKG"

cat > "$STAGE/welcome.html" <<EOF
<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
body { font-family: -apple-system, sans-serif; font-size: 13px; line-height: 1.45; }
h2 { font-size: 15px; margin-top: 18px; } ul { margin-top: 6px; }
</style></head><body>
<p>You will be guided through the installation of macfw ${VERSION}.</p>
<h2>Supported interfaces</h2><ul>
<li>M-Audio FireWire 410</li>
<li>M-Audio FireWire 1814</li>
</ul>
<p>The package detects connected supported hardware and installs only the matching CoreAudio driver, transport service and native control panel. If both interfaces are connected, both are installed.</p>
<p>At least one supported interface must be connected during installation.</p>
</body></html>
EOF

cat > "$STAGE/Distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>macfw ${VERSION}</title>
    <organization>${IDENTIFIER}</organization>
    <welcome file="welcome.html" mime-type="text/html"/>
    <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
    <options customize="never" require-scripts="true"/>
    <choices-outline><line choice="hardware"/><line choice="drivers"/></choices-outline>
    <choice id="hardware" visible="false"><pkg-ref id="${IDENTIFIER}.hardware-gate"/></choice>
    <choice id="drivers" visible="false"><pkg-ref id="${IDENTIFIER}"/></choice>
    <pkg-ref id="${IDENTIFIER}.hardware-gate" version="${VERSION}">hardware-gate.pkg</pkg-ref>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}">macfw.pkg</pkg-ref>
</installer-gui-script>
EOF

productbuild --distribution "$STAGE/Distribution.xml" --package-path "$STAGE" \
    --resources "$STAGE" "$PKG"

echo "built: $PKG"
echo "devices: M-Audio FireWire 410, M-Audio FireWire 1814"
echo "version: $VERSION"
echo "build: $GIT_SHA"
