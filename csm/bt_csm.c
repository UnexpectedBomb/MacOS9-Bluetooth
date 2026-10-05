/*
 *  bt_csm.c -- "Bluetooth", a Control Strip module: a battery gauge for paired
 *  devices, with a click to switch between them.
 *
 *  ======================================================================
 *  ⭐ WHY THERE IS A GAUGE HERE NOW, AND WHY THIS FILE ONCE SAID THERE COULD NOT BE
 *  ======================================================================
 *
 *  Until 2026-09-15 this module's header said a battery gauge was NOT BUILDABLE. That
 *  claim was half right, and the half that was wrong is worth keeping written down
 *  because the same mistake is easy to make again:
 *
 *    ✓ STILL TRUE -- in HID-PROXY mode the level is unreachable. The card presents
 *      boot-protocol interfaces whose reports carry Keyboard/Keypad page (0x07) and
 *      nothing else, and no software of ours is in the path at all. Mac OS drives the
 *      keyboard directly.
 *
 *    ✗ WAS WRONG -- generalising that into "the level cannot be obtained". Since v11.x
 *      we bind the card at 05AC:8204, own the HIDP link and speak GET_REPORT ourselves.
 *      The level comes back on UNDECLARED VENDOR REPORT IDs (Feature 71 = percent,
 *      Input 48 = state) that the device's own public HID report descriptor never
 *      mentions. ⚠ A DESCRIPTOR SAYS WHAT A DEVICE ADVERTISES, NOT WHAT IT ANSWERS.
 *      Measured on the A1016 at 66-68%. docs/CSM-BATTERY-FEASIBILITY.md is the study.
 *
 *  ⇒ So the gauge is shown when OUR stack owns the card, and is NOT shown when it does
 *  not. That is the same honesty rule docs/M3-DESIGN.md set -- a CSM must never display
 *  a level it cannot obtain -- applied to a premise that has since changed.
 *
 *  ======================================================================
 *  ⚠⚠ A STALE LEVEL IS A LIE, AND THE WORST CASE IS THE ONE THAT MATTERS
 *  ======================================================================
 *
 *  The driver NEVER reads `Preferences:Bluetooth Battery` back in -- its device table
 *  starts empty at every driver load and the file is rewritten wholesale on each flush.
 *  So every record in that file was read during the session that wrote it. Good.
 *
 *  ⚠ But the FILE outlives the session. Boot into a session where the keyboard never
 *  connects and the file from LAST boot is still sitting there, and a reader that
 *  trusts it shows a confident 66% for a keyboard whose battery is flat. That is the
 *  gauge lying at exactly the moment it matters most.
 *
 *  ⇒ FRESHNESS IS TESTED, NOT ASSUMED: FileWrittenThisBoot() compares the file's
 *  modification date against when this machine booted (now - uptime). A file written
 *  before boot is from a previous session and its contents are not shown at all.
 *
 *  ⚠ whenTicks IS NOT A FRESHNESS SIGNAL and is not used as one -- TickCount() restarts
 *  at boot, so a file written late in one session looks *newer* than one written early
 *  in the next. bt_keyfile.c says so where it writes the field. It is used here only
 *  for the reading's age WITHIN a session, which is the one thing it can honestly say,
 *  and it is shown in the menu rather than the cell.
 *
 *  ======================================================================
 *  ⭐ THREE DESIGN CHOICES THAT REMOVE MOST OF THE RISK OF CSM WORK
 *  ======================================================================
 *
 *  1. ⚠⚠ NO MUTABLE FRAGMENT GLOBALS. Adding a mutable `static` to a CSM made the
 *     ENTIRE Control Strip fail to load once on this project -- a Control-Strip-loaded
 *     PEF does not get its writable data section instantiated the way an application
 *     does. All state lives in a heap block allocated in sdevInitModule and handed
 *     back to us as `params` on every later call. Read-only constants are fine.
 *
 *  2. ⚠⚠ THE CELL IS LIVE NOW, SO IT MUST SELF-DRAW -- AND A CACHED PORT CAN DANGLE.
 *     The strip repaints a module only when its width changes or it is externally
 *     invalidated; there is no "redraw my content" API. A changing reading therefore
 *     has to repaint itself from sdevPeriodicTickle through a GrafPtr cached from the
 *     last real sdevDrawStatus. The CPU-temp module proved (v1.8, on this same G4) that
 *     the strip can rebuild its window while the module stays loaded, leaving that
 *     pointer DANGLING -- and QuickDraw then WRITES pixels through a freed port's
 *     PixMap chain, a silent system-heap scribbler that surfaces much later as a bus
 *     error in someone else's code. PortLooksAlive() below is that module's guard,
 *     carried over deliberately rather than re-derived.
 *
 *     ⇒ This is the cost of the gauge. The old fixed-label design paid none of it,
 *     which is why the old header called the label a feature. It was, until there was
 *     something worth showing.
 *
 *  3. ⭐ STATE COMES FROM THE USB BUS AND FROM FILES, NOT FROM THE DRIVER'S COUNTER
 *     BLOCK. Scanning the System heap for 'BTP1' would work, but the block is FOUND by
 *     matching 'ENDS' at kWEnd -- a word index that has now moved THIRTEEN times (720
 *     as of v14.4) and would make this module a fourth binary needing a lockstep bump.
 *     The bus and the preference files say the same things more directly and cannot go
 *     stale against the driver:
 *
 *         05AC:1000 present -> HID-proxy: Mac OS is driving it, no level obtainable
 *         05AC:8204 present -> HCI: our stack has it, the gauge is live
 *         neither            -> no Bluetooth hardware on the bus
 */

#include <MacTypes.h>
#include <MacMemory.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Menus.h>
#include <Files.h>
#include <Folders.h>
#include <TextUtils.h>
#include <Events.h>
#include <OSUtils.h>
#include <DateTimeUtils.h>
#include <Icons.h>
#include <Processes.h>
#include <USB.h>
#include <ControlStrip.h>
#include <string.h>

#define kA1044Vendor  0x05AC
#define kA1044Proxy   0x1000
#define kA1044HCI     0x8204

/* ⭐ A GENERIC USB BLUETOOTH CONTROLLER, not just Apple's card. Class 0xE0 subclass 0x01
 * protocol 0x01 IS "Bluetooth HCI transport" and nothing else legitimately claims it,
 * which is why the driver's own Rules 2a/2b in src/bt_probe.c match on it with ANY
 * vendor and product.
 *
 * ⚠⚠ THIS MODULE DID NOT KNOW THAT UNTIL 1.9, AND IT WAS A REAL DEFECT ON A DONGLE. The
 * driver would bind a dongle, read the battery and publish it, while ScanBusState
 * recognised only 05AC:1000/8204 and therefore answered kStAbsent -- so the menu said
 * "No Bluetooth hardware" and, worse, Refresh() skipped LoadBattery entirely because it
 * gates on kStHCI. The gauge would never have appeared. Hardware identity was standing in
 * for "is our stack live", and those are different questions the moment the hardware
 * changes.
 *
 * ⚠ DEVICE-LEVEL ONLY. A controller may instead declare class 0x00 at device level and
 * put 0xE0/0x01/0x01 on interface 0 -- the driver handles both shapes, but seeing the
 * interface form needs the CONFIGURATION descriptor, and this module only fetches the
 * device descriptor. The dongle actually measured for this project (0x0A12:0x0001)
 * declares at device level, so that shape is covered; the interface-only shape would
 * still read as absent. Recorded rather than silently half-supported. */
#define kBTClass      0xE0
#define kBTSubClass   0x01
#define kBTProto      0x01

/* ⚠⚠ THE PREFS FILES ARE CONTRACTS WITH TWO OTHER BINARIES, and nothing links them.
 *
 *   Preferences:Bluetooth Device Kinds    cpanel/bt_panel.c WRITES, we READ
 *   Preferences:Bluetooth Battery         src/bt_keyfile.c  WRITES, we READ
 *
 * Magic, format version and record layout must match CodSave/CodLoad in the panel and
 * BT_BatteryFlushIfDirty in the driver exactly. CMake checks the filenames and the
 * magic numbers in both directions -- see this directory's CMakeLists.txt. */
#define kCodMagic   0x42544344UL      /* 'BTCD' -- device kinds  */
#define kCodFmtVer  1UL
#define kBatMagic   0x42544241UL      /* 'BTBA' -- battery       */
/* ⚠⚠ VERSION 2 = the address fields are the top 3 bytes then the low 3, matching the
 * Device Kinds file. Version 1 wrote 2+4, so THE JOIN BELOW MATCHED NOTHING and every
 * device appeared twice -- once named without a level, once with a level and the generic
 * name. Driver 14.5 fixed the writer and moved the version; refusing a version 1 file is
 * the correct behaviour, because its addresses cannot be joined to anything. */
#define kBatFmtVer  2UL
/* ⚠⚠ A THIRD FILE, and a third contract. `Preferences:Bluetooth Device Names` is
 * written by the DRIVER (src/bt_keyfile.c), not the panel, because the driver is
 * what issued Remote_Name_Request. kNameChars MUST equal the driver's kNameChars in
 * bt_pump.h -- the records are fixed-size and a disagreement misaligns every row
 * after the first, which reads as garbled names rather than as a size bug. */
#define kNmMagic    0x42544E4DUL      /* 'BTNM' -- driver WRITES, we READ */
#define kNmFmtVer   1UL
#define kNameChars  24
#define kMaxDev     8                 /* matches kBatMaxDev and BT_MAX_LINK_KEYS */

enum { kStUnknown = 0, kStProxy = 1, kStHCI = 2, kStAbsent = 3 };

#define kNoReading  (-1)              /* devPct when this device has not answered */

typedef struct {
    short         state;
    short         devCount;
    /* ⚠⚠ THE JOIN KEY between the two preference files and the persisted selection.
     * Top 3 bytes in devHi, low 3 in devLo -- BOTH files must agree on that split or
     * nothing matches and each device is listed twice. They disagreed once; see the
     * kBatFmtVer note above. An address is compared, never formatted, so the split is
     * invisible in the UI and a mismatch shows up only as duplicate rows. */
    unsigned long devHi[kMaxDev];
    unsigned long devLo[kMaxDev];
    unsigned long devCod[kMaxDev];    /* class of device, 0 when not yet learned   */
    /* ⭐ The device's OWN name, from Remote_Name_Request via the driver's names
     * file. Empty when we have none, in which case the class-of-device label is
     * used instead -- a real name always beats a generic one. */
    unsigned char devName[kMaxDev][kNameChars];
    short         devPct[kMaxDev];    /* 0..100, or kNoReading                     */
    unsigned long devWhen[kMaxDev];   /* TickCount of the reading; age only        */

    /* Selection is stored by ADDRESS, never by index: the device list is rebuilt from
     * two files every few seconds and an index silently comes to mean a different
     * device the moment one is added or removed. The panel has been bitten by exactly
     * that (kQuitItem, twice). */
    unsigned long selHi, selLo;
    Boolean       haveSel;

    /* ⭐ "Hide Battery Level", as Battery Monitor offers. The cell collapses to just the
     * icon, so this CHANGES THE CELL WIDTH -- see CellWidth and the sdevResizeDisplay
     * return from the click. Persisted, because a display preference the user has to
     * re-set every boot is not a preference. */
    Boolean       hideLevel;

    /* ⚠ The module's own icon family, detached at init and OWNED by us -- so
     * sdevCloseModule must DisposeIconSuite it before freeing this block. NULL is a
     * supported state: every draw path falls back to text. */
    Handle        iconSuite;

    /* self-draw cache -- see design note 2 */
    GrafPtr       cachePort;
    Rect          cacheRect;
    Boolean       haveCache;
    short         shownPct;           /* last value actually drawn */
    short         shownState;
    unsigned long lastRefresh;
} BTCsmState;

#define kRefreshTicks  300            /* 5s: a battery does not move faster, and this
                                       * is a file open + a bus walk, not a register */
#define kLowPct        20             /* at or below this the gauge fill goes red */

/* ---- freshness ----------------------------------------------------------- */

/* ⭐ WAS THIS FILE WRITTEN DURING THE SESSION WE ARE IN? See the header's second block.
 *
 * Boot time is (now - uptime). TickCount() is ticks since boot, nominally 60/sec.
 *
 * ⚠ THE TICK RATE IS NOMINAL, NOT EXACT (the Ticks global runs ~60.15Hz on this
 * hardware), so the estimate drifts by roughly 0.25% of uptime -- about a minute after
 * ten hours awake. That is harmless HERE and the reason is worth stating rather than
 * leaving to be rediscovered: the thing being distinguished is "this session" from "a
 * previous session", and those are separated by a shutdown plus a boot. The natural gap
 * is minutes at minimum, far wider than the drift. It would NOT be safe to reuse this
 * to compare two events within one session. */
static Boolean FileWrittenThisBoot(const FSSpec *spec)
{
    CInfoPBRec    pb;
    Str255        nm;
    unsigned long now, bootAt, upSecs;

    BlockMoveData(spec->name, nm, (Size)(spec->name[0] + 1));
    memset(&pb, 0, sizeof(pb));
    pb.hFileInfo.ioNamePtr   = nm;
    pb.hFileInfo.ioVRefNum   = spec->vRefNum;
    pb.hFileInfo.ioDirID     = spec->parID;
    pb.hFileInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr) return false;

    GetDateTime(&now);
    upSecs = (unsigned long)(TickCount() / 60);
    if (upSecs >= now) return false;          /* clock not set: cannot judge, so do not */
    bootAt = now - upSecs;

    /* +2s of margin for the clock's one-second granularity at both ends. */
    return (Boolean)(pb.hFileInfo.ioFlMdDat + 2 >= bootAt);
}

/* ---- device list --------------------------------------------------------- */

static short FindDev(BTCsmState *st, unsigned long hi, unsigned long lo)
{
    short i;
    for (i = 0; i < st->devCount; i++)
        if (st->devHi[i] == hi && st->devLo[i] == lo) return i;
    return -1;
}

static short AddDev(BTCsmState *st, unsigned long hi, unsigned long lo)
{
    short i = FindDev(st, hi, lo);
    if (i >= 0) return i;
    if (st->devCount >= kMaxDev) return -1;
    i = st->devCount++;
    st->devHi[i]  = hi;
    st->devLo[i]  = lo;
    st->devCod[i] = 0;
    st->devPct[i] = kNoReading;
    st->devWhen[i] = 0;
    st->devName[i][0] = 0;
    return i;
}

/* The devices the control panel has learned. This is the PRIMARY list and it sets the
 * menu order, because it is stable across boots -- the battery file only contains
 * whoever happened to answer this session, so ordering by it would shuffle the menu
 * under the user. Failure is silent: we simply list nothing and still show state. */
/* ⭐ CAN WE ACTUALLY DRIVE THIS? Peripheral major class AND a keyboard (0x40) or
 * pointing-device (0x80) minor bit. A peripheral with neither is a joystick, gamepad or
 * digitizer -- no decoder in this product handles those. The full rationale is beside
 * KindName, which decodes the same bits for display.
 *
 * ⚠ Defined HERE, above LoadDevices, because that is what calls it. Placing it next to
 * KindName read better and did not compile. */
static Boolean KindIsDrivable(unsigned long cod)
{
    if (((cod >> 8) & 0x1F) != 5) return false;      /* not a Peripheral */
    return (Boolean)((cod & 0xC0UL) != 0);           /* keyboard and/or pointer */
}

static void LoadDevices(BTCsmState *st)
{
    FSSpec        spec;
    short         vRefNum, ref;
    long          dirID, n;
    unsigned long hdr[3];
    short         i, count;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Kinds", &spec) != noErr) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;

    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) { FSClose(ref); return; }
    /* ⚠ BOTH magic and format version, because a future format read as this one would
     * list garbage. The panel validates the same pair on the way in. */
    if (hdr[0] != kCodMagic || hdr[1] != kCodFmtVer) { FSClose(ref); return; }
    count = (short)hdr[2];
    if (count < 0) count = 0;
    /* ⚠ Bound on the WRITE index, not the stored count. A hand-reasoned bound is this
     * project's most repeated mistake -- 304 typed against a 313-word block. */
    for (i = 0; i < count && st->devCount < kMaxDev; i++) {
        unsigned long rec[3];
        short         k;
        n = (long)sizeof(rec);
        if (FSRead(ref, &n, rec) != noErr || n != (long)sizeof(rec)) break;
        if (rec[2] == 0) continue;                /* no class of device: nothing to say */
        /* ⭐ Only devices we can drive. See KindIsDrivable: the file may still hold
         * phones and audio devices from before the scanner learned to ignore them, and
         * a menu line saying "Phone - not reporting" promises a reading that can never
         * come. Skipped silently -- there is nothing useful to tell the user about a
         * device this product does not support. */
        if (!KindIsDrivable(rec[2])) continue;
        k = AddDev(st, rec[0], rec[1]);
        if (k >= 0) st->devCod[k] = rec[2];
    }
    FSClose(ref);
}

/* The levels. ⚠ Read ONLY when the file is from this boot -- see the header. A device
 * that answered this session but is not in the panel's list is still added, so a level
 * is never dropped just because the panel has not learned the device's kind yet. */
static void LoadBattery(BTCsmState *st)
{
    FSSpec        spec;
    short         vRefNum, ref;
    long          dirID, n;
    unsigned long hdr[3];
    short         i, count;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Battery", &spec) != noErr) return;
    if (!FileWrittenThisBoot(&spec)) return;      /* previous session: show nothing */
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;

    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) { FSClose(ref); return; }
    if (hdr[0] != kBatMagic || hdr[1] != kBatFmtVer) { FSClose(ref); return; }
    count = (short)hdr[2];
    if (count < 0) count = 0;
    for (i = 0; i < count && st->devCount < kMaxDev; i++) {
        unsigned long rec[4];            /* addrHi, addrLo, percent, whenTicks */
        short         k;
        n = (long)sizeof(rec);
        if (FSRead(ref, &n, rec) != noErr || n != (long)sizeof(rec)) break;
        /* ⚠ The writer already refuses anything above 100, but this is a separate
         * binary reading a file on disk and it validates for itself. A gauge that can
         * read 255% is worse than no gauge. */
        if (rec[2] > 100UL) continue;
        k = AddDev(st, rec[0], rec[1]);
        if (k >= 0) {
            st->devPct[k]  = (short)rec[2];
            st->devWhen[k] = rec[3];
        }
    }
    FSClose(ref);
}

/* ⭐ THE DEVICE'S OWN NAME, from the driver's names file. A real name ("Chris's
 * Keyboard") beats a class-of-device label ("Keyboard") whenever we have one, and with
 * two identical devices the generic label cannot tell them apart at all -- which is the
 * situation the A1015 creates.
 *
 * ⚠ NO FRESHNESS TEST HERE, and that is deliberate rather than an omission. Unlike a
 * battery level, a device's NAME does not go stale: last boot's name for an address is
 * still that address's name, and showing it is strictly better than showing "Keyboard".
 * The freshness rule exists because a stale LEVEL is a lie about the present; a stale
 * name is not.
 *
 * ⚠ Records are fixed-size (2 longs + kNameChars) so the loop can be bounded. Names are
 * NUL-padded by the writer; this terminates defensively anyway, because a short read or
 * a foreign file must not hand DrawString an unterminated buffer. */
static void LoadNames(BTCsmState *st)
{
    FSSpec        spec;
    short         vRefNum, ref;
    long          dirID, n;
    unsigned long hdr[3];
    short         i, count;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Names", &spec) != noErr) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;

    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) { FSClose(ref); return; }
    if (hdr[0] != kNmMagic || hdr[1] != kNmFmtVer) { FSClose(ref); return; }
    count = (short)hdr[2];
    if (count < 0) count = 0;
    for (i = 0; i < count; i++) {
        unsigned long rec[2];
        unsigned char nm[kNameChars];
        short k;
        n = (long)sizeof(rec);
        if (FSRead(ref, &n, rec) != noErr || n != (long)sizeof(rec)) break;
        n = (long)kNameChars;
        if (FSRead(ref, &n, nm) != noErr || n != (long)kNameChars) break;
        nm[kNameChars - 1] = 0;
        /* ⚠ Only names devices we already know about -- a name alone is not a reason to
         * list a device. The kinds file decides WHO is listed (and filters what we
         * cannot drive); this only decides what they are CALLED. */
        k = FindDev(st, rec[0], rec[1]);
        if (k >= 0) {
            short j;
            for (j = 0; j < kNameChars; j++) st->devName[k][j] = nm[j];
        }
    }
    FSClose(ref);
}

/* ⚠⚠ A FOURTH FILE, and it OVERRIDES the third. `Preferences:Bluetooth Nicknames` is
 * written by the PANEL when the user renames a device, and the driver never touches it
 * -- which is exactly why it is a separate file. The driver refreshes Device Names from
 * whatever the device calls itself every time a name request answers, so a nickname
 * stored there would be silently replaced by the manufacturer's name.
 *
 * ⚠ LOADED AFTER LoadNames, because it must win. Same 3+3 join as everything else.
 * The panel drops the record when the user clears the field, so an empty name here is
 * a corrupt file rather than "no nickname" -- it is skipped either way. */
#define kAlMagic  0x4254414CUL        /* 'BTAL' -- panel WRITES, we READ */
#define kAlFmtVer 1UL

static void LoadNicknames(BTCsmState *st)
{
    FSSpec        spec;
    short         vRefNum, ref;
    long          dirID, n;
    unsigned long hdr[3];
    short         i, count;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Nicknames", &spec) != noErr) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;

    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) { FSClose(ref); return; }
    if (hdr[0] != kAlMagic || hdr[1] != kAlFmtVer) { FSClose(ref); return; }
    count = (short)hdr[2];
    if (count < 0) count = 0;
    for (i = 0; i < count; i++) {
        unsigned long rec[2];
        unsigned char nm[kNameChars];
        short k;
        n = (long)sizeof(rec);
        if (FSRead(ref, &n, rec) != noErr || n != (long)sizeof(rec)) break;
        n = (long)kNameChars;
        if (FSRead(ref, &n, nm) != noErr || n != (long)kNameChars) break;
        nm[kNameChars - 1] = 0;
        if (nm[0] == 0) continue;
        k = FindDev(st, rec[0], rec[1]);
        if (k >= 0) {
            short j;
            for (j = 0; j < kNameChars; j++) st->devName[k][j] = nm[j];
        }
    }
    FSClose(ref);
}

/* ---- state discovery ----------------------------------------------------- */

/* Which personality is on the bus? See design note 3.
 *
 * ⚠ Bounded and error-tolerant, for the reason BTCheck's bus dump had to be: an
 * enumeration that stops at the first hiccup silently truncates, and how much of the
 * bus you see then depends on where the hiccup falls. Keep going, bounded. */
static short ScanBusState(void)
{
    USBDeviceRef      ref = 0;
    CFragConnectionID connID;
    short             n = 0, stumbles = 0, found = kStAbsent;

    for (;;) {
        USBDeviceDescriptor d;
        OSStatus err = USBGetNextDeviceByClass(&ref, &connID, kUSBAnyClass,
                                               kUSBAnySubClass, kUSBAnyProtocol);
        if (err != noErr) {
            if (++stumbles > 8) break;
            continue;
        }
        stumbles = 0;
        if (++n > 24) break;
        memset(&d, 0, sizeof(d));
        if (USBGetDeviceDescriptor(&ref, &d, (UInt32)sizeof(d)) != noErr) continue;
        {
            /* ⚠ Byte-swapped: the descriptor fields are little-endian on the wire and
             * this is a big-endian machine. Same helper the panel and BTCheck use. */
            UInt16 vid = (UInt16)(((d.vendor  & 0x00FFu) << 8) | ((d.vendor  >> 8) & 0xFFu));
            UInt16 pid = (UInt16)(((d.product & 0x00FFu) << 8) | ((d.product >> 8) & 0xFFu));
            if (vid == kA1044Vendor && pid == kA1044HCI)   return kStHCI;
            /* ⭐ Any standard HCI controller counts as "our stack can own this" -- a USB
             * dongle on a Mac with no internal module port is the whole point. Checked
             * BEFORE the proxy test so a dongle is never mistaken for a half state. */
            if (d.deviceClass    == kBTClass
                && d.deviceSubClass == kBTSubClass
                && d.protocol       == kBTProto)          return kStHCI;
            if (vid == kA1044Vendor && pid == kA1044Proxy) found = kStProxy;
        }
    }
    /* ⚠ If nothing enumerated at all we cannot tell absent from unreadable, and those
     * are different facts. Say unknown rather than claiming there is no hardware. */
    if (n == 0) return kStUnknown;
    return found;
}

static void Refresh(BTCsmState *st)
{
    if (st == NULL) return;

    /* The bus scan touches no disk, so state is always current. */
    st->state = ScanBusState();

    /* ⚠⚠ SBSafeToAccessStartupDisk BEFORE ANY FILE MANAGER CALL -- the strip tickles us
     * in contexts where touching the startup disk is not safe, and this is the
     * documented way to ask.
     *
     * ⚠ AND WHEN THE ANSWER IS NO, KEEP WHAT WE HAVE. Clearing the list first and
     * repopulating it only on success would blank the gauge to "BT" for every tickle
     * that lands in an unsafe moment and restore it on the next one -- a visible
     * flicker caused entirely by the refresh, not by anything changing. The previous
     * contents were validated when they were read and do not go stale within a
     * session; not re-reading them is not a reason to throw them away. */
    if (!SBSafeToAccessStartupDisk()) return;

    st->devCount = 0;
    LoadDevices(st);
    /* ⭐ THE GAUGE IS GATED ON OUR STACK OWNING THE CARD. In proxy mode Mac OS is
     * driving the keyboard and nothing of ours is asking for a level, so whatever is in
     * the file is not about right now. See the header's first block. */
    if (st->state == kStHCI) LoadBattery(st);
    /* ⚠ LAST, because it only annotates devices the two loads above have already
     * added -- a name is not a reason to list a device. */
    LoadNames(st);
    /* ⚠ AFTER LoadNames, never before: the user's own name must win over the one
     * the device supplies, and this simply overwrites it. */
    LoadNicknames(st);
}

/* ---- selection ----------------------------------------------------------- */

/* The index the cell is showing. Falls back rather than showing nothing: the persisted
 * address may name a device that is not present this session. */
static short SelectedIndex(BTCsmState *st)
{
    short i;
    if (st == NULL || st->devCount == 0) return -1;
    if (st->haveSel) {
        i = FindDev(st, st->selHi, st->selLo);
        if (i >= 0) return i;
    }
    for (i = 0; i < st->devCount; i++)          /* prefer one that has a level */
        if (st->devPct[i] != kNoReading) return i;
    return 0;
}

static short CurrentPct(BTCsmState *st)
{
    short i;
    if (st == NULL || st->state != kStHCI) return kNoReading;
    i = SelectedIndex(st);
    if (i < 0) return kNoReading;
    return st->devPct[i];
}

/* ⭐ PERSISTED WITH SBSavePreferences, NOT the sdevSaveSettings return value -- that
 * long is kept for the SESSION only and is not written to disk, which is how the
 * CPU-temp module lost its C/F setting on every reboot. Nine bytes: a presence flag
 * then the eight-byte address. */
static void LoadPrefs(BTCsmState *st)
{
    Handle h = NULL;
    if (SBLoadPreferences("\pBluetooth CSM Settings", &h) == noErr
        && h != NULL && GetHandleSize(h) >= 9) {
        unsigned char *p = (unsigned char *)*h;
        if (p[0] == 1) {
            st->selHi = ((unsigned long)p[1] << 24) | ((unsigned long)p[2] << 16)
                      | ((unsigned long)p[3] << 8)  |  (unsigned long)p[4];
            st->selLo = ((unsigned long)p[5] << 24) | ((unsigned long)p[6] << 16)
                      | ((unsigned long)p[7] << 8)  |  (unsigned long)p[8];
            st->haveSel = true;
        }
        /* ⚠ THE 10th BYTE IS CHECKED SEPARATELY, not folded into the >= 9 test above.
         * 1.5 and earlier wrote a NINE byte blob, and those are still on disk in every
         * existing install -- reading byte 9 out of one would read past the handle.
         * Absent means "do not hide", which is the previous behaviour. */
        if (GetHandleSize(h) >= 10)
            st->hideLevel = (Boolean)(p[9] != 0);
    }
    if (h != NULL) DisposeHandle(h);
}

static void SavePrefs(BTCsmState *st)
{
    Handle h = NewHandle(10);
    if (h != NULL) {
        unsigned char *p = (unsigned char *)*h;
        p[9] = (unsigned char)(st->hideLevel ? 1 : 0);
        p[0] = (unsigned char)(st->haveSel ? 1 : 0);
        p[1] = (unsigned char)((st->selHi >> 24) & 0xFF);
        p[2] = (unsigned char)((st->selHi >> 16) & 0xFF);
        p[3] = (unsigned char)((st->selHi >> 8)  & 0xFF);
        p[4] = (unsigned char)( st->selHi        & 0xFF);
        p[5] = (unsigned char)((st->selLo >> 24) & 0xFF);
        p[6] = (unsigned char)((st->selLo >> 16) & 0xFF);
        p[7] = (unsigned char)((st->selLo >> 8)  & 0xFF);
        p[8] = (unsigned char)( st->selLo        & 0xFF);
        SBSavePreferences("\pBluetooth CSM Settings", h);
        DisposeHandle(h);
    }
}

/* ---- naming -------------------------------------------------------------- */

/* Major device class is bits 8..12 of the 24-bit CoD. Spelled out rather than shared,
 * because this is a separately built binary -- same reasoning as the panel's copy. */
/* (KindIsDrivable is defined above LoadDevices, which calls it.)
 *
 * ⭐ CAN WE ACTUALLY DRIVE THIS? Peripheral major class AND a keyboard (0x40) or
 * pointing-device (0x80) minor bit. A peripheral with neither is a joystick, gamepad or
 * digitizer -- no decoder here handles those.
 *
 * ⚠⚠ THE CSM NEEDS ITS OWN COPY OF THIS TEST, and 1.9 shipped without one. Driver 14.6
 * added the same filter to the SCANNER, which stops new undrivable devices entering a
 * scan -- but `Preferences:Bluetooth Device Kinds` already held phones and audio devices
 * learned in earlier sessions, and this module lists whatever is in that file. So the
 * cell's menu still read "Phone - not reporting" and "Audio device - not reporting", for
 * devices that will NEVER report a battery level. Filtering at the point of DISCOVERY
 * does not clean up what discovery already wrote down.
 *
 * ⚠ Applied in LoadDevices ONLY, never to the battery file. A device that actually
 * reported a level is drivable BY DEMONSTRATION, whatever its stored class says -- and if
 * those two ever disagree the reading is the better evidence. LoadBattery therefore still
 * adds anything that answered. */
static const char *KindName(unsigned long cod)
{
    if (cod == 0) return "Bluetooth device";     /* kind not learned yet */
    switch ((cod >> 8) & 0x1F) {
    case 1: return "Computer";
    case 2: return "Phone";
    case 3: return "Network";
    case 4: return "Audio device";
    case 5: {
        /* Peripheral minor class: 0x40 keyboard, 0x80 pointer, 0xC0 both. */
        unsigned long minor = cod & 0xC0;
        if (minor == 0x40) return "Keyboard";
        if (minor == 0x80) return "Mouse";
        if (minor == 0xC0) return "Keyboard and mouse";
        return "Peripheral";
    }
    case 6: return "Imaging device";
    default: return "Bluetooth device";
    }
}

/* ⭐ ONE FACT PER LINE, and this used to fold two together. It returned "Bluetooth is on
 * - checking" whenever nothing had a reading yet, which covered two different situations
 * with one phrase: nothing is paired, and something is paired but has not answered. The
 * device lines below already distinguish those -- a paired device says "- not reporting",
 * and an empty list now says so explicitly -- so the status line no longer needs to
 * guess which case it is in, and does not.
 *
 * ⚠ THE CELL DELIBERATELY DOES NOT CARRY THIS. "no device paired" is ~96px of text
 * against a 95px cell, so putting it there would double the cell's width in the state
 * where the module has the least to say -- and it would be WRONG in most of the states
 * that produce an empty gauge (asleep, out of range, not yet answered, stale file, or
 * the other device selected). The cell stays terse and honest; the explanation lives
 * here, where there is room and where the answer is actually known. */
static const char *StateLine(short state)
{
    switch (state) {
    case kStProxy:  return "Handled by Mac OS - no battery level";
    case kStHCI:    return "Bluetooth is on";
    case kStAbsent: return "No Bluetooth hardware";
    default:        return "Status unavailable";
    }
}

/* ---- small string helpers ------------------------------------------------ */

static void PStrAppendC(Str255 s, const char *p)
{
    while (*p && s[0] < 200) s[++s[0]] = (unsigned char)*p++;
}

static void PStrAppendNum(Str255 s, long v)
{
    Str255 n;
    short  i;
    NumToString(v, n);
    for (i = 1; i <= n[0] && s[0] < 200; i++) s[++s[0]] = n[i];
}

/* ---- drawing ------------------------------------------------------------- */

/* ⭐ ALL MEASURED FROM A PICT OF APPLE'S BATTERY MONITOR CELL, by counting dark pixels
 * per column across its 93x22 cell -- see DrawBarGraph. Icon x0..16, divider x21, graph
 * x26..x79 as eight 5px segments 2px apart, 13px tall. The divider sits midway between
 * the icon and the graph in that cell, which is where kDivGap puts it. */
#define kBars     8
#define kBarW     5               /* one segment, outline included */
#define kBarGap   2
#define kBarH    13
#define kGraphW  (kBars * kBarW + (kBars - 1) * kBarGap)    /* 54 */
#define kDivGap   5               /* icon -> divider -> graph */
#define kDivH    13
#define kGap      3
#define kCellPad  6

/* ⭐ THE POPUP TRIANGLE, measured off Apple's cell like everything else here: x84..87,
 * y8..15, so 4 wide and 8 tall, a vertical LEFT edge tapering to a point on the RIGHT.
 * Pure #000000 -- NOT the #404040 the segment outlines use, which is worth stating
 * because using the outline grey here would have looked like a mistake nobody could name.
 *
 * ⚠ NOT the same triangle as cpu-temp/csm's DrawArrow, which points DOWN and is 5x3. That
 * one is a perfectly good popup marker; it is simply not the one on screen beside this
 * module, and "consistent with Battery Monitor" is the whole point of the request. */
#define kArrowW   4
#define kArrowH   8
#define kArrowGap 4               /* graph x79 -> arrow x84 in Apple's cell */

/* ⭐ THE BLUETOOTH ICON SITS LEFT OF THE GAUGE, and the reason is identification rather
 * than decoration: Apple's Battery Monitor puts a battery icon left of ITS gauge, and on
 * a machine running both modules two bare gauges side by side would not say which one is
 * the Bluetooth device's. Requested 2026-09-21 with a screenshot of that module's cell
 * (93x22: icon, a thin divider, an 8-segment bar graph, then the popup arrow).
 *
 * ⭐ ID 128 is the CELL family in icons/bt_csm_cell_icon.r, SEPARATE from the file's
 * Finder icon at -16455. 1.2-1.6 shared one family for both; the user supplied dedicated
 * cell artwork and the two jobs really are different -- -16455 is a 32x32-first document
 * icon seen in a folder, 128 is a 16x16 glyph read at a glance in a 22px strip. The
 * source was 10x16 with four colours and fully binary alpha, so it is baked verbatim
 * (centred, never resampled) by scripts/cell-icon-to-r.py. PlotIconSuite picks the ics
 * members for a destination rect this size.
 *
 * ⚠ CMake checks this number against the ID in the .r file: SBGetDetachIconSuite takes a
 * NUMBER, and if the two drift the suite comes back NULL, the cell silently falls back to
 * a text label, and it looks like a design choice rather than a broken reference.
 *
 * ⓘ PRIOR ART READ, NOT GUESSED (the user put Battery Monitor on the share). Its PEF
 * imports SBGetDetachIconSuite + PlotIconSuite + DisposeIconSuite -- the recipe used
 * below -- plus SBLoadPreferences/SBSavePreferences, which is how this module already
 * persists its device selection.
 *
 * ⏭ IT ALSO IMPORTS SBDrawBarGraph + SBGetBarGraphWidth, which is ControlStripLib's OWN
 * segmented bar graph and is what draws those segments. This module does NOT use it yet,
 * deliberately: SBDrawBarGraph(level, barCount, direction, topLeft) does not document
 * whether `level` is a filled-BAR count (0..barCount) or a PERCENTAGE (0..100), and the
 * two differ at every value except the endpoints. Apple's own GetScaledBatteryInfo hints
 * at the former but that is an inference from a symbol name, and a wrong guess is a gauge
 * that silently misreports. Pinning it down needs a PPC disassembly this machine has no
 * working tool for (Apple's objdump has PowerPC stripped; Retro68's binutils has no ELF
 * target). The hand-drawn gauge below is verified across all of 0..100 instead. Worth
 * revisiting -- it would be more idiomatic -- but not on a coin flip. */
#define kIconSize 16
#define kIconID   128

static void SetupStripFont(GrafPtr port)
{
    short fontID = 0, fontSize = 0;
    if (port != NULL) SetPort(port);
    if (SBGetControlStripFontID(&fontID) == noErr) TextFont(fontID);
    if (SBGetControlStripFontSize(&fontSize) == noErr) TextSize(fontSize);
    TextFace(0);
}

/* ★★★★ CARRIED OVER FROM cpu-temp/csm v1.8 -- NEVER DRAW THROUGH THE CACHED PORT
 * WITHOUT PROVING IT IS STILL A PORT.
 *
 * The tickle self-repaint SetPorts into a GrafPtr cached from a PAST sdevDrawStatus.
 * Nothing guarantees that port still exists: if the Control Strip rebuilds or disposes
 * its window/offscreen while the module stays loaded, the cached pointer dangles, and
 * EraseRect/DrawString then make QuickDraw chase a freed port's PixMap chain -- reading
 * garbage and WRITING pixel data through it. That is a silent SYSTEM-HEAP scribbler:
 * the damage surfaces much later in whoever touches the corrupted blocks (observed on a
 * G4 MDD as a bus error in _BitsToPix with a wild PixMap pointer, and MacsBug reporting
 * the system heap's free list bad). The window opens on desktop churn -- every volume
 * mount/eject repaints the strip.
 *
 * The defence is layered, because a heap-validity oracle does not exist on OS 9:
 *   1. structural checks before every self-draw (below);
 *   2. any failure CLEARS the cache, so self-drawing stops until the next REAL
 *      sdevDrawStatus re-arms it with a fresh port;
 *   3. the cache is refreshed on every sdevDrawStatus. */
static Boolean PortLooksAlive(GrafPtr p, const Rect *cell)
{
    UInt32 a = (UInt32)p, h, m, base;
    SInt16 ver, rb;
    volatile SInt16 *pr;
    if (a < 0x1000UL || a >= 0x60000000UL || (a & 1)) return false;
    ver = *(volatile SInt16 *)((char *)p + 6);      /* portVersion: CGrafPort sets both top bits */
    if ((ver & (SInt16)0xC000) != (SInt16)0xC000) return false;
    h = *(volatile UInt32 *)((char *)p + 2);        /* portPixMap (a Handle) */
    if (h < 0x1000UL || h >= 0x60000000UL || (h & 3)) return false;
    m = *(volatile UInt32 *)h;                      /* master pointer -> PixMap */
    if (m < 0x1000UL || m >= 0x60000000UL || (m & 1)) return false;
    rb = *(volatile SInt16 *)((char *)m + 4);       /* PixMap rowBytes: flag bit must be set */
    if (!(rb & (SInt16)0x8000)) return false;
    base = *(volatile UInt32 *)m;                   /* baseAddr */
    if (base < 0x1000UL) return false;
    pr = (volatile SInt16 *)((char *)p + 16);       /* portRect {top,left,bottom,right} */
    if (cell->top < pr[0] || cell->left < pr[1] || cell->bottom > pr[2] || cell->right > pr[3])
        return false;
    return true;
}

/* ⭐ A SEGMENTED BAR GRAPH, MEASURED OFF APPLE'S OWN CELL RATHER THAN EYEBALLED.
 *
 * 1.3 drew a solid-fill battery glyph, and the user's note was simply that it does not
 * look like Battery Monitor. It does not, so this replaces it.
 *
 * The geometry is taken from a PICT of Battery Monitor's strip cell by counting dark
 * pixels per column (93x22 cell): icon at x0..16, a divider line at x21, then the graph
 * from x26 to x79 -- EIGHT segments, each 5px wide including its outline, 2px apart,
 * 13px tall. 8*5 + 7*2 = 54, so SBGetBarGraphWidth(8) returns 54 and the pitch is 7.
 * Those are the constants below; nothing here is a guess about how it looks.
 *
 * ⚠ DRAWN BY HAND RATHER THAN WITH SBDrawBarGraph, and not for want of trying. Apple's
 * module does use ControlStripLib's own SBDrawBarGraph(level, barCount, direction,
 * topLeft), but `level`'s range is undocumented -- filled-BAR count (0..barCount) or
 * PERCENTAGE (0..100)? Those differ at every value but the endpoints, so a wrong guess
 * is a gauge that silently misreports, which is the one thing this feature must not do.
 * I disassembled Battery Monitor to settle it (capstone via pip; the PEF's 47 imports
 * each reached through a bl->glue-stub->bctr, TOC slot -0x7c identified as PlotIconSuite
 * from its argument shape) and the answer is not visible at the call site: Apple computes
 * barCount at RUNTIME from the available width, so no constant appears. ⇒ Recorded as an
 * open question instead of guessed at. The arithmetic below is verified across 0..100. */
static void DrawBarGraph(short x, short y, short pct)
{
    Rect     r;
    RGBColor save, fill, edge;
    short    i, lit;

    /* Segments lit, rounded to nearest. ⚠ A nonzero level must never light ZERO
     * segments -- an all-empty graph reads as flat when the battery is merely low, and
     * low is exactly when someone is looking. Rounding alone first lights a segment at
     * 7%, so the floor covers 1..6%. The printed number carries the precision; eight
     * segments cannot distinguish 99 from 100 and are not asked to. */
    lit = (short)(((long)pct * (long)kBars + 50L) / 100L);
    if (lit < 0)     lit = 0;
    if (lit > kBars) lit = kBars;
    if (lit == 0 && pct > 0) lit = 1;

    /* ⭐ THE OUTLINE GREY IS MEASURED, NOT CHOSEN: Apple's cell uses #404040 for the
     * segment outlines on the strip's #C0C0C0 face, never pure black. 1.4 drew pure
     * #000000 and read as starker than every module beside it. */
    edge.red = edge.green = edge.blue = 0x4040;

    /* ⚠ THE FILL COLOUR IS A JUDGEMENT CALL, AND THE EVIDENCE COULD NOT MAKE IT. The
     * user's screenshot of Battery Monitor is entirely greyscale in the graph region
     * (verified by counting distinct colours: #C0C0C0/#FFFFFF/#404040/#000000/#A0A0A0,
     * the only hue in the whole cell being 12 pixels of #9999FF inside its ICON) --
     * because that MDD has no battery, so every segment there is EMPTY. A charged
     * reference would have settled it; none was available, and Apple loads its RGBColors
     * from its data section rather than building them from immediates, so the
     * disassembly did not yield them either. Asked, and the user had no preference.
     *
     * ⇒ Green, red at or below kLowPct: the universal battery convention, readable
     * without parsing the digits, and it carries real information rather than decoration.
     * Both are exact members of the Mac 8-bit palette cube (levels 255/204/153/102/51/0
     * per channel), so they do not dither on a 256-colour display. */
    if (pct <= kLowPct) { fill.red = 0xFFFF; fill.green = 0x0000; fill.blue = 0x0000; }
    else                { fill.red = 0x0000; fill.green = 0x9999; fill.blue = 0x0000; }

    GetForeColor(&save);
    for (i = 0; i < kBars; i++) {
        short bx = (short)(x + i * (kBarW + kBarGap));
        SetRect(&r, bx, y, (short)(bx + kBarW), (short)(y + kBarH));
        /* ⭐ EVERY segment keeps its outline and only the INTERIOR fills, which is what
         * Apple's cell does -- all eight boxes stay visible, so the graph reads as a
         * scale with a level on it rather than a bar of unknown length. 1.4 painted lit
         * segments solid with no outline, which lost that. The 1px inset leaves a 3x11
         * interior, matching the measured geometry. */
        RGBForeColor(&edge);
        FrameRect(&r);
        if (i < lit) {
            Rect ir = r;
            InsetRect(&ir, 1, 1);
            RGBForeColor(&fill);
            PaintRect(&ir);
        }
    }
    RGBForeColor(&save);
}

/* The "this module has a menu" marker. Row widths 1,2,3,4,4,3,2,1 reproduce the measured
 * shape exactly rather than approximating a triangle. */
static void DrawArrow(short x, short y)
{
    RGBColor save, blk;
    short    i, w;

    blk.red = blk.green = blk.blue = 0;
    GetForeColor(&save);
    RGBForeColor(&blk);
    for (i = 0; i < kArrowH; i++) {
        w = (i < kArrowH / 2) ? (short)(i + 1) : (short)(kArrowH - i);
        MoveTo(x, (short)(y + i));
        Line((short)(w - 1), 0);
    }
    RGBForeColor(&save);
}

/* Draw into rect `r` of the CURRENT port. `erase` TRUE when we are repainting ourselves
 * during the periodic tickle -- the strip has not cleared the cell for us, unlike a
 * normal sdevDrawStatus. */
static void DrawCell(BTCsmState *st, const Rect *r, Boolean erase)
{
    Str255   s;
    FontInfo fi;
    short    cellW, cellH, textW, total, x, baseline, pct;
    Boolean  gauge, icon;

    SetupStripFont(NULL);                 /* port already set by the caller */
    pct   = CurrentPct(st);
    gauge = (Boolean)(pct != kNoReading);
    icon  = (Boolean)(st != NULL && st->iconSuite != NULL);

    /* ⭐ "Hide Battery Level" suppresses the graph AND the number, leaving the icon as a
     * plain Bluetooth status indicator. The level is still in the menu -- hiding is
     * about the strip cell, not about withholding the reading. */
    if (st != NULL && st->hideLevel) gauge = false;

    /* ⭐ "N/A" WHEN THERE IS NOTHING TO READ, not "BT" and not an empty gauge. Requested
     * 2026-09-21, and it is the honest form: an empty gauge outline is a MEASUREMENT of
     * zero, which is a different claim from having no measurement -- and it would read as
     * a flat battery, the one reading a user must never be shown wrongly. So when there
     * is no level the gauge is not drawn at all; the icon still identifies the module. */
    /* ⭐ NO PERCENTAGE IN THE CELL, as of 2.1. It made the cell ~104px against Battery
     * Monitor's 93, and the number is already one click away in the menu -- where it sits
     * beside the device NAME, which the cell has no room to show anyway. One place for it
     * is enough, and the gauge carries the at-a-glance reading.
     *
     * ⚠ "N/A" STAYS, because it is not a percentage -- it is the only thing that says
     * there is nothing to read. Removing it too would leave that state indistinguishable
     * from a full gauge at a glance, and an EMPTY gauge is not available as a substitute:
     * an empty gauge is a measurement of zero, which is a flat battery, which is a
     * different claim. So: a reading draws the graph, no reading draws "N/A". */
    s[0] = 0;
    if (!gauge) {
        if (st != NULL && st->hideLevel) {
            /* Hidden: the icon IS the whole cell -- unless the icon is missing, in which
             * case something must be drawn or the cell renders blank. */
            if (!icon) PStrAppendC(s, "BT");
        } else {
            PStrAppendC(s, "N/A");
        }
    }

    GetFontInfo(&fi);
    cellW = (short)(r->right - r->left);
    cellH = (short)(r->bottom - r->top);
    textW = StringWidth(s);

    /* Width of what we are ACTUALLY drawing, so it centres in the fixed cell whether or
     * not the icon loaded and whether or not there is a level. The divider only earns
     * its pixels when there is both an icon to its left and a graph to its right --
     * it is a separator, and a separator with one side is just a stray line. */
    /* ⚠⚠ THIS MUST MIRROR THE DRAW SEQUENCE BELOW TERM FOR TERM, and an earlier version
     * did not: it subtracted a kGap to compensate for one the draw path never consumed,
     * so the whole row sat 3px left of centre whenever the graph was showing. A layout
     * total and a draw walk that disagree are two descriptions of one thing, and the
     * cheapest way to keep them honest is to write them in the same order with the same
     * conditions -- which is what these two blocks now do. Gaps belong to the element
     * they PRECEDE. */
    total = 0;
    if (icon) {
        total = (short)(total + kIconSize);
        total = (short)(total + (gauge ? kDivGap : (textW > 0 ? kGap : 0)));
    }
    if (icon && gauge) total = (short)(total + 1 + kDivGap);      /* the divider */
    if (gauge)         total = (short)(total + kGraphW + kGap);
    total = (short)(total + textW);
    /* The arrow rides at the right in EVERY state -- the menu exists whether or not
     * there is a reading, and whether or not the level is hidden. */
    total = (short)(total + kArrowGap + kArrowW);
    x = (short)(r->left + (cellW - total) / 2);
    if (x < r->left) x = r->left;
    baseline = (short)(r->top + (cellH - (fi.ascent + fi.descent)) / 2 + fi.ascent);

    if (erase) EraseRect(r);              /* clear our own cell before repaint */

    if (icon) {
        Rect  ir;
        short iy = (short)(r->top + (cellH - kIconSize) / 2);
        SetRect(&ir, x, iy, (short)(x + kIconSize), (short)(iy + kIconSize));
        /* ⚠ Return value ignored ON PURPOSE and this is the one place that is right: a
         * failed plot costs a missing glyph, the text beside it still carries the reading,
         * and there is no recovery a strip module could usefully take mid-draw. */
        (void)PlotIconSuite(&ir, kAlignNone, kTransformNone, st->iconSuite);
        /* ⚠ Same expression as the total above, deliberately. With the level hidden there
         * is neither a graph nor text, so no gap is owed at all -- adding one would put
         * the icon off-centre in the one state where the icon IS the cell. */
        x = (short)(x + kIconSize + (gauge ? kDivGap : (s[0] > 0 ? kGap : 0)));
    }
    if (icon && gauge) {
        /* ⚠ The divider took whatever fore colour the port happened to carry, which was
         * pure black. Apple's is the same #404040 as the segment outlines -- it is the
         * same kind of mark, so it gets the same grey rather than inheriting one. */
        RGBColor dsave, dedge;
        short    dy = (short)(r->top + (cellH - kDivH) / 2);
        dedge.red = dedge.green = dedge.blue = 0x4040;
        GetForeColor(&dsave);
        RGBForeColor(&dedge);
        MoveTo(x, dy);
        Line(0, (short)(kDivH - 1));      /* the divider, as Apple's cell has */
        RGBForeColor(&dsave);
        x = (short)(x + 1 + kDivGap);
    }
    if (gauge) {
        DrawBarGraph(x, (short)(r->top + (cellH - kBarH) / 2), pct);
        x = (short)(x + kGraphW + kGap);
    }
    if (s[0] > 0) {
        MoveTo(x, baseline);
        DrawString(s);
        x = (short)(x + textW);
    }
    DrawArrow((short)(x + kArrowGap), (short)(r->top + (cellH - kArrowH) / 2));
}

/* ⭐ FIXED WIDTH, measured at the WIDEST content ("100%" plus the glyph) rather than at
 * what is showing. A width that tracked the content would make the strip re-lay-out
 * every time a level crossed 100 -> 99 or a device disconnected, shuffling every module
 * to its right; and the module would still need the self-draw path for the digits. One
 * stable width costs a few pixels when the cell reads "BT" and buys both. */
static short CellWidth(BTCsmState *st, GrafPtr port)
{
    Str255 s;
    SetupStripFont(port);

    /* ⭐ HIDDEN: just the icon. This is the ONE state in which the width legitimately
     * differs, and the click handler returns sdevResizeDisplay so the strip re-asks.
     * ⚠ With no icon suite there would be nothing at all to draw, so the fallback label
     * is measured instead -- an empty cell is not a display state, it is a bug. */
    if (st != NULL && st->hideLevel) {
        if (st->iconSuite != NULL)
            return (short)(kIconSize + kArrowGap + kArrowW + kCellPad);
        s[0] = 0; PStrAppendC(s, "BT");
        return (short)(StringWidth(s) + kArrowGap + kArrowW + kCellPad);
    }

    /* ⚠ Sized for the WIDEST content -- icon, divider, the 8-segment graph, the arrow --
     * and it reserves the icon's width unconditionally, even though a failed icon load
     * draws nothing there. The width must not depend on runtime state: the strip
     * re-lays-out every module to our right when it changes, and this one's whole design
     * is a stable cell with self-drawn contents (design note 2).
     *
     * ⭐ 2.1 DROPPED THE PERCENTAGE and this is where it pays: 16+5+1+5+54+4+4+6 = 95px
     * against 2.0's ~104 and Battery Monitor's 93.
     *
     * ⓘ No text is measured here any more, and that is now correct rather than an
     * oversight: the widest state is icon + divider + graph + arrow, and the only text
     * the cell can draw ("N/A", or "BT" if the icon failed) is far narrower than the
     * graph it replaces. Checked across all four states -- the widest comes to 92 inside
     * a 95px cell. */
    return (short)(kIconSize + kDivGap + 1 + kDivGap + kGraphW
                   + kArrowGap + kArrowW + kCellPad);
}

/* ---- the pair-mode flag -------------------------------------------------- */

/* ⭐ THE ONE ACTION WORTH A MENU-BAR SHORTCUT. Entering pairing mode is the only step
 * in the whole product that otherwise requires opening the control panel.
 *
 * ⚠ Creates the same zero-length file cpanel/bt_panel.c creates and src/bt_switch.c
 * consumes at its next Initialize. Its EXISTENCE is the whole message. */
static Boolean SetPairFlag(void)
{
    FSSpec spec;
    short  vRefNum, ref;
    long   dirID;
    OSErr  err;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return false;
    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Pair At Restart", &spec);
    if (err != noErr && err != fnfErr) return false;
    if (err == fnfErr && FSpCreate(&spec, 'BTcp', 'BTpf', smSystemScript) != noErr)
        return false;
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return false;
    (void)FSClose(ref);
    /* ⚠ Flushed, or a crash before the restart loses the very thing we promised. */
    (void)FlushVol(NULL, spec.vRefNum);
    return true;
}

/* ---- opening the control panel ------------------------------------------- */

/* ⚠ THE PANEL IS A REAL APPLICATION, type 'APPL' creator 'BTcp' -- cpanel/CMakeLists.txt
 * says why it is not a 68K 'cdev'. So it is launched, not opened as a cdev. */
#define kPanelCreator  'BTcp'

/* ⭐ FOUND BY SCANNING THE CONTROL PANELS FOLDER, not via the Desktop database. The UT
 * launcher CSM uses PBDTGetAPPL because a game can live anywhere, and it paid for that:
 * a stale Desktop DB returns a GHOST FSSpec and LaunchApplication then fails with
 * fnfErr(-43) on a file the lookup just claimed to have found. A control panel lives in
 * the Control Panels folder by definition, so asking the folder is both simpler and
 * cannot go stale.
 *
 * ⚠ Bounded. An unbounded catalog enumeration is a hang waiting for a corrupt directory,
 * and this runs in the Control Strip's context where a hang takes the whole strip. */
static Boolean FindPanel(FSSpec *out)
{
    CInfoPBRec pb;
    Str255     nm;
    short      vRefNum, i;
    long       dirID;

    if (FindFolder(kOnSystemDisk, kControlPanelFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return false;
    for (i = 1; i <= 512; i++) {
        memset(&pb, 0, sizeof(pb));
        nm[0] = 0;
        pb.hFileInfo.ioNamePtr   = nm;
        pb.hFileInfo.ioVRefNum   = vRefNum;
        pb.hFileInfo.ioDirID     = dirID;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;       /* end of directory */
        if (pb.hFileInfo.ioFlAttrib & ioDirMask) continue;
        if (pb.hFileInfo.ioFlFndrInfo.fdCreator == kPanelCreator
            && pb.hFileInfo.ioFlFndrInfo.fdType == 'APPL')
            return (Boolean)(FSMakeFSSpec(vRefNum, dirID, nm, out) == noErr);
    }
    return false;
}

/* ⭐ ALREADY RUNNING? BRING IT FORWARD INSTEAD OF LAUNCHING A SECOND COPY. Same
 * Process-Manager walk the UT launcher uses, and the reason is the same: launching an
 * app that is already open is at best a no-op and at worst two instances fighting over
 * one preferences file -- and this panel writes the pairing flag the switcher consumes. */
static Boolean PanelToFront(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    Str255              nm;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof(info));
        info.processInfoLength = (UInt32)sizeof(ProcessInfoRec);
        info.processName       = nm;
        info.processAppSpec    = NULL;
        if (GetProcessInformation(&psn, &info) == noErr
            && info.processSignature == kPanelCreator) {
            return (Boolean)(SetFrontProcess(&psn) == noErr);
        }
    }
    return false;
}

static void OpenPanel(const FSSpec *spec)
{
    LaunchParamBlockRec lpb;

    if (PanelToFront()) return;
    memset(&lpb, 0, sizeof(lpb));
    lpb.launchBlockID       = extendedBlock;
    lpb.launchEPBLength     = extendedBlockLen;
    lpb.launchFileFlags     = 0;
    lpb.launchControlFlags  = launchContinue | launchNoFileFlags;
    lpb.launchAppSpec       = (FSSpecPtr)spec;
    lpb.launchAppParameters = NULL;
    /* ⚠ No alert on failure, and that is a deliberate difference from the UT launcher.
     * That module needed one because its app could be anywhere and often was not there;
     * here the item is only ENABLED when FindPanel has already located the file, so the
     * common failure it would report cannot arise. A modal dialog from strip context
     * needs SBOpenModuleResourceFile + SBModalDialogInContext and its own DLOG/DITL --
     * real machinery, and not worth it for a case the enable check removes. */
    (void)LaunchApplication(&lpb);
}

/* ---- menu ---------------------------------------------------------------- */

#define kMenuID   1024

/* Item numbers are computed, never assumed: the device list is variable-length, and a
 * hardcoded index would silently point at the wrong action the moment a device is
 * added or removed. That exact bug (kQuitItem) bit the control panel twice. */
static void DoClick(BTCsmState *st, Rect *statusRect, Boolean *widthChanged)
{
    MenuHandle m;
    short      chosen, firstDev = 0, pairItem, hideItem, panelItem, stateItem, sel;
    Str255     s;
    Boolean    havePanel;
    FSSpec     panelSpec;
    short      i;

    if (widthChanged != NULL) *widthChanged = false;

    if (st != NULL) Refresh(st);

    m = NewMenu(kMenuID, "\pBluetooth");
    if (m == NULL) return;

    /* ⭐ OPEN THE CONTROL PANEL FIRST, IN ITS OWN SECTION. That is where Apple's own
     * modules put it, and the placement is not arbitrary: it is the one item that leaves
     * the strip entirely, so it belongs apart from the items that change what the strip
     * itself shows. Requested 2026-09-21 for consistency with the stock modules.
     *
     * ⚠ FindPanel runs BEFORE the menu is shown because the item's ENABLED state depends
     * on it -- this module's standing rule is to offer only what can actually work (see
     * the pairing item), which is also why there is no failure alert. */
    havePanel = FindPanel(&panelSpec);
    panelItem = 1;
    AppendMenu(m, "\pOpen Bluetooth Control Panel\311");
    if (!havePanel) DisableItem(m, panelItem);
    AppendMenu(m, "\p(-");

    /* The state, disabled -- information, not a control.
     * ⚠ Its index is COMPUTED now. It used to be item 1 and was written as the literal
     * 1 in two calls; moving the panel item above it would have left those pointing at
     * the panel, silently retitling and DISABLING it. Item numbers in this menu are
     * computed precisely because the shape keeps changing. */
    stateItem = (short)(CountMenuItems(m) + 1);
    s[0] = 0;
    PStrAppendC(s, StateLine(st ? st->state : kStUnknown));
    AppendMenu(m, "\px");
    SetMenuItemText(m, stateItem, s);
    DisableItem(m, stateItem);

    /* ⭐ THE DEVICES ARE SELECTABLE NOW -- picking one points the cell's gauge at it.
     * Each line carries its own level, so the menu answers "what about the other one?"
     * without needing to switch first. */
    sel = (st != NULL) ? SelectedIndex(st) : -1;
    if (st != NULL && st->devCount > 0) {
        AppendMenu(m, "\p(-");
        firstDev = (short)(CountMenuItems(m) + 1);
        for (i = 0; i < st->devCount; i++) {
            s[0] = 0;
            /* ⭐ THE DEVICE'S OWN NAME WHEN WE HAVE ONE. KindName is the fallback, not
             * the default: "Keyboard" is fine for one device and useless for two of a
             * kind, which is exactly what adding the A1015 creates. */
            if (st->devName[i][0] != 0) {
                short k;
                for (k = 0; k < kNameChars && st->devName[i][k] != 0 && s[0] < 200; k++)
                    s[++s[0]] = st->devName[i][k];
            } else {
                PStrAppendC(s, KindName(st->devCod[i]));
            }
            if (st->devPct[i] != kNoReading) {
                PStrAppendC(s, "  ");
                PStrAppendNum(s, (long)st->devPct[i]);
                PStrAppendC(s, "%");
                /* ⚠ AGE, NOT FRESHNESS. whenTicks is only comparable within the session
                 * that wrote it, and LoadBattery has already established that this file
                 * IS from this session -- so the subtraction is meaningful here and
                 * nowhere else. The driver reads the level when a device connects, so a
                 * large age is normal and does not mean the level is wrong. */
                if (st->devWhen[i] != 0) {
                    unsigned long now = TickCount();
                    if (now > st->devWhen[i]) {
                        unsigned long mins = (now - st->devWhen[i]) / (60UL * 60UL);
                        if (mins >= 1) {
                            PStrAppendC(s, " (");
                            PStrAppendNum(s, (long)mins);
                            PStrAppendC(s, " min ago)");
                        }
                    }
                }
            } else if (st->state == kStHCI) {
                PStrAppendC(s, "  - not reporting");
            }
            AppendMenu(m, "\px");
            SetMenuItemText(m, CountMenuItems(m), s);
            if (i == sel) SetItemMark(m, CountMenuItems(m), (short)sdevMenuItemMark);
            /* ⚠ Only worth selecting when there is more than one device. A lone
             * checkmarked item that does nothing when clicked is a control that lies. */
            if (st->devCount < 2) DisableItem(m, CountMenuItems(m));
        }
    } else {
        /* ⭐ SAY SO WHEN THERE IS NOTHING PAIRED. Until 2.2 an empty list produced no
         * lines at all, so the menu read "Bluetooth is on" and then jumped straight to
         * the actions -- leaving the one question the user actually had ("why is there no
         * gauge?") unanswered by both the cell and the menu.
         *
         * ⚠ This is the ONE place that claim can honestly be made. An empty list is
         * something the module KNOWS; every other reason for a missing gauge -- asleep,
         * out of range, not yet answered, a stale file, the other device selected -- is
         * indistinguishable from here, which is exactly why the cell must not try to
         * explain itself and why the per-device lines carry "- not reporting" instead.
         *
         * ⚠ Shown in every state, not just kStHCI: with no hardware or in proxy mode it
         * is still true and still the answer to the same question. The status line above
         * already gives the hardware's side of it. */
        short noneItem;
        AppendMenu(m, "\p(-");
        noneItem = (short)(CountMenuItems(m) + 1);
        s[0] = 0;
        PStrAppendC(s, "No devices paired");
        AppendMenu(m, "\px");
        SetMenuItemText(m, noneItem, s);
        DisableItem(m, noneItem);
    }

    AppendMenu(m, "\p(-");

    /* ⭐ HIDE/SHOW BATTERY LEVEL, as Battery Monitor offers. The wording states what the
     * item WILL DO rather than describing the current state -- "Hide" when it is showing
     * -- because a menu item is a verb. A checkmark on a "Hide" label would be
     * ambiguous about which way it is set. */
    hideItem = (short)(CountMenuItems(m) + 1);
    s[0] = 0;
    PStrAppendC(s, (st != NULL && st->hideLevel) ? "Show Battery Level"
                                                 : "Hide Battery Level");
    AppendMenu(m, "\px");
    SetMenuItemText(m, hideItem, s);
    if (st == NULL) DisableItem(m, hideItem);

    pairItem = (short)(CountMenuItems(m) + 1);
    AppendMenu(m, "\pTurn On Pairing at Restart\311");
    /* ⚠ Only offered when it can do something. In HCI mode pairing is ALREADY on, and
     * with no hardware there is nothing to turn on -- an item that cannot work is
     * worse than an absent one. */
    if (st == NULL || st->state != kStProxy) DisableItem(m, pairItem);

    InsertMenu(m, hierMenu);
    chosen = 0;
    if (statusRect != NULL) {
        long r = SBTrackPopupMenu(statusRect, m);
        chosen = (short)(r & 0xFFFF);
    }
    DeleteMenu(kMenuID);
    DisposeMenu(m);

    if (st == NULL || chosen == 0) return;

    if (chosen == panelItem) {
        if (havePanel) OpenPanel(&panelSpec);
        return;
    }

    if (chosen == hideItem) {
        st->hideLevel = (Boolean)(!st->hideLevel);
        SavePrefs(st);
        /* ⚠⚠ THIS ONE GENUINELY CHANGES THE CELL WIDTH, so unlike every other state
         * change in this module it must tell the strip to re-ask. Returning
         * sdevResizeDisplay is the ONLY documented way to make that happen, and it works
         * here precisely because CellWidth really does return a different number --
         * which is why it does NOT work for a content-only change at a fixed width (the
         * trap noted in design note 2). Both halves are required: the bit AND a changed
         * width. */
        if (widthChanged != NULL) *widthChanged = true;
        st->shownPct    = -30000;
        st->lastRefresh = TickCount() - kRefreshTicks;
        /* ⚠ DROP THE SELF-DRAW CACHE. The cached rect is the OLD width, and the tickle
         * could fire before the strip has acted on sdevResizeDisplay -- painting the new
         * contents centred in the old cell for a frame. Clearing the cache parks
         * self-drawing until the resize's sdevDrawStatus re-arms it with the new rect,
         * which is the same fail-safe PortLooksAlive uses. */
        st->haveCache = false;
        st->cachePort = NULL;
        return;
    }

    if (chosen == pairItem) {
        /* ⚠ NO ALERT FROM STRIP CONTEXT HERE. Putting up a modal dialog from a CSM
         * needs SBOpenModuleResourceFile plus SBModalDialogInContext and its own
         * DLOG/DITL -- proven on the UT launcher, but it is a lot of machinery for a
         * confirmation, and this action is harmless and idempotent: it writes a flag
         * the switcher consumes once, and doing it twice is the same as doing it once.
         * The feedback is the menu's own state on the next click. */
        (void)SetPairFlag();
        Refresh(st);
        return;
    }

    if (firstDev > 0 && chosen >= firstDev && chosen < firstDev + st->devCount) {
        short k = (short)(chosen - firstDev);
        st->selHi   = st->devHi[k];
        st->selLo   = st->devLo[k];
        st->haveSel = true;
        SavePrefs(st);                 /* write now: the strip's own save may never come */

        /* ⚠⚠ THE CELL MUST FOLLOW THE CHOICE IMMEDIATELY, and two separate things gate
         * that. shownPct forces the tickle to consider the value changed -- but the
         * tickle only does any work once kRefreshTicks have elapsed, so on its own the
         * cell would keep showing the OTHER device's level for up to five seconds after
         * the user picked this one. Winding lastRefresh back makes the very next tickle
         * due, and those arrive continuously while the machine is idle.
         *
         * ⚠ Returning sdevResizeDisplay from the click would NOT do it: the strip
         * repaints on a width CHANGE, and this module's width is fixed by design. That
         * is the same trap the CPU-temp module shipped -- see design note 2. */
        st->shownPct    = -30000;
        st->lastRefresh = TickCount() - kRefreshTicks;
    }
}

/* ---- entry point --------------------------------------------------------- */

/* ⚠ VERIFIED SIGNATURE, and it is NOT in ControlStrip.h: four discrete args, no
 * EventRecord. `params` is the refCon returned from sdevInitModule. */
pascal long ControlStripModule(long message, long params,
                               Rect *statusRect, GrafPtr statusPort)
{
    BTCsmState *st = (BTCsmState *)params;

    switch (message) {
    case sdevInitModule: {
        /* ⚠ NewPtrSys: System heap, so it survives whatever the strip does to its own
         * zone, and it is the refCon that keeps state OUT of the fragment's data
         * section. Tolerate failure by running stateless -- every path below already
         * checks for NULL, because a module that crashes takes the whole strip. */
        BTCsmState *ns = (BTCsmState *)NewPtrSys((Size)sizeof(BTCsmState));
        if (ns == NULL) return 0;
        memset(ns, 0, sizeof(BTCsmState));
        ns->shownPct   = -30000;
        ns->shownState = -1;
        /* ⚠ Detach the icon family ONCE, here. 0xFFFFFFFF asks for every member, so
         * PlotIconSuite can pick the right depth and size at draw time. Failure is
         * tolerated: iconSuite stays NULL and the cell falls back to text alone. */
        if (SBGetDetachIconSuite(&ns->iconSuite, kIconID, 0xFFFFFFFFUL) != noErr)
            ns->iconSuite = NULL;
        LoadPrefs(ns);                 /* the remembered device, from disk */
        Refresh(ns);
        ns->lastRefresh = TickCount();
        return (long)ns;
    }

    case sdevCloseModule:
        /* ⚠⚠ THE ICON SUITE IS OURS AND MUST BE DISPOSED BEFORE THE BLOCK THAT HOLDS IT.
         * Until 1.2 this case could honestly say nothing global was installed; 1.3 owns a
         * detached icon family, so freeing the refCon without releasing it would leak the
         * suite on every strip reload. Order matters -- read the handle, then free the
         * block. `true` disposes the icon DATA as well as the suite handle, which is
         * correct for a suite we detached rather than borrowed.
         *
         * ⚠ The cached port is borrowed, never owned, so it must NOT be disposed. */
        if (st != NULL) {
            if (st->iconSuite != NULL) {
                (void)DisposeIconSuite(st->iconSuite, true);
                st->iconSuite = NULL;
            }
            DisposePtr((Ptr)st);
        }
        return 0;

    case sdevFeatures:
        /* ⚠ sdevDontAutoTrack matters: without it the strip auto-tracks and only calls
         * us on mouse-UP, so the menu appears late and feels backwards against every
         * other module. With it we drive tracking ourselves via SBTrackPopupMenu. */
        return (1L << sdevWantMouseClicks)
             | (1L << sdevDontAutoTrack)
             | (1L << sdevHasCustomHelp);

    case sdevGetDisplayWidth:
        return CellWidth(st, statusPort);

    case sdevPeriodicTickle:
        /* ⭐ THE CELL REPAINTS ITSELF HERE. The strip only redraws a module when its
         * width changes or it is externally invalidated, and this module's width is
         * fixed by design -- so returning sdevResizeDisplay would never repaint
         * anything and the reading would sit frozen until the user prodded it (the
         * CPU-temp module shipped that bug and users reported it). */
        if (st != NULL) {
            unsigned long now = TickCount();
            if ((now - st->lastRefresh) >= kRefreshTicks) {
                short np, nst;
                st->lastRefresh = now;
                Refresh(st);
                np  = CurrentPct(st);
                nst = st->state;
                if (st->haveCache && st->cachePort != NULL
                    && (np != st->shownPct || nst != st->shownState)
                    && SBIsControlStripVisible()) {
                    /* The cached port must PROVE it is still a live port before we draw
                     * through it -- see PortLooksAlive. On failure, drop the cache: the
                     * next real sdevDrawStatus re-arms it with a fresh port. */
                    if (!PortLooksAlive(st->cachePort, &st->cacheRect)) {
                        st->haveCache = false;
                        st->cachePort = NULL;
                    } else {
                        GrafPtr savePort;
                        GetPort(&savePort);
                        SetPort(st->cachePort);
                        DrawCell(st, &st->cacheRect, true);   /* erase + repaint in place */
                        SetPort(savePort);
                    }
                }
                st->shownPct   = np;
                st->shownState = nst;
            }
        }
        return 0;

    case sdevDrawStatus:
        if (statusPort != NULL) SetPort(statusPort);
        if (st != NULL && statusRect != NULL && statusPort != NULL) {
            st->cachePort = statusPort;   /* remember where/how to self-repaint */
            st->cacheRect = *statusRect;  /* during the periodic tickle          */
            st->haveCache = true;
            /* keep change-tracking current so the next tickle does not repaint
             * needlessly right after this strip-driven draw */
            st->shownPct   = CurrentPct(st);
            st->shownState = st->state;
        }
        if (statusRect != NULL) DrawCell(st, statusRect, false);  /* cell already clear */
        return 0;

    case sdevMouseClick: {
        /* ⚠ sdevResizeDisplay is returned ONLY when the width actually changed, i.e.
         * when Hide/Show was toggled. Returning it on every click would ask the strip to
         * re-lay-out every module to our right each time someone opens the menu, and it
         * is also the bit whose misuse (returning it for a CONTENT change at a fixed
         * width) is the trap in design note 2. sdevNeedToSave is not returned because
         * this module writes its own preferences immediately via SBSavePreferences --
         * the strip's save value is session-only and would not survive a restart. */
        Boolean resized = false;
        DoClick(st, statusRect, &resized);
        return resized ? (1L << sdevResizeDisplay) : 0;
    }

    case sdevShowBalloonHelp:
        /* ⓘ Splitting a "\p" string across adjacent literals for line width is SAFE in
         * this toolchain -- MEASURED, not assumed, because the opposite is a plausible
         * enough theory to write down by mistake. `"\pIJKL" "MNOP"` compiles to
         * `08 'IJKLMNOP'`: the length byte describes the fully concatenated literal, so
         * it cannot truncate the balloon. (Retro68's gcc does this by default; there is
         * no -fpascal-strings option here -- it is not a recognised flag.) */
        if (statusRect != NULL)
            SBShowHelpString(statusRect,
                "\pBattery level of a paired Bluetooth device. "
                "Click to choose a device or turn on pairing.");
        return 0;

    default:
        return 0;
    }
}
