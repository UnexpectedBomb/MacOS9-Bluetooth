/*
 *  bt_defer.c  --  get from SECONDARY INTERRUPT LEVEL to TASK LEVEL, on demand.
 *
 *  THE PROBLEM THIS SOLVES
 *  -----------------------
 *  After ProbeInitialize returns, every line of this driver runs at secondary
 *  interrupt level (docs/M2-DESIGN.md §3c). But the link key store has to reach a
 *  FILE, and the File Manager is task level only -- calling it from a completion is
 *  a silent hard hang with no NMI and no log, which this project has hit three times
 *  (EHCI r18, n4, r95). So a bond can never survive a reboot without a way to run
 *  code at task level, triggered by something that happened at interrupt level.
 *
 *  THE MECHANISM, AND IT IS APPLE'S OWN
 *  ------------------------------------
 *  Reverse-engineered from Apple's USBMassStorageSupport and written up in
 *  ../usb2-ehci/docs/APPLE-UMSS-RE.md:
 *
 *      NMInstall with nmStr = 0 -- a notification with no string and no icon
 *      displays NOTHING. It exists purely so the Notification Manager invokes
 *      nmResp AT TASK LEVEL. NMInstall only enqueues, so it is callable from
 *      anywhere.
 *
 *  Apple uses exactly this to reach UnmountVol and Eject from a USB completion.
 *  Four call sites, all with nmStr = 0, all with procInfo 192.
 *
 *  ★ INDEPENDENTLY CORROBORATED: Notification.h:80 pins
 *  `uppNMProcInfo = 0x000000C0` = 192, precisely the procInfo that RE recorded.
 *
 *  ⚠⚠ WHY THE UPP IS BUILT AT TASK LEVEL AND NOT LAZILY
 *  NewNMUPP is a macro for NewRoutineDescriptor (Notification.h:82-84), and
 *  NewRoutineDescriptor is a MixedMode ALLOCATOR -- it is on the audit's FORBIDDEN
 *  list for exactly that reason. So the UPP must be created once, in
 *  ProbeInitialize, while we still have task level. Creating it on first use would
 *  put an allocator in a completion.
 *
 *  ⚠ WHAT THIS DOES NOT SOLVE. The NM queue is serviced from some application's
 *  event loop -- the Finder qualifies -- so BEFORE THE DESKTOP LOADS THERE MAY BE
 *  NO SUCH LOOP. A deferred flush queued during startup may not run until the
 *  desktop is up, and a prompt queued then may never appear at all. That is why the
 *  design never makes boot-time operation depend on this: the file is READ at
 *  ProbeInitialize, which is already task level, and only the WRITE is deferred.
 */

#include <Notification.h>
/* ⚠ stdbool BEFORE the Toolbox headers, and it is required. The build defines
 * TYPE_BOOL=1 so BTstack gets MacTypes.h to skip its own `enum { false, true }`,
 * but Folders.h then uses `true`/`false` itself and fails to compile with nothing
 * defining them. stdbool.h supplies both as macros. Same trap, same fix, as the
 * note at the top of bt_probe.c. */
#include <stdbool.h>
#include <MacTypes.h>

#include "bt_pump.h"            /* the counters bt_probe.c mirrors */
#include "bt_defer.h"
#include "bt_keyfile.h"
#include "bt_inject.h"          /* BT_MouseServiceAtTask -- the cursor device */

/* ⚠ STATIC, NOT ON THE STACK. NMInstall enqueues this record by POINTER and the
 * Notification Manager reads it later, at task level, long after the completion
 * that queued it has returned. A stack NMRec would be freed under it. */
static NMRec  gNM;
static NMUPP  gNMUPP;
static int    gReady;

/* volatile: written at interrupt level, read at task level and vice versa. */
static volatile int gQueued;

unsigned long gDeferReqs, gDeferRuns, gDeferInstallErrs, gDeferDropped;

/* ---- the task-level half -------------------------------------------------- *
 * ⚠ THIS RUNS AT TASK LEVEL. That is the entire point of the file, and it is what
 * makes the File Manager legal here. It is deliberately NOT an audit root, for the
 * same reason ProbeInitialize is not: the audit's roots are the interrupt-level
 * entry points, and adding a task-level proc would make every File Manager call it
 * reaches look like a violation when it is in fact the sanctioned path. */
static pascal void bt_defer_resp(NMRecPtr nmReqPtr)
{
    gDeferRuns++;

    /* NMRemove first, and unconditionally. The record must leave the queue before
     * anything else can go wrong, or a failure below would strand it and no further
     * notification could ever be installed. Compare
     * [[reference_os9_recovery_paths_fail_open]]: an early return that does not
     * release its flag strands the mechanism. */
    (void)NMRemove(nmReqPtr);
    gQueued = 0;

    /* Now do the actual task-level work. Everything reachable from here may use the
     * File Manager.
     *
     * ⚠ ORDER: the mode switch goes first. It is time-sensitive -- the card is waiting
     * to be transitioned and every retry costs a round trip -- whereas the key-file
     * flush is housekeeping that is equally correct a moment later. */
    BT_DeferredSwitchIfPending();
    BT_KeyFileFlushIfDirty();
    /* ★ v14.2: and the battery publish, for the same reason and in the same context --
     * task level, where the File Manager is legal. Housekeeping like the flush above:
     * equally correct a moment later, so it goes last. */
    BT_BatteryFlushIfDirty();
    /* ★ and the device names, same context and same reason as the battery file. */
    BT_NamesFlushIfDirty();
    /* ★ mouse: create the cursor device if a report has arrived and there is none yet.
     * CursorDeviceNewDevice allocates, so it cannot run at the interrupt level where the
     * report was decoded. Idempotent and free when nothing is wanted -- it checks its
     * own flag and returns. Self-healing if a request is coalesced away: every dropped
     * report asks again. */
    BT_MouseServiceAtTask();
    /* ★ and any owed low-battery warning. Last, because it can put a DIALOG in front
     * of the user: the file writes above must be on disk before anything blocks on a
     * click. */
    BT_BatteryWarnIfPending();
}

/* ---- the interrupt-level half -------------------------------------------- */

/* ================================================================================
 *  A USER-VISIBLE ALERT, for the low-battery warning.
 * ================================================================================
 *
 * ⚠⚠ A SECOND NMRec, NOT THE DEFER ONE. gNM above deliberately has nmStr = 0 so it
 * displays NOTHING -- it exists only to borrow task level. This record is the opposite:
 * it exists to put a string in front of the user. Reusing gNM would either silence this
 * alert or make every task-level hop pop a dialog, and only one notification per record
 * may be outstanding at a time.
 *
 * ⚠ THE STRING IS COPIED INTO STATIC STORAGE. The Notification Manager keeps the record
 * and the string BY POINTER until the user dismisses the alert, which can be minutes. A
 * caller's buffer -- let alone a stack one -- would be long gone. Same reasoning as the
 * static NMRec above, applied to the text.
 */
static NMRec        gAlertNM;
static NMUPP        gAlertUPP;
static short        gAlertReady, gAlertQueued;
static unsigned char gAlertStr[256];

unsigned long gAlertsPosted, gAlertsDropped, gAlertErr;

static pascal void bt_alert_resp(NMRecPtr nmReqPtr)
{
    /* ⚠ NMRemove first and unconditionally, exactly as the defer response does: the
     * record must leave the queue before anything else can fail, or the alert can never
     * be posted again [[reference_os9_recovery_paths_fail_open]]. */
    (void)NMRemove(nmReqPtr);
    gAlertQueued = 0;
}

/* ⚠ TASK LEVEL ONLY, from ProbeInitialize, for the same NewRoutineDescriptor reason as
 * BT_DeferInit. */
void BT_AlertInit(void)
{
    gAlertUPP = NewNMUPP(&bt_alert_resp);
    if (gAlertUPP == NULL) return;      /* no trampoline; alerts simply never appear */

    gAlertNM.qType      = nmType;
    gAlertNM.nmMark     = 0;
    gAlertNM.nmIcon     = NULL;
    gAlertNM.nmSound    = NULL;
    gAlertNM.nmStr      = NULL;         /* set per post, from gAlertStr */
    gAlertNM.nmResp     = gAlertUPP;
    gAlertNM.nmRefCon   = 0;
    gAlertNM.nmFlags    = 0;
    gAlertNM.nmPrivate  = 0;
    gAlertNM.nmReserved = 0;

    gAlertQueued = 0;
    gAlertReady  = 1;
}

/* ⚠ TASK LEVEL. Takes a PASCAL string and copies it. Coalesces: one alert outstanding at
 * a time, because installing the same static record twice corrupts the queue. A dropped
 * alert is counted rather than swallowed -- if the log ever shows drops, the policy
 * above it is posting too often, which is a policy bug worth seeing. */
void BT_PostAlert(const unsigned char *pstr)
{
    OSErr err;

    if (!gAlertReady || pstr == NULL || pstr[0] == 0) return;
    if (gAlertQueued) { gAlertsDropped++; return; }

    BlockMoveData(pstr, gAlertStr, (Size)(pstr[0] + 1));
    gAlertNM.nmStr = (StringPtr)gAlertStr;

    gAlertQueued = 1;
    err = NMInstall(&gAlertNM);
    if (err != noErr) {
        gAlertQueued = 0;               /* ⚠ fail OPEN: never latch the flag on error */
        gAlertErr = 0x100UL | (unsigned long)(unsigned char)err;
        return;
    }
    gAlertsPosted++;
}

/* ⚠ TASK LEVEL, from ProbeFinalize. A queued notification whose record and string live
 * in a fragment that is going away would leave the Notification Manager holding dangling
 * pointers -- the exact hazard os9_control_strip_module records for a CSM's NMRemove. */
void BT_AlertFinalize(void)
{
    if (gAlertQueued) {
        (void)NMRemove(&gAlertNM);
        gAlertQueued = 0;
    }
    gAlertReady = 0;
    if (gAlertUPP != NULL) { DisposeNMUPP(gAlertUPP); gAlertUPP = NULL; }
}

void BT_DeferInit(void)
{
    /* ⚠ TASK LEVEL ONLY -- called from ProbeInitialize. See the header comment on
     * NewRoutineDescriptor. */
    gNMUPP = NewNMUPP(&bt_defer_resp);
    if (gNMUPP == NULL) return;         /* no trampoline; the flush simply never runs */

    gNM.qType     = nmType;
    gNM.nmMark    = 0;
    gNM.nmIcon    = NULL;
    gNM.nmSound   = NULL;
    gNM.nmStr     = NULL;   /* ⚠ 0 = display NOTHING. This is the whole trick. */
    gNM.nmResp    = gNMUPP;
    gNM.nmRefCon  = 0;
    gNM.nmFlags   = 0;
    gNM.nmPrivate = 0;
    gNM.nmReserved = 0;

    gQueued = 0;
    gReady  = 1;
}

void BT_DeferRequest(void)
{
    OSErr err;

    if (!gReady) return;

    /* ⚠ COALESCE, and count what that drops. A pairing burst can set the dirty flag
     * several times before the Notification Manager gets a turn, and installing the
     * same static NMRec twice would corrupt the queue. One outstanding notification
     * is enough: the response reads the CURRENT dirty state, so a drop here loses
     * nothing except a redundant wakeup. Counting it keeps that claim checkable
     * rather than assumed. */
    if (gQueued) { gDeferDropped++; return; }

    gQueued = 1;
    gDeferReqs++;

    err = NMInstall(&gNM);
    if (err != noErr) {
        /* ⚠ RELEASE THE FLAG ON FAILURE. Leaving it set would mean no notification
         * could ever be queued again -- the mechanism would latch itself off after
         * one transient error. */
        gQueued = 0;
        gDeferInstallErrs++;
    }
}

void BT_DeferFinalize(void)
{
    /* Task level, from ProbeFinalize. Pull any outstanding notification before the
     * code holding the UPP goes away, or the NM would later call into freed memory. */
    if (gQueued) {
        (void)NMRemove(&gNM);
        gQueued = 0;
    }
    if (gNMUPP != NULL) {
        DisposeNMUPP(gNMUPP);
        gNMUPP = NULL;
    }
    gReady = 0;
}
