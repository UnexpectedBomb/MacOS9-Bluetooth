#!/usr/bin/env python3
"""pef-clear-main.py -- remove a PEF's main-symbol declaration.

    pef-clear-main.py <in.pef> <out.pef>

WHY THIS EXISTS
---------------
Retro68's MakePEF emits a loader header with `mainSection = 1` and
`mainOffset = 0xffffffff`. Section 1 is the 384-byte data section, so that offset
is wildly out of bounds: it is a malformed main-symbol declaration, not a valid one.

Every stock Mac OS 9 USB class driver declares **no main symbol at all**. Measured
on three of them, pulled off the machine's own Extensions folder:

    USBSoundSpace2Driver    mainSection=-1  mainOffset=0x00000000
    USBNomadJukeboxDriver   mainSection=-1  mainOffset=0x00000000
    USB Software Locator    mainSection=-1  mainOffset=0x00000000
    ours (before this fix)  mainSection=1   mainOffset=0xffffffff

A USB class driver is not entered through main. The Expert loads the fragment and
reaches it through two exported data symbols, `TheUSBDriverDescription` and
`TheClassDriverPluginDispatchTable`. There is nothing for main to point at.

⚠ This is the OPPOSITE of what a Device Manager native driver needs. For an
'ndrv' loaded by the Device Manager, main must point AT DoDriverIO, which is what
`usb2-ehci/patch-pef-main.py` is for. Do not confuse the two: same field, opposite
requirement, decided by who loads the fragment.

⚠ NOT PROVEN to be the cause of the -43 the Expert reports. It is the last
structural difference between our container and three known-good ones after the
filename, the cfrg, the export hash table and the descriptor bytes were all
verified to match. It is malformed either way and free to correct.

Writes a NEW file rather than editing in place, per claude-os9/CLAUDE.md.
"""
import struct
import sys

PEF_TAG1, PEF_TAG2 = b'Joy!', b'peff'
SECTION_LOADER = 4


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    d = bytearray(open(src, 'rb').read())
    if d[0:4] != PEF_TAG1 or d[4:8] != PEF_TAG2:
        print('ERROR: %s is not a PEF container' % src)
        return 1

    nsec = struct.unpack('>H', d[32:34])[0]
    loader = None
    for i in range(nsec):
        o = 40 + i * 28
        if d[o + 24] == SECTION_LOADER:
            loader = struct.unpack('>I', d[o + 20:o + 24])[0]
            break
    if loader is None:
        print('ERROR: no loader section in %s' % src)
        return 1

    was_sec, was_off = struct.unpack('>iI', d[loader:loader + 8])
    if (was_sec, was_off) == (-1, 0):
        print('pef-clear-main: already clear (mainSection=-1, mainOffset=0)')
    else:
        print('pef-clear-main: mainSection %d -> -1, mainOffset 0x%08x -> 0x0'
              % (was_sec, was_off))
        d[loader:loader + 8] = struct.pack('>iI', -1, 0)

    # Nothing else may move: the loader header is fixed-size and we only rewrote
    # two words inside it.
    assert len(d) == len(open(src, 'rb').read()), 'length changed'

    open(dst, 'wb').write(bytes(d))

    check_sec, check_off = struct.unpack('>iI', bytearray(open(dst, 'rb').read())[loader:loader + 8])
    if (check_sec, check_off) != (-1, 0):
        print('ERROR: readback failed: mainSection=%d mainOffset=0x%x'
              % (check_sec, check_off))
        return 1
    print('pef-clear-main: wrote %s (%d bytes), verified' % (dst, len(d)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
