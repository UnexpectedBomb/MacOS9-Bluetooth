/*
 *  bt_keyfile.c  --  the link key store's FILE half. TASK LEVEL ONLY.
 *
 *  ⚠⚠ EVERY FUNCTION IN THIS FILE MAY TOUCH THE FILE MANAGER, WHICH MEANS EVERY
 *  FUNCTION IN THIS FILE IS TASK LEVEL ONLY. There are exactly three legal callers:
 *
 *      BT_KeyFileLoad()          <- ProbeInitialize   (task level)
 *      BT_KeyFileFlushIfDirty()  <- the NM response   (task level, bt_defer.c)
 *      BT_KeyFileFlushIfDirty()  <- ProbeFinalize     (task level)
 *
 *  Nothing here may ever be called from a USL completion. scripts/level-audit.py is
 *  what enforces that: its roots are the interrupt-level entry points, so if any of
 *  these ever becomes reachable from one, the File Manager calls below will show up
 *  as FORBIDDEN hits. That is the check, and it must stay clean.
 *
 *  WHY THE SPLIT EXISTS AT ALL (docs/M3-DESIGN.md §3b)
 *  BTstack's link key callbacks are invoked from hci.c while it handles
 *  HCI_EVENT_LINK_KEY_REQUEST and Link Key Notification, both of which arrive
 *  through our interrupt-IN completion. So the store BTstack talks to must be pure
 *  RAM (src/bt_linkkey_db.c), and this file moves that RAM to and from disk at the
 *  only moments we are allowed to.
 *
 *  ⚠ THE WRITE IS TEMP-FILE-THEN-SWAP, NEVER A TRUNCATING WRITE. This project's
 *  first hard rule exists because a truncating open destroyed 2,800 lines of
 *  hardware-debugged work: "if a script genuinely must transform a file, write to a
 *  temp file and mv on success. Never open(original, 'w')." The same reasoning
 *  applies to a driver rewriting its own key store: a crash midway through an
 *  in-place write would leave a half-written file that loads as garbage, and the
 *  user's bond is the thing being protected.
 */

/* ⚠ stdbool BEFORE the Toolbox headers, and it is required. The build defines
 * TYPE_BOOL=1 so BTstack gets MacTypes.h to skip its own `enum { false, true }`,
 * but Folders.h then uses `true`/`false` itself and fails to compile with nothing
 * defining them. stdbool.h supplies both as macros. Same trap, same fix, as the
 * note at the top of bt_probe.c. */
#include <stdbool.h>
#include <MacTypes.h>
#include <Files.h>
#include <Folders.h>
#include <MacMemory.h>

#include <string.h>

#include "bt_pump.h"
#include "bt_keyfile.h"
#include "bt_linkkey_db.h"
#include "bt_defer.h"    /* v14.3: BT_BatteryNote must ASK for task level, not just
                          * mark dirty -- see the note there. ENQUEUE ONLY, so it is
                          * safe from the interrupt-level response handler. */

/* ---- on-disk format ------------------------------------------------------- *
 * Fixed-size records, no parser, no allocation. Byte layout is written out
 * explicitly rather than by dumping a struct, because a struct's padding is a
 * compiler decision and this file has to be readable by a future build.
 *
 *   header 20 bytes:  'BTK1' | version(2) | count(2) | localAddr(6) | pad(6)
 *   record 24 bytes:  peerAddr(6) | key(16) | type(1) | pad(1)
 */
#define kKeyFileMagic     0x42544B31UL      /* 'BTK1' */
#define kKeyFileVersion   1
#define kHeaderSize       20
#define kRecordSize       24
#define kMaxRecords       8                 /* must match BT_MAX_LINK_KEYS */

unsigned long gKfLoads, gKfLoaded, gKfFlushes, gKfWritten, gKfErr, gKfRejected;
/* ★★★★★★ gKfNoFile -- THE ONE PATH THAT COUNTED NOTHING, AND IT IS THE PATH WE KEEP
 * LANDING IN. Three sessions this week have started with `load attempts 1, records
 * LOADED 0, file errors 0, file REJECTED 0`, and that row cannot be acted on because
 * TWO COMPLETELY DIFFERENT CAUSES PRODUCE IT:
 *
 *   - the file could not be OPENED (absent, or unopenable at that moment), or
 *   - the file opened fine and honestly contained zero records.
 *
 * The first is a bug that loses a pairing on every reboot and costs the user a battery
 * pull. The second is normal on a first run. Nothing in the log separated them, so
 * three reboots' worth of evidence says the same ambiguous thing.
 *
 * ⚠ The silent return was deliberate -- "no file yet: normal" -- and that reasoning was
 * right for a first run and wrong for every run after. Counting it does not make it an
 * error; it makes it VISIBLE. */
unsigned long gKfNoFile;

static void put16(unsigned char *p, unsigned short v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static unsigned short get16(const unsigned char *p)   { return (unsigned short)((p[0] << 8) | p[1]); }

/* Resolve System Folder:Preferences:OS9 Bluetooth Keys.
 *
 * ⚠ FindFolder at extension-load time was listed as an open question in
 * docs/M3-DESIGN.md §10.2. It is used here rather than assumed safe: the return is
 * checked, and a failure is COUNTED and then simply means no persistence this boot.
 * The driver must still work without the file -- losing a bond is a degradation,
 * failing to load is not. */
static Boolean keyFileSpec(FSSpec *spec)
{
    short vRefNum;
    long  dirID;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr)
        return false;

    {
        OSErr err = FSMakeFSSpec(vRefNum, dirID, "\pOS9 Bluetooth Keys", spec);
        if (err != noErr && err != fnfErr) return false;
        if (spec->name[0] == 0) return false;
    }
    return true;
}

/* ---- load ---------------------------------------------------------------- *
 * Called from ProbeInitialize, BEFORE hci_init hands BTstack the db pointer, so a
 * key is in RAM before anything can ask for one. */

/* ★★★★ v15.4: ADOPT THE PANEL'S SIGNATURE, so the Finder draws our icon.
 *
 * Every file this stack generates is now created with creator 'BTcp' -- the control
 * panel's -- because the Finder resolves a document's icon through its CREATOR, and the
 * panel is the one piece of the stack that carries a BNDL. Three different creators
 * meant three different generic icons.
 *
 * ⚠⚠ THIS FUNCTION IS NOT OPTIONAL TIDYING, and FSpExchangeFiles is the reason. It swaps
 * the two files' CONTENTS and leaves each catalog entry's Finder info where it was, which
 * is exactly why it is safe for atomic writes -- the live file keeps its identity. The
 * side effect is that a file created by an older build keeps its OLD creator forever, no
 * matter how many times we rewrite it. Without this, the icon would appear only for users
 * who had never run an earlier version.
 *
 * ⚠ TASK LEVEL ONLY. FSpGetFInfo/FSpSetFInfo are File Manager calls; every caller below
 * is a flush that already writes files, so they are all task level already. Do not call
 * this from anywhere that is not. */
static void KfAdoptSignature(const FSSpec *spec)
{
    FInfo fi;
    if (FSpGetFInfo(spec, &fi) != noErr) return;
    if (fi.fdCreator == 'BTcp') return;          /* already ours; nothing to do */
    fi.fdCreator = 'BTcp';
    (void)FSpSetFInfo(spec, &fi);                /* best effort: an icon is not worth
                                                  * failing a write that succeeded */
}

void BT_KeyFileLoad(void)
{
    FSSpec        spec;
    short         refNum;
    long          count;
    unsigned char hdr[kHeaderSize];
    unsigned char rec[kRecordSize];
    unsigned short n, i;
    bd_addr_t     localNow;

    gKfLoads++;

    if (!keyFileSpec(&spec))                      { gKfErr++; return; }
    /* ⚠ COUNTED, NOT SILENT. See gKfNoFile: this is normal on a first run and a real
     * bug on any run after one that wrote records. */
    if (FSpOpenDF(&spec, fsRdPerm, &refNum) != noErr) { gKfNoFile++; return; }

    count = kHeaderSize;
    if (FSRead(refNum, &count, hdr) != noErr || count != kHeaderSize) {
        gKfErr++; FSClose(refNum); return;
    }

    /* ⚠ VALIDATE BEFORE TRUSTING. A stale or foreign file must be discarded, not
     * half-loaded: a wrong key is worse than no key, because it makes a peer's
     * authentication fail in a way that looks like our bug. */
    if (((unsigned long)hdr[0] << 24 | (unsigned long)hdr[1] << 16
         | (unsigned long)hdr[2] << 8 | hdr[3]) != kKeyFileMagic
        || get16(&hdr[4]) != kKeyFileVersion) {
        gKfRejected++; FSClose(refNum); return;
    }

    /* ⚠ THE LOCAL ADDRESS CHECK IS THE POINT OF set_local_bd_addr, NOT A FORMALITY.
     * A link key is bonded against the LOCAL controller's address. Swap the dongle
     * and every stored key becomes not merely useless but actively wrong -- the peer
     * would be offered a key it never agreed to. BTstack provides the hook precisely
     * because USB dongles get swapped on desktop systems. */
    if (BT_LinkKeyGetLocalAddr(localNow)) {
        if (memcmp(&hdr[8], localNow, sizeof(bd_addr_t)) != 0) {
            gKfRejected++; FSClose(refNum); return;
        }
    }

    n = get16(&hdr[6]);
    if (n > kMaxRecords) n = kMaxRecords;

    for (i = 0; i < n; i++) {
        count = kRecordSize;
        if (FSRead(refNum, &count, rec) != noErr || count != kRecordSize) { gKfErr++; break; }
        BT_LinkKeyLoadOne(&rec[0], &rec[6], rec[22]);
        gKfLoaded++;
    }

    FSClose(refNum);

    /* Loading is not a change. Clearing the flag here stops the very first deferred
     * flush from rewriting a file we just read, byte for byte. */
    BT_LinkKeyClearDirty();
}

/* ---- flush --------------------------------------------------------------- */

static OSErr writeAllRecords(short refNum)
{
    unsigned char hdr[kHeaderSize];
    unsigned char rec[kRecordSize];
    bd_addr_t     local;
    long          count;
    int           i, n;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'T'; hdr[2] = 'K'; hdr[3] = '1';
    put16(&hdr[4], kKeyFileVersion);

    n = BT_LinkKeyCount();
    if (n > kMaxRecords) n = kMaxRecords;
    put16(&hdr[6], (unsigned short)n);

    if (BT_LinkKeyGetLocalAddr(local))
        memcpy(&hdr[8], local, sizeof(bd_addr_t));

    count = kHeaderSize;
    if (FSWrite(refNum, &count, hdr) != noErr) return ioErr;

    for (i = 0; i < n; i++) {
        bd_addr_t addr;
        unsigned char key[16];
        unsigned char type;

        /* ⚠ ARCHIVED RECORDS GO TO THE FILE TOO. Restore has to survive a reboot;
         * that is the one moment the user most needs it. */
        if (!BT_LinkKeyGetByIndexEx(i, addr, key, &type, 1)) continue;

        memset(rec, 0, sizeof(rec));
        memcpy(&rec[0], addr, sizeof(bd_addr_t));
        memcpy(&rec[6], key, 16);
        rec[22] = type;

        count = kRecordSize;
        if (FSWrite(refNum, &count, rec) != noErr) return ioErr;
        gKfWritten++;
    }
    return noErr;
}

void BT_KeyFileFlushIfDirty(void)
{
    FSSpec spec, tmp;
    short  refNum, vRefNum;
    long   dirID;
    OSErr  err;

    if (!BT_LinkKeyIsDirty()) return;
    gKfFlushes++;

    if (!keyFileSpec(&spec)) { gKfErr++; return; }
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) { gKfErr++; return; }

    /* ⚠ TEMP FILE FIRST. See the header comment: never write the live file in
     * place. If anything below fails, the previous good file is untouched. */
    if (FSMakeFSSpec(vRefNum, dirID, "\pOS9 Bluetooth Keys tmp", &tmp) != noErr
        && FSMakeFSSpec(vRefNum, dirID, "\pOS9 Bluetooth Keys tmp", &tmp) != fnfErr) {
        gKfErr++; return;
    }
    (void)FSpDelete(&tmp);

    err = FSpCreate(&tmp, 'BTcp', 'BTKy', smSystemScript);
    if (err != noErr && err != dupFNErr) { gKfErr++; return; }
    if (FSpOpenDF(&tmp, fsWrPerm, &refNum) != noErr) { gKfErr++; return; }

    (void)SetEOF(refNum, 0);
    err = writeAllRecords(refNum);
    FSClose(refNum);

    if (err != noErr) { gKfErr++; (void)FSpDelete(&tmp); return; }

    /* Swap into place. FSpExchangeFiles keeps the original's identity where it
     * exists; if there is no original yet, a rename is the whole job. */
    if (FSpExchangeFiles(&tmp, &spec) == noErr) {
        KfAdoptSignature(&spec);
        (void)FSpDelete(&tmp);
    } else {
        (void)FSpDelete(&spec);
        if (FSpRename(&tmp, "\pOS9 Bluetooth Keys") != noErr) { gKfErr++; return; }
        KfAdoptSignature(&spec);
    }

    (void)FlushVol(NULL, spec.vRefNum);

    /* ⚠ ONLY NOW. Clearing dirty before the swap succeeded would silently lose the
     * key it was protecting -- the flag is the only record that a write is owed. */
    BT_LinkKeyClearDirty();
}

/* ---- v11.1: clear the switcher's self-healing marker ---------------------------
 *
 * ★★★ THE OTHER HALF OF USBBluetoothSwitch v1.3. The switcher writes
 * "Bluetooth Switch Attempted" BEFORE it switches the card, and declines to switch on
 * any boot where it finds that file still there -- meaning the previous boot switched
 * and this driver never came up. Deleting it is how we say "I am alive".
 *
 * ⚠⚠ IF THIS NEVER RUNS, THE FALLBACK LATCHES FOREVER and the card stays with Mac OS
 * every boot. That is the SAFE direction by design -- the keyboard keeps working -- but
 * it is indistinguishable from "the feature was never built", so the counters below
 * exist to tell those apart in a log.
 *
 * ⚠ TASK LEVEL ONLY. The File Manager below task level is a silent hard hang on this
 * platform [[reference_os9_no_filemgr_at_interrupt]], which is why this lives in this
 * file with the rest of the task-level file work and is called from the defer
 * trampoline rather than from a completion.
 *
 * ⚠ Called repeatedly and cheap after the first success: once gMarkCleared is set it
 * returns immediately, so it does not stat the Preferences folder on every hop. */
unsigned long gMarkCleared, gMarkErr, gMarkRuns;

void BT_ClearSwitchMarker(void)
{
    FSSpec spec;
    short  vRefNum;
    long   dirID;
    OSErr  err;

    if (gMarkCleared) return;
    gMarkRuns++;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                     &vRefNum, &dirID);
    if (err != noErr) { gMarkErr = 0x100UL | (unsigned long)(unsigned short)err; return; }

    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Switch Attempted", &spec);
    if (err == fnfErr) {
        /* Already gone -- the forced-switch path does not write one, so this is normal
         * there. Treat as done so we stop looking. */
        gMarkCleared = 1;
        return;
    }
    if (err != noErr) { gMarkErr = 0x100UL | (unsigned long)(unsigned short)err; return; }

    err = FSpDelete(&spec);
    if (err != noErr) { gMarkErr = 0x100UL | (unsigned long)(unsigned short)err; return; }
    gMarkCleared = 1;
}

/* ================================================================================
 *  v14.2: PUBLISH THE BATTERY LEVEL WHERE A CONTROL STRIP MODULE CAN READ IT.
 * ================================================================================
 *
 * ⭐ WHY A FILE AND NOT THE COUNTER BLOCK. The CSM deliberately does not read `'BTP1'`,
 * and that decision is recorded in csm/bt_csm.c: the block is FOUND by matching 'ENDS'
 * at kWEnd, a word index that has now moved THIRTEEN times, and a CSM reading it would
 * become a fourth binary needing a lockstep bump. It reads the USB bus for state and
 * `Preferences:Bluetooth Device Kinds` for the device list instead, neither of which can
 * go stale against the driver. Battery follows the same rule.
 *
 * ⭐ THE FORMAT IS THE DEVICE KINDS FORMAT, deliberately, because the CSM already parses
 * that shape correctly: a three-long header of magic, format version and count, then
 * fixed-size records. Reusing a proven layout beats inventing a second one.
 *
 *     header:  'BTBA', 2, count
 *     record:  addrHi, addrLo, percent, whenTicks      (4 longs, per device)
 *
 * ⚠⚠ addrHi IS THE TOP **3** BYTES AND addrLo THE LOW 3 -- the same split as every other
 * address in this codebase (kWRlkA0Hi, the link-key database, the panel's Device Kinds
 * file). Version 1 wrote 2+4 here and nowhere else, which broke the join; see the
 * publish site in bt_btstack.c for the whole story.
 *
 * ⚠ NO STATE FIELD YET, ON PURPOSE. BatteryState (Input 48) now ANSWERS -- the A1016
 * returns report 48 with value 0 -- but what the value MEANS is not established (Tiger
 * declares 0..2 and does not say what 0 is), so it is not written and not displayed.
 * M3-DESIGN's rule stands: a CSM must not display a level it cannot obtain, and the same
 * goes for a field whose meaning is a guess. Adding it later is another version bump.
 *
 * ⚠⚠ whenTicks IS NOT A FRESHNESS GUARANTEE and a reader must not treat it as one.
 * TickCount() restarts at boot, so a file written late in one session looks *newer* than
 * one written early in the next. It is there to tell two readings apart within a
 * session, nothing more. Whether a level is worth showing is answered by whether the
 * device is CONNECTED, which the CSM already knows from the USB bus.
 *
 * ⚠ TASK LEVEL ONLY, like everything else in this file. Called from the NM defer
 * response and ProbeFinalize, beside BT_KeyFileFlushIfDirty, and from nowhere below task
 * level [[reference_os9_no_filemgr_at_interrupt]]. */

#define kBatMagic   0x42544241UL      /* 'BTBA' */
/* ⚠⚠ VERSION 2 = THE ADDRESS FIELDS ARE 3+3, NOT 2+4. Version 1 (driver 14.2-14.4)
 * wrote addrHi=bytes[0..1], addrLo=bytes[2..5] -- an encoding used NOWHERE else in this
 * codebase -- so the only reader could not join this file to the panel's Device Kinds
 * file and listed every device twice. The layout is byte-identical between the two
 * versions; only the meaning of two fields changed, which a reader has no way to
 * detect. That is exactly what a format version is for, so it moved. See the publish
 * site in bt_btstack.c and csm/CMakeLists.txt, which fails the build unless both
 * binaries carry the same number. */
#define kBatFmtVer  2UL
#define kBatMaxDev  8                 /* matches the CSM's kMaxDev and BT_MAX_LINK_KEYS */

unsigned long gBatFlushes, gBatFlushErr, gBatPublished;

/* ★ THE LOW-BATTERY WARNING. 5% is the user's chosen threshold; the clear level is
 * higher on purpose so the latch cannot chatter around a single value -- see the note in
 * BT_BatteryNote. gBatWarnPend is a BITMASK of slots owing a warning, written at
 * interrupt level and read at task level, hence volatile. */
#define kLowBatteryPct    5
#define kLowBatteryClear 10

static unsigned char   gBatWarned[kBatMaxDev];   /* latched: already warned */
static unsigned char   gBatWarnPct[kBatMaxDev];  /* the level that tripped it */
static volatile unsigned long gBatWarnPend;
unsigned long gBatWarnings;                      /* alerts actually posted */

static unsigned long gBatAddrHi[kBatMaxDev], gBatAddrLo[kBatMaxDev];
static unsigned long gBatPctOf[kBatMaxDev],  gBatWhenT[kBatMaxDev];
static short         gBatNDev;
static int           gBatDirty;

/* ⭐ RECORD A READING, keyed by address. Called from the GET_REPORT response handler at
 * interrupt level, so it does PLAIN STORES ONLY -- the file write happens later, at task
 * level, in BT_BatteryFlushIfDirty.
 *
 * ⚠⚠ THE VALUE IS THE **LAST** BYTE OF THE PAYLOAD, NOT THE FIRST, and that is measured
 * rather than assumed. Tiger's ioreg declares Feature 71 as size=1, and the A1016
 * answered with TWO bytes: 0x47 0x44 -- the report ID echoed back, then 68. Taking the
 * first byte would report "71%" forever. Last-byte handles both the ID-echoing form and
 * a bare one-byte answer, which is why it is written this way rather than indexed.
 *
 * ⚠ A value above 100 is NOT published. It is not a percentage, whatever it is, and a
 * gauge is worse than no gauge if it can read 255%. BTCheck still shows the raw bytes. */
void BT_BatteryNote(unsigned long addrHi, unsigned long addrLo,
                    const unsigned char *payload, unsigned short len,
                    unsigned long nowTicks)
{
    unsigned long pct;
    short i, slot = -1;

    if (payload == 0 || len == 0) return;
    pct = (unsigned long)payload[len - 1];
    if (pct > 100UL) return;

    for (i = 0; i < gBatNDev; i++)
        if (gBatAddrHi[i] == addrHi && gBatAddrLo[i] == addrLo) { slot = i; break; }
    if (slot < 0) {
        if (gBatNDev >= kBatMaxDev) return;     /* full: drop rather than overwrite */
        slot = gBatNDev++;
        gBatAddrHi[slot] = addrHi;
        gBatAddrLo[slot] = addrLo;
    }
    gBatPctOf[slot] = pct;
    gBatWhenT[slot] = nowTicks;
    gBatPublished++;
    gBatDirty = 1;

    /* ★★★ LOW BATTERY: LATCH THE WARNING HERE, POST IT AT TASK LEVEL.
     *
     * ⚠⚠ HYSTERESIS IS THE WHOLE DESIGN, not a refinement. The driver re-reads the level
     * every time a device connects, so a keyboard sitting at exactly the threshold would
     * otherwise put a dialog in front of the user on every single connect -- and a
     * warning that appears constantly is one the user learns to dismiss without reading,
     * which is worse than no warning. So: latch on the way DOWN through kLowBatteryPct,
     * and only re-arm once the level comes back up past kLowBatteryClear. A fresh battery
     * re-arms it; a flat one warns once.
     *
     * ⚠ PER DEVICE. Two devices at 4% are two different facts and both deserve saying,
     * and a shared latch would silence whichever reported second.
     *
     * ⚠ Interrupt level: plain stores plus BT_DeferRequest, which only ENQUEUES. The
     * alert itself is the Notification Manager with a string, and putting a dialog up
     * from here is exactly what the task-level hop exists to avoid. */
    if (pct <= kLowBatteryPct) {
        if (!gBatWarned[slot]) {
            gBatWarned[slot]  = 1;
            gBatWarnPend     |= (unsigned long)(1UL << slot);
            gBatWarnPct[slot] = (unsigned char)pct;
            BT_DeferRequest();
        }
    } else if (pct > kLowBatteryClear) {
        gBatWarned[slot] = 0;           /* re-armed: a replaced battery can warn again */
    }

    /* ⚠⚠⚠ MARKING DIRTY IS NOT ENOUGH, AND v14.2 SHIPPED WITHOUT THIS LINE. Measured:
     * `readings published 1` beside `file flushes 0` -- the level was read, accepted and
     * never written, so a CSM would have found no file at all.
     *
     * BT_BatteryFlushIfDirty runs only from the NM defer response and ProbeFinalize, and
     * the defer response runs only when somebody ENQUEUES it. Nothing else did that
     * session -- `key file flushes` was 0 in the same log for exactly the same reason --
     * so the dirty flag sat there until shutdown.
     *
     * ⇒ The pattern was already documented two lines from where I should have looked, at
     * bt_linkkey_db.c:288: "db_put_link_key sets gLkDirty and then calls BT_DeferRequest
     * to get task level". I copied the dirty flag and not the request.
     *
     * ⚠ SAFE FROM HERE. BT_DeferRequest only ENQUEUES an NM record -- it does not run the
     * response proc -- which is why it is classified interrupt-safe in the level audit
     * and why the link-key path calls it from the same kind of context. Do not be tempted
     * to call BT_BatteryFlushIfDirty directly: that is the File Manager, and this
     * function runs at interrupt level [[reference_os9_no_filemgr_at_interrupt]]. */
    BT_DeferRequest();
}

/* ================================================================================
 *  THE DEVICE NAMES FILE, so the CSM can show a real name.
 * ================================================================================
 *
 * The driver is the authoritative source: it is what issued Remote_Name_Request and got
 * an answer. The CSM cannot read the counter block (kWEnd has moved fourteen times), so
 * the name goes to a file, exactly as the battery level does.
 *
 * ⭐ A SEPARATE FILE RATHER THAN A FIELD IN Device Kinds, and the reason is ownership.
 * Device Kinds belongs to the CONTROL PANEL; this belongs to the DRIVER. Adding a name
 * column there would put two writers on one format and force a panel/CSM lockstep bump
 * for a fact the panel does not produce. Keeping them apart also leaves room for the
 * rename the user asked about: a user-supplied ALIAS is a panel-owned preference, so it
 * belongs in the panel's file and can simply take precedence over this one. Driver
 * reports facts; panel stores choices.
 *
 *     header:  'BTNM', 1, count
 *     record:  addrHi, addrLo, then kNameChars bytes, NUL-padded   (32 bytes)
 *
 * ⚠ addrHi/addrLo are TOP 3 BYTES then low 3, as everywhere else. The battery file's
 * format-1 mistake is not repeated.
 * ⚠ TASK LEVEL ONLY, from the defer response beside the battery flush. */
#define kNmMagic   0x42544E4DUL      /* 'BTNM' -- matches the layout note above.
                                      * ⚠ Written 'BNMM' for one draft; a magic that
                                      * disagrees with its own comment is how a reader
                                      * gets written against the wrong constant. */
#define kNmFmtVer  1UL
#define kNmChars   24                /* ⚠ MUST equal kNameChars in bt_pump.h */

unsigned long gNmFlushes, gNmFlushErr;
static int    gNmDirty;

/* ⚠ Interrupt-safe: a flag and a request, nothing more. Called when a name arrives. */
void BT_NamesNote(void)
{
    gNmDirty = 1;
    BT_DeferRequest();
}

void BT_NamesFlushIfDirty(void)
{
    FSSpec spec, tmp;
    short  refNum, vRefNum;
    long   dirID, n;
    OSErr  err;
    unsigned long hdr[3];
    short  i, wrote = 0;

    if (!gNmDirty || gNameCount == 0) return;
    gNmDirty = 0;
    gNmFlushes++;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) { gNmFlushErr++; return; }
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Names", &spec) != noErr
        && FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Names", &spec) != fnfErr) {
        gNmFlushErr++; return;
    }
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Names tmp", &tmp) != noErr
        && FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Names tmp", &tmp) != fnfErr) {
        gNmFlushErr++; return;
    }
    (void)FSpDelete(&tmp);

    err = FSpCreate(&tmp, 'BTcp', 'BTNm', smSystemScript);
    if (err != noErr && err != dupFNErr) { gNmFlushErr++; return; }
    if (FSpOpenDF(&tmp, fsWrPerm, &refNum) != noErr) { gNmFlushErr++; return; }
    (void)SetEOF(refNum, 0);

    /* ⚠ Count the rows that will actually be WRITTEN, not gNameCount, and write the
     * header last-but-first: a header claiming more records than follow is how a reader
     * walks off the end. Counted here by pre-scanning rather than patched afterwards. */
    for (i = 0; i < (short)gNameCount && i < kNameSlots; i++)
        if (gNameText[i][0] != 0) wrote++;

    hdr[0] = kNmMagic; hdr[1] = kNmFmtVer; hdr[2] = (unsigned long)wrote;
    n = (long)sizeof(hdr);
    err = FSWrite(refNum, &n, hdr);

    for (i = 0; i < (short)gNameCount && i < kNameSlots && err == noErr; i++) {
        unsigned long rec[2];
        unsigned char nm[kNmChars];
        short k;
        if (gNameText[i][0] == 0) continue;     /* no name: nothing to say */
        rec[0] = gNameAddr[i][0];
        rec[1] = gNameAddr[i][1];
        n = (long)sizeof(rec);
        err = FSWrite(refNum, &n, rec);
        if (err != noErr) break;
        /* ⚠ NUL-pad the whole field. Writing only the used bytes would make the record
         * size depend on the name's length, and a fixed-size record is the one property
         * that lets a reader bound its loop. */
        for (k = 0; k < kNmChars; k++) nm[k] = 0;
        for (k = 0; k < kNmChars - 1 && gNameText[i][k] != 0; k++) nm[k] = gNameText[i][k];
        n = (long)kNmChars;
        err = FSWrite(refNum, &n, nm);
    }
    if (err == noErr) err = FSClose(refNum);
    else              (void)FSClose(refNum);
    if (err != noErr) { gNmFlushErr++; (void)FSpDelete(&tmp); return; }

    (void)FSpCreate(&spec, 'BTcp', 'BTNm', smSystemScript);
    if (FSpExchangeFiles(&tmp, &spec) == noErr) { KfAdoptSignature(&spec); (void)FSpDelete(&tmp); }
    else                                        gNmFlushErr++;
}

/* ★★★ POST ANY OWED LOW-BATTERY WARNING. TASK LEVEL, from the defer response.
 *
 * ⚠ TAKE AND CLEAR THE PENDING BIT BEFORE POSTING, not after. BT_PostAlert goes through
 * the Notification Manager, which can take real time, and a reading arriving during it
 * must be able to queue a NEW warning rather than be dropped as already-handled. Same
 * ordering rule as BT_MediaServiceAtTask's pending mask, and for the same reason.
 *
 * ⚠ ONE PER PASS. Only one Notification Manager alert may be outstanding, so a second
 * device owing a warning keeps its bit and is posted on the next hop -- which is
 * requested explicitly before returning, rather than left to whatever happens to call
 * next. That is the v14.2 lesson: marking state dirty is not the same as asking for the
 * hop that services it. */
void BT_BatteryWarnIfPending(void)
{
    short         slot;
    unsigned long pend = gBatWarnPend;
    unsigned char s[64];
    short         n = 0;
    unsigned long pct;

    if (pend == 0) return;

    for (slot = 0; slot < kBatMaxDev; slot++)
        if (pend & (unsigned long)(1UL << slot)) break;
    if (slot >= kBatMaxDev) { gBatWarnPend = 0; return; }

    gBatWarnPend = pend & ~(unsigned long)(1UL << slot);
    pct = (unsigned long)gBatWarnPct[slot];

    /* ⚠ BUILT BY HAND, NOT sprintf. This is driver context: the log-buffer vsprintf
     * stack smash is recorded in reference_os9_logbuf_vsprintf_smash, and a fixed
     * 64-byte Pascal string with a two-digit number needs none of it. The text names the
     * level, because "low battery" without a number tells the user nothing they can act
     * on -- 5% and 1% are different decisions. */
    {
        const char *a = "Bluetooth device battery is very low (";
        const char *b = "%). Replace or recharge it soon.";
        while (*a && n < 60) s[++n] = (unsigned char)*a++;
        if (pct >= 10) s[++n] = (unsigned char)('0' + (pct / 10) % 10);
        s[++n] = (unsigned char)('0' + pct % 10);
        while (*b && n < 62) s[++n] = (unsigned char)*b++;
        s[0] = (unsigned char)n;
    }
    BT_PostAlert(s);
    gBatWarnings++;

    /* Another device still owes one: ask for the next hop explicitly. */
    if (gBatWarnPend != 0) BT_DeferRequest();
}

/* ★ THE SCAN FILTER'S ESCAPE HATCH, read once at task level.
 *
 * The scanner lists only devices we can drive (see GAP_EVENT_INQUIRY_RESULT in
 * bt_btstack.c). That is right for shipping and wrong for the bench: the PHONE is this
 * project's only re-pairable test instrument, because deleting the A1016's bond is a
 * one-way door needing a battery pull. `Preferences:Bluetooth Show All Devices` turns
 * the filter off.
 *
 * ⚠ READ HERE, AT TASK LEVEL, AND ONLY READ AT INTERRUPT LEVEL. The inquiry result
 * arrives at interrupt level where the File Manager is illegal
 * [[reference_os9_no_filemgr_at_interrupt]], so the answer has to be in RAM before the
 * first scan. ProbeInitialize is that window, beside BT_KeyFileLoad for the same reason.
 *
 * ⚠ EXISTENCE IS THE WHOLE MESSAGE -- the file is never opened or read, so it can be
 * zero-length and its contents can never be wrong. Same idiom as the pairing flag.
 * ⚠ NOT consumed: unlike the pairing flag this is a standing preference, so it is left
 * in place rather than deleted. A tester creates it once and forgets about it. */
void BT_LoadShowAllFlag(void)
{
    FSSpec spec;
    short  vRefNum;
    long   dirID;

    gShowAllDevices = 0;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Show All Devices", &spec) == noErr)
        gShowAllDevices = 1;
}


/* ⚠ TEMP FILE THEN FSpExchangeFiles, exactly as the key file does and for the same
 * reason: a half-written live file is worse than a missing one, because a reader cannot
 * tell it is half-written. See the note at BT_KeyFileFlushIfDirty. */
void BT_BatteryFlushIfDirty(void)
{
    FSSpec spec, tmp;
    short  refNum, vRefNum;
    long   dirID, n;
    OSErr  err;
    unsigned long hdr[3];
    short  i;

    if (!gBatDirty || gBatNDev <= 0) return;
    gBatFlushes++;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) { gBatFlushErr++; return; }
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Battery", &spec) != noErr
        && FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Battery", &spec) != fnfErr) {
        gBatFlushErr++; return;
    }
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Battery tmp", &tmp) != noErr
        && FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Battery tmp", &tmp) != fnfErr) {
        gBatFlushErr++; return;
    }
    (void)FSpDelete(&tmp);

    err = FSpCreate(&tmp, 'BTcp', 'BTBa', smSystemScript);
    if (err != noErr && err != dupFNErr) { gBatFlushErr++; return; }
    if (FSpOpenDF(&tmp, fsWrPerm, &refNum) != noErr) { gBatFlushErr++; return; }
    (void)SetEOF(refNum, 0);

    hdr[0] = kBatMagic; hdr[1] = kBatFmtVer; hdr[2] = (unsigned long)gBatNDev;
    n = (long)sizeof(hdr);
    err = FSWrite(refNum, &n, hdr);

    for (i = 0; i < gBatNDev && err == noErr; i++) {
        unsigned long rec[4];
        rec[0] = gBatAddrHi[i]; rec[1] = gBatAddrLo[i];
        rec[2] = gBatPctOf[i];  rec[3] = gBatWhenT[i];
        n = (long)sizeof(rec);
        err = FSWrite(refNum, &n, rec);
    }
    if (err == noErr) err = FSClose(refNum);
    else              (void)FSClose(refNum);

    if (err != noErr) { gBatFlushErr++; (void)FSpDelete(&tmp); return; }

    /* ⚠ The live file will NOT exist on a first publish, and FSpExchangeFiles needs both
     * sides. Create it first so the swap has something to swap with -- this is the
     * FIRST-RUN case, not an edge case, so it is handled rather than assumed away. */
    (void)FSpCreate(&spec, 'BTcp', 'BTBa', smSystemScript);
    if (FSpExchangeFiles(&tmp, &spec) == noErr) {
        KfAdoptSignature(&spec);
        (void)FSpDelete(&tmp);
        gBatDirty = 0;
        (void)FlushVol(0, vRefNum);
    } else {
        gBatFlushErr++;
        (void)FSpDelete(&tmp);
    }
}
