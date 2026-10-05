#!/bin/bash
# package-dist.sh -- produce clean distribution artifacts for the GitHub release.
#
# Adapted from cpu-temp/csm/scripts/package-dist.sh, and it exists for the same reason:
# Retro68's `Rez -o file.bin` writes a MacBinary header whose CRC strict decoders reject
# (The Unarchiver, modern StuffIt Expander/Deluxe, Apple's own /usr/bin/macbinary all say
# "CRC does not verify"). The raw build .bin can therefore be undecodable for a downloader
# even though the author's own OS 9 StuffIt un-bins it happily. That is exactly the kind of
# failure that only shows up after a community member hits it.
#
# This re-encodes each artifact with Apple's tools into MacBinary II with a correct CRC,
# plus BinHex (.hqx), which is pure 7-bit ASCII and survives any transfer. The AirPort
# Extreme release ships .hqx ONLY, so that is what leads here.
#
# ⚠ The type, creator and Finder name are read OUT OF THE BUILD, not hardcoded. Hardcoding
# them is how a renamed artifact silently ships with the wrong identity: this project
# renamed the extension to "USB Bluetooth Support" and the strip to "Bluetooth Strip"
# mid-development, and a hardcoded table would not have followed.
#
# Requires macOS (uses /usr/bin/macbinary, /usr/bin/binhex, /usr/bin/SetFile).
# Usage:  scripts/package-dist.sh <output dir>
set -euo pipefail

OUT="${1:?output directory}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# The five shipped pieces. Paths only: everything else comes from the MacBinary header.
SRCS="
$ROOT/build/USBBluetoothSupport-v17.6.bin
$ROOT/build/USBBluetoothSwitch-v1.7.bin
$ROOT/cpanel/build/Bluetooth-19.2.bin
$ROOT/csm/build/BluetoothStrip-v2.5.bin
$ROOT/bt-check/build/BTCheck_v99.105.bin
"

mkdir -p "$OUT"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

for SRC in $SRCS; do
    [ -f "$SRC" ] || { echo "⛔ missing: $SRC" >&2; exit 1; }

    # 1. read identity AND rebuild a real forked file, in one pass.
    #    ⚠ Tab-separated, and read with IFS=$'\t': three of the five Finder names contain
    #    spaces ("USB Bluetooth Support", "Bluetooth Strip"), so a default-IFS `read` would
    #    split the name across the variables and stamp the wrong type on the wrong file.
    IFS=$'\t' read -r NAME TYPE CREATOR < <(python3 - "$SRC" "$TMP" <<'PY'
import sys, struct, os
src, tmp = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
name = d[2:2+d[1]].decode('mac_roman')
typ  = d[65:69].decode('mac_roman')
cre  = d[69:73].decode('mac_roman')
dlen = struct.unpack('>I', d[83:87])[0]          # data fork (the PEF)
rlen = struct.unpack('>I', d[87:91])[0]          # resource fork
rstart = 128 + ((dlen + 127) // 128) * 128       # forks are 128-byte padded
dst = os.path.join(tmp, name)
open(dst, 'wb').write(d[128:128+dlen])
open(dst + '/..namedfork/rsrc', 'wb').write(d[rstart:rstart+rlen])
print(f"{name}\t{typ}\t{cre}")
PY
)

    # 2. stamp type/creator + Finder flags.
    #    B = kHasBundle (the control panel and strip carry icon families),
    #    C = kHasCustomIcon (so the Finder shows our icon rather than a generic one).
    /usr/bin/SetFile -t "$TYPE" -c "$CREATOR" "$TMP/$NAME"
    /usr/bin/SetFile -a BC "$TMP/$NAME"

    # 3. BinHex, verified. Named for the Finder name so the download says what it becomes.
    SAFE="$(printf '%s' "$NAME" | tr ' ' '-')"
    /usr/bin/binhex encode "$TMP/$NAME" -o "$OUT/$SAFE.hqx" -n
    /usr/bin/binhex probe  "$OUT/$SAFE.hqx" >/dev/null
    printf '  [ok] %-24s -> %-34s type %s creator %s\n' "$NAME" "$SAFE.hqx" "$TYPE" "$CREATOR"
done

echo ""
echo "wrote $(ls -1 "$OUT"/*.hqx | wc -l | tr -d ' ') BinHex artifacts to $OUT"
