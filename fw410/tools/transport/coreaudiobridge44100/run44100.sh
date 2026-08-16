#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RATEPROBE="$HERE/../../control/rateprobe/rateprobe"
BRIDGE="$HERE/coreaudiobridge44100"

restore_rate() {
    tries=0
    while [ "$tries" -lt 10 ]; do
        if "$RATEPROBE" 48000 --execute --keep >/dev/null 2>&1; then
            return 0
        fi
        tries=$((tries + 1))
        sleep 1
    done
    echo "warning: unable to confirm automatic restore to 48000 Hz" >&2
    return 1
}

trap 'restore_rate || true' EXIT INT TERM

"$RATEPROBE" 44100 --execute --keep
"$BRIDGE" --execute "$@"
restore_rate
trap - EXIT INT TERM
