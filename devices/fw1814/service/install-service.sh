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

# Preserve persistent transport tuning across development reinstalls. Keep this
# list explicit so one-shot diagnostics such as playback-only or preload-tone
# modes are not accidentally made permanent.
PRESERVED_ENV_KEYS=(
    MACFW_VERBOSE
    MACFW_44_ROLLING_TX
    MACFW_44_ROLLING_TX_CYCLES
    MACFW_44_ROLLING_PCM_RESERVE_FRAMES
    MACFW_48_ROLLING_TX
    MACFW_48_ROLLING_TX_CYCLES
    MACFW_AUDIO_SERVICE_PERIOD_US
    MACFW_FW1814_FORCE_RECOVERY
)
ENV_BACKUP="$(mktemp /tmp/macfw-fw1814-env.XXXXXX)"
trap 'rm -f "$ENV_BACKUP"' EXIT
if [[ -f "$LAUNCHD_PLIST" ]]; then
    for key in "${PRESERVED_ENV_KEYS[@]}"; do
        if value="$(/usr/libexec/PlistBuddy \
                -c "Print :EnvironmentVariables:$key" \
                "$LAUNCHD_PLIST" 2>/dev/null)"; then
            printf '%s\t%s\n' "$key" "$value" >> "$ENV_BACKUP"
        fi
    done
fi

SUPERVISOR="$FW1814_DIR/transport/fw1814supervisor"
ENGINE48="$FW1814_DIR/transport/fw1814analog48"
ENGINE44="$FW1814_DIR/transport/fw1814analog44"
ENGINE96="$FW1814_DIR/transport/fw1814analog96"
ENGINE88="$FW1814_DIR/transport/fw1814analog88"
ENGINE176="$FW1814_DIR/transport/fw1814analog176"
ENGINE192="$FW1814_DIR/transport/fw1814analog192"
INIT="$FW1814_DIR/tools/fw1814init"
BOOT="$FW1814_DIR/tools/fwboot1814"
FIRMWARE_RESET="$FW1814_DIR/tools/fw1814firmwarereset"
BUS_RESET="$FW1814_DIR/../../common/tools/firewirebusreset/firewirebusreset"
CONTROL="$FW1814_DIR/tools/control/fw1814ctl/fw1814ctl"
STATE_CONTROL="$FW1814_DIR/tools/control/fw1814state/fw1814state"
DEVICE_PROBE="$FW1814_DIR/tools/fw1814deviceprobe"
STATE_FILE="$INSTALL_ROOT/control-state.conf"
for file in "$SUPERVISOR" "$ENGINE48" "$ENGINE44" "$ENGINE88" "$ENGINE96" "$ENGINE176" "$ENGINE192" "$INIT" "$BOOT" "$FIRMWARE_RESET" "$BUS_RESET" "$CONTROL" "$STATE_CONTROL" "$DEVICE_PROBE"; do
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
install -o root -g wheel -m 0755 "$SUPERVISOR" "$BIN_DIR/fw1814supervisor"
install -o root -g wheel -m 0755 "$ENGINE48" "$BIN_DIR/fw1814analog48"
install -o root -g wheel -m 0755 "$ENGINE44" "$BIN_DIR/fw1814analog44"
install -o root -g wheel -m 0755 "$ENGINE88" "$BIN_DIR/fw1814analog88"
install -o root -g wheel -m 0755 "$ENGINE96" "$BIN_DIR/fw1814analog96"
install -o root -g wheel -m 0755 "$ENGINE176" "$BIN_DIR/fw1814analog176"
install -o root -g wheel -m 0755 "$ENGINE192" "$BIN_DIR/fw1814analog192"
# CoreAudio caches the available sample-rate list.
killall coreaudiod >/dev/null 2>&1 || true
install -o root -g wheel -m 0755 "$INIT" "$BIN_DIR/fw1814init"
install -o root -g wheel -m 0755 "$BOOT" "$BIN_DIR/fwboot1814"
install -o root -g wheel -m 0755 "$FIRMWARE_RESET" "$BIN_DIR/fw1814firmwarereset"
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

preserved_env_count=0
while IFS=
chown root:wheel "$LOG"
chmod 0644 "$LOG"

launchctl bootstrap system "$LAUNCHD_PLIST"
launchctl enable system/$LABEL
launchctl kickstart -k system/$LABEL

echo "installed macfw FW1814 transport runtime: $INSTALL_ROOT"
echo "runtime build: $runtime_version build $runtime_build"
echo "loaded launchd service: $LABEL"
echo "preserved launchd transport tuning variables: $preserved_env_count"
echo "automatic reconnect + guarded bootloader recovery: enabled"
echo "automatic 44.1/48/88.2/96/176.4/192 kHz transport selection: enabled"
echo "rate-aware transport recovery: bus reset at lower rates; guarded firmware reboot at 176.4/192 kHz"
echo "persistent validated routing state: $STATE_FILE"
echo "log: $LOG"
\t' read -r key value; do
    [[ -n "$key" ]] || continue
    /usr/libexec/PlistBuddy \
        -c "Delete :EnvironmentVariables:$key" \
        "$LAUNCHD_PLIST" >/dev/null 2>&1 || true
    /usr/libexec/PlistBuddy \
        -c "Add :EnvironmentVariables:$key string $value" \
        "$LAUNCHD_PLIST"
    preserved_env_count=$((preserved_env_count + 1))
done < "$ENV_BACKUP"
chown root:wheel "$LAUNCHD_PLIST"
chmod 0644 "$LAUNCHD_PLIST"

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
echo "automatic 44.1/48/88.2/96/176.4/192 kHz transport selection: enabled"
echo "rate-aware transport recovery: bus reset at lower rates; guarded firmware reboot at 176.4/192 kHz"
echo "persistent validated routing state: $STATE_FILE"
echo "log: $LOG"
