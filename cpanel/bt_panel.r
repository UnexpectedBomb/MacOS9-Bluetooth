/*
 *  bt_panel.r  --  resources for the Bluetooth control panel.
 *
 *  ⚠ NO vers(1). A vers resource with ID 1 stamped into a patched Mac OS ROM breaks
 *  boot on this project's hardware, and the habit of reaching for vers(1) is what
 *  caused that. An application is not a ROM and vers(1) would be harmless here, but
 *  vers(2) is what the Finder shows for a control panel anyway -- and keeping to (2)
 *  alone means there is never a stray vers(1) lying around to be copied into
 *  something that cannot take it. See reference_os9_rom_vers1_breaks_boot.
 */

#include "SysTypes.r"
#include "Types.r"

/* ★★★★★★ vers(1) IS REQUIRED FOR APPLE SYSTEM PROFILER, and its absence is why ASP
 * listed this panel as "Version: Not available" beside USB Overdrive 1.4 and
 * Appearance 1.1.4. ASP's Control Panels tab reads vers ID 1; nothing else answers it.
 *
 * ⚠ THE OLD REASONING IS KEPT BELOW AND IS NARROWED, NOT DISCARDED. vers(1) in a
 * patched Mac OS ROM breaks boot on this hardware -- that is real and it cost a grey
 * screen [[reference_os9_rom_vers1_breaks_boot]]. Avoiding vers(1) HERE was defence
 * against the habit of copying one into something that cannot take it. But the driver
 * (bt_probe.r) and BTCheck have carried vers(1) all along without incident, so the
 * panel was the odd one out, and the cost was a visible blank in the one place a user
 * checks what is installed. A ROM is not an application; the rule belongs to the ROM
 * build, not to every file in the project.
 *
 * ⚠ BOTH RESOURCES CARRY THE SAME STRINGS, and scripts/bump-version.py expects TWO of
 * each pattern in this file. Adding a third vers, or wording them differently, breaks
 * the stamper -- which is the point: it fails loudly rather than letting them drift. */
resource 'vers' (1) {
    0x019, 0x20, development, 0x00, verUS,
    "19.2",
    "Bluetooth 19.2 - idle at boot no longer reads as a fault"
};

resource 'vers' (2) {
    0x019, 0x20, development, 0x00, verUS,
    "19.2",
    "Bluetooth 19.2 - idle at boot no longer reads as a fault"
};

/* ---- Finder bundle, so the supplied icon family actually shows -------------- *
 * Same shape as the CPU Temp CSM's, retargeted to this creator and to the icon
 * family the user supplied, which is at ID 128 -- the application convention.
 *
 * ⚠ The kHasBundle flag is NOT set by Rez. scripts/set-custom-icon.py sets it after
 * the fact, and without it the Finder never reads BNDL/FREF and the icon silently
 * does not appear. That silence is the whole reason this comment exists. */

/* The required signature resource. Content is conventionally a version string. */
data 'BTcp' (0) {
    "19.2, github.com/UnexpectedBomb"
};

/* FREF: file type 'APPL', local icon ID 0, empty name. */
data 'FREF' (128) {
    $"4150 504C 0000 00"
};

/* ---- DOCUMENT ICONS, v15.4 --------------------------------------------------
 * One FREF per file type this stack generates, every one pointing at LOCAL icon 1,
 * which the BNDL maps to the family at ID 129. The panel's own icon stays local 0
 * -> 128.
 *
 * ⚠ THIS ONLY WORKS BECAUSE EVERY GENERATED FILE NOW HAS CREATOR 'BTcp'. The Finder
 * finds a document's icon by looking up its CREATOR's bundle -- not its type -- so
 * while the driver wrote 'BTky'/'BTba' and the switcher wrote 'OS9B', those files
 * resolved to no bundle at all and got the generic blank page. Changing the creators
 * and adding these FREFs are two halves of one change; neither works alone.
 *
 * The seven types, and who writes each:
 *   BTKy  the link-key database          driver
 *   BTNm  each device's own name         driver
 *   BTBa  battery levels                 driver
 *   BTal  nicknames you set              control panel
 *   BTcd  class of device per address    control panel
 *   BTpf  pair-at-restart flag           control panel and Control Strip
 *   BTmk  switch-attempted marker        switcher
 *
 * ⚠ 'TEXT' is deliberately NOT claimed. Bluetooth Panel Status is a plain text log,
 * and an FREF for 'TEXT' under our creator would be this panel asserting an opinion
 * about every text file the Finder shows. */
data 'FREF' (129) { $"4254 4B79 0001 00" };   /* BTKy -- link keys        */
data 'FREF' (130) { $"4254 4E6D 0001 00" };   /* BTNm -- device names     */
data 'FREF' (131) { $"4254 4261 0001 00" };   /* BTBa -- battery levels   */
data 'FREF' (132) { $"4254 616C 0001 00" };   /* BTal -- nicknames        */
data 'FREF' (133) { $"4254 6364 0001 00" };   /* BTcd -- device kinds     */
data 'FREF' (134) { $"4254 7066 0001 00" };   /* BTpf -- pair at restart  */
data 'FREF' (135) { $"4254 6D6B 0001 00" };   /* BTmk -- switch marker    */

/* BNDL: owner 'BTcp'.
 *   FREF array: local 0 -> 128 (the app itself), locals 1..7 -> 129..135
 *   ICN#  array: local 0 -> 128 (panel icon),    local  1    -> 129 (document icon) */
data 'BNDL' (128) {
    $"4254 6370 0000 0001"
    $"4652 4546 0007"
    $"0000 0080"
    $"0001 0081"
    $"0002 0082"
    $"0003 0083"
    $"0004 0084"
    $"0005 0085"
    $"0006 0086"
    $"0007 0087"
    $"4943 4E23 0001"
    $"0000 0080"
    $"0001 0081"
};

/* The icon family itself: ICN#/icl4/icl8/ics#/ics4/ics8 at 128, generated from the
 * artwork the user supplied. Rez cannot `read` these types -- the same trap the CPU
 * Temp CSM hit with PICT -- so they are emitted inline as hex.
 *
 * ⚠ Rez #include dependencies are NOT tracked by CMake. bt_icon.r MUST stay in this
 * target's DEPENDS or an artwork change silently will not be re-Rez'd. */
#include "bt_icon.r"
/* The document icon family at 129. Same DEPENDS caveat as above. */
#include "bt_prefs_icon.r"

/* The per-kind 16x16 device icons for the list's rows, at IDs 200..205. Separate file
 * and separate IDs from the family above: 128 is the panel's OWN icon, shown by the
 * Finder, and these are row artwork shown by PlotIconID. Same DEPENDS caveat -- it is
 * wired into the stamp target in CMakeLists.txt. */
#include "bt_device_icons.r"

/* The group-box Bluetooth logo, one per radio state: 210 colour (On), 211 greyscale
 * (Off). Same DEPENDS caveat as the two above. */
#include "bt_radio_icons.r"
