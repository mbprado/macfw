#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)"
PROBE_DIR="$ROOT/fw410/tools/device/fwprobe"
PROBE="$PROBE_DIR/fwprobe"

echo "macfw FW1814 read-only fingerprint"
echo "building the proven generic FireWire/BeBoB probe..."
make -C "$PROBE_DIR"

echo
echo "No FireWire writes will be issued by this fingerprint command."
echo "Collecting registry identity, configuration ROM and BeBoB information block."
echo

exec "$PROBE" --rom --info
