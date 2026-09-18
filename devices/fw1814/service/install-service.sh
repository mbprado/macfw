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
ENGINE96="$FW1814_DIR/transport/fw1814analog96"
ENGINE88="$FW1814_DIR/transport/fw1814analog88"
INIT="$FW1814_DIR/tools/fw1814init"
BOOT="$FW1814_DIR/tools/fwboot1814"
BUS_RESET="$FW1814_DIR/../../common/tools/firewirebusreset/firewirebusreset"
CONTROL="$FW1814_DIR/tools/control/fw1814ctl/fw1814ctl"
STATE_CONTROL="$FW1814_DIR/tools/control/fw1814state/fw1814state"
DEVICE_PROBE="$FW1814_DIR/tools/fw1814deviceprobe"
STATE_FILE="$INSTALL_ROOT/control-state.conf"
ENABLE96="$INSTALL_ROOT/enable-96-experimental"
ENABLE88="$INSTALL_ROOT/enable-88-experimental"

if [[ "${MACFW_INSTALL_EXPERIMENTAL88:-0}" == 1 && ! -x "$ENGINE88" ]]; then
    echo "error: experimental FW1814 88.2 kHz engine is missing: $ENGINE88" >&2
    exit 1
fi

if [[ "${MACFW_INSTALL_EXPERIMENTAL96:-0}" == 1 && ! -x "$ENGINE96" ]]; then
    echo "error: experimental FW1814 96 kHz engine is missing: $ENGINE96" >&2
    exit 1
fi

for file in "$SUPERVISOR" "$ENGINE48" "$ENGINE44" "$INIT" "$BOOT" "$BUS_RESET" "$CONTROL" "$STATE_CONTROL" "$DEVICE_PROBE"; do
    if [[ ! -x "$file" ]]; then
        echo "error: required FW1814 runtime binary is missing or not executable: $file" >&2
        echo "build with:" >&2
        echo "  make fw1814-runtime" >&2
        exit 1
    fi
done

if [[ "${MACFW_SKIP_HARDWARE_GATE:-0}" == 1 ]]; then
    echo "forced install: skipping the FW1814-only hardware gate"
else
    set +e
    "$DEVICE_PROBE" --require-supported
    probe_status=$?
    set -e
    if [[ $probe_status -ne 0 ]]; then
        if [[ $probe_status -eq 3 ]]; then
            echo "error: no supported M-Audio FireWire 1814 is connected" >&2
        else
            echo "error: FW1814 device detection failed with status $probe_status" >&2
        fi
        exit "$probe_status"
    fi
fi

if [[ "${1:-}" == "--check-only" ]]; then
    echo "hardware and runtime preflight passed"
    exit 0
fi

launchctl bootout system/$LABEL >/dev/null 2>&1 || true

install -d -o root -g wheel -m 0755 "$BIN_DIR"
had_enable96=0
[[ -e "$ENABLE96" ]] && had_enable96=1
had_enable88=0
[[ -e "$ENABLE88" ]] && had_enable88=1
install -o root -g wheel -m 0755 "$SUPERVISOR" "$BIN_DIR/fw1814supervisor"
install -o root -g wheel -m 0755 "$ENGINE48" "$BIN_DIR/fw1814analog48"
install -o root -g wheel -m 0755 "$ENGINE44" "$BIN_DIR/fw1814analog44"
if [[ "${MACFW_INSTALL_EXPERIMENTAL96:-0}" == 1 ]]; then
    install -o root -g wheel -m 0755 "$ENGINE96" "$BIN_DIR/fw1814analog96"
    install -o root -g wheel -m 0644 /dev/null "$ENABLE96"
elif [[ "${MACFW_INSTALL_EXPERIMENTAL88:-0}" != 1 ]]; then
    rm -f "$ENABLE96" "$BIN_DIR/fw1814analog96"
fi
if [[ "${MACFW_INSTALL_EXPERIMENTAL88:-0}" == 1 ]]; then
    install -o root -g wheel -m 0755 "$ENGINE88" "$BIN_DIR/fw1814analog88"
    install -o root -g wheel -m 0644 /dev/null "$ENABLE88"
elif [[ "${MACFW_INSTALL_EXPERIMENTAL96:-0}" != 1 ]]; then
    rm -f "$ENABLE88" "$BIN_DIR/fw1814analog88"
fi
if [[ "${MACFW_INSTALL_EXPERIMENTAL96:-0}" == 1 || $had_enable96 == 1 ||
      "${MACFW_INSTALL_EXPERIMENTAL88:-0}" == 1 || $had_enable88 == 1 ]]; then
    # CoreAudio caches available formats; reload after the opt-in gate changes.
    killall coreaudiod >/dev/null 2>&1 || true
fi
install -o root -g wheel -m 0755 "$INIT" "$BIN_DIR/fw1814init"
install -o root -g wheel -m 0755 "$BOOT" "$BIN_DIR/fwboot1814"
install -o root -g wheel -m 0755 "$BUS_RESET" "$BIN_DIR/firewirebusreset"
install -o root -g wheel -m 0755 "$CONTROL" "$BIN_DIR/fw1814ctl"
install -o root -g wheel -m 0755 "$STATE_CONTROL" "$BIN_DIR/fw1814state"
install -o root -g wheel -m 0755 "$DEVICE_PROBE" "$BIN_DIR/fw1814deviceprobe"

runtime_version="$(sed -n 's/^#define MACFW_VERSION "\([^"]*\)"/\1/p' "$FW1814_DIR/version.h" | head -n 1)"
runtime_build="$(git -C "$FW1814_DIR" rev-parse --short=12 HEAD 2>/dev/null || true)"
[[ -n "$runtime_version" ]] || runtime_version="unknown"
[[ -n "$runtime_build" ]] || runtime_build="unknown"
printf 'version=%s\nbuild=%s\n' "$runtime_version" "$runtime_build" > "$INSTALL_ROOT/runtime-build.conf"
chown root:wheel "$INSTALL_ROOT/runtime-build.conf"
chmod 0644 "$INSTALL_ROOT/runtime-build.conf"

if [[ ! -e "$STATE_FILE" ]]; then
    : > "$STATE_FILE"
fi
chown root:wheel "$STATE_FILE"
chmod 0666 "$STATE_FILE"

install -o root -g wheel -m 0644 \
    "$SCRIPT_DIR/com.mbprado.macfw.fw1814.transport.plist" "$LAUNCHD_PLIST"

: > "$LOG"
chown root:wheel "$LOG"
chmod 0644 "$LOG"

launchctl bootstrap system "$LAUNCHD_PLIST"
launchctl enable system/$LABEL
launchctl kickstart -k system/$LABEL

echo "installed macfw FW1814 transport runtime: $INSTALL_ROOT"
echo "runtime build: $runtime_version build $runtime_build"
echo "loaded launchd service: $LABEL"
echo "automatic reconnect + guarded bootloader recovery: enabled"
echo "automatic 44.1/48 kHz transport selection: enabled"
echo "validated clean bus reset before transport recovery: enabled"
echo "persistent validated routing state: $STATE_FILE"
echo "log: $LOG"
