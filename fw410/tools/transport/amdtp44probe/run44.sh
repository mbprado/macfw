#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RATEPROBE="$HERE/../../control/rateprobe/rateprobe"
PROBE="$HERE/amdtp44probe"

restore() {
    "$RATEPROBE" 48000 --execute --keep >/dev/null 2>&1 || true
}
trap restore EXIT INT TERM

"$RATEPROBE" 44100 --execute --keep
"$PROBE" --execute "$@"
"$RATEPROBE" 48000 --execute --keep
trap - EXIT INT TERM
