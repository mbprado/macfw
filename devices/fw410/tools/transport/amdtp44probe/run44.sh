#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RATEPROBE="$HERE/../../control/rateprobe/rateprobe"
PROBE="$HERE/amdtp44probe"

restore_rate() {
    target="$1"
    attempt=1
    while [ "$attempt" -le 10 ]; do
        if "$RATEPROBE" "$target" --execute --keep; then
            return 0
        fi
        echo "rate restore attempt $attempt failed; waiting for FW410 re-enumeration..." >&2
        sleep 1
        attempt=$((attempt + 1))
    done
    echo "unable to restore FW410 to ${target} Hz after bus reset" >&2
    return 1
}

restore_on_exit() {
    restore_rate 48000 >/dev/null 2>&1 || true
}
trap restore_on_exit EXIT INT TERM

"$RATEPROBE" 44100 --execute --keep
"$PROBE" --execute "$@"
restore_rate 48000
trap - EXIT INT TERM
