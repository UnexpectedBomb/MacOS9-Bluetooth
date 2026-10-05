#!/usr/bin/env python3
"""
verify-probe-gate.py -- prove the BUILT driver's diagnostic gate matches the source.

    verify-probe-gate.py <built.ndrv> <src/bt_pump.h>

⚠⚠ WHY THIS EXISTS. v6.8 was staged and RUN ON HARDWARE with kHidLevel0Probe
compiled as 0 while bt_pump.h said 1. The log came back reporting "registered at
LEVEL 2" and no synthesised events, and the run answered nothing.

The cause was mundane and will recur: the gate was flipped to 0 to check that the
shipping path still compiled, flipped back to 1, and both edits plus both builds
happened inside ONE SECOND. make compares mtimes, saw the header as no newer than
the objects, and skipped the rebuild -- so the artifact on disk was the one built
with the probe off. Nothing in the source tree was wrong. Nothing in `make` output
said anything was wrong. Only the binary knew.

⇒ Read the ARTIFACT, not the source. Same principle already applied in this repo to
the descriptor records (the Expert reads bytes, so the bytes get checked) and to the
version stamps (Get Info shows a string, so the string gets checked). A build that
cannot be trusted to reflect its own source is worse than a build that fails.

Exit 1 on disagreement, so it can sit in the build.
"""
import re
import sys

PREFIX = b"OS9BT-GATES-"


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    ndrv, header = sys.argv[1], sys.argv[2]

    with open(header, encoding="utf-8") as f:
        src = f.read()

    # ⚠ EVERY gate, because v6.8 proved a single switch was the wrong shape. A checker
    # that tracked only one of them would let the other go stale unnoticed, which is
    # precisely the failure it exists to catch.
    #
    # ⚠⚠ kHidAutoAuth was added at v8.8 and this list still named two gates, so a
    # build with auto-auth left ON -- it authenticates any inbound link with no prompt
    # -- verified as a correct shipping build. Two of three gates is the same defect as
    # one of two. Adding a gate means adding it HERE and to kProbeGateMarker, and the
    # marker's shape check below now fails on an artifact that predates a new gate
    # rather than quietly ignoring the missing segment.
    want = {}
    for name in ("kHidSynthFeatures", "kHidLevel0", "kHidAutoAuth",
                 "kHidLongSupervision", "kHidLinkPolicy",
                 "kHidUseBtstackHost"):
        m = re.search(r"#define\s+%s\s+([01])" % name, src)
        if not m:
            print("ERROR: could not find %s in %s" % (name, header))
            return 1
        want[name] = m.group(1)
    expect = (b"OS9BT-GATES-SYNTH" + want["kHidSynthFeatures"].encode()
              + b"-LVL" + (b"0" if want["kHidLevel0"] == "1" else b"2")
              + b"-AUTH" + want["kHidAutoAuth"].encode()
              + b"-SUP"  + want["kHidLongSupervision"].encode()
              + b"-POL"  + want["kHidLinkPolicy"].encode()
              + b"-HOST" + want["kHidUseBtstackHost"].encode())

    with open(ndrv, "rb") as f:
        blob = f.read()

    # ⚠ Match the PREFIX and any marker-shaped tail, not the exact expected shape. A
    # regex spelling out the current segments would find NOTHING in an artifact built
    # before a gate was added, and "0 markers found" reads like a linker problem when
    # the real answer is that the object files are stale.
    found = [m.group(0) for m in re.finditer(PREFIX + rb"[A-Z0-9-]+", blob)]
    uniq = sorted(set(found))
    if len(uniq) != 1:
        print("ERROR: %s contains %d distinct gate markers %r -- cannot tell which "
              "build this is" % (ndrv, len(uniq), uniq))
        return 1

    if uniq[0].count(b"-") != expect.count(b"-"):
        print("PROBE GATE MARKER IS THE WRONG SHAPE -- THE OBJECT FILES ARE STALE.")
        print("  %s describes %d gates (%r)" % (header, len(want), expect.decode()))
        print("  %s carries %r" % (ndrv, uniq[0].decode()))
        print("")
        print("  A gate was added to bt_pump.h and this artifact was linked before")
        print("  bt_btstack.c was recompiled. Force it:")
        print("      touch src/*.c src/*.h && make")
        return 1

    if uniq[0] != expect:
        print("PROBE GATE MISMATCH -- THE BINARY DOES NOT MATCH THE SOURCE.")
        print("  %s implies %r" % (header, expect.decode()))
        print("  %s was built as %r" % (ndrv, uniq[0].decode()))
        print("")
        print("  This is the v6.8 failure: sub-second edits defeat make's mtime")
        print("  comparison and the rebuild is silently skipped. Force it:")
        print("      touch src/*.c src/*.h && make")
        return 1

    print("verify-probe-gate: %s built as %s, matching %s"
          % (ndrv, uniq[0].decode(), header))
    return 0


if __name__ == "__main__":
    sys.exit(main())
