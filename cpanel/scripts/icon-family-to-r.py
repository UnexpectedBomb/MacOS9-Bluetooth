#!/usr/bin/env python3
"""
icon-family-to-r.py -- turn classic Mac icon families into a Rez-able .r file.

    icon-family-to-r.py <file-with-icons-in-its-resource-fork> <out.r>

⚠ RENAMED from icons-to-r.py. This script and scripts/custom-icon-to-r.py were BOTH
called "icons-to-r.py" while doing different things -- the same collision that made
the control panel ship a stale MacBinary CRC for about twenty revisions under two
scripts both named set-custom-icon.py.

  * THIS script emits EVERY family it finds, each at its REAL resource id. That is
    what a set of numbered icons needs (the panel's device icons at 200..205).
  * custom-icon-to-r.py emits exactly ONE family hardcoded to ID -16455, a file's own
    Finder icon.

The collision had already caused a wrong fact in the tree: the header this script
stamps into its output used to hardcode "scripts/icons-to-r.py", so cpanel/bt_icon.r
credited the OTHER script -- which cannot have produced it, since that one only emits
-16455 and bt_icon.r holds ID 128. The header is now derived from __file__ below, so
it cannot drift from reality again.

WHY THIS EXISTS. Rez cannot `read 'ICN#'` (or icl4/icl8/ics#/ics4/ics8) the way it
can read some other types, which is the same trap the CPU Temp CSM hit with PICT --
see feedback_pict_resource_workflow. So the bytes have to be emitted inline as hex
and #included by the target's .r file.

⚠ Rez #include dependencies are NOT tracked by CMake. The generated .r MUST be in
the target's DEPENDS list or an artwork change will silently not be re-Rez'd, and the
build will keep stamping the OLD icon with no error at all.

Reads the resource fork via the ..namedfork/rsrc path, so it works on a file sitting
on an AFP/SMB share where the fork is not a separate file.
"""
import os
import struct
import sys


def read_fork(path):
    """The resource-fork bytes, from either a fork-ful file or a raw fork dump.

    ⚠ BOTH FORMS ARE NEEDED, and only supporting the first made the committed archive
    useless. Reading exclusively through "..namedfork/rsrc" works on a file that still
    carries its fork -- but GIT DOES NOT STORE RESOURCE FORKS, so the archived copy of
    the artwork beside the generated .r can only ever be a data-fork file holding the
    raw fork bytes. Regenerating from it then failed with FileNotFoundError, which
    defeats the entire reason for archiving it.

    So: try the named fork, and fall back to the file's own bytes. A resource fork is
    self-describing enough to tell the difference -- its first 16 bytes are four
    offsets that must land inside the file -- so this guesses nothing.
    """
    try:
        with open(path + "/..namedfork/rsrc", "rb") as f:
            r = f.read()
        if r:
            return r
    except (FileNotFoundError, NotADirectoryError, OSError):
        pass
    with open(path, "rb") as f:
        r = f.read()
    if len(r) < 16:
        sys.exit("%s: too small to be a resource fork" % path)
    data_off, map_off, data_len, map_len = struct.unpack(">4I", r[:16])
    if map_off + 30 > len(r) or data_off + data_len > len(r):
        sys.exit("%s: no resource fork, and the data fork is not one either "
                 "(header offsets fall outside the file)" % path)
    return r


def read_resources(path):
    """Yield (type, id, bytes) for every resource in a classic resource fork."""
    r = read_fork(path)
    data_off, map_off, _, _ = struct.unpack(">4I", r[:16])
    type_off = struct.unpack(">H", r[map_off + 24:map_off + 26])[0] + map_off
    ntypes = struct.unpack(">h", r[type_off:type_off + 2])[0] + 1
    for i in range(ntypes):
        e = type_off + 2 + i * 8
        rtype = r[e:e + 4].decode("mac-roman")
        count = struct.unpack(">h", r[e + 4:e + 6])[0] + 1
        ref_off = struct.unpack(">H", r[e + 6:e + 8])[0] + type_off
        for j in range(count):
            re_ = ref_off + j * 12
            rid = struct.unpack(">h", r[re_:re_ + 2])[0]
            off = struct.unpack(">I", b"\0" + r[re_ + 5:re_ + 8])[0] + data_off
            length = struct.unpack(">I", r[off:off + 4])[0]
            yield rtype, rid, r[off + 4:off + 4 + length]


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: icon-family-to-r.py <icon-file> <out.r>")
    src, dst = sys.argv[1], sys.argv[2]
    res = list(read_resources(src))
    if not res:
        sys.exit("no resources found -- is the resource fork empty?")

    # ⚠ Write to a temp file and rename, never open(dst, 'w') directly. A truncating
    # open that then fails mid-write is exactly how this project lost ~2,800 lines of
    # ehci_vhub.c; the rule is in CLAUDE.md and it applies to generators too.
    tmp = dst + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        # ⚠ DERIVED, NOT HARDCODED. A literal path here is how cpanel/bt_icon.r came to
        # credit a script that cannot have produced it: the string said
        # "scripts/icons-to-r.py" no matter which file was running. Taking the last
        # three path components off __file__ means the credit follows the script,
        # including through a rename like this one.
        f.write("/*\n *  GENERATED by %s -- do not hand-edit.\n"
                " *  Regenerate if the artwork changes.\n"
                " *\n *  Rez cannot `read` these types, so the bytes are inline hex.\n */\n\n"
                % "/".join(os.path.abspath(__file__).split(os.sep)[-3:]))
        for rtype, rid, body in res:
            f.write("data '%s' (%d) {\n" % (rtype, rid))
            h = body.hex().upper()
            for k in range(0, len(h), 64):
                f.write('    $"' + h[k:k + 64] + '"\n')
            f.write("};\n\n")
    # ⚠ NO LOCAL `import os` HERE. There used to be one, and adding a use of os
    # earlier in this function turned it into an UnboundLocalError: a function-level
    # import makes the name local for the WHOLE function, including lines above it.
    # os is imported at module scope.
    os.replace(tmp, dst)

    for rtype, rid, body in res:
        print("  %s id %d  %d bytes" % (rtype, rid, len(body)))
    print("wrote %s" % dst)


if __name__ == "__main__":
    main()
