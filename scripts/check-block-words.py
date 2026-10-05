#!/usr/bin/env python3
"""check-block-words.py -- the panel's block indices must be the driver's block indices.

⚠⚠ WHY THIS EXISTS. The counter block is a flat array of unsigned longs shared by three
programs. src/bt_probe.c defines the word numbers in enums; cpanel/bt_panel.c HAND-COPIES
the ones it reads as #defines, with a "MUST match src/bt_probe.c" comment to keep them
honest. A comment is not a check, and the project has now been bitten three times in one
day by two places that had to agree and drifted -- the devices-list column tables, the
Help window's create-vs-resize geometry, and a status reading the dormant HID path's
counter. An index that drifts does not crash: the panel silently reports whatever
unrelated counter now lives at that number.

⚠ IT RESOLVES IMPLICIT ENUM VALUES. Most of the driver's words have no `= N` -- they
inherit previous+1 -- so a regex for `kWFoo = 123` sees about a third of them and calls
the rest "not found". A checker that quietly skips two thirds of its subject is worse
than none, because it reports CLEAN.

Two rules, and neither needs a list of exceptions:
  1. A panel #define whose name IS a driver block word must have the driver's value.
  2. A panel gBlock[kWFoo] whose name is NOT a driver block word at all is an error --
     it is reading a word nothing writes. (This is also what keeps unrelated names such
     as kWinW, the window width, out of the check: they are never block subscripts.)
"""
import re
import sys
import pathlib

HERE = pathlib.Path(__file__).resolve().parent.parent


def driver_words(src: str) -> dict:
    """Every kW* enumerator in bt_probe.c, with implicit values resolved."""
    words = {}
    for body in re.findall(r'enum\s*\{(.*?)\}\s*;', src, re.S):
        if 'kW' not in body:
            continue
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)   # strip comments first,
        body = re.sub(r'//[^\n]*', '', body)                # or a name in prose counts
        nxt = 0
        for item in body.split(','):
            item = item.strip()
            if not item:
                continue
            m = re.match(r'^([A-Za-z_]\w*)\s*(?:=\s*(-?\d+|0[xX][0-9a-fA-F]+))?$', item)
            if m:
                name, val = m.group(1), m.group(2)
                if val is not None:
                    nxt = int(val, 0)
                if name.startswith('kW'):
                    words[name] = nxt
                nxt += 1
                continue

            # ⭐ `kWCount = kWEnd + 1` -- ONE earlier enumerator plus a small literal.
            # Resolved rather than skipped, because the allocation-size rule below must
            # be able to read kWCount, and 14.6's heap overrun is exactly what happens
            # when that value is invisible to this script. Deliberately narrow: a single
            # already-known name and a constant, nothing that needs evaluating.
            m = re.match(r'^([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*([+-])\s*(\d+)$', item)
            if m and m.group(2) in words:
                name = m.group(1)
                base = words[m.group(2)]
                off = int(m.group(4))
                nxt = base + off if m.group(3) == '+' else base - off
                if name.startswith('kW'):
                    words[name] = nxt
                nxt += 1
                continue
            # anything else is a real expression: skip, and do not guess its value
    return words


def prefixed_words(src: str, prefix: str) -> dict:
    """Every <prefix>* enumerator in a source file, with implicit values resolved."""
    words = {}
    for body in re.findall(r'enum\s*\{(.*?)\}\s*;', src, re.S):
        if prefix not in body:
            continue
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        body = re.sub(r'//[^\n]*', '', body)
        nxt = 0
        for item in body.split(','):
            item = item.strip()
            if not item:
                continue
            m = re.match(r'^([A-Za-z_]\w*)\s*(?:=\s*(-?\d+|0[xX][0-9a-fA-F]+))?$', item)
            if not m:
                continue
            name, val = m.group(1), m.group(2)
            if val is not None:
                nxt = int(val, 0)
            if name.startswith(prefix):
                words.setdefault(name, nxt)     # first enum wins; see the kSw note below
            nxt += 1
    return words


def check_switcher(errs: list) -> int:
    """⚠⚠ THE PANEL MIRRORS src/bt_switch.c's kSw ENUM POSITIONALLY, and a silent
    off-by-one there reports a healthy switcher as broken -- which is worse than saying
    nothing, because the line exists to be trusted when nothing else can answer.

    ⚠ bt_switch.c has a SECOND enum (the verdict vocabulary) that also contains a kSw
    name, so the resolver takes the FIRST definition of each name. The block enum comes
    first in that file; if that ever changes, this check starts comparing the wrong
    table and must be revisited rather than trusted.

    ⚠⚠ AND IT CHECKS BOTH MIRRORS, NOT JUST THE PANEL. Until 2026-09-17 this function
    compared src/bt_switch.c against cpanel/bt_panel.c only -- but bt-check/bt_check.c
    carries the SAME hand-copied enum, and bt_check.c's own comments record that the
    terminator has now moved four times. So the one binary whose entire job is to read
    these blocks was the one copy nothing verified, and a drift there prints "NO 'BTSW'
    BLOCK" over a perfectly healthy switcher -- a false negative in the diagnostic, which
    is the worst place to have one."""
    sw = prefixed_words((HERE / 'src/bt_switch.c').read_text(), 'kSw')
    if len(sw) < 20:
        errs.append(f"  only parsed {len(sw)} kSw words from src/bt_switch.c -- "
                    f"the enum shape must have changed; refusing to report a pass")
        return 0
    checked = 0
    for rel in ('cpanel/bt_panel.c', 'bt-check/bt_check.c'):
        mirror = prefixed_words((HERE / rel).read_text(), 'kSw')
        if not mirror:
            continue                              # does not read it: nothing to check
        for name, val in mirror.items():
            if name in sw:
                checked += 1
                if sw[name] != val:
                    errs.append(f"  {name}: {rel} says {val}, "
                                f"src/bt_switch.c says {sw[name]}")
            else:
                errs.append(f"  {name}: {rel} defines it but src/bt_switch.c does not")
    return checked


def main() -> int:
    probe = (HERE / 'src/bt_probe.c').read_text()
    panel = (HERE / 'cpanel/bt_panel.c').read_text()

    drv = driver_words(probe)
    if len(drv) < 50:
        print(f"check-block-words: only parsed {len(drv)} driver words -- the enum "
              f"shape must have changed. Refusing to report a pass.", file=sys.stderr)
        return 2

    errs, checked = [], 0

    # Rule 1: hand-copied indices must agree.
    for name, val in re.findall(r'^#define\s+(kW\w+)\s+(\d+)', panel, re.M):
        if name in drv:
            checked += 1
            if drv[name] != int(val):
                errs.append(f"  {name}: cpanel/bt_panel.c says {val}, "
                            f"src/bt_probe.c says {drv[name]}")

    # Rule 2: every word the panel actually subscripts must be a word the driver defines.
    for name in sorted(set(re.findall(r'gBlock\s*\[\s*(kW\w+)', panel))):
        if name not in drv:
            errs.append(f"  {name}: the panel reads gBlock[{name}] but src/bt_probe.c "
                        f"defines no such block word")

    # ⚠⚠⚠ Rule 3: THE DRIVER'S OWN ALLOCATION MUST COVER ITS OWN TERMINATOR.
    #
    # THIS RULE EXISTS BECAUSE 14.6 SHIPPED WITHOUT IT. kWCount is the allocation size
    # passed to NewPtrSys and kWEnd is the index EnsureBlock writes 'ENDS' to, so the
    # count must exceed the index -- but both were hand-typed, and moving kWEnd to 733
    # for the mouse counters while kWCount stayed 721 made the driver write 48 bytes past
    # its System heap block, over the next block's header.
    #
    # The symptom named nothing: the driver just never came up, so the switch marker was
    # never cleared, so the NEXT boot's switcher correctly declined, so the keyboard came
    # up in proxy mode. Three designed-in fallbacks between cause and symptom.
    #
    # ⚠ Rules 1 and 2 PASSED throughout, because both compare the driver against the
    # OTHER binaries and this drift was WITHIN the driver. A cross-binary checker is
    # structurally blind to a single-binary invariant -- which is the same blind spot
    # reference_static_audit_blind_spots warns about, in a different script.
    if 'kWEnd' in drv and 'kWCount' in drv:
        checked += 1
        if drv['kWCount'] <= drv['kWEnd']:
            errs.append(
                f"  kWCount ({drv['kWCount']}) must EXCEED kWEnd ({drv['kWEnd']}): "
                f"EnsureBlock allocates kWCount words and writes 'ENDS' at kWEnd, so "
                f"this overruns the System heap block by "
                f"{(drv['kWEnd'] - drv['kWCount'] + 1) * 4} bytes. Write "
                f"kWCount = kWEnd + 1, never a literal.")
    elif 'kWEnd' in drv:
        errs.append("  kWCount is not parseable from src/bt_probe.c -- the allocation "
                    "size can no longer be checked against kWEnd. Refusing to pass: "
                    "this is the 14.6 heap-overrun guard.")

    swchecked = check_switcher(errs)

    if errs:
        print("BLOCK WORD MISMATCH -- the panel would read the wrong counter, silently:",
              file=sys.stderr)
        print("\n".join(errs), file=sys.stderr)
        return 1

    print(f"   [ok] block words: {checked} driver + {swchecked} switcher index(es) "
          f"agree across {len(drv)} driver words")
    return 0


if __name__ == '__main__':
    sys.exit(main())
