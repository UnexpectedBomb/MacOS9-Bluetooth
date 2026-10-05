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
 * scripts/bump-version.py now REFUSES to bump unless this string starts
 * "Bluetooth Support <ver> - ", so the convention is enforced rather than
 * remembered. Do not reword that prefix without updating the script. */
resource 'vers' (1) {
    0x17, 0x60, finalStage, 0x00, verUS,
    "17.6",
    "Bluetooth Support 17.6 - native Bluetooth for Mac OS 9"
};
