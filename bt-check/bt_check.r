/* bt_check.r -- resources for the BTCheck diagnostic app.
 *
 * The result window is created programmatically in bt_check.c (NewWindow), so the
 * only resources needed are the memory partition and a version stamp.
 */

#include "Processes.r"
#include "Types.r"

/* Read-only diagnostic. It holds 96 summary lines of Str255 (about 24 KB) and
 * one window, so 640 KB preferred / 512 KB minimum is comfortable headroom,
 * matching the FireWire probes. */
resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    multiFinderAware,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    notDisplayManagerAware,
    reserved,
    reserved,
    640 * 1024,    /* preferred */
    512 * 1024     /* minimum   */
};

resource 'vers' (1, "BTCheck") {
    0x99,
    0x99,                   /* 99.110 -- BCD. ⚠ It had drifted to 26.0 while the target
                             * name said v45, so Get Info disagreed with the filename,
                             * and this comment then itself drifted to naming 47.0
                             * while the field above read 0x61. bt_check.c has no copy
                             * at all -- CMakeLists injects kBTCheckVer and names the
                             * app from the same string. Rez cannot read that define,
                             * so these lines still exist -- but they are NO LONGER
                             * hand-maintained: scripts/bump-version.py now rewrites
                             * all four stamps in this file (the BCD byte, the short
                             * string, the long string's leading version, and the
                             * driver version it says it expects) and REFUSES to write
                             * anything if any one of them fails to match. Use the
                             * script; do not edit these by hand. */
    release,
    0x00,
    verUS,
    "99.110",
    "99.110"
};

/* ★ RELEASE 18.6 (user, 2026-11-16): PLAIN VERSIONS, as for every file of the stack.
 * vers(1) is the number alone; vers(2) names the release. The driver version this build
 * reads is no longer in Get Info: the log's own banner states it (kExpectedDriverTag),
 * which is where a mismatch is actually diagnosed.
 * ⚠ The BCD pair above stays pinned at 0x99/0x99 (see the note there), so ASP's Version
 * column reads "99.9.9" for BTCheck: BCD cannot spell 99.110. Get Info and the log are
 * the places this tool's version is read. */
resource 'vers' (2, "BTCheck") {
    0x99,
    0x99,
    release,
    0x00,
    verUS,
    "99.110",
    "OS 9 Bluetooth 18.6"
};
