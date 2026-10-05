#!/usr/bin/env python3
"""
set-finder-flags.py -- turn on the Finder flags a built artifact needs.

Rez stamps the output's Finder type/creator but has no option for Finder flags, so a
freshly Rez'd file has fdFlags = 0.  This sets:
  * 0x0400 (kHasCustomIcon) so the custom icon at resource ID -16455 shows; and
  * 0x2000 (kHasBundle)     so the Finder reads the file's BNDL/FREF.

It works on BOTH artifact formats because each lays the Finder type+creator immediately
before the Finder flags:
  * MacBinary (.bin): type@65, creator@69, flags hi-byte @73  (== type+8)
  * raw HFS  (.dsk):  the catalog FInfo has fdType, fdCreator, then fdFlags

Usage:  set-finder-flags.py <file> [TYPECREA]   (default signature: sdevUTcs)
Idempotent: re-running just re-asserts the bit.

⚠⚠ RENAMED FROM set-custom-icon.py, AND THAT NAME COLLISION WAS A REAL BUG.
There were two scripts called set-custom-icon.py -- bluetooth/scripts/ (in/out form,
recomputes the MacBinary CRC) and this one (in-place, by signature, did NOT). Both
targets referenced "${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py", which
resolved to a DIFFERENT file for each. The driver got the correct one; the control panel
got this one, and shipped a stale CRC in every build for about twenty revisions:

    BTCheck_v61.bin              stored 0x6467  computed 0x6467   ok
    USBBluetoothSupport-v4.1.bin stored 0x1CD8  computed 0x1CD8   ok
    Bluetooth.bin                stored 0xE1FB  computed 0xEF7E   WRONG

⚠ THE OLD DOCSTRING'S REASONING IS WHAT WENT WRONG. It said "HFS catalog records carry
no checksum, so an in-place byte patch is safe" -- true for .dsk, and false for
MacBinary, which carries a CRC-16 over bytes 0..123 at offset 124 that StuffIt Expander
checks. Patching byte 73 invalidates it. The rule the comment stated was right about the
format it was written for and silently wrong about the other one.
"""

import os
import struct
import sys
import tempfile
from pathlib import Path

FLAGS_HI = 0x24   # high byte of kHasBundle (0x2000) | kHasCustomIcon (0x0400)


def crc16(data):
    """MacBinary II CRC: CRC-16-CCITT over bytes 0..123, seed 0. Same routine as
    bluetooth/scripts/set-custom-icon.py, which is known to produce archives Expander
    accepts -- verified against three shipped .bins whose stored CRC it reproduces."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def patch(path: Path, sig: bytes) -> None:
    data = bytearray(path.read_bytes())
    n = data.count(sig)
    if n != 1:
        sys.exit(f"{path}: expected exactly one {sig!r}, found {n}")
    i = data.index(sig)
    flags_hi = i + len(sig)          # +8: high byte of fdFlags
    before = data[flags_hi]
    data[flags_hi] |= FLAGS_HI

    # ★ MacBinary puts the type at offset 65, so a signature found THERE means the CRC
    # at 124 now covers a byte we just changed. An HFS catalog record has no checksum,
    # so the .dsk path is left exactly as it was.
    note = ""
    if i == 65 and len(data) >= 128:
        old_crc, = struct.unpack('>H', bytes(data[124:126]))
        new_crc = crc16(bytes(data[0:124]))
        struct.pack_into('>H', data, 124, new_crc)
        note = f"; MacBinary CRC 0x{old_crc:04X} -> 0x{new_crc:04X}"

    # ⚠ TEMP FILE + os.replace, NOT write_bytes. The previous version called
    # path.write_bytes(), which opens with 'wb' and TRUNCATES before writing -- the
    # exact shape that destroyed ehci_vhub.c and put rule 1 in CLAUDE.md. A build
    # artifact is cheaper to lose than a source file, but the pattern is the pattern.
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), suffix='.flagstmp')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(bytes(data))
        if Path(tmp).read_bytes() != bytes(data):
            raise RuntimeError("temp file verify failed")
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise

    print(f"{path.name}: fdFlags hi 0x{before:02X} -> 0x{data[flags_hi]:02X} "
          f"(kHasBundle | kHasCustomIcon) at offset {flags_hi}{note}")


if __name__ == "__main__":
    if not (2 <= len(sys.argv) <= 3):
        sys.exit(__doc__.strip())
    target = Path(sys.argv[1])
    signature = (sys.argv[2] if len(sys.argv) == 3 else "sdevUTcs").encode("ascii")
    patch(target, signature)
