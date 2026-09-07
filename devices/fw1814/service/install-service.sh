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
ENGINE="$FW1814_DIR/transport/fw1814analog48"
INIT="$FW1814_DIR/tools/fw1814init"

for file in "$SUPERVISOR" "$ENGINE" "$INIT"; do
    if [[ ! -x "$file" ]]; then
        echo "error: required FW1814 runtime binary is missing or not executable: $file" >&2
        echo "build with:" >&2
        echo "  make -C devices/fw1814/transport clean all" >&2
        echo "  make -C devices/fw1814/tools fw1814init" >&2
        exit 1
    fi
done

launchctl bootout system/$LABEL >/dev/null 2>&1 || true

install -d -o root -g wheel -m 0755 "$BIN_DIR"
install -o root -g wheel -m 0755 "$SUPERVISOR" "$BIN_DIR/fw1814supervisor"
install -o root -g wheel -m 0755 "$ENGINE" "$BIN_DIR/fw1814analog48"
install -o root -g wheel -m 0755 "$INIT" "$BIN_DIR/fw1814init"

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
echo "log: $LOG"
