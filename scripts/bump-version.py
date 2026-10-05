#!/usr/bin/env python3
"""bump-version.py -- move the driver, control panel and BTCheck version stamps together.

WHY THIS EXISTS
---------------
A bump touches 15 sites across 8 files: CMake target names, three 'vers' resources, the
driver's USBDriverDescription (three copies), the counter-block build tag (two copies,
plus two comments), and two readers' kExpectedDriverTag. Doing that by hand has already
cost this project real time:

  * BTCheck v46 shipped writing a log file called BTCheck_v45.log, because three of the
    four places its version lived were missed.
  * Driver v4.2 shipped with a 'vers' resource still reading 4.1, so Get Info and the
    control panel both told the user the wrong version while the block tag said v420.

⚠⚠ SAFETY, and it is the point of the design rather than a footnote. CLAUDE.md's first
rule exists because a Python read-modify-write truncated a 2,800-line source file:
`open(path,'w')` truncated, the write then raised, and the file was gone. So this script:

  1. reads every file first and COUNTS each expected pattern,
  2. REFUSES to write anything unless every count matches exactly,
  3. writes each file to a temp file in the same directory,
  4. re-reads the temp and compares it against the intended bytes,
  5. only then os.replace()s it into place -- atomic, and it cannot truncate.

It never opens an original file for writing. A mismatch anywhere aborts the whole run
before the first write, so a partial bump is not a state this can produce.

USAGE
    scripts/bump-version.py                     # show current versions
    scripts/bump-version.py --driver 5.5        # dry run, shows every edit
    scripts/bump-version.py --driver 5.5 --apply
    scripts/bump-version.py --panel 3.1 --btcheck 62 --apply

⚠ Version format: MAJOR.MINOR with a SINGLE-DIGIT MINOR ("5.4"). The counter-block build
tag is four ASCII bytes 'v' major minor '0', so 4.10 is impossible and 4.9 must go to
5.0. The script enforces it rather than letting it become a silent mismatch.

⚠⚠ THE MAJOR MAY NOW BE 0-35, because the driver hit the old single-digit ceiling at
9.9. Majors 10 and up are encoded in the tag as letters -- 10 is 'A', so 10.0 is 'vA00'
-- which keeps the tag four bytes AND keeps it monotonic, since '9' (0x39) < 'A' (0x41).
See majchar() and bcd(): at 10 the 'vers' BCD fields stop coinciding with their decimal
spelling, and src/bt_probe.c writes that field as a bare literal while bt_probe.r writes
it in hex, so the crossing changes the FORM of one literal. The count checks are what
prove every copy moved.
"""

import argparse
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def path(rel):
    return os.path.join(ROOT, rel)


def read(rel):
    with open(path(rel), encoding='utf-8') as f:
        return f.read()


def current():
    """Read the versions from their single sources of truth."""
    # ⚠⚠ TWO-DIGIT MAJOR ACCEPTED, AND THIS IS THE SECOND TIME THIS EXACT TRAP HAS BEEN
    # SET. See the BTcheck note just below: a digits-only regex that cannot parse its own
    # source of truth makes the whole script exit "could not read current versions",
    # which silently removes the verification pass every bump depends on. The driver
    # crossed 9.9 -> 10.0, so this regex had to widen BEFORE that bump was applied --
    # caught in the falsification pass, one command before it would have broken.
    drv = re.search(r'set\(DRIVER_VER "(\d{1,2})\.(\d)"\)', read('CMakeLists.txt'))
    # ⚠ TWO-PART ACCEPTED. BTCheck went 99 -> 99.1 when the integer form hit the
    # one-byte BCD ceiling, and this digits-only regex then failed to parse its own
    # source of truth -- which made the whole script exit "could not read current
    # versions" and silently took away the verification pass every bump depends on.
    btc = re.search(r'set\(BTCHECK_VER "(\d+(?:\.\d+)?)"\)',
                    read('bt-check/CMakeLists.txt'))
    pnl = re.search(r'^\s*"(\d{1,2})\.(\d)",\s*$', read('cpanel/bt_panel.r'), re.M)
    if not (drv and btc and pnl):
        sys.exit("could not read current versions -- has a source of truth moved?")
    return (f"{drv.group(1)}.{drv.group(2)}", btc.group(1),
            f"{pnl.group(1)}.{pnl.group(2)}")


def majchar(maj):
    """The build tag's major-version CHARACTER.

    ⚠⚠ THE TAG IS FOUR BYTES AND 9.9 WAS THE CEILING. 'v' major minor '0' has exactly
    one byte for the major, so the driver could not be bumped past 9.9 -- and it hit
    that ceiling with v9.9 staged. Majors 10 and up therefore continue into the
    letters: 10 -> 'A', 11 -> 'B', ... 35 -> 'Z'.

    ⭐ AND THE ORDERING STILL HOLDS, which is why letters rather than some escape:
    '9' is 0x39 and 'A' is 0x41, so 'v990' < 'vA00' byte-for-byte and the tags stay
    monotonic across the boundary. Anything comparing tags by value keeps working.

    ⚠ EnsureBlock finds the block by matching a tag that BEGINS 'v' and does not care
    what follows, so no reader logic changes. Only this encoding does."""
    n = int(maj)
    return str(n) if n < 10 else chr(ord('A') + n - 10)


def bcd(part):
    """A NumVersion field as its BCD byte. 9 -> 0x09, 10 -> 0x10.

    ⚠ BCD, NOT DECIMAL, and the difference only appears at 10: the 'vers' resource and
    USBDriverDescription store major/minor as binary-coded decimal, so major 10 is
    0x10 and NOT the decimal literal 10 (which would read as 16). Below 10 the two
    spellings coincide, which is exactly why this went unnoticed until now."""
    n = int(part)
    return (n // 10) * 16 + (n % 10)


def tag(ver):
    """The counter-block build tag: 'v' major minor '0' as four ASCII bytes."""
    maj, minor = ver.split('.')
    return f"0x76{ord(majchar(maj)):02X}{ord(minor):02X}30UL"


def edits_for_driver(old, new):
    om, on = old.split('.')
    nm, nn = new.split('.')
    return [
        ('CMakeLists.txt', f'set(DRIVER_VER "{old}")', f'set(DRIVER_VER "{new}")', 1),
        # ⚠ BCD, and 10 is where decimal and BCD stop coinciding -- see bcd().
        ('bt_probe.r', f'0x{bcd(om):02X}, 0x{on}0, finalStage',
                       f'0x{bcd(nm):02X}, 0x{nn}0, finalStage', 1),
        ('bt_probe.r', f'"{old}",', f'"{new}",', 1),
        # ⚠⚠ THE LONG STRING IS THE ONE GET INFO SHOWS, and it was the one stamp this
        # script did not cover. The Finder's "Version:" line displays the LONG version
        # string; the short string above is for list views and the Installer. So from
        # 4.2 through 5.4 the numeric fields and the short string were bumped here
        # while the long string stayed at 4.1 -- and the single place the user actually
        # reads the version was the single place still wrong. The panel already had
        # this rule; the driver did not, which is the whole reason it drifted.
        ('bt_probe.r', f'"Bluetooth Support {old} - ',
                       f'"Bluetooth Support {new} - ', 1),
        # ⚠ Three copies: one per descriptor rule, and the rule-1 copy is inside an
        # #if so a naive single replace would leave it stale and un-compiled-but-wrong.
        # ⚠⚠ THIS SITE SPELLS THE MAJOR AS A BARE DECIMAL LITERAL while bt_probe.r
        # spells the same field in hex, and below 10 those agree by coincidence. At 10
        # they do not: the field is BCD, so it must become 0x10 and NOT 10 (which the
        # Toolbox would read as 16). The replacement therefore changes the FORM of the
        # literal on the 9.9 -> 10.0 crossing, from `9, ` to `0x10, `, and the count
        # check below is what proves all four copies actually moved.
        ('src/bt_probe.c', f'{om if int(om) < 10 else f"0x{bcd(om):02X}"}, 0x{on}0, finalStage',
                           f'{nm if int(nm) < 10 else f"0x{bcd(nm):02X}"}, 0x{nn}0, finalStage', 4),
        # ⚠ THE COMMENT BESIDE EACH IS ALSO A VERSION STAMP, and it had drifted: all
        # three read "/* 5.4 */" while the BCD beside them said 5, 0x80 = 5.8. Same
        # class as the long vers string that sat at 4.1 from 4.2 through 5.4 -- a stamp
        # the bump touched the numbers of but not the words. The padding is preserved
        # because a single-digit minor is the same width either way, which the
        # single-digit check at the top of this script already guarantees.
        # ⚠ COUNT WENT 3 -> 4 when rule 1b (05AC:8204, the switched A1044) was
        # added. The text is counted regardless of kSendModeSwitch, because rule 1
        # sits inside an #if but its TEXT is still in the file.
        ('src/bt_probe.c', f'/* {old} ', f'/* {new} ', 4),
        ('src/bt_probe.c', tag(old), tag(new), 2),
        ('src/bt_probe.c', f"'v{majchar(om)}{on}0'", f"'v{majchar(nm)}{nn}0'", 2),
        ('cpanel/bt_panel.c', tag(old), tag(new), 1),
        ('bt-check/bt_check.c', tag(old), tag(new), 1),
        # The comments that name the version alongside the tag.
        ('cpanel/bt_panel.c', f"driver v{old}", f"driver v{new}", 1),
        ('bt-check/bt_check.c', f"driver v{old}", f"driver v{new}", 1),
        # ⚠⚠ AND THE OSTYPE SPELLED OUT IN THOSE SAME COMMENTS. bt_probe.c had this
        # rule and the two readers did not, so the 8.9 bump produced
        # `kExpectedDriverTag 0x76383930UL /* 'v880' = driver v8.9 */` in BOTH readers:
        # a comment contradicting the constant on its own line. That is EXACTLY the
        # drift that made the v93 run print "WRONG DRIVER" over a good log -- the
        # comment there said 'v650' while the constant said 'v810'. The whole point of
        # this script is that no version stamp is left to a human to remember, and a
        # comment naming a build tag is a version stamp.
        ('cpanel/bt_panel.c', f"'v{majchar(om)}{on}0'", f"'v{majchar(nm)}{nn}0'", 1),
        ('bt-check/bt_check.c', f"'v{majchar(om)}{on}0'", f"'v{majchar(nm)}{nn}0'", 1),
        # ⚠ BTCheck's Get Info string names the DRIVER version it expects, so a driver
        # bump has to move it -- nothing covered it before, and it was one bump away
        # from telling the user BTCheck expected a driver version that no longer
        # existed. Anchored on the CLOSING QUOTE so it cannot collide with the btcheck
        # rule that rewrites the leading "NN.0, " on the same line; the two patterns
        # overlap in text but replace disjoint ranges, in either order.
        ('bt-check/bt_check.r', f'expects driver v{old}"', f'expects driver v{new}"', 1),
    ]


def edits_for_btcheck(old, new):
    # ⚠⚠ THE 'vers' MAJOR FIELD IS ONE BCD BYTE, SO 99 IS THE CEILING -- and BTCheck
    # reached it. Formatting with :02d would emit "0x100," for 100: three digits in a
    # one-byte field, which Rez accepts and Get Info then renders arbitrarily.
    #
    # ⇒ RESOLVED BY GOING TWO-PART, the same shape the panel has always used. 99 became
    # 99.1, so the minor BCD byte carries the running number and there is headroom to
    # 99.99 without renumbering a single existing artifact or log. The integer form is
    # still accepted for everything up to 99.
    #
    # ⚠ The ceiling mattered for a real reason, not tidiness: it is why BTCheck was not
    # re-staged alongside driver 8.5, and a BTCheck older than the driver rendered a
    # SUCCESSFUL mode switch as "no completion recorded yet". A version scheme that
    # blocks a re-stage is a correctness problem.
    if '.' in str(new):
        maj, minr = str(new).split('.', 1)
        omaj, ominr = (str(old).split('.', 1) + ['0'])[:2]
        oldstr = str(old) if '.' in str(old) else f'{old}.0'
        # ⚠⚠⚠ THE BCD BYTES ARE NOW PINNED, AND THE STRINGS CARRY THE REAL VERSION.
        #
        # The two-part form bought headroom to 99.99 and BTCheck reached it, which
        # blocked re-staging the tool at all -- the exact "correctness problem" the note
        # above warns about, arrived.
        #
        # ⚠ AND THE OLD ENCODING WAS ALREADY WRONG ABOVE MINOR 15. The minor byte was
        # written as int(minr)*16, so 98 produced 0x620 -- three hex digits in a
        # ONE-BYTE field. Rez accepted it, Get Info rendered something arbitrary, and
        # nobody noticed because nobody reads the BCD.
        #
        # ⇒ Stop pretending the BCD can hold a running number. Pin both bytes at 0x99
        # and let the FILENAME and the LONG STRING carry the version, which is what is
        # actually read: reference_os9_getinfo_shows_long_vers_string records that Get
        # Info shows the long string, and the filename is what stops the wrong BTCheck
        # being staged beside a driver it does not match. Neither has a ceiling.
        #
        # ⚠ The pin is emitted only on the transition, so later bumps do not try to
        # replace the pinned bytes with themselves and trip the occurrence count.
        # ⚠⚠ THE TWO BCD BYTES ARE REPLACED AS ONE TWO-LINE PATTERN, and they have
        # to be. bt_check.r contains "    0x00," TWICE -- once as the minor version byte
        # and once as the 'prerelease' field after `release,` -- so a single-line rule
        # for the minor byte is ambiguous, and the first version of this refused with
        # "expected 1 of '    0x00,', found 2". Anchoring major-then-minor is unique.
        #
        # ⚠ The minor byte is BCD in the HIGH nibble: 0x10 is ".1", 0x25 is ".25" --
        # the same convention edits_for_panel uses for its 0x{N}0 form.
        edits = [
            ('bt-check/CMakeLists.txt', f'set(BTCHECK_VER "{old}")',
                                        f'set(BTCHECK_VER "{new}")', 1),
        ]
        # ⚠ NO BCD EDIT. The bytes are pinned at 0x99/0x99 and are no longer a version
        # stamp, so there is nothing here to bump. The first draft of this still tried
        # to compute the OLD bytes from the old version number -- which, once pinned,
        # describes a value the file no longer contains, and the script correctly
        # refused with "expected 1 of '    0x99,\n    0x640,', found 0". A transition
        # edit that outlives its transition is just a stale rule.
        edits += [
            # ⚠ The comment beside the BCD bytes IS a version stamp. Stale copies of
            # exactly this shape have bitten this project three separate times.
            ('bt-check/bt_check.r', f'/* {oldstr} -- BCD.', f'/* {new} -- BCD.', 1),
            ('bt-check/bt_check.r', f'"{oldstr}",', f'"{new}",', 1),
            ('bt-check/bt_check.r', f'"{oldstr}, expects driver v',
                                    f'"{new}, expects driver v', 1),
        ]
        return edits
    if int(new) > 99:
        sys.exit(f"btcheck {new} does not fit: the 'vers' major field is ONE BCD byte, "
                 f"so 99 is the ceiling. Use the two-part form instead -- 99.1, 99.2 -- "
                 f"which puts the running number in the minor byte and needs no "
                 f"renumbering of existing artifacts.")
    return [
        ('bt-check/CMakeLists.txt', f'set(BTCHECK_VER "{old}")',
                                    f'set(BTCHECK_VER "{new}")', 1),
        ('bt-check/bt_check.r', f'    0x{int(old):02d},', f'    0x{int(new):02d},', 1),
        # ⚠⚠ THIS RULE WAS BROKEN AND MADE EVERY BTCheck BUMP REFUSE. It was a single
        # pattern '"{old}.0"' with a count of 2, but only ONE of the two strings ends
        # in a quote right after the digit:
        #     "61.0",                        <- matches
        #     "61.0, expects driver v5.4"    <- a COMMA follows, so it never matched
        # PASS 1 therefore found 1 of an expected 2 and refused, every time. The two
        # strings need two patterns because they are two different shapes; collapsing
        # them into one with a count was the mistake.
        ('bt-check/bt_check.r', f'"{old}.0",', f'"{new}.0",', 1),
        ('bt-check/bt_check.r', f'"{old}.0, expects driver v',
                                f'"{new}.0, expects driver v', 1),
    ]


def edits_for_panel(old, new):
    om, on = old.split('.')
    nm, nn = new.split('.')
    return [
        ('cpanel/bt_panel.r', f'0x0{om}, 0x{on}0, development',
                              f'0x0{nm}, 0x{nn}0, development', 2),
        # ⚠ TWO of each now: the panel carries vers(1) for Apple System Profiler AND
        # vers(2) for the Finder, with identical strings. See the note in bt_panel.r.
        ('cpanel/bt_panel.r', f'"{old}",', f'"{new}",', 2),
        # ⚠ The versioned .bin's filename. Kept here rather than derived at stage
        # time so it cannot disagree with the 'vers' resource -- BTCheck v46 shipped
        # writing BTCheck_v45.log because its version lived in four hand-maintained
        # places, and this is the same hazard one artifact later.
        ('cpanel/CMakeLists.txt', f'set(PANEL_VER "{old}")',
                                  f'set(PANEL_VER "{new}")', 1),
        ('cpanel/bt_panel.r', f'"{old}, github', f'"{new}, github', 1),
        # ⚠ THE DESCRIPTION EMBEDS THE VERSION, and the first real use of this script
        # missed it: the panel bumped to 3.1 while its Get Info description still read
        # "Bluetooth 3.0 - ...". The convention is therefore ENFORCED here -- a
        # description that does not start "Bluetooth <ver> - " makes this refuse, which
        # surfaces the drift instead of shipping it.
        ('cpanel/bt_panel.r', f'"Bluetooth {old} - ', f'"Bluetooth {new} - ', 2),
        ('cpanel/bt_panel.c', f'BTPanel {old}  ticks ', f'BTPanel {new}  ticks ', 1),
        ('cpanel/bt_panel.c', f'Control panel {old} - device scanning',
                              f'Control panel {new} - device scanning', 1),
    ]


def check_version(label, v):
    """⚠ THE MINOR IS STILL ONE DIGIT. 4.9 goes to 5.0, never 4.10 -- the tag has one
    byte for it and no letter trick is applied there, because a minor is bumped far
    more often than a major and a silent 4.10 -> 'v9:0' would be unreadable garbage.
    The MAJOR may now be 0-35, encoded as a digit or a letter (see majchar)."""
    m = re.fullmatch(r'(\d{1,2})\.(\d)', v)
    if not m:
        sys.exit(f"{label} version {v!r} must be MAJOR.MINOR with a SINGLE-DIGIT minor "
                 f"-- the block build tag has one byte for it. 4.9 goes to 5.0, not 4.10.")
    if int(m.group(1)) > 35:
        sys.exit(f"{label} version {v!r}: the major must be 0-35, since the tag encodes "
                 f"it as one byte (0-9 then A-Z). Time for a wider tag.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--driver')
    ap.add_argument('--panel')
    ap.add_argument('--btcheck')
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()

    cur_drv, cur_btc, cur_pnl = current()
    if not (a.driver or a.panel or a.btcheck):
        print(f"   driver  {cur_drv}   (block tag {tag(cur_drv)})")
        print(f"   panel   {cur_pnl}")
        print(f"   btcheck {cur_btc}")
        return 0

    edits = []
    if a.driver:
        check_version('driver', a.driver)
        edits += edits_for_driver(cur_drv, a.driver)
    if a.panel:
        check_version('panel', a.panel)
        edits += edits_for_panel(cur_pnl, a.panel)
    if a.btcheck:
        # ⚠ TWO-PART IS NOW LEGAL, because the integer form hit the one-byte BCD
        # ceiling at 99. "99.1" puts the running number in the minor byte. The plain
        # integer form still works for anything <= 99.
        if not (a.btcheck.isdigit()
                or (a.btcheck.count('.') == 1
                    and all(part.isdigit() for part in a.btcheck.split('.')))):
            sys.exit("btcheck version must be an integer (<= 99) or N.M")
        edits += edits_for_btcheck(cur_btc, a.btcheck)

    # ---- PASS 1: read everything and verify every count BEFORE any write ----------
    files = {}
    problems = []
    for rel, old, new, want in edits:
        if rel not in files:
            files[rel] = read(rel)
        got = files[rel].count(old)
        status = 'ok' if got == want else 'MISMATCH'
        print(f"   [{status}] {rel}: {got}/{want}  {old[:44]}")
        if got != want:
            problems.append(f"{rel}: expected {want} of {old!r}, found {got}")

    if problems:
        print("\nREFUSING TO WRITE -- nothing has been modified:")
        for p in problems:
            print(f"   {p}")
        return 1

    # Apply in memory.
    for rel, old, new, want in edits:
        files[rel] = files[rel].replace(old, new)

    if not a.apply:
        print(f"\ndry run: {len(edits)} replacements across {len(files)} files. "
              f"Re-run with --apply.")
        return 0

    # ---- PASS 2: temp file, verify, os.replace. Never open an original to write ----
    for rel, text in files.items():
        p = path(rel)
        d = os.path.dirname(p)
        fd, tmp = tempfile.mkstemp(dir=d, suffix='.bumptmp')
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write(text)
            with open(tmp, encoding='utf-8') as f:
                if f.read() != text:
                    raise RuntimeError(f"temp file verify FAILED for {rel}")
            os.replace(tmp, p)
            print(f"   wrote {rel}")
        except BaseException:
            if os.path.exists(tmp):
                os.unlink(tmp)
            raise

    print("\ndone. Now: rebuild all three, re-run tests/run-tests.sh and the audit.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
