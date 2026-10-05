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
 * ⚠ The long string says DIAGNOSTIC out loud, because this extension takes away a
 * working keyboard for as long as it is installed. Anyone finding it in Extensions
 * months from now should learn that from Get Info and not from a dead keyboard. */
resource 'vers' (1) {
    0x01, 0x70, finalStage, 0x00, verUS,
    "1.7",
    "Bluetooth Switch 1.7 - claims the A1044 at boot; falls back for one boot if the driver did not come up"
};
