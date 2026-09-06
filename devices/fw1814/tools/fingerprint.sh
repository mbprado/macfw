#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)"
PROBE="$ROOT/fw410/tools/device/fwprobe/fwprobe"

echo "macfw FW1814 read-only fingerprint"
echo
echo "No FireWire writes will be issued by this fingerprint command."
echo "Collecting registry identity, configuration ROM and BeBoB information block."
echo

exec "$PROBE" --rom --info
