#!/bin/bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "error: run this uninstaller with sudo" >&2
    exit 1
fi

INSTALL_ROOT="/Library/Application Support/macfw/fw410"
LAUNCHD_PLIST="/Library/LaunchDaemons/com.mbprado.macfw.fw410.transport.plist"
LABEL="com.mbprado.macfw.fw410.transport"

launchctl bootout system/$LABEL >/dev/null 2>&1 || true
rm -f "$LAUNCHD_PLIST"
rm -rf "$INSTALL_ROOT"

echo "unloaded launchd service: $LABEL"
echo "removed macfw FW410 transport runtime: $INSTALL_ROOT"
echo "preserved log: /Library/Logs/macfw-fw410-transport.log"
