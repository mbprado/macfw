#!/bin/bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "error: run this uninstaller with sudo" >&2
    exit 1
fi

LABEL="com.mbprado.macfw.fw1814.transport"
LAUNCHD_PLIST="/Library/LaunchDaemons/com.mbprado.macfw.fw1814.transport.plist"
INSTALL_ROOT="/Library/Application Support/macfw/fw1814"

launchctl bootout system/$LABEL >/dev/null 2>&1 || true
rm -f "$LAUNCHD_PLIST"
rm -rf "$INSTALL_ROOT"

echo "removed macfw FW1814 transport service"
