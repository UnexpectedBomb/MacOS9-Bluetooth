/*
 *  bt_switch.r  --  code-fragment + version resources for USBBluetoothSwitch.
 *
 *  Same 'ndrv' shape as bt_probe.r: file type 'ndrv', creator 'usbd', with the two
 *  exported symbols the Expert FindSymbol's inside the fragment. The fragment NAME
 *  differs from the driver's ("OS9BTSwitch" vs "OS9BTProbe") because both fragments
 *  are registered at once and a name collision is not worth finding out about the
 *  hard way.
 *
 *  ⚠ NO ICON FAMILY, deliberately. bt_probe.r carries one so a Bluetooth extension is
 *  never mistaken for the EHCI Activator; this file wants the OPPOSITE -- a plain
 *  generic extension icon, visibly different from the real driver's, so the two are
 *  not confused in the Extensions folder. They are dragged in and out independently
 *  and that is exactly when a look-alike icon would cost a run.
 */

#include "CodeFragments.r"
#include "Types.r"

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "OS9BTSwitch"
    }
};

/* ⚠⚠ THE LONG STRING IS WHAT GET INFO SHOWS. The short one is for list views and the
 * Installer. That distinction cost this project four version bumps' worth of drift on
 * the main driver -- 4.2 through 5.4 all reported 4.1 in the only place the user ever
 * looks. See reference_os9_getinfo_shows_long_vers_string.
 *
 * ★ RELEASE 18.6 (user, 2026-11-16): PLAIN VERSIONS, the shape approved for AirPort 2.0.
 * vers(1) carries the number alone in both strings (Get Info's "Version:" line, the
 * Extensions Manager and ASP Version columns); vers(2)'s long string names the release
 * every file of this stack came from (OS 9 Bluetooth <release>: the line under the name in
 * Get Info, and the Package column). What this extension DOES -- it claims the internal
 * A1044 at start-up, and stands down for one boot if the driver did not come up -- moved
 * to the panel's Help and the README, which is where a user reads explanations.
 *
 * ⚠ The code is 1.7's, unchanged since release 17.6: only this resource differs. 1.8
 * (9b4d332, a second record for a DBT-120 in HID-proxy mode, 0A12:1000) never ran on
 * hardware, so it is not in this release; a DBT-120 that comes up as 0A12:0001 needs no
 * switcher at all. */
resource 'vers' (1) {
    0x01, 0x70, finalStage, 0x00, verUS,
    "1.7",
    "1.7"
};

resource 'vers' (2) {
    0x01, 0x70, finalStage, 0x00, verUS,
    "1.7",
    "OS 9 Bluetooth 18.6"
};
