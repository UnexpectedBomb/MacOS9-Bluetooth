/*
 *  bt_csm.r -- resources for the Bluetooth Control Strip module.
 *
 *  ⚠⚠ A 'cfrg' FRAGMENT, NOT AN 'sdev' RESOURCE, AND THAT IS NOT A STYLE CHOICE.
 *  Both forms load and run identically, but a module whose PEF is wrapped in an
 *  'sdev' Mixed-Mode resource is a SECOND-CLASS module the Control Strip will not
 *  let you drag out of the strip -- option-drag simply does nothing. Every shipping
 *  stock module is a cfrg fragment with the PEF in the DATA FORK. RE'd from the stock
 *  modules on disk; the requirement is undocumented. See os9_control_strip_module.
 *
 *  The file is still Finder type 'sdev' (that is how the strip recognises a module)
 *  but carries NO 'sdev' resource. The fragment `main` is pointed at
 *  ControlStripModule after MakePEF by scripts/patch-pef-main.py.
 */

/* ⚠ Rez has no built-in 'cfrg' template -- "Can't find type definition for 'cfrg'"
 * without this. Same first line as every other cfrg-based module in this repo. */
#include "CodeFragments.r"

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "Bluetooth"
    }
};

/* ⚠ 'vers' is defined inline: this Rez set's SysTypes.r does not carry the type, and
 * pulling in a header for one resource is not worth it. SAFE in a normal file's fork
 * -- the "vers(1) breaks boot" hazard is ROM-fork-only. */
type 'vers' {
    hex byte;                        /* major (BCD)            */
    hex byte;                        /* minor + bugfix (BCD)   */
    hex byte;                        /* release stage          */
    hex byte;                        /* non-release revision   */
    integer;                         /* region code            */
    pstring;                         /* short version          */
    pstring;                         /* long (Get Info) string */
};

/* ⚠⚠ BOTH STRINGS CARRY THE VERSION AND BOTH MUST BE BUMPED.
 *
 * Get Info shows the LONG string, the Finder's list views show the SHORT one, and this
 * project has already shipped a build where they disagreed. CMake now checks that both
 * contain CSM_VER, so a half-bumped release fails the build instead of shipping.
 *
 * ⚠ 1.0 said "no battery level", because at the time the level was believed
 * unobtainable. It is obtainable through our own HCI stack -- see the header of
 * bt_csm.c and docs/CSM-BATTERY-FEASIBILITY.md -- and 1.1 is that gauge. */
resource 'vers' (1) {
    0x02, 0x30, 0x80, 0x00,
    0,
    "2.5",
    "2.5 - Bluetooth device battery level; user-renamed devices"
};

/* ---- Finder bundle ---------------------------------------------------------- *
 * Stock Control Strip modules all carry a BNDL/FREF plus the kHasBundle flag (set
 * post-Rez by scripts/set-custom-icon.py). Without it the strip will not let you drag
 * a copy of the module out. Retargeted to creator 'BTcs'.
 *
 * ⭐ 1.2 HAS A REAL ICON FAMILY. The user drew it in ResEdit and delivered it as a
 * resource-fork-only donor on the Pi share; scripts/custom-icon-to-r.py extracted all
 * six classic members (ICN#, icl4, icl8, ics#, ics4, ics8) verbatim into
 * icons/bt_csm_icon.r, which is #included below.
 *
 * ⚠ VERBATIM, NOT TRANSCRIBED. Reading an icon off a picture of it loses both shape and
 * colour; the bytes come straight out of the donor's resource fork. See
 * os9_control_strip_module.
 *
 * ⚠⚠ BOTH HALVES OR NEITHER. The family at ID -16455 does nothing on its own -- the file
 * also needs the kHasCustomIcon Finder flag, which Rez cannot set, so
 * scripts/set-custom-icon.py sets it on the built MacBinary. 1.0 and 1.1 carried NO
 * family and were therefore built with --no-icon-flag, because setting the flag on a file
 * with no icon renders as a BLANK icon (reads as corrupt), a bug this project has shipped
 * once. 1.2 drops --no-icon-flag precisely because the family is now here. */

#include "icons/bt_csm_icon.r"

/* ⭐ THE STRIP CELL'S GLYPH IS A SEPARATE FAMILY AT ID 128, deliberately different
 * artwork from the file's Finder icon above. The user supplied it for the cell
 * specifically (2026-09-21), and the two jobs are genuinely different: -16455 is a
 * 32x32-first document icon seen in a folder, 128 is a 16x16 glyph read at a glance in a
 * 22px-tall strip beside Battery Monitor.
 *
 * ⚠ ID 128 is matched against kIconID in bt_csm.c by CMake -- SBGetDetachIconSuite asks
 * for a NUMBER, and if these two drift the module silently gets no icon and falls back
 * to a text label, which looks like a design choice rather than a broken reference. */
#include "icons/bt_csm_cell_icon.r"
data 'BTcs' (0) {
    "2.5, github.com/UnexpectedBomb"
};

/* FREF: file type 'sdev', local icon ID 0, empty name. */
data 'FREF' (128) {
    $"7364 6576 0000 00"
};

/* BNDL: owner 'BTcs'; FREF local 0 -> FREF 128; ICN# local 0 -> ICN# -16455
 * (0xBFB9 = -16455, the family in icons/bt_csm_icon.r).
 *
 * ⚠ THE THIRD FIELD IS "ENTRY COUNT MINUS ONE", so adding the ICN# mapping means
 * 0x0000 -> 0x0001 as well as appending the row. Copied from cpu-temp/csm, which is
 * the module on this machine known to show its icon correctly -- 1.0/1.1 had 0x0000
 * and FREF only, because they had no family to point at. */
data 'BNDL' (128) {
    $"4254 6373 0000 0001"
    $"4652 4546 0000 0000 0080"
    $"4943 4E23 0000 0000 BFB9"
};
