#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RATEPROBE="$HERE/../../control/rateprobe/rateprobe"
PROVEN_DIR="$HERE/../pcm44100warmup"
PROVEN_PLAYER="$PROVEN_DIR/pcm44100warmup"

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

if [ "$#" -ne 0 ]; then
    echo "note: pcm44100playback now uses the hardware-proven M-Audio startup path; extra arguments are ignored" >&2
fi

if [ ! -x "$PROVEN_PLAYER" ]; then
    make -C "$PROVEN_DIR"
fi

"$RATEPROBE" 44100 --execute --keep
"$PROVEN_PLAYER" --execute
restore_rate
trap - EXIT INT TERM
