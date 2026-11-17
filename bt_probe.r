/*
 *  bt_probe.r  --  code-fragment + version resources for the M0.0 probe driver.
 *  Mirrors the esata-sil3512 / usb2-ehci 'ndrv' cfrg template; fragment name
 *  must match nothing in particular, but the Expert FindSymbol's the two
 *  exported symbols (TheUSBDriverDescription, TheClassDriverPluginDispatchTable)
 *  inside it. Packaged as file type 'ndrv', creator 'usbd' (see CMakeLists.txt),
 *  which is what the USB Expert scans Extensions for.
 */

#include "CodeFragments.r"
#include "Types.r"

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "OS9BTProbe"
    }
};

/* Custom icon family, so a Bluetooth extension is never mistaken for the EHCI
 * Activator in the Extensions folder. GENERATED from the user's donor file by
 * scripts/custom-icon-to-r.py; the matching hasCustomIcon Finder flag is set on the
 * built MacBinary by scripts/set-custom-icon.py. Rez cannot `read` these, which
 * is why they are #included as data statements. */
#include "icons/bt_icons.r"

/* ⚠ THE NUMERIC FIELDS MUST TRACK THE STRING, and they had drifted: this read
 * 0x01, 0x40 -- BCD for version 1.4 -- while the string already said 2.1. Get Info
 * displays the STRING, so the mismatch was invisible there, but it is now the
 * designated way to check which extension is installed (the file itself carries a
 * FIXED name), so a stale number here would undermine the whole point.
 * Format: majorRev (BCD), minorAndBugRev (BCD), stage, nonFinalRelease.
 *
 * ⚠⚠ ALL THREE FIELDS BELOW ARE VERSION STAMPS, and the LONG string is the one the
 * Finder actually shows. Get Info's "Version:" line displays the LONG string, not
 * the short one -- the short string is what list views and the Installer read. That
 * is why this drifted unnoticed from 4.2 through 5.4: the numeric fields and the
 * short string were being bumped, the long string was not, and the only place the
 * user ever looks was therefore the only place still reading 4.1.
 *
 * ★ RELEASE 18.6 (user, 2026-11-16): PLAIN VERSIONS, the shape approved for AirPort
 * 2.0. vers(1) carries the number alone in BOTH strings, so Get Info's "Version:" line
 * reads "18.6" and nothing else; vers(2)'s long string names the release every file of
 * the stack came from, OS 9 Bluetooth <release> -- Get Info shows it under the file name
 * and Extensions Manager / ASP in their Package column. No description, no changelog,
 * no build tag: test builds say what they are in their FILENAMES and log banners.
 *
 * scripts/bump-version.py REFUSES to bump unless vers(1) reads "<ver>" in both strings
 * and the numeric pair appears in both resources, so the convention is enforced rather
 * than remembered (it used to enforce "Bluetooth Support <ver> - "). Its --release
 * option moves "OS 9 Bluetooth <release>" in all five .r files of the stack at once. */
resource 'vers' (1) {
    0x18, 0x70, finalStage, 0x00, verUS,
    "18.7",
    "18.7"
};

/* ★ RELEASE 18.7 (user, 2026-11-17): the driver and BTCheck only. The panel, strip and
 * switcher did not change and keep "OS 9 Bluetooth 18.6" -- the release each of them came
 * from, as Apple's own updates left untouched files naming the older package. So until
 * the next five-file release, bump-version.py reports this as two release names. */
resource 'vers' (2) {
    0x18, 0x70, finalStage, 0x00, verUS,
    "18.7",
    "OS 9 Bluetooth 18.7"
};
