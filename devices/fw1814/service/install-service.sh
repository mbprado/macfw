#!/bin/bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "error: run this installer with sudo" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW1814_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_ROOT="/Library/Application Support/macfw/fw1814"
BIN_DIR="$INSTALL_ROOT/bin"
LAUNCHD_PLIST="/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist"
LABEL="com.mbprado.macfw.fw1814.transport"
LOG="/Library/Logs/macfw-fw1814-transport.log"

SUPERVISOR="$FW1814_DIR/transport/fw1814supervisor"
ENGINE48="$FW1814_DIR/transport/fw1814analog48"
ENGINE44="$FW1814_DIR/transport/fw1814analog44"
INIT="$FW1814_DIR/tools/fw1814init"
BOOT="$FW1814_DIR/tools/fwboot1814"
BUS_RESET="$FW1814_DIR/../../common/tools/firewirebusreset/firewirebusreset"
CONTROL="$FW1814_DIR/tools/control/fw1814ctl/fw1814ctl"

for file in "$SUPERVISOR" "$ENGINE48" "$ENGINE44" "$INIT" "$BOOT" "$BUS_RESET" "$CONTROL"; do
    if [[ ! -x "$file" ]]; then
        echo "error: required FW1814 runtime binary is missing or not executable: $file" >&2
        echo "build with:" >&2
        echo "  make fw1814-runtime" >&2
        exit 1
    fi
done

launchctl bootout system/$LABEL >/dev/null 2>&1 || true

install -d -o root -g wheel -m 0755 "$BIN_DIR"
install -o root -g wheel -m 0755 "$SUPERVISOR" "$BIN_DIR/fw1814supervisor"
install -o root -g wheel -m 0755 "$ENGINE48" "$BIN_DIR/fw1814analog48"
install -o root -g wheel -m 0755 "$ENGINE44" "$BIN_DIR/fw1814analog44"
install -o root -g wheel -m 0755 "$INIT" "$BIN_DIR/fw1814init"
install -o root -g wheel -m 0755 "$BOOT" "$BIN_DIR/fwboot1814"
install -o root -g wheel -m 0755 "$BUS_RESET" "$BIN_DIR/firewirebusreset"
install -o root -g wheel -m 0755 "$CONTROL" "$BIN_DIR/fw1814ctl"

install -o root -g wheel -m 0644 \
    "$SCRIPT_DIR/com.mbprado.macfw.fw1814.transport.plist" "$LAUNCHD_PLIST"

: > "$LOG"
chown root:wheel "$LOG"
chmod 0644 "$LOG"

launchctl bootstrap system "$LAUNCHD_PLIST"
launchctl enable system/$LABEL
launchctl kickstart -k system/$LABEL

echo "installed macfw FW1814 transport runtime: $INSTALL_ROOT"
echo "loaded launchd service: $LABEL"
echo "automatic reconnect + guarded bootloader recovery: enabled"
echo "automatic 44.1/48 kHz transport selection: enabled"
echo "validated clean bus reset before transport recovery: enabled"
echo "log: $LOG"
