#!/bin/bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "error: run this installer with sudo" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW410_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_ROOT="/Library/Application Support/macfw/fw410"
LAUNCHD_PLIST="/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist"
LABEL="com.mbprado.macfw.fw410.transport"

HALTRANSPORT="$FW410_DIR/tools/transport/haltransport/haltransport"
BRIDGE44100="$FW410_DIR/tools/transport/halbridge44100/halbridge44100"
BRIDGE48000="$FW410_DIR/tools/transport/halbridge48000/halbridge48000"
FWBOOT="$FW410_DIR/tools/device/fwboot/fwboot"
DEVICEPROBE="$FW410_DIR/tools/device/deviceprobe/deviceprobe"
TRANSPORTSTATUS="$FW410_DIR/tools/transport/transportstatus/transportstatus"

for file in "$HALTRANSPORT" "$BRIDGE44100" "$BRIDGE48000" "$FWBOOT" "$DEVICEPROBE" "$TRANSPORTSTATUS"; do
    if [[ ! -x "$file" ]]; then
        echo "error: required runtime binary is missing or not executable: $file" >&2
        echo "build the FW410 tools before installing the service" >&2
        exit 1
    fi
done

set +e
"$DEVICEPROBE" --require-supported
probe_status=$?
set -e
if [[ $probe_status -ne 0 ]]; then
    if [[ $probe_status -eq 3 ]]; then
        echo "error: no supported macfw FireWire interface is connected" >&2
        echo "connect a supported interface in operational or bootloader mode and retry" >&2
    else
        echo "error: macfw device detection failed with status $probe_status" >&2
    fi
    exit "$probe_status"
fi

# Stop an older loaded instance before replacing its runtime files.
launchctl bootout system/$LABEL >/dev/null 2>&1 || true

install -d -o root -g wheel -m 0755 \
    "$INSTALL_ROOT/tools/transport/haltransport" \
    "$INSTALL_ROOT/tools/transport/halbridge44100" \
    "$INSTALL_ROOT/tools/transport/halbridge48000" \
    "$INSTALL_ROOT/tools/transport/transportstatus" \
    "$INSTALL_ROOT/tools/device/fwboot" \
    "$INSTALL_ROOT/tools/device/deviceprobe"

install -o root -g wheel -m 0755 "$HALTRANSPORT" \
    "$INSTALL_ROOT/tools/transport/haltransport/haltransport"
install -o root -g wheel -m 0755 "$BRIDGE44100" \
    "$INSTALL_ROOT/tools/transport/halbridge44100/halbridge44100"
install -o root -g wheel -m 0755 "$BRIDGE48000" \
    "$INSTALL_ROOT/tools/transport/halbridge48000/halbridge48000"
install -o root -g wheel -m 0755 "$TRANSPORTSTATUS" \
    "$INSTALL_ROOT/tools/transport/transportstatus/transportstatus"
install -o root -g wheel -m 0755 "$FWBOOT" \
    "$INSTALL_ROOT/tools/device/fwboot/fwboot"
install -o root -g wheel -m 0755 "$DEVICEPROBE" \
    "$INSTALL_ROOT/tools/device/deviceprobe/deviceprobe"

install -o root -g wheel -m 0644 \
    "$SCRIPT_DIR/com.mbprado.macfw.fw410.transport.plist" "$LAUNCHD_PLIST"

: > /Library/Logs/macfw-fw410-transport.log
chown root:wheel /Library/Logs/macfw-fw410-transport.log
chmod 0644 /Library/Logs/macfw-fw410-transport.log

launchctl bootstrap system "$LAUNCHD_PLIST"
launchctl enable system/$LABEL
launchctl kickstart -k system/$LABEL

echo "installed macfw FW410 transport runtime: $INSTALL_ROOT"
echo "loaded launchd service: $LABEL"
echo "log: /Library/Logs/macfw-fw410-transport.log"
