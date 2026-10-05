/*
 *  bt_panel.c  --  Bluetooth control panel, SCAFFOLDING (v0.2)
 *
 *  Container rationale in docs/CPANEL-SPIKE.md: this is a PowerPC application in the
 *  Control Panels folder, because the classic 'cdev' message protocol is entirely
 *  absent from these Universal Interfaces and Mac OS 8.5+ implements control panels
 *  as applications. v0.1 proved that launches and presents correctly.
 *
 *  v0.2 is DELIBERATELY NON-FUNCTIONAL. It exists to get the UI elements in place --
 *  a real selection table, a description pane, a real button stack, real platinum --
 *  so the layout can be judged before any of it is wired to the driver. Nothing here
 *  sends anything anywhere.
 *
 *  ⚠ THE LIST ROWS ARE PLACEHOLDERS and the description pane says so on screen. A
 *  scaffold that looks finished is worse than one that admits what it is; this
 *  project has already lost a run to a readout that asserted something untrue.
 *
 *  Modelled on the Extensions Manager control panel, per the user's direction: List
 *  Manager selection table, description pane beneath it, button stack on the right.
 *  Platinum comes from the Appearance Manager rather than hand-drawn greys, so it
 *  tracks the user's actual theme instead of guessing at it.
 */

#include <Appearance.h>
#include <Controls.h>
/* ⚠ pushButProc / radioButProc live in ControlDefinitions.h, NOT Controls.h, in
 * these Universal Interfaces. Including only Controls.h compiles the declarations
 * but leaves the proc IDs undefined, so the error lands on NewControl rather than on
 * the missing header. */
#include <ControlDefinitions.h>
#include <Devices.h>      /* OpenDeskAcc, for the Apple menu's desk accessories  */
#include <Dialogs.h>
#include <Files.h>        /* FSSpec, FSMakeFSSpec                                */
#include <Folders.h>      /* FindFolder, kExtensionFolderType                    */
#include <Resources.h>    /* FSpOpenResFile, Get1Resource -- the extension's vers */
#include <Events.h>
#include <Fonts.h>
#include <Lists.h>
#include <Menus.h>
#include <ToolUtils.h>    /* HiWord / LoWord, for MenuSelect's packed result     */
#include <USB.h>          /* app-level enumeration only -- see FindBluetoothHardware */
#include <Quickdraw.h>
#include <TextEdit.h>
#include <Balloons.h>   /* HMGetHelpMenuHandle, kHMHelpMenuID */
#include <Windows.h>
#include <Memory.h>
#include <string.h>

/* ---- the driver's counter block, exactly as BTCheck finds it --------------- *
 * ⚠ These MUST agree with the kW* enum in src/bt_probe.c. That agreement has been
 * broken five times in this project's history, which is why the driver carries a
 * build tag and every reader checks it. The panel becomes a WRITER once §5c's
 * command channel exists, and §5c is explicit that a writer which disagrees with the
 * block is worse than a reader that does -- so it verifies before it ever writes. */
#define kWMagic   0
#define kWBuild   1
/* ---- liveness counters, for the status file. ⚠ Offsets taken from the driver's own
 * enum in src/bt_probe.c (kWArmInt, kWIntComp, ... at 12/13; kWStkState = 64), not
 * counted by hand. They are here so a freeze can be attributed to a LEVEL. */
#define kWIntComp  13          /* interrupt-IN completions: the driver's pulse   */
/* ⚠ v18.9: read so the Card switch line can tell "no A1044 present" from "the
 * switcher is missing". vendor<<16 | product of whatever the DRIVER bound.
 * check-block-words.py fails the build if this drifts from src/bt_probe.c. */
#define kWIfaceVIDPID 155
#define kWStkPump  65          /* BTstack run-loop iterations                    */
#define kWEnd        794   /* ⚠ MUST track src/bt_probe.c -- CMake fails the build if not */

/* ---- M6: the scan channel. ⚠⚠ THESE MUST MATCH src/bt_probe.c EXACTLY ---------
 * The panel is a separately built binary, so a disagreement here is not a compile
 * error -- it is a button that does the wrong thing, or a list drawn from the wrong
 * words. That is why the driver carries a build tag and why this panel checks it
 * before it writes anything (§5c: "a writer that disagrees with the block is worse
 * than a reader that does"). */
#define kWCmd       192
#define kWCmdArg0   193
#define kWCmdArg1   194
#define kWCmdSeq    195
#define kWCmdAck    196
#define kWCmdResult 197
#define kWScanState 198
#define kWScanCount 199
#define kWScanBase  200
#define kMaxScan     16

/* ---- M7: addresses the driver holds link keys for. Two words each. */
#define kWKeyCount  264
#define kWKeyBase   265
/* ⚠ The controller's OWN bonded addresses, 4 slots x 2 words. MUST track
 * src/bt_probe.c's kWRlkA0Hi. */
/* ⚠ v10.7: MUST match src/bt_probe.c. The Disconnect button is enabled only when the
 * selected row IS the live peer, and these three words are how the panel knows. */
/* ⚠ MUST match src/bt_probe.c. Whether the HID CONTROL CHANNEL is open -- a separate
 * fact from an ACL link, and the one an input device actually depends on.
 *
 * ⚠⚠ TWO HID PATHS PUBLISH TWO DIFFERENT PAIRS OF WORDS, AND ONLY ONE PATH RUNS.
 * kWHidOpened/kWHidClosed come from the raw L2CAP_EVENT_CHANNEL_OPENED handler;
 * kWBhOpened/kWBhClosed come from BTstack's own hid_host. With kHidUseBtstackHost = 1
 * -- the shipping configuration -- hid_host owns the channels and the raw handler is
 * DORMANT, so kWHidOpened stays 0 forever. Reading only that word is why a keyboard
 * that was typing read "Linked, not ready". Both pairs are read now, so the status is
 * right whichever path is carrying the device. */
#define kWHidOpened     292
#define kWHidClosed     293
#define kWBhOpened      563
#define kWBhClosedMs    567
#define kWBhOpenedMs    565
#define kWBhClosed      566
#define kWBhLive        706   /* v13.9: the maintained "a channel is open" flag */
/* ★★★ 15.9: the two words BTCheck's "PAIRING SAFETY NET" is computed from. Both zero
 * means no copy of the key exists anywhere on this Mac, so Delete is a one-way door. */
#define kWLkStored      118
#define kWRlkCaptured   656
#define kWLinkUp        662
#define kWBlockHi       664
#define kWBlockLo       665
#define kWBlockActive   666
#define kWLiveCount     668
#define kWLiveA0Hi      669
#define kWNameCount     675
#define kWNameBase      676
#define kWConnPeerHi    311
#define kWConnPeerLo    312
#define kWRlkA0Hi   315
/* v8.2: the addresses that have PAGED this Mac. ⚠ Must match src/bt_probe.c's
 * kWPagedCount / kWPagedA0Hi and kPagedSlots -- a stride of 3, not 2. */
#define kWPagedCount 447
#define kWPagedA0Hi  448
#define kPagedSlots  4
#define kMaxKeys      8
#define kWKeyHandedMask 281
#define kWRadioOn       282
/* ⚠ MUST match src/bt_probe.c. "An inquiry is already running" -- not a failure. */
#define kBTAlreadyScanning (-1100)
/* Panel-only: a probe entry point was called and deliberately started nothing. */
#define kBTProbeDidCall    (-1101)
/* ⚠ MUST match src/bt_probe.c. "That address held no key" -- not success, not failure. */
#define kBTNoSuchBond      (-1102)

#define kCmdNone         0
#define kCmdStartInquiry 1
#define kCmdSetRadio     2
#define kCmdDeleteBond   3
/* ★★★★ M3.7, and the NUMBER must match src/bt_probe.c's kCmdPairDevice. The panel is
 * a separately built binary, so a renumbering silently changes what a button does --
 * which is why the driver's enum carries the same warning. */
#define kCmdPairDevice   4
/* ★★★★ ⚠ DESTRUCTIVE: removes a pairing from the CARD's own firmware store.
 * Number must match src/bt_probe.c's kCmdDelStoredKey. */
#define kCmdDelStoredKey 5
/* ★ v7.6: hand the card back to Mac OS. ⚠ Number must match src/bt_probe.c's
 * kCmdSwitchToProxy -- the two binaries are built separately. */
#define kCmdSwitchToProxy 6
/* ⚠ Number must match src/bt_probe.c's kCmdRestoreKey. */
#define kCmdRestoreKey   7
/* ⚠ Number must match src/bt_probe.c's kCmdDisconnect. */
#define kCmdDisconnect   8
/* ⚠ Number must match src/bt_probe.c's kCmdAllowDevice. */
#define kCmdAllowDevice  9

/* The servicer's counters. ⚠ These live in words that were SPARE, so kWEnd stays 303
 * and BTCheck v61 still finds the block. */
#define kWTimerRuns   299
#define kWMbxServiced 300
#define kWMbxStale    301

/* ★★★ THE MAILBOX IS THE NORMAL PATH AGAIN, and the driver's own timer services it.
 *
 * ⚠⚠ WHY THIS REPLACES THE DIRECT CALL. The run of 2026-09-03 established that a
 * command issued from a secondary interrupt handler an APPLICATION started is what
 * makes the USB Expert unload the driver: 3 teardowns in 11 presses of the real
 * handler, against 0 in 5 presses of an identical hop with an empty body and 0 across
 * 119 s idle. Writing six words into the shared block instead touches no fragment, no
 * CFM, no interrupt level -- there is nothing for the Expert to react to.
 *
 * ⚠ THE MAILBOX WAS RETIRED ONCE, and for a documented reason worth repeating here so
 * nobody restores the old servicer by mistake: it used to be read only on a USB
 * completion, and after bring-up the interrupt-IN read settles into a blocking wait,
 * so on an idle radio it never ran. Run 47: seq 9, ack 2, no new inquiry. The mailbox
 * was never the wrong idea; it had no reliable servicer. Driver v5.5 adds a
 * SetPersistentTimer that does nothing else.
 *
 * ⚠ SEQUENCE LAST. Arguments must be in place before the trigger word moves, because
 * the servicer reads the arguments only after seeing the sequence change.
 *
 * ⚠ Defined below, after gBlock: these touch the block and the pointer has to exist
 * first. The rationale lives here with the protocol constants. */
static OSStatus MailboxSend(unsigned long cmd, unsigned long a0, unsigned long a1);
static Boolean  MailboxCaughtUp(void);

/* ⚠⚠ THE EXACT DRIVER BUILD THIS PANEL MAY WRITE TO. Reading a block whose layout
 * has moved shows nonsense; WRITING to one corrupts it, and the command area sits at
 * word 192 where an older 192-word block had its terminator. So the panel reads any
 * block whose tag starts with 'v' and writes only to this one. */
/* ⚠ DEAD, and kept only because scripts/bump-version.py tracks it. The panel finds
 * the block by 'BTP1' + a leading 'v' + 'ENDS' at kWEnd and never compares this tag,
 * so it drifted to v8.1 (with a comment saying 'v650') across two driver versions
 * without consequence. BTCheck's copy of the same idea is live and now CMake-guarded;
 * this one is a decoy. Do not start using it without the guard. */
#define kExpectedDriverTag 0x76483630UL   /* 'vH60' = driver v17.6 */

/* ★★★★★★ THE SWITCHER'S OWN BLOCK, AND WHY THE PANEL NOW READS IT.
 *
 * ⚠⚠ THE PANEL COULD NOT TELL TWO OPPOSITE STATES APART, AND IT COST TWO REBOOTS ON
 * 2026-09-17. "Mac OS is handling Bluetooth" is what it says whenever there is no driver
 * block -- which is true BOTH when the card was never switched AND when it was switched
 * and the driver then failed to bind. Those need completely different responses, and
 * separating them was the entire object of that day's bisect. The one visible indicator
 * could not do it, so the answer had to come from BTCheck every single time.
 *
 * The switcher publishes its own 'BTSW' block in the System heap, and BTCheck has read
 * it for months. The panel simply never looked.
 *
 * ⚠ POSITIONAL, MIRRORING src/bt_switch.c's ENUM IN ORDER. These are indices into
 * somebody else's structure and a silent off-by-one reports a healthy switcher as
 * broken, so scripts/check-block-words.py verifies this enum against that one and the
 * build fails if they disagree. */
enum {
    kSwMagic = 0, kSwBuild, kSwValidate, kSwInit, kSwFinal,
    kSwNotifyC, kSwNotifyCode, kSwSeenProxy, kSwSeenOther, kSwOtherVid,
    kSwTried, kSwImmErr, kSwStatus, kSwStalls, kSwClearRc,
    kSwDeferRuns, kSwDeferErr, kSwVerdict, kSwStoodDown, kSwIdleDecline,
    kSwFlagSeen, kSwFlagErr, kSwMarkStale, kSwMarkWrote, kSwMarkErr,
    /* ⚠ Switcher 1.4 added kSwRefRetry BEFORE kSwEnd, so the terminator moved a THIRD
     * time. A stale index here reports NO 'BTSW' BLOCK over a perfectly healthy
     * switcher -- the Card switch line just goes quiet. scripts/check-block-words.py
     * catches this one; it did, which is why this line exists. */
    kSwRefRetry,
    /* ⚠ Switcher 1.5 added kSwMarkCleared BEFORE kSwEnd -- FIFTH move of this terminator. */
    kSwMarkCleared,
    kSwEnd, kSwCount
};
#define kSwMagic0     0x42545357UL   /* 'BTSW' */
#define kSwBuildMask  0xFFFF0000UL
#define kSwBuildVer   0x73770000UL   /* 'sw..' */

static unsigned long *gSwBlock = NULL;

#define kMagic0       0x42545031UL   /* 'BTP1' */
#define kMagicEnd     0x454E4453UL   /* 'ENDS' */
#define kMagicVer     0x76000000UL   /* leading 'v' only */
#define kMagicVerMask 0xFF000000UL

static unsigned long *gBlock = NULL;

/* The mailbox writers. See the protocol comment above kCmdStartInquiry for why this
 * is the normal path and what happened the last time it had no servicer. */
static OSStatus MailboxSend(unsigned long cmd, unsigned long a0, unsigned long a1)
{
    if (gBlock == NULL) return (OSStatus)cfragNoLibraryErr;
    gBlock[kWCmd]     = cmd;
    gBlock[kWCmdArg0] = a0;
    gBlock[kWCmdArg1] = a1;
    /* ⚠ LAST. The servicer reads the arguments only after it sees this move. */
    gBlock[kWCmdSeq]  = gBlock[kWCmdSeq] + 1;
    return noErr;
}

/* Has the driver picked up everything we asked for? ⚠ This is the instrument that
 * makes run 47's failure mode LOUD instead of silent: a mailbox nobody services shows
 * as a permanent gap between these two, and the pane says so. */
static Boolean MailboxCaughtUp(void)
{
    if (gBlock == NULL) return false;
    return (gBlock[kWCmdSeq] == gBlock[kWCmdAck]);
}

/* ---- is there any Bluetooth HARDWARE on the bus? -------------------------- *
 * ★★ WHY THE PANEL HAS TO ANSWER THIS ITSELF.
 *
 * v0.4 said "No Bluetooth driver is loaded" and then GUESSED at the reason, and the
 * user reasonably replied that the extension is plainly sitting in Extensions. Both
 * statements were true at once, because they are about different things:
 *
 *   the extension is a FILE. The driver is an 'ndrv' that the USB Expert loads ONLY
 *   WHEN IT MATCHES A DEVICE. No Bluetooth controller on the bus means no driver
 *   instance, no EnsureBlock, and no counter block -- with the file present and
 *   enabled the whole time.
 *
 * The correlation is total: runs 35, 40, 41 and 43 all reported NO BLOCK FOUND, and
 * in all four the card was absent from the bus. So the panel should look at the bus
 * and say which case it is, instead of offering the user a guess to check.
 *
 * ⚠ USBGetNextDeviceByClass and USBGetDeviceDescriptor are the APP-LEVEL half of the
 * USB Manager -- enumerate and read descriptors, no transfers -- which is exactly
 * what an application is permitted. BTCheck has used them since v21. They need
 * USBManagerLib, and kUSBAnyClass is 0xFFFF, NOT 0. */
/* ⚠ PLURAL. The user's machine has BOTH an internal A1044 and a USB dongle attached,
 * and run 47 confirmed OS 9 enumerates both at once (on two different controllers --
 * ASP shows USB 0 and USB 1). A single-module variable would report one and silently
 * hide the other, which is the same class of half-truth the readouts keep producing. */
#define kMaxModules 4
static short          gModuleCount = 0;
static unsigned short gHwVendor[kMaxModules], gHwProduct[kMaxModules];
#define gHasHardware  (gModuleCount > 0)

static unsigned short leWord(unsigned short v)   /* descriptors are little-endian */
{
    return (unsigned short)(((v & 0x00FFu) << 8) | ((v >> 8) & 0x00FFu));
}

/* ---- M6b: calling the driver directly, per the 2003 prior art ---------------- */
typedef OSStatus (*BTScanStartProc)(void);
typedef OSStatus (*BTSetRadioProc)(long on);
/* The connection array is gone: ResolveDriverEntries does its own walk inside
 * SetZone(SystemZone()), which is the part that was missing. Collecting connections in
 * one zone and resolving them in another was the shape of the bug. */
/* ⚠ NO CACHED PROC POINTERS HERE, DELIBERATELY. They existed and they crashed the
 * machine on the second scan -- a stale TVector into an unloaded fragment. Keeping
 * them "just for the fast path" would reintroduce exactly that. Resolve per call. */
static OSStatus          gLastScanRc = noErr;   /* what the last direct call returned */
static OSStatus          gLastRadioRc = noErr;
static OSStatus          gLastDeleteRc = noErr;
/* ⚠ Which KIND of delete, so the report cannot say a bond was forgotten when there was
 * never one to forget. */
static Boolean           gDeleteListOnly = false;
static Boolean           gDeleteTried = false;
/* ⚠ StandardAlert's own result, kept because the previous version threw it away and a
 * refused alert then looked exactly like a cancelled one. */
static OSStatus          gAlertRc  = 1;      /* 1 = never called */
static short             gAlertHit = 0;
/* ⚠ Mirrors the driver's published state, and starts TRUE because bring-up enables
 * both scans -- the radio is on before the panel ever opens. Defaulting to off would
 * draw a lie until the first poll corrected it. */
static Boolean           gRadioOn = true;
/* ★ Set when the DRIVER's radio state changed under us, so the main loop knows to
 * repaint the group-box logo. See the radio block in PollScan for why this is a flag
 * rather than a draw call. */
static Boolean           gRadioLogoDirty = false;
/* ★ THE COMPONENTS MESSAGE HAS TO BE ABLE TO CLEAR ITSELF MID-SESSION, and with the
 * switcher extension that is now the NORMAL case rather than an edge one: the panel
 * can be open while the card is still in HID-proxy, and a moment later the switcher
 * moves it, the card re-enumerates at 8204, and the driver binds. The message must go
 * away when that happens instead of sitting there contradicting a working radio.
 *
 * Same dirty-flag shape as gRadioLogoDirty, and for the same reason: SyncButtons runs
 * from contexts that have no business drawing, so it records that a repaint is owed
 * and the event loop pays it. -1 rather than a Boolean so the FIRST SyncButtons always
 * paints -- "unknown" and "false" are different, and starting at false would leave the
 * message unpainted until the state happened to change. */
static short             gCompState = -1;
static Boolean           gCompMsgDirty = false;
/* ★ The Pair button's own outcome, reported in the pane the same way the Delete
 * button's is -- "nothing happened" is the one report that cannot be acted on. */
static OSStatus          gLastPairRc = noErr;
static Boolean           gPairTried  = false;

static void FindBluetoothHardware(void)
{
    USBDeviceRef      ref = 0;
    CFragConnectionID connID;
    short             n = 0;

    for (;;) {
        USBDeviceDescriptor d;
        unsigned short v, p;

        if (USBGetNextDeviceByClass(&ref, &connID, kUSBAnyClass,
                                    kUSBAnySubClass, kUSBAnyProtocol) != noErr) break;
        if (++n > 32) break;
        memset(&d, 0, sizeof(d));
        if (USBGetDeviceDescriptor(&ref, &d, (UInt32)sizeof(d)) != noErr) continue;

        v = leWord(d.vendor);
        p = leWord(d.product);

        /* A Bluetooth controller, in any of the identities this project has measured:
         * the A1044's proxy (05AC:1000), its post-switch state (8202), the working
         * one Tiger drives (8204) and its siblings 8203/8205/8206/8207, CSR's own
         * vendor 0x0A12, or anything declaring device class 0xE0. */
        /* ⚠⚠ 0x8202 IS DELIBERATELY EXCLUDED. ASP names it "Communication (Apple
         * internal modem)", vendor "Apple Computer (HCF USB V.90 Data/Fax Modem)",
         * driver "Internal USB Modem". Listing the user's modem as a Bluetooth module
         * would be a plain falsehood, and a rule pinned to it shipped in the driver
         * for six versions before the ASP report caught it. 8203-8207 stay, since
         * those are Apple's real Bluetooth HCI product IDs per
         * CSRUSBBluetoothHCIController's own Info.plist. */
        if ((v == 0x05AC && (p == 0x1000 || (p >= 0x8203 && p <= 0x8207))) ||
             v == 0x0A12 || d.deviceClass == 0xE0) {
            /* ⚠⚠ DE-DUPLICATE BY VID:PID. Panel 0.9 listed "Internal Bluetooth Card"
             * TWICE, and the reason matters: OS 9 enumerates a device-class-0x00
             * composite ONCE PER INTERFACE, so the A1044 appears on the bus as THREE
             * entries -- run 34 established that. Adding each match made one physical
             * card look like three modules, filled the four slots, and pushed the
             * dongle off the end of the line entirely.
             *
             * One physical module per entry, keyed on VID:PID. */
            {
                short k; Boolean seen = false;
                for (k = 0; k < gModuleCount; k++)
                    if (gHwVendor[k] == v && gHwProduct[k] == p) { seen = true; break; }
                if (!seen && gModuleCount < kMaxModules) {
                    gHwVendor[gModuleCount]  = v;
                    gHwProduct[gModuleCount] = p;
                    gModuleCount++;
                }
            }

            /* ★★ AND KEEP THE CFM CONNECTION, which this loop has been receiving and
             * discarding all along.
             *
             * USBGetNextDeviceByClass hands back the CFragConnectionID of the driver
             * bound to each device. That is the handle the 2003 control panel in
             * vendor/bt-control-center used to FindSymbol its way into the driver and
             * call it directly -- the mechanism docs/SCAN-DESIGN.md §5 wrongly said did
             * not exist between an app and an 'ndrv'.
             *
             * ⚠ Collected per CONNECTION, not per module: the de-duplication above is
             * keyed on VID:PID because one physical card enumerates once per interface,
             * but each of those entries can carry a DIFFERENT fragment copy, and only
             * one of them bound the device. TryResolveDriver walks them all. */
            continue;               /* keep looking -- there may be more than one */
        }
    }
}

/* ★★ RESOLVE THE DRIVER'S ENTRY POINT.
 *
 * ⚠ FindSymbol succeeding is NOT the same as having found the RIGHT copy. An
 * INIT-installed driver exists as more than one fragment copy with separate globals,
 * and the A1044 alone puts three connections on this list. Every copy exports
 * BTScanStart, but only the one that actually bound the device has a started BTstack
 * behind it -- so a copy that resolves cleanly can still be the wrong one.
 *
 * The driver settles it rather than the panel guessing: an unbound copy returns
 * kUSBDeviceBusy from BTScanStart without touching its stack. So the test for "is this
 * the right copy" is to CALL it, and that is only sound because the call is harmless
 * on the wrong copy -- which is a property of the driver's own guard, not an accident.
 * On success the pointer is cached and no further probing happens. */
/* ⚠⚠ RESOLVED THE PRIOR ART'S WAY, because ours did not work.
 *
 * Run 52: "driver has no scan entry point" on every press, with the driver demonstrably
 * loaded, bound to the dongle and at HCI state WORKING. The export is genuinely in the
 * PEF export table -- the loader section was parsed directly and shows BTScanStart and
 * BTSetRadio as class 2 TVectors. So the failure was FindSymbol, not the driver.
 *
 * ★ This is almost certainly not a regression. It never worked. The arrows the user
 * remembers came from panel 1.0/1.1, where the block-and-poll SendCommand returned true
 * merely for having WRITTEN to the block, so the arrows spun whether or not anything
 * happened -- run 47 proved nothing was being serviced at all. The direct call only
 * shows arrows on a real noErr, so it has simply been honest about a failure that was
 * always there.
 *
 * Two differences from BTCC/BluetoothInterface.c, which is known to work:
 *
 *   1. ⭐⭐ It calls SetZone(SystemZone()) around the whole walk. FindSymbol resolves
 *      into a fragment living in the System heap; leaving the app zone current is the
 *      likelier of the two causes and is the one we simply never did.
 *   2. It asks for the Bluetooth class triple, not a wildcard. A connection ID returned
 *      for a wildcard match need not be the one we want.
 *
 * ⚠ The wildcard walk is kept as a FALLBACK rather than deleted, and which path
 * succeeded is recorded -- if the triple finds nothing on this hardware, that is itself
 * the finding, and losing the old path would hide it. */
static short    gResolveConns = 0;    /* connections offered by the class walk   */
static OSStatus gResolveRc    = noErr;/* last FindSymbol result                  */
static Boolean  gResolveTried = false;
static Boolean  gResolveByTriple = false;

/* ⚠⚠⚠ NEVER CACHE A RESOLVED TVECTOR. RESOLVE IMMEDIATELY BEFORE EVERY CALL.
 *
 * Panel 1.7 cached the pointer after the first successful resolve. The first scan
 * worked; the second crashed into MacsBug:
 *
 *     PowerPC illegal instruction at 5F451B4C
 *     5F451B4C  *dc.l  0x00DDDDDD
 *     Address 5F451B4C is in the Process Manager heap
 *     It is 0004882C bytes into this heap block:
 *        Start    Length      Tag
 *       5F409320 0004BE14+00   F          <- Tag F = FREE
 *
 * The PC was executing inside a FREED block filled with the Memory Manager's 0xDD
 * dispose pattern, with CTR and LR both in the same dead region -- the signature of a
 * bctrl through a TVector whose fragment had been unloaded.
 *
 * ★ THE WRONG ASSUMPTION: FindSymbol on an existing connection does NOT hold the
 * fragment alive. Unlike GetSharedLibrary it takes no reference, so a resolved pointer
 * is valid only until the USB Expert next tears that instance down -- and it does that
 * constantly: run 52 counted 13 Initialize calls in a single session.
 *
 * ⚠ Residual risk, stated rather than pretended away: the fragment could still be
 * unloaded between the resolve and the call. That window is microseconds at task level
 * instead of minutes, which is the difference between a bug that reproduces on the
 * second click and one that effectively does not happen. It cannot be closed from here
 * without holding a CFM reference, and taking one risks loading a SECOND copy of the
 * driver -- a worse failure than the one being fixed. */
/* ⚠⚠ BISECT SWITCH, and it is a MODIFIER KEY rather than a throwaway build.
 *
 * Run 55 proved one press costs exactly one driver teardown: Initialize 2, Finalize 1,
 * Notify 1, driver-being-removed 1, and inquiry state 0 -- the scan did not even run.
 * Something in ResolveNow unloads the driver. There are three candidates and they need
 * separating:
 *
 *   1. USBGetNextDeviceByClass with the EXACT TRIPLE 0xE0/0x01/0x01
 *   2. FindSymbol on the returned connection
 *   3. the call through the resolved TVector
 *
 * Launching the panel already does a WILDCARD walk and costs nothing (run 54:
 * Initialize 1, Notify 0), so the wildcard form is innocent and (1) means the triple
 * specifically.
 *
 * ★ Modifier-keyed so no reboot or throwaway build is needed.
 *
 * ★★ RESULT (panel 2.3): `acts W3>3`. The walk left the bind count UNCHANGED, so the
 * exact-triple USBGetNextDeviceByClass is INNOCENT. Two candidates remain, and the
 * second modifier separates them:
 *
 *   Option-click  = walk only                     -> W, proven harmless
 *   Shift-click   = walk + FindSymbol, NO call     -> S
 *   plain click   = walk + FindSymbol + call       -> N
 *
 * ★★★ RESULT (panel 2.4): acts S3>3. FindSymbol is innocent too. By elimination the
 * CALL is what unloads the driver -- which still spans "an app calling into a driver
 * fragment at all" versus "what our handler does once inside". Control-click calls
 * BTNoop, an exported function that touches NOTHING, to split those. */
enum { kProbeFull = 0, kProbeWalkOnly = 1, kProbeNoCall = 2, kProbeNoop = 3,
       kProbeNoopSIH = 4,
       /* ★★★ THE POSITIVE CONTROL, and the run after the mailbox landed cannot be
        * validated without it. Once the plain click routes to the mailbox, the OLD
        * direct-call path is unreachable by any modifier -- so a session with no
        * teardowns would be indistinguishable from a session that simply was not
        * churning, which is exactly the ambiguity that invalidated the first bisect.
        * This mode calls the real handler through CallSecondaryInterruptHandler2, the
        * thing PROVEN to churn, so one run can show both that the fix works and that
        * the mechanism was still live while it was being tested. */
       kProbeDirect = 5 };
static short gProbeMode = kProbeFull;
typedef OSStatus (*BTNoopProc)(void);

static OSStatus ResolveNow(BTScanStartProc *scanOut, BTSetRadioProc *radioOut)
{
    USBDeviceRef      ref;
    CFragConnectionID conn;
    CFragSymbolClass  cls;
    THz               save;
    Ptr               addr;
    short             pass;

    if (scanOut)  *scanOut  = NULL;
    if (radioOut) *radioOut = NULL;
    gResolveTried = true;
    gResolveConns = 0;
    gResolveRc    = noErr;

    /* ⚠ THE ZONE, and it is what made resolution work at all (panel 1.6 -> 1.7).
     * Restored on every exit -- leaving the System zone current would put every later
     * app allocation in the wrong heap. */
    save = GetZone();
    SetZone(SystemZone());

    /* Pass 0 = the prior art's exact triple (E0/01/01). Pass 1 = the wildcard walk,
     * kept as a fallback so that "the triple finds nothing here" stays visible. */
    for (pass = 0; pass < 2; pass++) {
        ref = 0;
        for (;;) {
            OSStatus e = (pass == 0)
                ? USBGetNextDeviceByClass(&ref, &conn, 0xE0, 0x01, 0x01)
                : USBGetNextDeviceByClass(&ref, &conn, kUSBAnyClass,
                                          kUSBAnySubClass, kUSBAnyProtocol);
            if (e != noErr) break;
            gResolveConns++;

            /* ★ THE BISECT. Walk only: no FindSymbol, no call. */
            if (gProbeMode == kProbeWalkOnly) continue;

            addr = NULL;
            gResolveRc = FindSymbol(conn, "\pBTScanStart", &addr, &cls);
            if (gResolveRc != noErr || addr == NULL) continue;
            if (scanOut) *scanOut = (BTScanStartProc)addr;
            gResolveByTriple = (pass == 0);

            if (radioOut) {
                addr = NULL;
                if (FindSymbol(conn, "\pBTSetRadio", &addr, &cls) == noErr && addr)
                    *radioOut = (BTSetRadioProc)addr;
            }
            SetZone(save);
            return noErr;
        }
    }

    SetZone(save);
    return (gResolveConns == 0) ? (OSStatus)cfragNoLibraryErr
                                : (gResolveRc != noErr ? gResolveRc
                                                       : (OSStatus)cfragNoSymbolErr);
}

static OSStatus DriverStartScan(void)
{
    BTScanStartProc scan = NULL;
    OSStatus        e;

    /* ★★ THE NORMAL PATH: leave it in the mailbox and let the driver's timer take it.
     * Placed FIRST so the plain click never touches CFM at all. The probe modes below
     * are the bisect instruments and are reached only by a modifier click. */
    if (gProbeMode == kProbeFull) return MailboxSend(kCmdStartInquiry, 0, 0);

    e = ResolveNow(&scan, NULL);
    if (e != noErr || scan == NULL) return e;
    /* ★ The bisect's second half: resolve fully, then deliberately DO NOT call, so a
     * bind-count change can only be attributed to FindSymbol. */
    if (gProbeMode == kProbeNoCall) return (OSStatus)cfragNoSymbolErr;

    /* ★★ Third split: call an entry point that does NOTHING. Resolved in the System
     * zone like everything else, so the only difference from a real scan is the body
     * of the function being entered. */
    if (gProbeMode == kProbeNoop || gProbeMode == kProbeNoopSIH) {
        USBDeviceRef      ref = 0;
        CFragConnectionID conn;
        CFragSymbolClass  cls;
        THz               save = GetZone();
        OSStatus          rc = cfragNoSymbolErr;
        /* kProbeNoop     -> BTNoop     : a bare exported function (proven innocent)
         * kProbeNoopSIH  -> BTNoopSIH  : same shape as BTScanStart, empty handler */
        const unsigned char *sym = (gProbeMode == kProbeNoopSIH)
                                 ? (const unsigned char *)"\pBTNoopSIH"
                                 : (const unsigned char *)"\pBTNoop";
        SetZone(SystemZone());
        while (USBGetNextDeviceByClass(&ref, &conn, 0xE0, 0x01, 0x01) == noErr) {
            Ptr a = NULL;
            if (FindSymbol(conn, sym, &a, &cls) == noErr && a != NULL) {
                SetZone(save);            /* ⚠ restore BEFORE calling out */
                (void)(*(BTNoopProc)a)();
                /* ⚠⚠ MUST NOT REPORT SUCCESS. BTNoop returns noErr, and the button
                 * handler reads noErr as "a scan is running" -- so it showed the
                 * chasing arrows and waited forever for an inquiry state that could
                 * never change, because no inquiry was ever started. The probe made
                 * the panel hang. A distinct code keeps the measurement (the call
                 * happened, and the action log already recorded the bind count either
                 * side of it) without lying about what it started. */
                return (OSStatus)kBTProbeDidCall;
            }
        }
        SetZone(save);
        return rc;
    }
    return (*scan)();
}

/* ★ THE ON/OFF CONTROL, now real. Same connection walk as DriverStartScan, and it
 * reuses whichever copy that already identified as the bound one -- the walk only
 * happens if no scan has been started yet. */
/* ★★ FORGET A BOND. Resolved per call like everything else -- see ResolveNow's comment
 * on why a cached TVector crashed the second press. */
typedef OSStatus (*BTDeleteBondProc)(long hi, long lo);

/* ★★★★ PAIR WITH THE SELECTED DEVICE, as the initiator.
 *
 * ⚠ Mailbox only, with no FindSymbol fallback. DriverDeleteBond keeps one because it
 * predates the mailbox; there is no reason to add a second path for a new command --
 * the churn rule exists because two ways in is how the churn started. If the mailbox
 * is unavailable the button reports that rather than quietly taking another route. */
static OSStatus DriverPairDevice(unsigned long hi, unsigned long lo)
{
    if (gProbeMode != kProbeFull) return cfragNoSymbolErr;
    return MailboxSend(kCmdPairDevice, hi, lo);
}

/* ★★★★ ⚠⚠ DELETE FROM THE CARD'S OWN STORE. Survives a reboot and Tiger shares it.
 * The caller must have confirmed the exact address with the user first. */
static OSStatus gLastHandBackRc = noErr;
static Boolean  gHandBackTried  = false;
static OSErr    gPairModeRc     = noErr;
static Boolean  gPairModeSet    = false;

/* ★★★ WRITE THE ONE-SHOT PAIR-MODE FLAG the switcher consumes at its next Initialize.
 *
 * Zero-length file, `Preferences:Bluetooth Pair At Restart`. Its EXISTENCE is the whole
 * message -- there is nothing to read inside it, which is deliberate: a flag with
 * contents is a flag that can be half-written, and the switcher checks for it during
 * early USB enumeration where the cheapest possible check is the right one.
 *
 * Returns a nonzero refnum on success, 0 on any failure. ⚠ The caller REPORTS the
 * result: a flag that silently failed to appear looks identical to a switcher that
 * ignored it, and the user would restart for nothing. */
static short PairFlagCreate(void)
{
    FSSpec spec;
    short  vRefNum, ref;
    long   dirID;
    OSErr  err;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return 0;
    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Pair At Restart", &spec);
    if (err != noErr && err != fnfErr) return 0;
    if (err == fnfErr) {
        /* 'BTcp' creator so it is obviously ours in the Finder, and a type nothing
         * opens -- this is a switch, not a document. */
        if (FSpCreate(&spec, 'BTcp', 'BTpf', smSystemScript) != noErr) return 0;
    }
    /* ⚠ Opened and closed rather than left alone, so "the file exists AND is usable"
     * is what gets reported rather than just "FSpCreate returned noErr". */
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return 0;
    (void)FSClose(ref);
    (void)FlushVol(NULL, spec.vRefNum);      /* ⚠ or a crash before the next restart
                                              * loses the very thing we just promised */
    return 1;
}

static OSStatus gLastRestoreRc;
static OSStatus gLastDiscRc;
static Boolean  gDiscTried;
/* ⚠ WHICH of the two the button sent, so the report can say what to expect next.
 * gDiscTried and gLastDiscRc were SET AND NEVER READ until v13.4: Delete reported its
 * outcome on the Scan line and Disconnect/Connect reported nothing at all, which is
 * most of why Connect read as a dead button. */
static Boolean  gDiscWasAllow;

/* ★★★★★ THE TRANSIENT AFTER Connect, AND WHY IT IS NOT CALLED "Attempting connection".
 *
 * ⚠⚠ WE DO NOT ATTEMPT ANYTHING. Connect sends kCmdAllowDevice, which stops this Mac
 * REFUSING the device. On this stack the keyboard is the initiator: it pages us when it
 * wakes. "Attempting connection" would describe an outgoing page we never send, and a
 * status that narrates work nobody is doing is the same overclaim as the "Connected"
 * that read true while the keyboard blinked and typed nothing.
 *
 * ⇒ "Waiting for device" says exactly what is happening, and it still does the job the
 * user asked of it: the row changes the instant you click, so the click is visibly not
 * ignored. The Configuration Details line carries the instruction ("press a key on the
 * device"), because the 21-character status column cannot.
 *
 * ⚠ IT EXPIRES, and that is not optional. A transient with no exit latches: the row
 * would read "Waiting for device" for the rest of the session about a keyboard that is
 * in a drawer. Thirty seconds, then it falls back to what it really is. */
#define kWaitTimeout 1800UL             /* ticks: 30 seconds */
static unsigned long gWaitHi, gWaitLo, gWaitTick;
static Boolean       gWaiting = false;
static Boolean  gRestoreDone;

static OSStatus DriverDeleteStoredKey(unsigned long hi, unsigned long lo)
{
    if (gProbeMode != kProbeFull) return cfragNoSymbolErr;
    return MailboxSend(kCmdDelStoredKey, hi, lo);
}

static OSStatus DriverDeleteBond(unsigned long hi, unsigned long lo)
{
    USBDeviceRef      ref = 0;
    CFragConnectionID conn;
    CFragSymbolClass  cls;
    THz               save = GetZone();
    OSStatus          rc = cfragNoSymbolErr;

    /* ★ Mailbox here too, completing the one rule. Delete issues no USB itself, so it
     * is not a demonstrated churn path -- but routing it differently from the other
     * two would mean the rule has an exception, and an exception is what future work
     * copies. The driver's audit confirms this command is CLEAN from interrupt level:
     * BT_DeleteBondByAddr touches only the RAM side of the key store and sets the
     * dirty flag, with the File Manager half deferred to task level as designed. */
    if (gProbeMode == kProbeFull) return MailboxSend(kCmdDeleteBond, hi, lo);

    SetZone(SystemZone());
    while (USBGetNextDeviceByClass(&ref, &conn, 0xE0, 0x01, 0x01) == noErr) {
        Ptr a = NULL;
        if (FindSymbol(conn, "\pBTDeleteBond", &a, &cls) == noErr && a != NULL) {
            SetZone(save);          /* ⚠ restore BEFORE calling out */
            return (*(BTDeleteBondProc)a)((long)hi, (long)lo);
        }
    }
    SetZone(save);
    return rc;
}

static OSStatus DriverSetRadio(Boolean on)
{
    BTSetRadioProc radio = NULL;
    OSStatus       e;
    /* ★ Same mailbox route as the scan, and for the same reason: this also sends HCI
     * commands, so leaving it on the direct-call path would leave a known way back to
     * the churn. One rule -- the application never enters the driver's fragment in
     * normal operation -- is far easier to keep true than a per-path judgement about
     * which commands are "safe enough". */
    if (gProbeMode == kProbeFull) return MailboxSend(kCmdSetRadio, on ? 1UL : 0UL, 0);
    e = ResolveNow(NULL, &radio);
    if (e != noErr || radio == NULL) return (e != noErr) ? e : (OSStatus)cfragNoSymbolErr;
    return (*radio)(on ? 1L : 0L);
}

static void FindDriverBlock(void)
{
    THz  zone = SystemZone();
    unsigned long lo = (unsigned long)zone->heapData;
    unsigned long hi = (unsigned long)zone->bkLim;
    unsigned long *p, *lim;

    /* ⚠⚠ THIS BOUND MUST SCALE WITH THE BLOCK, and for two versions it did not.
     *
     * The scan reads p[kWEnd] for every candidate, so it touches p + kWEnd*4 bytes.
     * A hardcoded 1024 was fine while the block was 256 words; at 288 words the last
     * candidates read up to 128 bytes PAST bkLim, every time the panel started. The
     * driver's own EnsureBlock got this right -- its limit is derived from kWCount --
     * and the panel's copy did not follow when the block grew.
     *
     * Almost certainly harmless in practice (it reads mapped RAM above the zone rather
     * than faulting) but it is an out-of-bounds read on every launch, and those are
     * exactly the defects that surface later as something unreproducible. */
    lim = (unsigned long *)(hi - ((kWEnd + 1) * 4 + 16));
    for (p = (unsigned long *)((lo + 3) & ~3UL); p < lim; p++) {
        if (p[kWMagic] == kMagic0 &&
            (p[kWBuild] & kMagicVerMask) == kMagicVer &&
            p[kWEnd] == kMagicEnd) { gBlock = p; break; }
    }

    /* ★ THE SWITCHER'S BLOCK, in the same walk. ⚠ A SEPARATE LOOP AND NOT AN `else` IN
     * THE ONE ABOVE: the two blocks are independent and either can exist without the
     * other -- indeed the interesting case for the user is exactly "no driver block, but
     * a switcher block that explains why". Bailing out of the walk on the first find
     * would have made the switcher invisible in precisely that state. */
    lim = (unsigned long *)(hi - ((kSwEnd + 1) * 4 + 16));
    for (p = (unsigned long *)((lo + 3) & ~3UL); p < lim; p++) {
        if (p[kSwMagic] == kSwMagic0 &&
            (p[kSwBuild] & kSwBuildMask) == kSwBuildVer &&
            p[kSwEnd] == kMagicEnd) { gSwBlock = p; return; }
    }
}

/* ---- is the EXTENSION FILE there, and what version? ----------------------- *
 * ★★ THIS IS WHAT MAKES "Extension detected" AND "Driver active" TWO DIFFERENT
 * FACTS, and it is the answer to the user's question about redundancy.
 *
 * Read from the counter block alone they WOULD be the same signal, because the block
 * only exists once the driver has loaded and bound something. Read from the FILE,
 * they separate -- and the gap between them is exactly the state this machine is in
 * every day now: extension present and correct, driver deliberately not bound to
 * anything because the mode switch ships off. That is precisely the confusion the
 * user hit when the panel said "no driver" with the extension plainly installed, so
 * the two lines earn their place.
 *
 * ⚠ The installed filename is USBBluetoothSupport. The Expert only scans Extensions
 * files whose name starts with "USB", so that name is load-bearing, not cosmetic. */
static Boolean gExtPresent = false;
static Str255  gExtVers;

static void FindExtensionFile(void)
{
    short   vRef, refNum, saveRes;
    long    dirID;
    FSSpec  spec;
    Handle  h;

    gExtVers[0] = 0;
    if (FindFolder(kOnSystemDisk, kExtensionFolderType, kDontCreateFolder,
                   &vRef, &dirID) != noErr) return;
    /* ★★★★ v18.1: EITHER NAME. The Expert only scans Extensions files whose name begins
     * "USB" (measured 2026-08-30 by renaming one file with no rebuild: the non-USB name
     * never appeared in the scan at all). That rule permits "USB Bluetooth Support",
     * which reads far better than the run-together form -- the 2003 prior art called
     * itself "USB Bluetooth Driver", with spaces -- but whether a SPACE after USB is
     * accepted has never been tested.
     *
     * ⚠ This function is why the test needs this change FIRST. It matched one exact
     * name, so renaming the extension by hand would have made the panel report the
     * extension MISSING -- indistinguishable, to anyone reading the panel, from the
     * rename having broken the driver. A test whose failure mode mimics the thing it is
     * testing is not a test. Both names are accepted until one is chosen. */
    if (FSMakeFSSpec(vRef, dirID, "\pUSB Bluetooth Support", &spec) != noErr
     && FSMakeFSSpec(vRef, dirID, "\pUSBBluetoothSupport",   &spec) != noErr) return;
    gExtPresent = true;             /* the file exists; the version is a bonus */

    /* ⚠ SAVE AND RESTORE THE CURRENT RESOURCE FILE. Opening a resource fork makes it
     * current, and leaving it that way would send every later Get1Resource of ours
     * into the driver's fork. Read-only permission, and closed immediately. */
    saveRes = CurResFile();
    refNum = FSpOpenResFile(&spec, fsRdPerm);
    if (refNum != -1) {
        h = Get1Resource('vers', 1);
        if (h != NULL && *h != NULL) {
            /* 'vers' layout: 4 bytes numeric, then a Pascal short version string. */
            unsigned char *p = (unsigned char *)*h;
            short n = p[6];
            if (n > 0 && n < 32) { gExtVers[0] = (unsigned char)n;
                                   BlockMoveData(&p[7], &gExtVers[1], n); }
        }
        CloseResFile(refNum);
    }
    UseResFile(saveRes);
}

/* A readable name for whatever Bluetooth hardware is present. ⚠ The USB product
 * STRING cannot be had here: string descriptors need a control transfer, which is
 * driver territory, and an application only gets the numeric descriptor. So this is a
 * lookup over the identities this project has actually measured, and it prints the
 * VID:PID alongside so an unrecognised part is still reportable. */
static const char *ModuleName(unsigned short v, unsigned short p)
{
    if (v == 0x05AC && p == 0x1000)                return "Internal Bluetooth Card";
    if (v == 0x05AC && p >= 0x8203 && p <= 0x8207) return "Internal Bluetooth Card (HCI)";
    /* ⚠ "BT DONGLE10" is the actual USB product string ASP read off this dongle, and
     * 0A12:0001 is the generic CSR part -- the same personality Apple's own
     * CSRUSBBluetoothHCIController claims as VID 2578 PID 1. We cannot read product
     * STRINGS here (string descriptors need a control transfer, which is driver
     * territory), so a known part is named from its IDs and anything else gets the
     * generic label plus its VID:PID. */
    if (v == 0x0A12 && p == 0x0001)                return "BT DONGLE10";
    if (v == 0x0A12)                               return "CSR USB adapter";
    return "USB Bluetooth adapter";
}

/* ---- geometry ------------------------------------------------------------- *
 * 600x360 fits a 640x480 screen. Widened from v0.2's 520 because the button stack
 * was crowding the list -- there was a 12px gutter and it read as a collision. The
 * gutter is now 24px and the buttons have their own column.
 *
 * A control panel a vintage user cannot open on a period display is not a control
 * panel, so 640x480 remains the constraint rather than an aspiration. */
/* ⚠ 360 -> 380, and kDescB with it. The scan-status and class-naming-caveat lines
 * pushed the pane over its bottom edge at the four-module cap -- checked
 * arithmetically, 354 against a 342 limit, rather than discovered on screen. 380
 * still leaves 100px of margin on a 480-line display. */
#define kWinW 600
#define kWinH 462

#define kListL  12
#define kListT  46
#define kListR 380
#define kListB 190

/* ★ DEVICE INFORMATION, DIRECTLY UNDER THE TABLE.
 *
 * Extensions Manager puts item information immediately below its list, and now that a
 * scan returns several devices at once the address is the only thing distinguishing
 * them -- so the detail for the selected row has to be adjacent to the selection, not
 * pushed below a block of driver diagnostics. */
#define kInfoT 200
#define kInfoB 300

/* The driver/module diagnostics move down to make room. */
#define kDescT 310
#define kDescB 452
/* ⚠ Apple's About boxes align the product NAME with the framed box below it and
 * leave the icon outside that column on the left. One constant for both so they
 * cannot drift. */
#define kAboutTextL 66
#define kBtnL  404
#define kBtnR  588

/* ★★ THE ON/OFF GROUP IS NARROWER THAN THE BUTTON COLUMN, and its width is MEASURED
 * rather than written down, because the content that determines it is itself measured:
 * the radios are sized to their titles in the system font (see MakeControls). A literal
 * box width would be a guess about Charcoal's metrics that happens to be right on this
 * system and wrong on one with a different system font.
 *
 * The values below are only what would be used if MakeControls had not run yet. It
 * always runs before any drawing -- but a zero-width group box is a baffling way to
 * discover an ordering change, so these are plausible rather than 0. */
static short gGrpR  = kBtnL + 108;   /* the group's right edge                        */
static short gLogoL = kBtnL + 66;    /* the state logo's left edge, right of the labels */

/* Forward declarations: the drawing helpers are defined below the routines that use
 * them, and an implicit declaration would compile with the wrong signature and then
 * fail at the definition -- which is what happened on the first build of this file. */
/* ★ A caption ON a group box's top rule, which is what Platinum does and what the
 * Bluetooth radio group already did inline. Factored out so the three captioned
 * groups cannot drift apart.
 *
 * ⚠ MEASURED AFTER THE FONT IS SET. Charcoal is wider than Geneva at the same size,
 * and a notch erased to Geneva's width leaves the frame rule showing through the
 * caption -- the exact bug the radio group's comment records. */
static void GroupCaption(const Rect *r, const char *text);
static void ErasePlatinum(const Rect *r);
/* v12.5: both modal loops call this so dragging one does not leave a trail. */
static void RedrawPanelWindow(void);
/* v12.8: grey the menus out while a modal window is up, and put them back after. */
static void SetMenusForModal(Boolean modal);
static void FrameGroup(const Rect *r, Boolean primary);
static void DrawComponentMsg(void);

static WindowPtr     gWin;
static ListHandle    gList;
static ControlHandle gDelete, gDisconnect, gConfigure, gSetup, gOnRadio, gOffRadio;
static ControlHandle gRename;      /* v15.3: sits under Pair, needs a selection */
/* v12.3: button width, measured from the widest label at build time. */
static short gBtnW = 120;
static ControlHandle gArrows;      /* chasing arrows, shown while a scan runs */
static short         gSelRow = -1;

static void PStr(Str255 d, const char *s)
{
    short n = (short)strlen(s);
    if (n > 254) n = 254;
    d[0] = (unsigned char)n;
    BlockMoveData(s, &d[1], n);
}

/* Append helpers for the status file. ⚠ Both clamp at 255: a Str255 that silently
 * wraps its length byte is how a log line becomes a memory smash. */
static void PStrCat(Str255 d, const char *s)
{
    short n = (short)strlen(s), have = d[0];
    if (have + n > 255) n = (short)(255 - have);
    if (n <= 0) return;
    BlockMoveData(s, &d[1 + have], n);
    d[0] = (unsigned char)(have + n);
}

static void PStrCatNum(Str255 d, long v)
{
    Str255 t;
    NumToString(v, t);
    { short n = t[0], have = d[0];
      if (have + n > 255) n = (short)(255 - have);
      if (n <= 0) return;
      BlockMoveData(&t[1], &d[1 + have], n);
      d[0] = (unsigned char)(have + n); }
}

/* ---- M6: LIVE SCAN RESULTS. The placeholders are gone. ---------------------- *
 * ⭐ NAME AND KIND COME FROM THE CLASS OF DEVICE, and that is the whole reason this
 * slice needs no new driver feature. The 2003 control panel in
 * vendor/bt-control-center does exactly this -- IsAMouse and IsAKeyboard on the CoD,
 * then a name and an icon -- and skips the Remote Name Request entirely
 * (docs/SCAN-DESIGN.md §1).
 *
 * ⚠ THE HONEST LIMIT, stated here and on screen: this cannot tell two keyboards
 * apart, because the class of device says what a thing IS and not which one it is.
 * The Remote Name Request is the upgrade; it is not the price of admission. */
typedef struct {
    unsigned long addrHi, addrLo;   /* 3 bytes each                              */
    unsigned long cod;
    unsigned long parm;             /* clock offset << 16 | page-scan rep << 8    */
} ScanRow;

static ScanRow gScan[kMaxScan];
static short   gScanRows = 0;

/* ★★ BONDED DEVICES, WHICH A SCAN CANNOT FIND.
 *
 * Run 49: the user's phone paired with the dongle and never appeared in the panel.
 * `inquiry responses 5, phones 0` -- a paired phone stops advertising itself, so it
 * answers no inquiry and no amount of scanning will surface it. Listing it from the
 * BOND instead is what Tiger's Devices tab does, and it is what the user expected.
 *
 * ⚠ A bonded row has no class of device: the key store keeps addresses and keys, not
 * CoD. So such a row is named honestly rather than guessed at -- see MergeBonded. */
static unsigned long gKeyHi[kMaxKeys], gKeyLo[kMaxKeys];
static short         gKeyRows = 0;
static short         gLastKeyCount = -1;

/* Rows shown = scan results, plus any bond no scan result covers. */
#define kMaxRows (kMaxScan + kMaxKeys)
typedef struct {
    unsigned long addrHi, addrLo, cod;
    unsigned long parm;         /* clock offset << 16 | page-scan rep << 8         */
    Boolean       fromScan;     /* false = listed from the bond alone              */
    /* ★ true = listed from the CARD's own key store, not from an inquiry and not
     * from our bond. The A1016 can only ever appear this way -- it pages its host
     * instead of advertising -- so this flag is what lets the Kind column say so
     * rather than calling a known keyboard "Unknown". */
    Boolean       inCard;
    /* ⚠ SEPARATE FROM fromScan, and that separation is the whole fix. A row can be
     * absent from this inquiry yet still have a class of device we observed in an
     * earlier one. Naming keyed off fromScan is what made a paired phone read
     * "Unknown" for ten seconds every time a scan restarted. */
    Boolean       codKnown;     /* cod is meaningful: seen now, or seen before     */
    Boolean       paired;       /* we hold a key -- NOT proof the peer agrees      */
    Boolean       handed;       /* the controller asked for that key BY ADDRESS    */
    /* ★★★ v8.2: true = this address PAGED us. The strongest row source there is for
     * the device this project exists for, and until now the only one not drawn.
     *
     * ⚠ It is the source that survives deleting the card's key. inCard rows vanish
     * the moment Delete_Stored_Link_Key succeeds -- which is exactly when the A1016
     * most needs a row, because that is when it must be paired again. A keyboard
     * that has lost its host pages; it does not advertise. Without this the test
     * cannot be run at all: the row disappears and Pair has nothing to act on. */
    Boolean       paged;
} DispRow;
static DispRow gRow[kMaxRows];
static short   gRowCount = 0;

static unsigned long gKeyHandedMask = 0;

/* ★★★ LAST-KNOWN CLASS OF DEVICE, PER ADDRESS.
 *
 * ⚠ WHY THIS IS NOT THE GUESSING THE COMMENT ABOVE FORBIDS. That warning is about
 * inventing a kind from an ADDRESS, which carries no class information at all. This
 * remembers a CoD the controller actually reported for that exact address. Recalling
 * an observed fact and fabricating an unobserved one are opposites.
 *
 * THE BUG THIS FIXES, reported on the 3.6 test run. Pressing the scan button made a
 * paired phone flip from "Phone"/"Phone" to "Paired device"/"Unknown" for about ten
 * seconds, then flip back. Nothing was wrong with the row logic: BT_ScanStart clears
 * the driver's responder list before starting the new inquiry, so until the phone
 * answers AGAIN the panel sees zero scan results and the device exists only as a bond
 * -- and a bond genuinely carries no CoD. The panel was therefore discarding, every
 * single scan, a fact it had already learned and that cannot change: class of device
 * is a property of the device, not of the inquiry.
 *
 * ⚠⚠ IT WAS RAM ONLY UNTIL 7.5, AND THAT JUSTIFICATION HAS BEEN OVERTAKEN.
 *
 * The old note here said persisting it was "a much larger change than the flicker
 * justifies". That reasoning was sound about a FLICKER -- ten seconds of "Unknown"
 * during a scan, then back. It is wrong about what the user actually reported: after
 * every relaunch a PAIRED PHONE reads "Paired device" / "Unknown" and STAYS that way,
 * because nothing restores the CoD until something scans again. Two things changed
 * around the old decision:
 *
 *   - it is permanent now, not transient. A bonded device the panel cannot name is
 *     the panel failing at its main job, not a cosmetic blink.
 *   - "just scan" stopped being free. An inquiry competes with page scanning on this
 *     controller, so the advice for pairing is now often DON'T scan -- which means
 *     the one thing that repopulated the cache is a thing the user is told to avoid.
 *
 * ⇒ A ~100-byte file in Preferences. It holds no secrets: an address and a class of
 * device, both of which the panel already displays, and deliberately NOT the link key
 * -- that stays in the driver's own key file and is never touched from here.
 *
 * ⚠ Whole-file rewrite on change, because the file is at most kMaxRows * 12 bytes and
 * a partial update is the shape that corrupts. Every failure is silent and
 * non-latching: a missing or unreadable file simply means nothing has been learned
 * yet, which is the honest starting state. */
#define kCodMagic  0x42544344UL      /* 'BTCD' */
#define kCodFmtVer 1UL

static unsigned long gCodHi[kMaxRows], gCodLo[kMaxRows], gCodVal[kMaxRows];
static short         gCodRows = 0;

/* Open the cache file, creating it if asked. Returns 0 on failure -- never fatal. */
static short CodOpen(Boolean create)
{
    FSSpec spec;
    short  vRefNum, ref;
    long   dirID;
    OSErr  err;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return 0;
    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Device Kinds", &spec);
    if (err != noErr && err != fnfErr) return 0;
    if (err == fnfErr) {
        if (!create) return 0;
        if (FSpCreate(&spec, 'BTcp', 'BTcd', smSystemScript) != noErr) return 0;
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return 0;
    return ref;
}

/* ⭐ CAN WE ACTUALLY DRIVE THIS? Peripheral major class AND a keyboard (0x40) or
 * pointing-device (0x80) minor bit. Spelled out here rather than shared, because this is
 * a separately built binary -- same reasoning as KindName's copy.
 *
 * ⚠⚠ THE FILTER HAS TO BE HERE TOO, not only in the driver's scanner. Driver 14.6 stopped
 * undrivable devices entering a SCAN, but this file is what the CSM reads, and it already
 * held phones and audio devices learned in earlier sessions -- so the CSM's menu showed
 * "Phone - not reporting" for a device that can never report. Filtering discovery does
 * not clean up what discovery already wrote down.
 *
 * ⚠ Defined ABOVE CodSave/CodLoad on purpose: CodLoad calls it, and C needs it declared
 * first. It was briefly placed next to CodRemember, which reads better and does not
 * compile. */
static Boolean CodIsDrivable(unsigned long cod)
{
    if (((cod >> 8) & 0x1F) != 5) return false;      /* not a Peripheral */
    return (Boolean)((cod & 0xC0UL) != 0);           /* keyboard and/or pointer */
}

/* ---- DEVICE NICKNAMES, v15.3 ------------------------------------------------
 *
 * ⚠⚠ A SEPARATE FILE FROM THE DRIVER'S NAMES, AND THAT IS THE WHOLE DESIGN. The
 * driver writes "Bluetooth Device Names" from what the remote device calls ITSELF,
 * refreshing it whenever a name request answers. An alias written into that file would
 * be silently overwritten the next time the device introduced itself -- the user's
 * choice quietly replaced by the manufacturer's, with nothing to show why.
 *
 * So the nickname lives here, is written only by this panel, and is read by whoever
 * displays a name. The join is the 3+3 address pair, the same as every other file in
 * this project [[project_os9_bluetooth_csm]] -- a 2+4 battery file once matched nothing
 * and listed every device twice.
 *
 * An EMPTY nickname is how a device goes back to its real name: the record is dropped
 * rather than stored blank, so "clear it" and "never set one" are the same state and
 * there is no third case for a reader to get wrong. */
#define kAlMagic   0x4254414CUL      /* 'BTAL' */
#define kAlFmtVer  1UL
#define kAlChars   24                /* C string, NUL-terminated, same width as 'BTNM' */
#define kMaxAlias  8

static unsigned long gAlHi[kMaxAlias], gAlLo[kMaxAlias];
static char          gAlName[kMaxAlias][kAlChars];
static short         gAlRows   = 0;
static Boolean       gAlLoaded = false;

static short AlOpen(Boolean create)
{
    FSSpec spec;
    short  vRefNum, ref;
    long   dirID;
    OSErr  err;

    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return 0;
    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Nicknames", &spec);
    if (err != noErr && err != fnfErr) return 0;
    if (err == fnfErr) {
        if (!create) return 0;
        if (FSpCreate(&spec, 'BTcp', 'BTal', smSystemScript) != noErr) return 0;
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return 0;
    return ref;
}

static void AlLoad(void)
{
    short         ref;
    unsigned long hdr[3];
    long          n;
    short         i;

    gAlRows   = 0;
    gAlLoaded = true;                 /* ⚠ set even on failure: a missing file is a
                                       * valid, empty answer, not a reason to retry
                                       * on every redraw. */
    ref = AlOpen(false);
    if (ref == 0) return;
    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) { (void)FSClose(ref); return; }
    if (hdr[0] != kAlMagic || hdr[1] != kAlFmtVer)               { (void)FSClose(ref); return; }
    for (i = 0; i < (short)hdr[2] && i < kMaxAlias; i++) {
        unsigned long a[2];
        char          nm[kAlChars];
        n = (long)sizeof(a);
        if (FSRead(ref, &n, a) != noErr || n != (long)sizeof(a)) break;
        n = (long)kAlChars;
        if (FSRead(ref, &n, nm) != noErr || n != (long)kAlChars) break;
        nm[kAlChars - 1] = 0;         /* ⚠ never trust a file to be terminated */
        if (nm[0] == 0) continue;     /* empty == no nickname; see the header */
        gAlHi[gAlRows] = a[0];
        gAlLo[gAlRows] = a[1];
        BlockMoveData(nm, gAlName[gAlRows], kAlChars);
        gAlRows++;
    }
    (void)FSClose(ref);
}

static void AlSave(void)
{
    short         ref = AlOpen(true);
    unsigned long hdr[3];
    long          n;
    short         i;

    if (ref == 0) return;
    hdr[0] = kAlMagic;
    hdr[1] = kAlFmtVer;
    hdr[2] = (unsigned long)gAlRows;
    /* ⚠ TRUNCATE FIRST, same reason as CodSave: a shrinking list would otherwise
     * leave the tail of the longer previous file behind the new header. */
    if (SetFPos(ref, fsFromStart, 0) != noErr) { (void)FSClose(ref); return; }
    if (SetEOF(ref, 0) != noErr)               { (void)FSClose(ref); return; }
    n = (long)sizeof(hdr);
    if (FSWrite(ref, &n, hdr) != noErr)        { (void)FSClose(ref); return; }
    for (i = 0; i < gAlRows; i++) {
        unsigned long a[2];
        a[0] = gAlHi[i]; a[1] = gAlLo[i];
        n = (long)sizeof(a);
        if (FSWrite(ref, &n, a) != noErr) break;
        n = (long)kAlChars;
        if (FSWrite(ref, &n, gAlName[i]) != noErr) break;
    }
    (void)FSClose(ref);
}

/* NULL when this device has no nickname. */
static const char *AliasFor(unsigned long hi, unsigned long lo)
{
    short i;
    if (!gAlLoaded) AlLoad();
    for (i = 0; i < gAlRows; i++)
        if (gAlHi[i] == hi && gAlLo[i] == lo) return gAlName[i];
    return NULL;
}

/* An empty or NULL name REMOVES the nickname -- that is how the user restores the
 * device's real name, and it keeps "cleared" and "never set" the same state. */
static void AliasSet(unsigned long hi, unsigned long lo, const char *nm)
{
    short i, k;
    if (!gAlLoaded) AlLoad();
    for (i = 0; i < gAlRows; i++) {
        if (gAlHi[i] != hi || gAlLo[i] != lo) continue;
        if (nm == NULL || nm[0] == 0) {           /* drop the row, keep it packed */
            for (k = i; k < gAlRows - 1; k++) {
                gAlHi[k] = gAlHi[k + 1]; gAlLo[k] = gAlLo[k + 1];
                BlockMoveData(gAlName[k + 1], gAlName[k], kAlChars);
            }
            gAlRows--;
        } else {
            for (k = 0; k < kAlChars - 1 && nm[k] != 0; k++) gAlName[i][k] = nm[k];
            gAlName[i][k] = 0;
        }
        AlSave();
        return;
    }
    if (nm == NULL || nm[0] == 0)  return;        /* nothing to clear */
    if (gAlRows >= kMaxAlias)      return;        /* ⚠ silent, but bounded and rare */
    gAlHi[gAlRows] = hi; gAlLo[gAlRows] = lo;
    for (k = 0; k < kAlChars - 1 && nm[k] != 0; k++) gAlName[gAlRows][k] = nm[k];
    gAlName[gAlRows][k] = 0;
    gAlRows++;
    AlSave();
}

static void CodSave(void)
{
    short         ref = CodOpen(true);
    unsigned long hdr[3];
    long          n;
    short         i;

    if (ref == 0) return;
    hdr[0] = kCodMagic;
    hdr[1] = kCodFmtVer;
    hdr[2] = (unsigned long)gCodRows;
    /* ⚠ TRUNCATE FIRST. Without SetEOF a shrinking list leaves the tail of the
     * previous, longer file in place -- and the count in the header would then
     * disagree with the bytes after it. */
    if (SetFPos(ref, fsFromStart, 0) != noErr) { (void)FSClose(ref); return; }
    if (SetEOF(ref, 0) != noErr)               { (void)FSClose(ref); return; }
    n = (long)sizeof(hdr);
    if (FSWrite(ref, &n, hdr) != noErr)        { (void)FSClose(ref); return; }
    for (i = 0; i < gCodRows; i++) {
        unsigned long rec[3];
        rec[0] = gCodHi[i]; rec[1] = gCodLo[i]; rec[2] = gCodVal[i];
        n = (long)sizeof(rec);
        if (FSWrite(ref, &n, rec) != noErr) break;   /* partial file, header count
                                                      * still bounds the read */
    }
    (void)FSClose(ref);
}

static void CodLoad(void)
{
    short         ref = CodOpen(false);
    unsigned long hdr[3];
    long          n;
    short         i, count;
    short         dropped = 0;             /* undrivable entries pruned on the way in */

    if (ref == 0) return;                  /* no file yet: nothing learned. Fine. */
    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) {
        (void)FSClose(ref); return;
    }
    /* ⚠ VALIDATE BOTH. A file from a future format read as this one would load
     * garbage addresses and mislabel real devices, which is worse than forgetting. */
    if (hdr[0] != kCodMagic || hdr[1] != kCodFmtVer) { (void)FSClose(ref); return; }
    count = (short)hdr[2];
    if (count < 0) count = 0;
    if (count > kMaxRows) count = kMaxRows;      /* never trust a stored length */
    /* ⚠⚠ THE WRITE BOUND IS gCodRows, NOT count, AND THEY ARE NOT THE SAME THING.
     *
     * count is clamped to kMaxRows above, which is only safe if gCodRows starts at 0.
     * It does today -- CodLoad runs first in main -- but that is a fact about call
     * order, and call order is exactly the kind of premise that expires silently when
     * someone adds a second call site or a Revert command. This project has already
     * paid for one hand-reasoned bound: a 304 typed against a 313-word block, read out
     * of range. Bound the actual write index instead. */
    for (i = 0; i < count && gCodRows < kMaxRows; i++) {
        unsigned long rec[3];
        n = (long)sizeof(rec);
        if (FSRead(ref, &n, rec) != noErr || n != (long)sizeof(rec)) break;
        if (rec[2] == 0) continue;                /* a zero CoD means nothing */
        /* ⭐⭐ DROP UNDRIVABLE ENTRIES ON THE WAY IN, so the file SELF-CLEANS. Every
         * install so far has accumulated phones and audio devices in this file, and
         * refusing to learn new ones (CodRemember) would leave the old ones there
         * forever. Dropping them here means the next CodSave writes a clean file without
         * needing a migration step or asking the user to delete anything.
         *
         * ⚠ gCodRows is not advanced, so the row is genuinely gone rather than blanked --
         * a zeroed row would still be written back out and still be a record. */
        if (!CodIsDrivable(rec[2])) { dropped++; continue; }
        gCodHi[gCodRows] = rec[0];
        gCodLo[gCodRows] = rec[1];
        gCodVal[gCodRows] = rec[2];
        gCodRows++;
    }
    (void)FSClose(ref);

    /* ⭐⭐ REWRITE IMMEDIATELY IF ANYTHING WAS DROPPED, so the user never has to delete
     * this file by hand. Without this the cleanup would sit in RAM until some unrelated
     * event happened to trigger a CodSave -- which, on a machine that has already learned
     * every device it will ever see, might be never. One save on the first launch after
     * updating, and the file is clean for good.
     *
     * ⚠ Safe here: the arrays are fully populated by this point, the file is CLOSED
     * above, and CodSave writes via its own temp-file path. */
    if (dropped > 0) CodSave();
}

static void CodRemember(unsigned long hi, unsigned long lo, unsigned long cod)
{
    short i;
    if (cod == 0) return;                      /* nothing worth remembering */
    /* ⚠ Never LEARN an undrivable device. The pairing UI may still show one (the phone
     * is this project's test instrument and the show-all marker exists for that), but
     * writing it to the file the CSM reads is what put those lines on screen. */
    if (!CodIsDrivable(cod)) return;
    for (i = 0; i < gCodRows; i++)
        if (gCodHi[i] == hi && gCodLo[i] == lo) {
            /* ⚠ Only write when the value actually MOVED. This is called from the row
             * rebuild, which runs on every poll; saving unconditionally would rewrite
             * a Preferences file several times a second forever. */
            if (gCodVal[i] != cod) { gCodVal[i] = cod; CodSave(); }
            return;
        }
    /* ⚠ Full is not an error and must not latch: the newest observation simply has
     * nowhere to go, and every existing entry stays valid. kMaxRows is the most rows
     * that can ever be displayed, so overflow needs more distinct devices than the
     * panel can show at once. */
    if (gCodRows < kMaxRows) {
        gCodHi[gCodRows] = hi; gCodLo[gCodRows] = lo; gCodVal[gCodRows] = cod;
        gCodRows++;
        CodSave();
    }
}

/* The remembered CoD for an address, or 0 if we have never seen one. */
static unsigned long CodRecall(unsigned long hi, unsigned long lo)
{
    short i;
    for (i = 0; i < gCodRows; i++)
        if (gCodHi[i] == hi && gCodLo[i] == lo) return gCodVal[i];
    return 0;
}

/* ★ Addresses the user has deleted that the driver's published list has not caught up
 * with yet. See the reload in PollScan for why this is needed at all. */
static unsigned long gDelHi[kMaxKeys], gDelLo[kMaxKeys];
static short         gDelCount = 0;

static Boolean AddrPendingDelete(unsigned long hi, unsigned long lo)
{
    short i;
    for (i = 0; i < gDelCount; i++)
        if (gDelHi[i] == hi && gDelLo[i] == lo) return true;
    return false;
}

/* ★★★★★★ HIDDEN UNTIL THE NEXT SCAN -- A DELETED DEVICE LEAVES THE LIST AT ONCE.
 *
 * ⚠⚠ THIS IS A DIFFERENT LIST FROM gDelHi ABOVE AND THE DIFFERENCE IS THE POINT.
 * AddrPendingDelete suppresses a stale BOND, so it only ever filtered the key rows --
 * and a row has four sources. Deleting a device the user had also scanned left the
 * scan row standing, deleting one that had paged us left the paged row standing, and
 * the status was whatever the remaining sources said. Reported twice: "the row
 * remained in the list".
 *
 * ⇒ This one suppresses the ADDRESS, from every source, and MergeBonded consults it
 * once at the top instead of each source deciding for itself.
 *
 * ⭐ AND IT CLEARS ON THE NEXT SCAN, which is the user's own model of the two buttons:
 * a deleted device disappears immediately and comes back, as Available, only after a
 * scan finds it again. That is also what resolves the objection the paged-row source
 * records against being filtered -- that suppressing it would hide the device at the
 * one moment you need to pair it. It would, if the suppression outlived the scan. It
 * does not: Scan For Devices is exactly the act that lifts it. */
static unsigned long gHideHi[kMaxKeys], gHideLo[kMaxKeys];
static short         gHideCount = 0;

static Boolean AddrHidden(unsigned long hi, unsigned long lo)
{
    short i;
    for (i = 0; i < gHideCount; i++)
        if (gHideHi[i] == hi && gHideLo[i] == lo) return true;
    return false;
}

static void HideAddr(unsigned long hi, unsigned long lo)
{
    if (gHideCount >= kMaxKeys || AddrHidden(hi, lo)) return;
    gHideHi[gHideCount] = hi;
    gHideLo[gHideCount] = lo;
    gHideCount++;
}

/* ★★★★★ LIFT THE HIDE ON EVIDENCE, NOT ON THE ACT OF SCANNING.
 *
 * ⚠⚠ v13.4 CLEARED THE WHOLE LIST WHEN A SCAN STARTED, which is looser than the rule
 * the user actually stated: a deleted device must NOT come back unless it is in pairing
 * mode when the scan runs. Clearing on scan-start brought it back from the module's
 * store or from a stale page record whether it was discoverable or not -- so a device
 * you had deliberately removed reappeared merely because you went looking for a
 * different one.
 *
 * ⇒ An address leaves the hide list only when something PROVES it is there: it answered
 * an inquiry (it is discoverable, which for these devices means pairing mode), or it is
 * genuinely connected. The second case is not a concession -- a panel that hides a
 * device you are typing on is the same lie this session has spent its time removing. */
static void UnhideAddr(unsigned long hi, unsigned long lo)
{
    short i, n = 0;
    for (i = 0; i < gHideCount; i++) {
        if (gHideHi[i] == hi && gHideLo[i] == lo) continue;
        gHideHi[n] = gHideHi[i];
        gHideLo[n] = gHideLo[i];
        n++;
    }
    gHideCount = n;
}

/* ---- sorting, the Extensions Manager way ------------------------------------
 * Click a header to sort by it; click the same header again to reverse. The
 * active column's header is drawn pressed so the sort key is never a mystery. */
static short   gSortCol  = 0;        /* 0 Name, 1 Kind, 2 Status */
/* v12.1: our item in the SYSTEM Help menu. 0 = we never got one. */
static short   gHelpItem = 0;
static Boolean gSortDesc = false;

static const char *RowNameOf(const DispRow *d);
static const char *RowKindOf(const DispRow *d);

/* Rank for the Status column, so sorting is by MEANING rather than alphabetical --
 * "Available" before "Paired" before "Connected" is not a useful ordering. */
static short StatusRankOf(const DispRow *d)
{
    if (d->handed) return 0;         /* strongest evidence first */
    if (d->paired) return 1;
    return 2;
}

/* ⚠ Compares VALUES, not indices. The first version took indices into gRow while the
 * sort was moving elements underneath it, which is only correct by accident. */
static short CmpRows(const DispRow *a, const DispRow *b)
{
    short c = 0;
    switch (gSortCol) {
    case 0: c = (short)strcmp(RowNameOf(a), RowNameOf(b)); break;
    case 1: c = (short)strcmp(RowKindOf(a), RowKindOf(b)); break;
    /* ⚠ Status moved from column 3 to column 2 when the ID column was removed. A
     * stale number here would sort the list by a column that no longer exists, which
     * looks like "the sort is broken" rather than like a renumbering. */
    case 2: c = (short)(StatusRankOf(a) - StatusRankOf(b)); break;
    }
    return gSortDesc ? (short)-c : c;
}

/* Insertion sort: at most 24 rows, and STABLE, so equal rows keep discovery order
 * instead of reshuffling on every poll. */
static void SortRows(void)
{
    short i, j;
    for (i = 1; i < gRowCount; i++) {
        DispRow key = gRow[i];
        j = i;
        while (j > 0 && CmpRows(&gRow[j - 1], &key) > 0) {
            gRow[j] = gRow[j - 1];
            j--;
        }
        gRow[j] = key;
    }
}

/* Returns 0 = no key, 1 = key on file, 2 = key on file AND handed to the controller. */
static short BondLevel(unsigned long hi, unsigned long lo)
{
    short i;
    for (i = 0; i < gKeyRows; i++)
        if (gKeyHi[i] == hi && gKeyLo[i] == lo)
            return (gKeyHandedMask & (1UL << i)) ? 2 : 1;
    return 0;
}

/* ================================================================================
 *  THE BATTERY LEVEL, read from the driver's file rather than its counter block.
 * ================================================================================
 *
 * ⚠⚠ NOT kWBatPctVal. That word holds the RAW PAYLOAD OF THE LAST Feature 71 RESPONSE,
 * whichever device happened to answer most recently -- it is not per device. Showing it
 * against a SELECTED row would attribute one device's level to another the moment two
 * are paired, which is exactly the kind of confident-and-wrong readout this panel exists
 * to avoid. `Preferences:Bluetooth Battery` is keyed by ADDRESS, so it can answer the
 * question the detail pane actually asks.
 *
 * ⚠ Addresses in that file are TOP 3 BYTES then low 3 -- the same split gCodHi/gCodLo
 * and gRow[].addrHi/addrLo use, so the join is direct. Driver 14.5 fixed this (format
 * version 2); version 1 wrote 2+4 and is refused below rather than mis-joined.
 *
 * ⚠⚠ FRESHNESS IS TESTED, same as in the CSM and for the same reason: the file outlives
 * the session that wrote it, so a boot where nothing connected would otherwise show a
 * confident level for a flat battery. A file older than this boot is ignored entirely.
 */
#define kBatMagic   0x42544241UL      /* 'BTBA' */
#define kBatFmtVer  2UL
#define kBatRows    8

static unsigned long gBatHi[kBatRows], gBatLo[kBatRows];
static short         gBatPct[kBatRows];
static short         gBatRows;

/* mtime >= boot? See the CSM's FileWrittenThisBoot for why the tick-rate drift is
 * harmless HERE (it separates sessions, not events within one). */
static Boolean BatFileFresh(const FSSpec *spec)
{
    CInfoPBRec    pb;
    Str255        nm;
    unsigned long now, upSecs;

    BlockMoveData(spec->name, nm, (Size)(spec->name[0] + 1));
    memset(&pb, 0, sizeof(pb));
    pb.hFileInfo.ioNamePtr   = nm;
    pb.hFileInfo.ioVRefNum   = spec->vRefNum;
    pb.hFileInfo.ioDirID     = spec->parID;
    pb.hFileInfo.ioFDirIndex = 0;
    if (PBGetCatInfoSync(&pb) != noErr) return false;
    GetDateTime(&now);
    upSecs = (unsigned long)(TickCount() / 60);
    if (upSecs >= now) return false;
    return (Boolean)(pb.hFileInfo.ioFlMdDat + 2 >= now - upSecs);
}

static void BatLoad(void)
{
    FSSpec        spec;
    short         vRefNum, ref;
    long          dirID, n;
    unsigned long hdr[3];
    short         i, count;

    gBatRows = 0;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) return;
    if (FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Battery", &spec) != noErr) return;
    if (!BatFileFresh(&spec)) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;

    n = (long)sizeof(hdr);
    if (FSRead(ref, &n, hdr) != noErr || n != (long)sizeof(hdr)) { FSClose(ref); return; }
    if (hdr[0] != kBatMagic || hdr[1] != kBatFmtVer) { FSClose(ref); return; }
    count = (short)hdr[2];
    if (count < 0) count = 0;
    for (i = 0; i < count && gBatRows < kBatRows; i++) {
        unsigned long rec[4];
        n = (long)sizeof(rec);
        if (FSRead(ref, &n, rec) != noErr || n != (long)sizeof(rec)) break;
        if (rec[2] > 100UL) continue;        /* not a percentage, whatever it is */
        gBatHi[gBatRows]  = rec[0];
        gBatLo[gBatRows]  = rec[1];
        gBatPct[gBatRows] = (short)rec[2];
        gBatRows++;
    }
    FSClose(ref);
}

/* The level for one address, or -1 when this device has not reported one. */
static short BatLevelFor(unsigned long hi, unsigned long lo)
{
    short i;
    for (i = 0; i < gBatRows; i++)
        if (gBatHi[i] == hi && gBatLo[i] == lo) return gBatPct[i];
    return -1;
}

/* Build the display list: every scan result, then every bond not already present. */
static void MergeBonded(void)
{
    short i, j;

    /* ⭐ Refresh the battery cache with the rows it will be displayed against, so
     * the two can never describe different moments. Cheap: a 28-byte file. */
    BatLoad();

    gRowCount = 0;
    for (i = 0; i < gScanRows && gRowCount < kMaxRows; i++) {
        short lvl;
        /* ⚠ ONE FILTER, AT THE TOP OF EVERY SOURCE. See AddrHidden: a row has FOUR
         * sources and deleting a device only ever suppressed one of them. */
        if (AddrHidden(gScan[i].addrHi, gScan[i].addrLo)) continue;
        lvl = BondLevel(gScan[i].addrHi, gScan[i].addrLo);
        gRow[gRowCount].addrHi   = gScan[i].addrHi;
        gRow[gRowCount].addrLo   = gScan[i].addrLo;
        gRow[gRowCount].cod      = gScan[i].cod;
        gRow[gRowCount].parm     = gScan[i].parm;
        gRow[gRowCount].fromScan = true;
        gRow[gRowCount].codKnown = (gScan[i].cod != 0);
        gRow[gRowCount].inCard   = false;
        gRow[gRowCount].paged    = false;
        gRow[gRowCount].paired   = (lvl >= 1);
        gRow[gRowCount].handed   = (lvl == 2);
        /* Learn it here, where a real inquiry response is in hand. */
        CodRemember(gScan[i].addrHi, gScan[i].addrLo, gScan[i].cod);
        gRowCount++;
    }
    for (i = 0; i < gKeyRows && gRowCount < kMaxRows; i++) {
        Boolean dup = false;
        for (j = 0; j < gScanRows; j++)
            if (gScan[j].addrHi == gKeyHi[i] && gScan[j].addrLo == gKeyLo[i])
                { dup = true; break; }
        if (dup) continue;
        if (AddrHidden(gKeyHi[i], gKeyLo[i])) continue;
        gRow[gRowCount].addrHi   = gKeyHi[i];
        gRow[gRowCount].addrLo   = gKeyLo[i];
        /* ★ Recall, do not guess. A bond carries no CoD, but we may have observed one
         * for this exact address in an earlier inquiry -- and if we have, the row keeps
         * reading "Phone" instead of dropping to "Unknown" the moment a scan restarts.
         * Never seen it? cod stays 0 and codKnown false, and the row says so. */
        gRow[gRowCount].cod      = CodRecall(gKeyHi[i], gKeyLo[i]);
        gRow[gRowCount].codKnown = (gRow[gRowCount].cod != 0);
        gRow[gRowCount].parm     = 0;
        gRow[gRowCount].fromScan = false;
        gRow[gRowCount].paired   = true;
        gRow[gRowCount].handed   = (gKeyHandedMask & (1UL << i)) ? true : false;
        gRow[gRowCount].inCard   = false;
        gRow[gRowCount].paged    = false;
        gRowCount++;
    }

    /* ★★★★ AND THE ADDRESSES THE CARD ITSELF HOLDS -- a third source, and without it
     * the Pair button cannot reach the one device this project exists for.
     *
     * ⚠ THE A1016 DOES NOT APPEAR IN AN INQUIRY. It is already bonded to the A1044
     * from Tiger, so switching it on makes it PAGE ITS HOST rather than advertise as
     * discoverable -- which is exactly what every run since v6.6 has measured:
     * Connection Requests 1 to 3 per session, and not one inquiry response. A list
     * built only from inquiry responders and from OUR key store therefore has no row
     * for it, and a Pair button that acts on the selection has nothing to act on.
     *
     * The driver already publishes what is needed: Read_Stored_Link_Key with
     * Read_All_Flag 1 returns the controller's own bonded addresses, and they sit in
     * the block at kWRlkA0Hi onward. Those rows are selectable now.
     *
     * ⚠ Marked paired = false DELIBERATELY, even though the CARD has a key for them.
     * "Paired" in this panel means WE hold the bond, which is what Delete acts on --
     * claiming otherwise would offer to forget a key that is not ours to forget, and
     * would make Delete lie. The distinction is the whole point of the goal: the card
     * holds a Tiger-made bond, and M3.7 is about replacing it with one of ours.
     *
     * ⚠ Deduplicated against BOTH earlier sources. The keyboard may also answer an
     * inquiry one day, and two rows for one address would let the user Pair the wrong
     * one and get a different result each time. */
    {
        short slot;
        for (slot = 0; slot < 4 && gRowCount < kMaxRows; slot++) {
            unsigned long hi, lo;
            Boolean dup = false;
            if (gBlock == NULL) break;
            hi = gBlock[kWRlkA0Hi + slot * 2 + 0];
            lo = gBlock[kWRlkA0Hi + slot * 2 + 1];
            if (hi == 0 && lo == 0) continue;
            if (AddrPendingDelete(hi, lo)) continue;
            for (j = 0; j < gRowCount; j++)
                if (gRow[j].addrHi == hi && gRow[j].addrLo == lo) { dup = true; break; }
            /* ⚠⚠ MERGE, DO NOT SKIP -- AND THIS COST A HARDWARE RUN.
             *
             * This used to `continue` on a duplicate, leaving the earlier row with
             * inCard = false. That was harmless only while our key database and the
             * card's store held DIFFERENT addresses. v12.0's capture made them hold
             * the SAME ones, so the A1016 stopped being a card row and became a
             * database row -- and the Delete button, which routed on
             * `inCard && !paired`, silently changed destination from "forget the
             * card's key" to "forget OUR copy". The user deleted a bond expecting the
             * first and got the second, destroying the very copy Restore depends on.
             *
             * ⇒ A row must carry BOTH facts. Being in our database and being in the
             * card's store are independent, and after capture they are normally both
             * true. */
            if (dup) { gRow[j].inCard = true; continue; }
            if (AddrHidden(hi, lo)) continue;
            gRow[gRowCount].addrHi   = hi;
            gRow[gRowCount].addrLo   = lo;
            gRow[gRowCount].cod      = CodRecall(hi, lo);
            gRow[gRowCount].codKnown = (gRow[gRowCount].cod != 0);
            gRow[gRowCount].parm     = 0;
            gRow[gRowCount].fromScan = false;
            gRow[gRowCount].paired   = false;   /* see the note above -- not OUR bond */
            gRow[gRowCount].handed   = false;
            gRow[gRowCount].inCard   = true;
            gRow[gRowCount].paged    = false;
            gRowCount++;
        }
    }

    /* ★★★★ v8.2: AND A FOURTH SOURCE -- EVERY ADDRESS THAT HAS PAGED THIS MAC.
     *
     * ⚠ THIS IS WHAT MAKES THE A1016 TEST POSSIBLE AT ALL, and the reason is a gap in
     * the three sources above. The goal is to pair the keyboard in OS 9 with no help
     * from Tiger, which means deleting the Tiger-made key from the card's store first.
     * The moment that delete succeeds:
     *
     *   - it is not an inquiry responder  (v91 measured: inquiry responses 1, and that
     *     one was the phone -- PERIPHERAL 0. A bonded keyboard pages, it does not
     *     advertise, and after the delete it still pages rather than advertising)
     *   - it is not in our key file       (records LOADED 0 -- we have never held it)
     *   - it is no longer in the card     (we just deleted it, and the re-read drops
     *     the inCard row within a second)
     *
     * ⇒ zero rows. No selection, nothing for Pair to act on, and the test cannot be
     * run. Yet the keyboard is right there paging us several times a session, with its
     * class of device attached saying "keyboard" -- the driver has recorded that since
     * v6.6 and nothing has ever drawn it.
     *
     * ⚠ paired = false and inCard = false, both honestly: a page is not a bond, and
     * after the delete the card holds nothing for it either. Delete stays disabled on
     * these rows, which is correct -- there is nothing to delete. Pair is what they
     * are for. */
    {
        short slot;
        for (slot = 0; slot < kPagedSlots && gRowCount < kMaxRows; slot++) {
            unsigned long hi, lo, cod;
            Boolean dup = false;
            if (gBlock == NULL) break;
            hi  = gBlock[kWPagedA0Hi + slot * 3 + 0];
            lo  = gBlock[kWPagedA0Hi + slot * 3 + 1];
            cod = gBlock[kWPagedA0Hi + slot * 3 + 2];
            if (hi == 0 && lo == 0) continue;
            /* ⚠ STILL not filtered by AddrPendingDelete -- that list suppresses rows
             * whose BOND we just removed, and this row asserts no bond.
             *
             * ⚠⚠ BUT IT IS FILTERED BY AddrHidden, AND THE OBJECTION THIS COMMENT USED
             * TO RAISE AGAINST THAT IS ANSWERED RATHER THAN IGNORED. It said filtering
             * here would hide the device at the one moment you need to pair it again.
             * True of a suppression that outlives the scan; AddrHidden is lifted BY the
             * scan, so pressing Scan For Devices is exactly what brings it back. */
            if (AddrHidden(hi, lo)) continue;
            for (j = 0; j < gRowCount; j++)
                if (gRow[j].addrHi == hi && gRow[j].addrLo == lo) { dup = true; break; }
            if (dup) continue;
            gRow[gRowCount].addrHi   = hi;
            gRow[gRowCount].addrLo   = lo;
            /* ⭐ A page carries a real class of device, so unlike an inCard row this
             * one can name the device honestly. Fall back to what we recalled. */
            gRow[gRowCount].cod      = cod ? cod : CodRecall(hi, lo);
            gRow[gRowCount].codKnown = (gRow[gRowCount].cod != 0);
            gRow[gRowCount].parm     = 0;
            gRow[gRowCount].fromScan = false;
            gRow[gRowCount].paired   = false;
            gRow[gRowCount].handed   = false;
            gRow[gRowCount].inCard   = false;
            gRow[gRowCount].paged    = true;
            if (cod) CodRemember(hi, lo, cod);
            gRowCount++;
        }
    }
    SortRows();
}

/* Major device class is bits 8..12 of the 24-bit CoD -- same extraction as the
 * driver's cod_major, deliberately spelled out rather than shared, because the two
 * binaries are built separately. */
static short CodMajor(unsigned long cod) { return (short)((cod >> 8) & 0x1F); }

static const char *KindForCoD(unsigned long cod)
{
    switch (CodMajor(cod)) {
    case 1:  return "Computer";
    case 2:  return "Phone";
    case 3:  return "Network";
    case 4:  return "Audio";
    case 5:  return "Peripheral";
    case 6:  return "Imaging";
    case 7:  return "Wearable";
    case 8:  return "Toy";
    default: return "Unknown";
    }
}

/* A name from the class alone. The minor class distinguishes keyboard from mouse
 * within Peripheral, which is what makes this useful for the device M4 cares about:
 * bit 6 of the minor field is keyboard, bit 7 is pointing device. */
static const char *NameForCoD(unsigned long cod)
{
    short major = CodMajor(cod);
    short minor = (short)((cod >> 2) & 0x3F);

    if (major == 5) {
        if ((minor & 0x30) == 0x10) return "Bluetooth Keyboard";
        if ((minor & 0x30) == 0x20) return "Bluetooth Mouse";
        if ((minor & 0x30) == 0x30) return "Keyboard/Mouse";
        return "Bluetooth Peripheral";
    }
    /* ★ AUDIO, BY MINOR CLASS -- and this is a discriminator, not decoration.
     *
     * "Audio Device" cannot tell the user whether the thing that answered is their own
     * headphones or a neighbour's car stereo, and the address cannot either. Run 45 saw
     * NINE audio devices answer in one inquiry, so this is the crowded case, not the
     * hypothetical one. The minor class arrives free in the same result. */
    if (major == 4) {
        switch (minor) {
        case 0x01: return "Headset";
        case 0x02: return "Hands-free Device";
        case 0x04: return "Microphone";
        case 0x05: return "Loudspeaker";
        case 0x06: return "Headphones";
        case 0x07: return "Portable Audio";
        case 0x08: return "Car Audio";
        case 0x09: return "Set-top Box";
        case 0x0A: return "HiFi Audio";
        case 0x0B: return "VCR";
        case 0x0C: case 0x0D: return "Video Camera";
        case 0x0E: return "Video Monitor";
        /* ⭐ 0x0F IS WHY A PROJECTOR CALLED ITSELF AN AUDIO DEVICE. "Video Display and
         * Loudspeaker" is the assigned name and it is what a projector with speakers
         * reports; the table stopped at 0x0E, so it fell through to the default below
         * and the user watched an Epson projector introduce itself as audio equipment. */
        case 0x0F: return "Video Display";
        case 0x10: return "Video Conferencing";
        case 0x12: return "Toy";
        /* ⚠⚠ "A/V Device", NOT "Audio Device". Major class 4 is AUDIO/VIDEO, and this
         * branch is reached exactly when the minor class is one we do not know -- so
         * naming it audio is asserting the half of the category we have no evidence
         * for. An honest "we know it is A/V and not which" costs one character and
         * cannot be wrong; the old label was wrong for every video device that ever
         * gets added to the spec. */
        default:   return "A/V Device";
        }
    }
    if (major == 2) return "Phone";
    if (major == 1) return "Computer";
    return "Bluetooth Device";
}

/* ---- PER-KIND ROW ICONS ----------------------------------------------------- *
 *
 * The artwork the user supplied, as six 16x16 families at IDs 200..205 in
 * bt_device_icons.r. IDs, not an array, because Rez resource IDs are what
 * PlotIconID takes and an index would just be a second thing to keep in step.
 *
 * ⚠ kIconGenericBT is BOTH a classification result and the fallback: a row whose
 * class we cannot read is honestly a generic Bluetooth device, which is exactly what
 * that glyph says. kIconPanelOwn is the last resort only -- see the LDEF. */
#define kIconGenericBT  200
#define kIconKeyboard   201
#define kIconMouse      202
#define kIconPhone      203
#define kIconComputer   204
#define kIconAudio      205
#define kIconPanelOwn   128

/* ⚠ THE SAME SWITCH SHAPE AS NameForCoD, DELIBERATELY, and it must stay that way: a
 * row whose name says "Bluetooth Keyboard" beside a mouse icon is worse than no icon
 * at all, because the two disagree in front of the user. The minor-class masks below
 * are copied from NameForCoD rather than re-derived for the same reason. */
static short IconForCoD(unsigned long cod)
{
    short major = CodMajor(cod);
    short minor = (short)((cod >> 2) & 0x3F);

    if (major == 5) {
        if ((minor & 0x30) == 0x10) return kIconKeyboard;
        if ((minor & 0x30) == 0x20) return kIconMouse;
        /* 0x30 is a combo keyboard/mouse. NameForCoD calls it "Keyboard/Mouse"; the
         * keyboard is the half this project is actually chasing, so it gets the
         * keyboard glyph rather than a generic one. */
        if ((minor & 0x30) == 0x30) return kIconKeyboard;
        return kIconGenericBT;      /* a Peripheral that is neither */
    }
    /* Every audio minor maps to the one headphones glyph. NameForCoD distinguishes
     * nine of them in TEXT because that is the crowded case, but nine 16x16 audio
     * icons would be nine near-identical smudges. */
    if (major == 4) return kIconAudio;
    if (major == 2) return kIconPhone;
    if (major == 1) return kIconComputer;
    return kIconGenericBT;
}

/* ⚠ A bond carries no class of device -- the key store keeps addresses and keys, not
 * CoD -- so a row listed from the bond alone cannot be named from its class. It says so
 * rather than inventing a kind it might not be. */
/* ⚠⚠ ALL THREE OF THESE KEY OFF codKnown, NOT fromScan, and they must stay in step
 * with each other. Mixing the two tests would let the name and the icon disagree about
 * the same row, which is worse than either being unhelpful. */
/* ★★★★ v11.5: THE DEVICE'S OWN NAME, unpacked from the block.
 *
 * The user: the Name column should read "Pixel 7 Pro" and "A1016 Keyboard", not "Phone"
 * and "Bluetooth Keyboard" -- the Kind column already carries the generic word, so a
 * class-of-device label in the Name column was a duplicate of its own neighbour.
 *
 * ⚠ The driver can only learn a name while the device is CONNECTED (Remote_Name_Request
 * pages otherwise, and this card refuses to page), so a name is remembered in the Kinds
 * prefs file the moment it is seen and used from there ever after -- which is what lets
 * an offline device still show its real name. */
static const char *NameFromBlock(const DispRow *d, char *scratch)
{
    short si;
    if (gBlock == NULL) return NULL;
    for (si = 0; si < (short)gBlock[kWNameCount] && si < 2; si++) {
        unsigned long base = (unsigned long)kWNameBase + (unsigned long)si * 8;
        short wi, n = 0;
        if (gBlock[base + 0] != d->addrHi || gBlock[base + 1] != d->addrLo) continue;
        for (wi = 0; wi < 6; wi++) {
            unsigned long w = gBlock[base + 2 + wi];
            short k;
            for (k = 3; k >= 0; k--) {
                char ch = (char)((w >> (k * 8)) & 0xFF);
                if (ch == 0) { scratch[n] = 0; return n ? scratch : NULL; }
                /* ⚠ A remote name is attacker-controlled text from another device.
                 * Anything outside printable ASCII is dropped rather than drawn -- a
                 * control character in a DrawString is at best a glyph nobody can read
                 * and at worst a cursor that walks out of the cell. */
                if (ch >= 32 && ch < 127 && n < 23) scratch[n++] = ch;
            }
        }
        scratch[n] = 0;
        return n ? scratch : NULL;
    }
    return NULL;
}

/* ⚠ Keyboard/mouse/pointing per the Bluetooth class-of-device major+minor. Only these
 * care whether the HID channel is open; everything else is "connected" the moment the
 * ACL link exists. */
/* ★★★★★★ THE INSTALLED DRIVER'S VERSION AS A NUMBER, from the tag it publishes.
 *
 * ⚠⚠ THE PANEL AND THE DRIVER SHIP SEPARATELY AND THE USER INSTALLS THEM SEPARATELY --
 * that is not a hypothetical, it is how this whole session went: seven panel builds
 * against one driver. So a panel rule that depends on driver BEHAVIOUR has to ask which
 * driver is actually there, and the tag is already in the block.
 *
 * ⭐ Returns major * 100 + minor, so 13.2 is 1302 and comparisons read naturally.
 * Returns 0 when there is no block or the tag is not one of ours, which compares LESS
 * than every real version -- so an unknown driver gets the conservative branch.
 *
 * ⚠ ONE DECODER. The About box had its own copy of this arithmetic; two copies of the
 * same encoding is how the column tables and the Help window geometry drifted earlier
 * today, and this one is no more entitled to a second copy than they were. */
static short DriverVerBCD(void)
{
    unsigned long v;
    char  c0, cM, cm;
    short maj = -1;

    if (gBlock == NULL) return 0;
    v  = gBlock[kWBuild];
    c0 = (char)((v >> 24) & 0xFF);
    cM = (char)((v >> 16) & 0xFF);
    cm = (char)((v >>  8) & 0xFF);

    if      (cM >= '0' && cM <= '9') maj = (short)(cM - '0');
    else if (cM >= 'A' && cM <= 'Z') maj = (short)(10 + (cM - 'A'));
    if (c0 != 'v' || maj < 0 || cm < '0' || cm > '9') return 0;
    return (short)(maj * 100 + (cm - '0'));
}

/* ★★★★★★ IS A HID CHANNEL ACTUALLY OPEN? ASK BOTH PATHS.
 *
 * ⚠⚠ THE BUG THIS FIXES: the A1016 was connected, typing, and reading "Linked, not
 * ready". The log settles it -- BTstack's hid_host reported "channels OPENED 1,
 * closed 0, HID REPORTS 53" while the raw L2CAP counter this test used read 0. The
 * keyboard was never the problem; the panel was asking the handler that does not run.
 *
 * ⚠ THIS IS THE SAME DEFECT AS v10.2's, WRITTEN DOWN AND THEN REPEATED. bt_btstack.c
 * already carries the note: BTCheck read kWHidDecodeRc, "a word written only where the
 * decoder runs -- which was the dormant l2cap handler", so fifty perfect reports
 * printed as NOT RECEIVED. An assumption about which handler is live, expressed as a
 * choice of counter, and it expired silently the second time too.
 *
 * ⇒ OPENS MINUS CLOSES, not "opened at least once". A cumulative counter never returns
 * to zero, so `!= 0` would have reported a disconnected keyboard as ready for the rest
 * of the session -- trading one wrong status for its mirror image. */
/* ★★★★★★ 15.6 -- ASK "WHICH HAPPENED LAST", NOT "WHICH HAPPENED MORE OFTEN".
 *
 * ⚠⚠ THE COUNT TEST WAS WRONG AND IT REPORTED A WORKING KEYBOARD AS BROKEN. It read
 * `gBlock[kWBhOpened] > gBlock[kWBhClosed]`, which silently assumes every close was
 * preceded by a COUNTED open. It is not: an incoming HID channel that is accepted and
 * then dies before HID_SUBEVENT_CONNECTION_OPENED ever fires bumps `closed` without ever
 * bumping `opened`. One of those, and the two counters sit equal forever.
 *
 * Measured 2026-09-17 23:33 (BTCheck v99.68, and the run BEFORE it is the control):
 *
 *                          worked (99.67)      stuck (99.68)
 *     incoming connections  1                   2
 *     channels OPENED       1                   1
 *     closed                0                   1
 *     opened at (ms)        46873               1852607
 *     closed at (ms)        0                   1851546
 *     HID REPORTS           56                  78      <== reports in BOTH
 *
 * 1 > 0 is true, so the first read "Connected". 1 > 1 is false, so the second read
 * "Linked, not ready" -- while 78 HID reports were arriving and the user was typing. And
 * it can never recover: nothing will open a second channel, so the arithmetic is stuck
 * for the rest of the session, which is exactly the "it never changed to Connected"
 * report.
 *
 * ⇒ The driver already publishes the timestamps (kWBhOpenedMs 565, kWBhClosedMs 567), so
 * this needs nothing from the driver. Comparing them answers the real question -- was the
 * most recent event an open or a close -- and is correct in every case the counts get
 * wrong:
 *     never opened, never closed   0 > 0            false   right
 *     opened, still open           N > 0            true    right
 *     opened then closed           closed > opened  false    right
 *     closed-without-open, then opened  1852607 > 1851546  TRUE   <== the fixed case
 *
 * ⚠ THE RAW L2CAP PATH KEEPS THE COUNT TEST, and that is a known asymmetry rather than an
 * oversight: kWHidOpened/kWHidClosed (292/293) have no Ms twins in the driver's block, so
 * there is nothing to compare. That path is dormant on this hardware -- BTCheck has
 * reported `accepted (HID ctrl) 0` in every run since hid_host took over -- so it is the
 * fallback for a route we do not use. If it is ever revived, give it two Ms words and
 * bring it over; do not trust the counts.
 *
 * ⚠ Wraparound: these are millisecond counters, so they wrap after ~49 days of uptime and
 * a driver reload restarts them from zero. Both failure modes are a wrong reading for one
 * session on a machine that has been up for seven weeks, which is not worth a wider fix. */
/* ★★★★★★ 15.8 -- STOP DERIVING THIS. READ THE FLAG THE DRIVER MAINTAINS.
 *
 * ⚠⚠ THIS FUNCTION HAS NOW BEEN WRONG TWICE, IN OPPOSITE DIRECTIONS, and both times it
 * was because it inferred "a channel is open" from counters that do not mean that:
 *
 *   15.5 and earlier: `kWBhOpened > kWBhClosed`. Wrong after a close with no COUNTED
 *     open -- a channel accepted and killed before OPENED fires bumps `closed` and never
 *     `opened`, so the two sit equal forever. Measured 2026-09-17: reported "Linked, not
 *     ready" while 78 HID reports were arriving and the user was typing.
 *
 *   15.6 and 15.7: `kWBhOpenedMs > kWBhClosedMs`. Fixes that case and breaks another,
 *     because gBhOpenedMs is stamped on EVERY OPENED event including failures. Measured
 *     2026-09-18: reported "Connected" with `open status 105`, `HID REPORTS 0`, and a
 *     keyboard that would not type. The count test would have been right there.
 *
 * ⇒ Neither expression is the fact. The fact is a piece of driver state, so the driver
 * keeps it: kWBhLive is set where a channel becomes usable and cleared where it stops.
 * The same word now backs the driver's own outgoing-connect guard, which had the third
 * variant of this bug. If a future reader needs "is it open", read this word -- do not
 * reconstruct it from cid, counts or timestamps. All three have been tried.
 *
 * ⚠ The raw L2CAP path keeps its count test: kWHidOpened/kWHidClosed (292/293) have no
 * live flag and no timestamps, and that path is dormant (`accepted (HID ctrl) 0` in every
 * run since hid_host took over). Give it a flag too if it is ever revived. */
static Boolean HidChannelOpen(void)
{
    if (gBlock == NULL) return false;
    if (gBlock[kWBhLive] != 0)                       return true;  /* hid_host  */
    if (gBlock[kWHidOpened] > gBlock[kWHidClosed])   return true;  /* raw L2CAP */
    return false;
}

/* ★★★★★★ 15.9 -- IS DELETING THIS BOND A ONE-WAY DOOR?
 *
 * ⚠⚠ THIS EXISTS BECAUSE THE INFORMATION WAS ALREADY THERE AND THE PANEL DID NOT READ IT.
 * BTCheck has printed a "PAIRING SAFETY NET" section in every run for weeks, and when
 * both these counters are zero it says, in as many words, "NOTHING CAPTURED. DO NOT
 * DELETE THE A1016 BOND YET... deleting it would be a ONE-WAY DOOR". On 2026-09-20 the
 * bond was deleted anyway, on a machine where that warning was in the log on screen, and
 * the A1016 then refused to pair through repeated scans: `link key requests 1`,
 * `keys published 0`, `last ACL conn status 8` (Connection Timeout). Recovery was a
 * BATTERY PULL, because a device that believes it is bonded reconnects rather than
 * re-advertising, so a power cycle cannot clear it.
 *
 * ⇒ The party holding the stale bond is the DEVICE. Our database and the card's memory
 * were both empty afterwards, exactly as intended -- and there is no way to tell the
 * keyboard. So the delete does what it claims locally and silently strands the peer.
 * That is worth a sentence in the dialog rather than a paragraph in a log nobody reads
 * before clicking.
 *
 * ⚠ IT WARNS, IT DOES NOT REFUSE. Delete must stay clickable: the user's own standing
 * requirement is that it works on an unpaired row purely as "get this off my list", and
 * a button that sometimes refuses is worse than one that always explains. The warning is
 * also suppressed when there is no bond at all -- nothing to lose, nothing to say.
 *
 * ⚠ gBlock NULL means the driver is not loaded and we genuinely cannot tell. Say nothing
 * rather than guess: an unfounded scare is its own defect. */
static Boolean DeleteIsOneWay(void)
{
    if (gBlock == NULL) return false;
    return (Boolean)(gBlock[kWRlkCaptured] == 0 && gBlock[kWLkStored] == 0);
}

static Boolean IsInputDevice(unsigned long cod)
{
    unsigned long major = (cod >> 8) & 0x1F;
    if (major != 0x05) return false;            /* 5 = Peripheral */
    return true;
}

static const char *RowNameOf(const DispRow *d)
{
    /* ★★★★★ v15.3: THE USER'S OWN NAME BEATS EVERYTHING, including the device's.
     * That ordering is the point of the feature: someone with two identical Apple
     * keyboards renames one, and a name the device supplies must not win back. */
    {
        const char *al = AliasFor(d->addrHi, d->addrLo);
        if (al != NULL) return al;
    }
    /* ⭐ The device's own name beats every generic label we could invent. */
    {
        static char nameBuf[24];
        const char *nm = NameFromBlock(d, nameBuf);
        if (nm != NULL) return nm;
    }
    /* ⚠ A row that exists only because the CARD holds a key for it is neither a
     * "Paired device" (we hold no bond) nor unknown-in-general -- and calling it
     * "Paired device" would be the panel claiming a bond it does not have, which is
     * the same lie the paired flag was kept false to avoid. */
    if (!d->codKnown && d->inCard) return "In card's memory";
    /* ⚠ v8.2: a paged row holds no bond of ours, so "Paired device" would be a
     * straight falsehood on it -- and it is the row the user is being asked to pair,
     * which makes that the worst possible place to claim a pairing already exists. */
    if (!d->codKnown && d->paged)  return "Nearby device";
    return d->codKnown ? NameForCoD(d->cod) : "Paired device";
}
static const char *RowKindOf(const DispRow *d)
{
    if (!d->codKnown && d->inCard) return "Not paired here";
    if (!d->codKnown && d->paged)  return "Asked to connect";
    return d->codKnown ? KindForCoD(d->cod) : "Unknown";
}
/* ⚠ A bond-only row has no CoD, so it gets the generic glyph -- NOT a guess from the
 * address. This is the same honesty RowNameOf applies by saying "Paired device", and
 * the icon has to agree with it: a keyboard icon on a row named "Paired device" would
 * be inventing a fact the key store does not hold. */
static short RowIconOf(const DispRow *d)
{
    return d->codKnown ? IconForCoD(d->cod) : kIconGenericBT;
}

/* The icon for a display row by index, or 0 when the row does not exist. Mirrors the
 * range guard in CellText -- the List Manager can ask to draw a cell we have no device
 * for, and 0 means "draw nothing" rather than plotting a misleading glyph. */
static short RowIconID(short row)
{
    if (row < 0 || row >= gRowCount) return 0;
    return RowIconOf(&gRow[row]);
}

/* ============================ THE CUSTOM LDEF ================================
 *
 * ★★★ ONE CELL PER ROW, AND THE LDEF DRAWS THE COLUMNS.
 *
 * The List Manager has a SINGLE cellSize for the whole list, which is why four real
 * columns can never have independent widths. The way out -- and the way Extensions
 * Manager does it, confirmed by finding LDEF:2 in its resource fork -- is to stop
 * asking the List Manager for columns at all:
 *
 *     dataBounds is ONE column wide, so a "cell" IS a row.
 *     cellSize.h is the whole table width, so the uniform size stops mattering.
 *     Our LDEF draws Name / Kind / ID / Status inside that row rect at OUR offsets.
 *
 * Selection, scrolling, hit-testing and the scroll bar all keep working, because they
 * operate on rows -- which is exactly what we now have. Column widths become ours
 * entirely, which is what makes them adjustable.
 *
 * ⚠⚠ AND IT NEEDS NO CODE RESOURCE. Retro68 builds 68K-only code resources, so an
 * 'LDEF' resource was the obstacle -- but Lists.h defines NewListDefUPP as
 * NewRoutineDescriptor(fn, uppListDefProcInfo, GetCurrentArchitecture()), i.e. a Mixed
 * Mode descriptor for a PowerPC function in this very application. The limitation
 * simply does not apply.
 *
 * ⚠ listDefProc is typed Handle, and the List Manager calls *listDefProc as code. So
 * the descriptor has to live INSIDE a handle, and that handle must stay LOCKED for the
 * list's lifetime -- a routine descriptor that moves under the List Manager is a jump
 * into whatever landed there instead. HLockHi + HLock below, never unlocked.
 */
static void AddrToStr(unsigned long hi, unsigned long lo, char *out);

/* ★★★★ v11.3: THE ID COLUMN IS GONE, at the user's suggestion and it is the right
 * call. The BD_ADDR was eating ~88px of a 355px list to show a value that the Device
 * Details pane below already prints in full, under "Address:" -- so the panel was
 * spending a quarter of its width on a duplicate, while Status had eleven characters
 * and could not say why a button was disabled.
 *
 * ⚠ I had been shaving 8px at a time off Name and ID to buy Status a few characters.
 * That was optimising the wrong thing: the question is not how to divide four columns,
 * it is whether there should be four. Deleting the redundant one gives Status ~190px
 * outright and costs nothing a user cannot see two inches lower. */
#define kNumCols 3
/* ⚠ Widths, not positions: a drag adjusts one width and the rest follow, which is what
 * keeps the columns contiguous with no arithmetic at the draw site. The last column
 * absorbs the remainder so rounding can never leave a gap at the right edge. */
/* ⚠ SIZED AGAINST THE LONGEST STRING EACH COLUMN CAN ACTUALLY HOLD, computed rather
 * than eyeballed -- the first guess gave Status 37px, which cannot fit "Connected"
 * and would have silently clipped the one label this project spent a day getting right:
 *
 *     Name   "Bluetooth Keyboard"  ~105px      Kind   "Hands-free Device"  ~99px
 *     ID     "94:DB:56:xx:xx:xx"   ~99px       Status "Connected"          ~60px
 *
 * ★ Those total 375 against 351 available, so something MUST be tight. Kind is the one
 * squeezed, because it is the only column whose value is also visible in the device
 * information pane below -- clipping it costs the least. That is the point of making
 * these adjustable: the user can widen whichever one their devices actually need. */
/* ⚠⚠ THE STATUS COLUMN HAD ~77 PIXELS -- ELEVEN CHARACTERS -- AND THAT IS WHY IT ONLY
 * EVER SAID ONE WORD. "Paired" had to cover both "we hold a key" and "and it is not
 * connected", which is how a correctly-disabled Disconnect button became
 * indistinguishable from a bug. The column could not afford the truth.
 *
 * Name and ID give up 24px between them (a BD_ADDR still fits in 88: it is fixed-width
 * and the font is not proportional enough to lose it), which buys Status ~101px, about
 * 15 characters. That is enough for "Paired, offline" and it is why the states below
 * are worded to that budget rather than to what reads best in isolation. ⚠ Measure
 * before lengthening any of them again. */
static short gColW[kNumCols] = { 150, 70, 0 };

/* ⚠⚠ THE TITLES SIT NEXT TO THE WIDTHS BECAUSE THEY ARE ONE TABLE, and because keeping
 * them apart is exactly how the header loop came to draw a column that does not exist.
 * See the header-drawing block: removing the ID column took kNumCols from 4 to 3 and
 * took this array from four titles to three, and left a hardcoded `c < 4` behind.
 *
 * ⇒ The size is DEDUCED from the initialiser and then checked against kNumCols below,
 * so a column added or removed without a matching title is a COMPILE ERROR rather than
 * a read off the end of an array. Declaring it [kNumCols] would have been the vacuous
 * version of this check: C would have zero-filled the missing entry and said nothing. */
static const char *gColTitle[] = { "Name", "Kind", "Status" };
typedef char gColTitleMatchesNumCols[
    (sizeof(gColTitle) / sizeof(gColTitle[0]) == kNumCols) ? 1 : -1];

static short ColLeft(short c)
{
    short i, x = kListL + 1;
    for (i = 0; i < c; i++) x += gColW[i];
    return x;
}

static short ColWidth(short c)
{
    if (c == kNumCols - 1) {
        short used = 0, i;
        for (i = 0; i < kNumCols - 1; i++) used += gColW[i];
        return (short)((kListR - 16 - kListL - 1) - used);
    }
    return gColW[c];
}

/* The text for one column of one display row. Shared by the LDEF and nothing else --
 * the header draws its own titles. */
static const char *CellText(short row, short col, char *scratch)
{
    if (row < 0 || row >= gRowCount) return "";
    switch (col) {
    case 0: return RowNameOf(&gRow[row]);
    case 1: return RowKindOf(&gRow[row]);
    /* ⚠ case 2 WAS the BD_ADDR and is now Status -- see kNumCols. AddrToStr is still
     * used by the Details pane and by both delete confirmations, so nothing is dead. */
    default:
        /* ★★★★ THE WORDS A USER MEANS, not the ones the implementation uses.
         *
         * This said "Bond on file" for a device that had just been successfully
         * paired -- accurate internally (we hold a key but have not yet handed it to
         * the controller) and useless to the person reading it, who had watched the
         * pairing succeed on their phone. Reported after the first genuine OS 9
         * pairing, and they were right.
         *
         * ⚠ "Connected" here means the CONTROLLER ASKED US FOR THIS KEY BY ADDRESS
         * THIS SESSION, which only happens on a real authenticated link. It is
         * evidence a link existed, NOT live link tracking -- the panel does not follow
         * ACL state per address, and claiming it did would be the kind of overstated
         * label this project keeps having to walk back. If that distinction ever
         * matters more than it does now, track the link and say so honestly. */
        /* ⚠⚠⚠ THE `handed` SHORTCUT IS GONE, AND IT HAD BEEN CONTRADICTING THE
         * COMMENT DIRECTLY BELOW IT FOR THREE VERSIONS. That line returned "Connected"
         * before anything else was consulted -- above the refusal check whose own
         * comment says "Disconnected OUTRANKS EVERY OTHER STATUS". It did not.
         *
         * ⚠ AND `handed` IS NOT A LIVE FACT. Its own comment says so: "the controller
         * asked us for this key by address THIS SESSION ... evidence a link existed,
         * NOT live link tracking". It is session-sticky and never clears, so a device
         * that connected once read "Connected" for the rest of the session however
         * thoroughly it had since gone away. That shortcut dates from before there WAS
         * live tracking; kWLiveCount arrived in v11.1 and nobody went back and removed
         * the guess it replaced.
         *
         * ⇒ Every status this column returns is now a statement about NOW. `handed`
         * still appears in Device Details, as "Paired - the controller used this key",
         * which is where a historical fact belongs.
         *
         * ⭐⭐ "Disconnected" OUTRANKS EVERY OTHER STATUS, and that precedence is the
         * safety of the whole feature. A refused device is indistinguishable from a
         * broken one from the outside -- you press keys and nothing happens -- so the
         * one place that can explain it must say so before it says anything else.
         * Showing "Paired" for a device we are actively refusing would be a lie of
         * exactly the kind the user has already been bitten by twice. */
        if (gBlock != NULL && gBlock[kWBlockActive] != 0
            && gBlock[kWBlockHi] == gRow[row].addrHi
            && gBlock[kWBlockLo] == gRow[row].addrLo) return "Disconnected by you";
        /* ★★★★★★ v11.1: "Connected" IS A DIFFERENT FACT FROM "Paired", AND THE LIST
         * HAS NEVER SAID WHICH.
         *
         * ⚠⚠ "Paired" here has always meant ONLY "this Mac holds a link key for it".
         * A device can be paired and not connected, or connected and not paired, and
         * the column said "Paired" for both -- so the user could not tell whether
         * Disconnect being greyed out meant a bug or simply nothing to disconnect.
         * That is the same "cannot tell the state by looking" fault as the stale rows,
         * in the one column whose whole job is to report state.
         *
         * The driver publishes the live connections (verified against BTstack's own
         * list, not a shadow), so the panel can finally distinguish all four. */
        {
            short k;
            for (k = 0; gBlock != NULL && k < (short)gBlock[kWLiveCount] && k < 2; k++) {
                /* ⚠⚠ BRACES, AND THE COMPILER CAUGHT THEIR ABSENCE. This `if` was
                 * braceless and I added a SECOND statement under it, so the final
                 * return sat OUTSIDE the address test: the moment the live list held
                 * any entry at all, EVERY row would have returned "Connected" whether
                 * it matched or not. -Wmisleading-indentation is exactly the warning
                 * for that, and it was in the build output I nearly skimmed past. */
                if (gBlock[kWLiveA0Hi + k * 2]     == gRow[row].addrHi
                 && gBlock[kWLiveA0Hi + k * 2 + 1] == gRow[row].addrLo) {
                    /* ★★★★★★ "Connected" WAS OVERCLAIMING, and the user caught it by
                     * asking what the statuses mean: the keyboard read "Connected"
                     * while it was blinking its pairing light and typing nothing.
                     *
                     * ⚠ An ACL link is NOT a working input device. For a keyboard or a
                     * mouse the user cares about exactly one thing -- is the HID
                     * channel open -- and that is a separate fact this column was
                     * quietly folding into the word "Connected". For a phone there is
                     * no HID channel and never will be, so "Connected" is the honest
                     * answer there; the distinction is only meaningful for an INPUT
                     * device, which is why it is gated on the class of device. */
                    if (IsInputDevice(gRow[row].cod) && !HidChannelOpen())
                        return "Linked, not ready";
                    return gRow[row].paired ? "Connected" : "Connected, no key";
                }
            }
        }
        /* ⭐ THE USER'S POINT, AND IT IS THE WHOLE REASON THIS COLUMN EXISTS: "the
         * phone's status should instead read 'Paired, disconnected' or else the user
         * will have no way of knowing why the Disconnect button is disabled and could
         * be led to believe it is a bug." A status that does not explain a disabled
         * control is the same fault as a stale row, moved one column to the right.
         *
         * ⚠ "offline" rather than "disconnected" ON PURPOSE: "Disconnected" is taken,
         * and it means something materially different -- that WE are refusing the
         * device because the user clicked Disconnect. Two states one word apart, one of
         * which the user chose and one of which just happened, must not share a name. */
        /* ⭐ THE USER'S OWN WORDING, now that there is room for it. With the ID
         * column gone Status has ~131px -- about 21 characters -- so the state can be
         * stated in full instead of compressed into one word that explained nothing. */
        /* ⚠ AFTER the live check, never before it. If the device HAS come back, the
         * live branch above has already said so and this must not overwrite a fact with
         * a hope -- which is precisely the shape of the bug that put "Connected" on a
         * keyboard that was typing nothing. */
        if (gWaiting && gWaitHi == gRow[row].addrHi && gWaitLo == gRow[row].addrLo)
            return "Waiting for device\311";
        if (gRow[row].paired) return "Paired, disconnected";
        return "Available";
    }
}

/* ⚠ CALLED BY THE LIST MANAGER, at task level, inside its own port and clip. It must
 * not change the port, must not move memory, and must not draw outside lRect. */
static pascal void BTListDef(short lMessage, Boolean lSelect, Rect *lRect,
                             Cell lCell, short lDataOffset, short lDataLen,
                             ListHandle lHandle)
{
    (void)lDataOffset; (void)lDataLen; (void)lHandle;

    switch (lMessage) {
    case lInitMsg:
    case lCloseMsg:
        break;

    case lDrawMsg:
    case lHiliteMsg: {
        short  c;
        Rect   r = *lRect;
        char   scratch[24];
        RgnHandle save = NewRgn();
        RGBColor savedFore, savedBack;
        /* ★ Baseline for VERTICALLY CENTRED text. The first version drew at
         * r.bottom - 3, which crammed every row against its own lower edge and is a
         * large part of why the table read as unfinished next to Extensions Manager. */
        short base = (short)(r.top + (r.bottom - r.top) / 2 + 4);

        GetForeColor(&savedFore);
        GetBackColor(&savedBack);

        /* ★★★★★★ ON A HILITE PASS, DO NOT REDRAW THE CONTENT AT ALL -- ONLY TOGGLE.
         *
         * ⚠⚠ THIS IS THE BUG THE USER PHOTOGRAPHED: "sometimes there's text that gets
         * overlayed on top of other text and it makes things hard to read until the
         * screen gets re-drawn." The cell was erased only on lDrawMsg, so a hilite pass
         * redrew the text STRAIGHT OVER whatever was already there. That was invisible
         * for as long as the text could not change between passes -- which is how it
         * behaved for months -- until v11.x gave the Status column five wordings that
         * change as devices connect and disconnect. Then a selection redraw started
         * landing "Connected, no key" on top of "Paired, disconnected".
         *
         * ⚠⚠⚠ AND THE OBVIOUS FIX -- "just erase on both messages" -- IS WRONG, which I
         * nearly shipped. The selection here is an InvertRect over the pixels already on
         * screen, and the contract below is "on lHiliteMsg invert ALWAYS", i.e. TOGGLE.
         * Erasing first destroys the state being toggled: a row being DESELECTED would
         * erase, redraw unselected, invert, and come out looking selected.
         *
         * ⇒ The right shape is the classic LDEF one: lHiliteMsg toggles the highlight
         * and touches nothing else. Content is drawn on lDrawMsg, where the erase has
         * always been correct. No overlay is possible because no text is drawn twice. */
        if (lMessage == lHiliteMsg) {
            Rect h = r;
            if (h.right > lRect->right) h.right = lRect->right;
            LMSetHiliteMode((unsigned char)(LMGetHiliteMode() & 0x7F));
            InvertRect(&h);
            return;
        }

        EraseRect(&r);

        TextFont(applFont); TextSize(9); TextFace(0);

        /* ★★ AN ICON PER ROW, which is the single biggest visual difference from
         * Extensions Manager -- its Name column carries a 16x16 icon and ours carried
         * nothing, so ours read as a spreadsheet rather than a device list.
         *
         * ★★★ NOW PER KIND, not one glyph for every row. The artwork the user supplied
         * is six 16x16 families at IDs 200..205 (bt_device_icons.r); IconForCoD picks
         * one from the same class-of-device switch that names the row, so the glyph and
         * the name can never disagree.
         *
         * ⚠⚠ Black on white for the plot -- PlotIconID goes through CopyMask, which
         * honours the port colours, and the row has just been erased to the list's
         * background. Restored below, unconditionally.
         *
         * ⚠ THE FALLBACK CHAIN IS NOT DECORATION. PlotIconID's result was previously
         * discarded, which was safe only because ID 128 ships in this very binary and
         * always exists. The device IDs are new, and a family that failed to Rez would
         * make EVERY row's icon silently vanish -- a blank first column with no error
         * anywhere. So: specific icon, else the generic Bluetooth glyph from the same
         * new family, else ID 128, which has shipped since the panel had icons at all.
         * Each step only runs if the previous one actually failed, so the normal path
         * is still a single call. This is the fail-open shape this project already
         * learned to use in its recovery paths. */
        { short iconID = RowIconID(lCell.v);
          if (iconID != 0) {
              Rect ir;
              ir.left   = (short)(ColLeft(0) + 2);
              ir.right  = (short)(ir.left + 16);
              ir.top    = (short)(r.top + ((r.bottom - r.top) - 16) / 2);
              ir.bottom = (short)(ir.top + 16);
              if (ir.bottom > r.bottom) ir.bottom = r.bottom;
              ForeColor(blackColor);
              BackColor(whiteColor);
              /* ⚠ The `iconID == kIconGenericBT` test comes FIRST and short-circuits.
               * Written the other way round -- "failed AND is not the generic one" --
               * a row whose icon already IS the generic glyph would fail out of the
               * chain entirely and never reach 128, which is the one ID guaranteed to
               * exist. That was the first version of this. */
              if (PlotIconID(&ir, atNone, ttNone, iconID) != noErr) {
                  if (iconID == kIconGenericBT ||
                      PlotIconID(&ir, atNone, ttNone, kIconGenericBT) != noErr)
                      (void)PlotIconID(&ir, atNone, ttNone, kIconPanelOwn);
              }
              RGBForeColor(&savedFore);
              RGBBackColor(&savedBack);
          } }

        for (c = 0; c < kNumCols; c++) {
            Rect cr;
            const char *s;
            Str255 t;
            short textLeft;

            SetRect(&cr, ColLeft(c), r.top, (short)(ColLeft(c) + ColWidth(c)), r.bottom);
            if (cr.right > r.right) cr.right = r.right;
            if (cr.left >= cr.right) continue;

            /* ⚠ CLIP PER COLUMN. Without this a long name runs straight through the
             * next column and the table stops looking like a table -- and the List
             * Manager's own clip is the whole row, so it will not save us. */
            if (save) { GetClip(save); ClipRect(&cr); }

            /* Column 0 makes room for the icon; the rest start at their own edge. */
            textLeft = (c == 0) ? (short)(cr.left + 22) : (short)(cr.left + 4);

            s = CellText(lCell.v, c, scratch);
            MoveTo(textLeft, base);
            PStr(t, s);
            DrawString(t);

            if (save) SetClip(save);
        }

        /* ⚠⚠ SEPARATORS ARE NO LONGER DRAWN HERE. Per-row drawing was the reason the
         * table did not read like Extensions Manager's: the rules stopped at the last
         * row, so the empty part of the well below the rows had no columns in it and
         * the whole thing looked like text floating in a box rather than a table.
         *
         * They are drawn once over the FULL well height by DrawColumnRules(), after
         * LUpdate, which makes them continuous by construction instead of by relying on
         * per-row segments tiling perfectly. */

        /* ⚠ HILITE LAST, and with the whole row: hiliting per column would leave the
         * separators unhilited and the selection would look broken.
         *
         * ⚠⚠ AND lHiliteMsg MEANS *TOGGLE*, NOT "DRAW IF SELECTED". This tested
         * `if (lSelect)` for BOTH messages, so a row being DESELECTED -- where lSelect
         * is false -- was never un-inverted and its highlight stayed on screen. The
         * table then looked multi-select while the List Manager had in fact selected
         * exactly one row: gSelRow comes from LGetSelect and every button acts on that
         * single index, so the behaviour was right and only the drawing lied.
         *
         * lOnlyOne was set on selFlags and was never the problem. The correct LDEF
         * contract is: on lDrawMsg invert IF selected, on lHiliteMsg invert ALWAYS. */
        /* ⚠ `lMessage == lHiliteMsg ||` is GONE because a hilite pass now returns far
         * above and never reaches here -- leaving it would be dead but misleading, and
         * the next reader would take it as evidence that hilite still falls through.
         * On lDrawMsg the rule is unchanged and unchanged for the original reason: draw
         * the row, then invert it IF it is selected. */
        if (lSelect) {
            Rect h = r;
            if (h.right > kListR - 16) h.right = kListR - 16;
            LMSetHiliteMode((unsigned char)(LMGetHiliteMode() & 0x7F));
            InvertRect(&h);
        }

        RGBForeColor(&savedFore);
        RGBBackColor(&savedBack);
        if (save) DisposeRgn(save);
        break;
    }
    }
}

static ListDefUPP gListDefUPP = NULL;
static Handle     gListDefHdl = NULL;

/* ★★ COLUMN RULES FOR THE WHOLE WELL, not per row.
 *
 * This is the difference that made the table look unfinished next to Extensions
 * Manager. EM's column rules run the full height of its list, straight past the last
 * row and down to the bottom of the well, so the empty space still reads as columns.
 * Drawing them inside each row instead left the lower part of the well blank, and the
 * rows then looked like text floating in a box.
 *
 * ⚠ Drawn AFTER LUpdate, so it sits on top of the rows -- which is also how EM looks,
 * its rules being visible through a selected row. */
static void DrawColumnRules(void)
{
    RGBColor grey, saved;
    RgnHandle clip;
    GrafPtr   savePort;
    short c;

    if (gList == NULL || gWin == NULL) return;

    /* ⚠ Set the port rather than inheriting it. Every call below -- GetForeColor,
     * ClipRect, MoveTo/LineTo -- is port-relative, and this is called from both the
     * update handler and the click handler, where the current port is only gWin
     * because it happens to have been left that way. That ambient dependency is the
     * kind of claim about surrounding code that expires without a compiler error. */
    GetPort(&savePort);
    SetPort(gWin);

    GetForeColor(&saved);
    clip = NewRgn();
    if (clip) GetClip(clip);

    /* ⚠ Clip to the well's INTERIOR. Without this a rule would draw over the theme
     * list-box frame and the scroll bar, which is exactly the kind of one-pixel
     * trespass that makes a window look homemade. */
    { Rect well;
      SetRect(&well, (short)(kListL + 1), (short)(kListT + 1),
              (short)(kListR - 16), (short)(kListB - 1));
      ClipRect(&well);

      grey.red = grey.green = grey.blue = 0xC000;
      RGBForeColor(&grey);
      for (c = 1; c < kNumCols; c++) {
          short x = ColLeft(c);
          if (x > well.left && x < well.right) {
              MoveTo(x, well.top);
              LineTo(x, (short)(well.bottom - 1));
          }
      }
    }

    if (clip) { SetClip(clip); DisposeRgn(clip); }
    RGBForeColor(&saved);
    SetPort(savePort);
}

static void BuildList(void)
{
    Rect  lr, db;
    Point csz;

    SetRect(&lr, kListL, kListT, kListR, kListB);
    /* ★ ONE column. A cell is a row; see the block comment above. */
    SetRect(&db, 0, 0, 1, 0);
    csz.h = (short)(kListR - kListL - 16);
    /* ★ 14 -> 18. Extensions Manager's rows are about 20px and carry a 16x16 icon;
     * ours were 14 with text jammed against the bottom edge, which is most of why the
     * table looked unfinished beside it. 18 fits a 16px icon with a pixel of air above
     * and below, and the well still shows 8 rows. */
    csz.v = 18;

    /* drawIt, no grow box, no horizontal scroll, WITH vertical scroll -- the
     * Extensions Manager shape. Rows arrive from the driver, not from a table here. */
    gList = LNew(&lr, &db, csz, 0, gWin, true, false, false, true);
    /* ★ SINGLE SELECTION. LNew has no flag for it -- selFlags is set on the record
     * afterwards, and lOnlyOne makes the List Manager deselect the previous row for
     * us instead of allowing a shift-drag to accumulate several.
     *
     * ⚠ NOT COSMETIC. Every button here acts on gRow[gSelRow], a SINGLE index, so a
     * multi-row selection was already a state the rest of the panel could not
     * represent: LGetSelect returns the first selected cell and the others simply do
     * not exist as far as Pair or Delete are concerned. The list was offering a
     * selection the actions cannot honour. */
    if (gList != NULL) (**gList).selFlags = lOnlyOne;
    if (gList == NULL) return;

    /* ★★ INSTALL THE LDEF. A handle whose CONTENTS are the routine descriptor, locked
     * high and never unlocked, because the List Manager calls *listDefProc as code.
     *
     * ⚠ If any step fails we leave the stock LDEF in place rather than installing a
     * half-built one: the list then draws single-column text, which is ugly and
     * obviously wrong, and that is far better than a jump through a bad pointer. */
    gListDefUPP = NewListDefUPP(&BTListDef);
    if (gListDefUPP != NULL) {
        gListDefHdl = NewHandle((Size)sizeof(RoutineDescriptor));
        if (gListDefHdl != NULL) {
            BlockMoveData(gListDefUPP, *gListDefHdl, (Size)sizeof(RoutineDescriptor));
            HLockHi(gListDefHdl);
            HLock(gListDefHdl);
            (**gList).listDefProc = gListDefHdl;
        }
    }
}

/* Format a BD_ADDR from the two packed words. */
static void AddrToStr(unsigned long hi, unsigned long lo, char *out)
{
    const char *dig = "0123456789ABCDEF";
    unsigned char b[6];
    short i, j = 0;
    b[0]=(unsigned char)(hi>>16); b[1]=(unsigned char)(hi>>8); b[2]=(unsigned char)hi;
    b[3]=(unsigned char)(lo>>16); b[4]=(unsigned char)(lo>>8); b[5]=(unsigned char)lo;
    for (i = 0; i < 6; i++) {
        if (i) out[j++] = ':';
        out[j++] = dig[(b[i] >> 4) & 15];
        out[j++] = dig[ b[i]       & 15];
    }
    out[j] = 0;
}

/* ★ Rebuild the list from the driver's published table.
 *
 * ⚠ REBUILT WHOLESALE rather than appended to. The 2003 panel appended, because its
 * driver handed it one result at a time through a callback; we are POLLING a table
 * the driver rewrites, and appending to a polled snapshot would duplicate rows every
 * time the poll caught the same entry. Wholesale is also what makes de-duplication
 * the driver's job alone, which is where it belongs. */
static void RefreshList(void)
{
    short r;

    if (gList == NULL) return;

    /* LSetDrawingMode, not LDoDraw: the latter is only a macro under the old-routine-
     * names conditional in Lists.h, so it compiles as an implicit declaration here. */
    LSetDrawingMode(false, gList);               /* no flicker while rebuilding */
    if ((**gList).dataBounds.bottom > 0)
        LDelRow(0, 0, gList);                    /* 0 = delete every row */

    /* ★ ONE CELL PER ROW. With our LDEF installed the cell needs no contents at all --
     * the LDEF reads gRow[cell.v] directly, and duplicating the text into List Manager
     * storage would be a second copy to keep in step with gRow[].
     *
     * ⚠⚠ BUT THE FALLBACK HAS TO ACTUALLY WORK, and mine did not. The install comment
     * claimed "if it fails we leave the stock LDEF in place: the list then draws
     * single-column text" -- which was FALSE, because this loop had stopped writing any
     * text. A failed install therefore produced a completely EMPTY table, which is
     * indistinguishable from the feature not existing. So when the LDEF is not
     * installed we put a single flat line in the cell: cramped, obviously a fallback,
     * and legible. A graceful degradation that degrades to nothing is not one. */
    for (r = 0; r < gRowCount; r++) {
        (void)LAddRow(1, r, gList);
        if (gListDefHdl == NULL) {
            char  line[96], addr[24];
            short n = 0, c;
            Cell  cell;
            for (c = 0; c < kNumCols && n < (short)(sizeof(line) - 2); c++) {
                const char *s = CellText(r, c, addr);
                while (*s && n < (short)(sizeof(line) - 2)) line[n++] = *s++;
                if (c < kNumCols - 1) line[n++] = ' ';
            }
            cell.h = 0; cell.v = r;
            LSetCell(line, n, cell, gList);
        }
    }
    /* ⚠ The four-column fill that used to live here is DELETED, not retired behind
     * an #if 0. The status-label reasoning it carried now lives in CellText, which is
     * where the labels are actually produced -- a commented-out copy would drift from
     * it and then mislead whoever reads it next. */
    LSetDrawingMode(true, gList);
    InvalRect(&(**gList).rView);
}

/* ★ Poll the block. Returns true if anything changed and the window needs redrawing.
 *
 * ⚠ NO NEW MECHANISM: the event loop already wakes ten times a second on
 * WaitNextEvent's timeout, so this rides that. docs/SCAN-DESIGN.md §5. */
static short gLastScanCount = -1;
/* ★★★ v7.4: and the count of devices that have PAGED us, watched the same way.
 *
 * ⚠⚠ v8.2 ADDED THE PAGED ROW SOURCE AND FORGOT THE TRIGGER, which made the source
 * useless in the one situation it was built for. MergeBonded() runs only when the scan
 * count or the key count changes; nothing looked at kWPagedCount. So on a fresh boot,
 * with the A1016's stored key deleted, the keyboard could page us all it liked and no
 * row would ever appear -- and with no row there is nothing to select and Pair cannot
 * be clicked at all. Worse, a scan was not a reliable workaround either: the scan
 * branch only fires when the RESULT COUNT changes, and an inquiry the keyboard does
 * not answer leaves it at 0.
 *
 * ⇒ A row source with no change trigger is not a feature, it is dead code. */
static short gLastPagedCount = -1;
static unsigned long gLastScanState = 0xFFFFFFFFUL;

/* ★★★★★★ AND THE SAME OMISSION AGAIN, ONE ROW SOURCE FURTHER ON: NOTHING WATCHED THE
 * LIVE CONNECTIONS OR THE HID CHANNEL.
 *
 * ⚠⚠ THIS IS THE WHOLE OF "I clicked Disconnect and the status did not change, then I
 * clicked Scan For Devices and it did." Every status the list can show comes out of
 * three places: the key list, the live-connection set, and the HID open/close counters.
 * PollScan watched the first and never looked at the other two -- so connecting,
 * disconnecting, and a HID channel opening or closing were all invisible until some
 * OTHER change happened to force a rebuild. A scan changes the responder count, which
 * is why a scan "fixed" it: the refresh was a side effect of an unrelated trigger.
 *
 * ⚠ THE DRIVER WAS NEVER AT FAULT AND NEITHER WAS THE DATA. BT_PublishFromTimer runs
 * BT_SampleLiveConns on the driver's own timer, so these words were correct within a
 * tick the entire time. The panel simply had no reason to look at them again. Checked
 * before writing any of this, because "the block is stale" and "nobody re-read the
 * block" need completely different fixes and look identical from the outside.
 *
 * ⚠ THE HID EPOCH IS A SUM OF MONOTONIC COUNTERS, so any open or close moves it by at
 * least one and it cannot alias -- unlike an XOR of addresses, which can. It is a
 * change signal only; HidChannelOpen() is what interprets the values. */
static short         gLastLiveCount = -1;
static unsigned long gLastLive[4]   = { 0, 0, 0, 0 };
static unsigned long gLastHidEpoch  = 0xFFFFFFFFUL;
/* ⚠ kWBlockActive/Hi/Lo: the refusal RowStatus reports as "Disconnected by you". Left
 * out of the first version of this poll, which is why Connect looked like a dead
 * button -- it cleared the refusal and nothing re-read the word. */
static unsigned long gLastBlock[3]  = { 0xFFFFFFFFUL, 0, 0 };

/* ★★★★ THE ONE-DEEP PENDING DISCONNECT, and it exists because the mailbox holds one
 * command. See the note at the Delete button: a second MailboxSend before the driver's
 * timer runs overwrites the first, and the servicer's `ack = seq` swallows the gap
 * silently. Delete needs to send two commands, so the second one waits its turn.
 *
 * ⚠ IT EXPIRES. Without a deadline a disconnect queued against a quiet radio could sit
 * here while the user re-pairs that very device, and then fire -- disconnecting
 * something they had just got working, minutes after the click that queued it. Five
 * seconds is far longer than the driver's timer needs and far shorter than a re-pair. */
#define kPendDiscTimeout 300UL          /* ticks: 5 seconds, for the WHOLE queue */
#define kPendMax         2
/* ⚠ WHICH command, not just which address: Delete sends kCmdAllowDevice for a device
 * it is already refusing and kCmdDisconnect for one that is connected. */
/* ★★★★★★ TWO DEEP, AND THE SECOND SLOT IS A BUG FIX NOT A GENERALISATION.
 *
 * ⚠⚠ DELETE LEFT A STANDING REFUSAL BEHIND. kCmdDisconnect sets the block and THEN
 * drops the link -- deliberately, so the peer cannot re-page in the window between the
 * two. Delete reused it to end the connection, and thereby installed a refusal against
 * a device whose pairing it had just removed. The user found it in T5: a deleted,
 * power-cycled, re-scanned A1016 came back reading "Disconnected by you" instead of
 * "Available", because the panel was still refusing a device it no longer had any
 * relationship with.
 *
 * ⇒ Delete on a CONNECTED device now queues TWO commands: disconnect, then allow. The
 * first ends the link so the row can go; the second takes the refusal back off, because
 * a standing instruction must not outlive the relationship it was attached to. That is
 * the same rule as everything else this week -- do not leave state behind that describes
 * something that is gone.
 *
 * ⚠ ONE DEADLINE FOR THE WHOLE QUEUE, stamped when it is filled. Per-command deadlines
 * would let a stalled queue live twice as long, and the point of the deadline is to
 * bound the operation, not each step of it. */
static unsigned long gPendCmd[kPendMax], gPendHi[kPendMax], gPendLo[kPendMax];
static short         gPendCount = 0;
static unsigned long gPendTick;

static void PendClear(void) { gPendCount = 0; }

static void PendQueue(unsigned long cmd, unsigned long hi, unsigned long lo)
{
    if (gPendCount >= kPendMax) return;
    if (gPendCount == 0) gPendTick = TickCount();
    gPendCmd[gPendCount] = cmd;
    gPendHi[gPendCount]  = hi;
    gPendLo[gPendCount]  = lo;
    gPendCount++;
}

/* ★★★ THE MAILBOX MADE THE SCAN ASYNCHRONOUS, AND THE ARROWS DID NOT NOTICE.
 *
 * Reported from run 3: plain clicks "often" showed no chasing arrows, while
 * Option+Shift clicks showed them every time. That difference is the whole diagnosis.
 * A direct call runs gap_inquiry_start before it returns, so kWScanState is already 1
 * by the panel's next poll. A mailbox write returns immediately and the driver's timer
 * picks it up up to 100 ms later -- during which kWScanState still reads 2, COMPLETE,
 * left over from the PREVIOUS inquiry. The poll then does exactly what it was told to:
 *
 *     if (gLastScanState == 2 && gArrows) HideControl(gArrows);
 *
 * ...and hides the arrows for a scan that has not started yet. "Often" rather than
 * "always" because it is a race against poll timing.
 *
 * ⚠ I PREDICTED THIS EXACT BUG when the mailbox was designed and did not fix it,
 * because the timer question looked more urgent. It was one poll away from being
 * visible.
 *
 * gScanReqSeq is the mailbox sequence our request was given. Until kWCmdAck reaches
 * it, the driver has not started our inquiry and kWScanState says nothing about it. */
static unsigned long gScanReqSeq = 0;

/* True while a scan request is written but not yet picked up. Signed subtraction so a
 * sequence wrap cannot invert the comparison. */
static Boolean ScanRequestPending(void)
{
    if (gBlock == NULL || gScanReqSeq == 0) return false;
    return (long)(gBlock[kWCmdAck] - gScanReqSeq) < 0;
}
static Boolean gScanAsked = false;      /* a scan is genuinely running             */

/* ⚠⚠ SEPARATE FROM gScanAsked, AND THE SEPARATION IS THE WHOLE POINT.
 *
 * v1.1 gated the failure text on gScanAsked, which was itself set only when the call
 * SUCCEEDED -- so the branch that reports a failure could never run. A failed press
 * fell through to the idle text and the button looked inert while saying nothing about
 * why. That is a diagnostic that cannot fire, which is the third bug of this exact
 * shape found in this codebase, and the first one I wrote myself.
 *
 * gScanTried means "the user pressed the button", full stop. Whether it worked is
 * gLastScanRc's business. */
static Boolean gScanTried = false;
static unsigned long gScanStartTick = 0;   /* when the arrows started spinning */
static Boolean gScanStalled = false;       /* the watchdog fired               */

/* ⚠ A GRACE PERIOD USED TO LIVE HERE, and the reasoning behind it was wrong in a way
 * worth recording. It read run 45's 142 empty event reads and 140 stall clears as
 * proof of an ongoing idle treadmill that would always eventually service a queued
 * command, so it waited two seconds before calling a pending command a failure.
 *
 * Those counters were a BURST DURING BRING-UP, not a steady state. Run 47 compared
 * the two snapshots directly: empty event reads 207 -> 207, stall clears 204 -> 204,
 * seq 2 -> 9, ack 2 -> 2. The treadmill had stopped, and no wait of any length would
 * have helped. The direct call removes the question rather than tuning the timeout. */

static Boolean PollScan(void)
{
    short         n, i;
    unsigned long st;
    Boolean       changed = false;

    if (gBlock == NULL) return false;

    /* ★★★★ DRAIN THE PENDING DISCONNECT. Delete queues one because the mailbox holds a
     * single command; this is where it gets its turn, once the driver has taken the
     * key deletion that went first. See PendQueue for why they are not sent there. */
    /* ★ EXPIRE THE POST-Connect TRANSIENT. ⚠ The redraw is part of the expiry: a state
     * that times out without repainting has not timed out as far as the user is
     * concerned. See gWaiting. */
    if (gWaiting && (TickCount() - gWaitTick) > kWaitTimeout) {
        gWaiting = false;
        RefreshList();
        changed = true;
    }

    if (gPendCount > 0) {
        if ((TickCount() - gPendTick) > kPendDiscTimeout) {
            PendClear();                /* ⚠ expired -- see the deadline note */
        } else if (MailboxCaughtUp() && gProbeMode == kProbeFull) {
            short i;
            (void)MailboxSend(gPendCmd[0], gPendHi[0], gPendLo[0]);
            /* ⚠ ONE PER CAUGHT-UP POLL. Shifting and sending the next immediately would
             * overwrite the one just sent -- the mailbox is a single slot and the
             * servicer acknowledges the CURRENT sequence number, which is the whole
             * reason this queue exists. */
            for (i = 1; i < gPendCount; i++) {
                gPendCmd[i - 1] = gPendCmd[i];
                gPendHi[i - 1]  = gPendHi[i];
                gPendLo[i - 1]  = gPendLo[i];
            }
            gPendCount--;
        }
    }

    /* ★ NO STALE-DATA WINDOW ANY MORE, and it is worth saying why the guard that used
     * to be here is gone rather than just deleting it.
     *
     * With the old block-and-poll channel, BT_ScanStart ran at some unknown later time
     * -- so between the button and the service, kWScanCount still described the
     * PREVIOUS inquiry and repopulating from it flashed old rows over a cleared list.
     * That needed a suppression window, which in turn had to be bounded so it could not
     * latch. The direct call is synchronous: by the time it returns, BT_ScanStart has
     * already emptied the table. There is nothing stale left to guard against, so the
     * guard and its bound both go. */
    st = gBlock[kWScanState];
    n  = (short)gBlock[kWScanCount];
    if (n < 0) n = 0;
    if (n > kMaxScan) n = kMaxScan;

    if (n != gLastScanCount) {
        for (i = 0; i < n; i++) {
            gScan[i].addrHi = gBlock[kWScanBase + i * 4 + 0];
            gScan[i].addrLo = gBlock[kWScanBase + i * 4 + 1];
            gScan[i].cod    = gBlock[kWScanBase + i * 4 + 2];
            gScan[i].parm   = gBlock[kWScanBase + i * 4 + 3];
        }
        /* ⚠ EVIDENCE OF DISCOVERABILITY, taken here where the inquiry results are. An
         * address that answers an inquiry has earned its row back; see UnhideAddr. */
        for (i = 0; i < n; i++) UnhideAddr(gScan[i].addrHi, gScan[i].addrLo);
        gScanRows = n;
        gLastScanCount = n;
        MergeBonded();
        RefreshList();
        changed = true;
    }

    /* ★★★ A DEVICE PAGED US -- REBUILD THE LIST. See gLastPagedCount for why this was
     * missing and what it cost. No copy step: MergeBonded reads the paged slots
     * straight out of the block, so the count is purely the change signal. */
    {
        short p = (short)gBlock[kWPagedCount];
        if (p < 0) p = 0;
        if (p > kPagedSlots) p = kPagedSlots;
        if (p != gLastPagedCount) {
            gLastPagedCount = p;
            MergeBonded();
            RefreshList();
            changed = true;
        }
    }

    /* ★★★★★ THE LIVE CONNECTIONS AND THE HID CHANNEL. See gLastLiveCount for why this
     * was missing and what it cost: Disconnect appearing to do nothing. */
    {
        short         lc  = (short)gBlock[kWLiveCount];
        unsigned long a0h = gBlock[kWLiveA0Hi + 0], a0l = gBlock[kWLiveA0Hi + 1];
        unsigned long a1h = gBlock[kWLiveA0Hi + 2], a1l = gBlock[kWLiveA0Hi + 3];
        unsigned long ep  = gBlock[kWBhOpened]  + gBlock[kWBhClosed]
                          + gBlock[kWHidOpened] + gBlock[kWHidClosed];
        /* ★★★★★ AND THE BLOCK STATE, WHICH IS WHY "Connect" LOOKED DEAD.
         *
         * ⚠⚠ THE BUTTON WAS WORKING. Connect sends kCmdAllowDevice, the driver clears
         * the refusal, and the panel had nothing watching kWBlockActive -- so the row
         * went on saying "Disconnected by you" and the click looked ignored. This is
         * the SAME defect as the live set one commit ago, one status further on, and I
         * shipped the fix for the first without asking what else RowStatus reads. It
         * reads four sources; I wired up two.
         *
         * ⇒ Every word RowStatus can consult is now watched here. The list is not a
         * judgement call: it is the set of block words that function reads. */
        unsigned long bka = gBlock[kWBlockActive];
        unsigned long bkh = gBlock[kWBlockHi], bkl = gBlock[kWBlockLo];
        if (lc  != gLastLiveCount || ep  != gLastHidEpoch ||
            a0h != gLastLive[0]   || a0l != gLastLive[1]  ||
            a1h != gLastLive[2]   || a1l != gLastLive[3]  ||
            bka != gLastBlock[0]  || bkh != gLastBlock[1] ||
            bkl != gLastBlock[2]) {
            gLastLiveCount = lc;
            gLastLive[0] = a0h; gLastLive[1] = a0l;
            gLastLive[2] = a1h; gLastLive[3] = a1l;
            gLastBlock[0] = bka; gLastBlock[1] = bkh; gLastBlock[2] = bkl;
            gLastHidEpoch = ep;
            /* ⚠ A CONNECTED DEVICE IS NEVER HIDDEN. See UnhideAddr. */
            if (lc > 0) UnhideAddr(a0h, a0l);
            if (lc > 1) UnhideAddr(a1h, a1l);
            /* ⚠⚠ AND THE WAIT ENDS WHEN THE DEVICE ARRIVES, not when the clock runs
             * out. RowStatus checks the live set first, so the row would read
             * "Connected" either way -- but leaving gWaiting set means a device that
             * connects and then drops inside the 30 seconds falls back to "Waiting for
             * device" instead of the truth. A transient must end on its success
             * condition, not only on its deadline. */
            if (gWaiting && ((lc > 0 && a0h == gWaitHi && a0l == gWaitLo) ||
                             (lc > 1 && a1h == gWaitHi && a1l == gWaitLo)))
                gWaiting = false;
            /* ⚠ RefreshList ALONE, and NOT MergeBonded -- checked rather than assumed.
             * The first version of this called MergeBonded too, with a comment claiming
             * the live set was a row source. It is not: MergeBonded builds gRow from the
             * inquiry responders, the key list and the PAGED slots, and never reads
             * kWLive. The live set decides what a row SAYS, not whether it exists, and a
             * device that has just connected gets its row from the paged count -- which
             * has its own branch above. Rebuilding the rows here would have been a
             * no-op justified by a false statement about the call graph, which is worse
             * than the no-op. */
            RefreshList();
            changed = true;
        }
    }

    /* ★ THE RADIO STATE COMES FROM THE DRIVER, not from what we last clicked. If
     * something else changes it -- another instance, a reload, bring-up itself -- the
     * buttons follow rather than asserting a state that is no longer true. */
    {
        Boolean on = (gBlock[kWRadioOn] != 0);
        if (on != gRadioOn) {
            gRadioOn = on;
            if (gOnRadio)  SetControlValue(gOnRadio,  on ? 1 : 0);
            if (gOffRadio) SetControlValue(gOffRadio, on ? 0 : 1);
            /* ⚠ FLAG, DO NOT DRAW. PollScan is a state function and its caller sets
             * the port AFTER it returns, so drawing the logo here would blit into
             * whatever port happened to be current. The main loop acts on this. */
            gRadioLogoDirty = true;
            changed = true;
        }
    }

    /* ★ Bonds change independently of scans -- a pairing can complete with no inquiry
     * running at all -- so they get their own change check rather than riding on the
     * responder count. */
    {
        short k = (short)gBlock[kWKeyCount];
        if (k < 0) k = 0;
        if (k > kMaxKeys) k = kMaxKeys;
        if (k != gLastKeyCount) {
            short i, n = 0, d, dn = 0;
            unsigned long mask = 0, pub = gBlock[kWKeyHandedMask];

            /* ⚠⚠ THIS RELOAD USED TO UNDO THE DELETE, ONE POLL LATER.
             *
             * Deleting set gLastKeyCount = -1 to force a fresh read, and the local
             * removal from gKeyHi/gKeyLo happened in the same breath. Then this ran,
             * saw k != -1, and reloaded ALL the keys straight back out of the block --
             * which the driver had not republished yet, because BT_KeyPublish only runs
             * on a USB completion. The device reappeared instantly and stayed until a
             * scan generated the traffic that finally refreshed the block. Two of my own
             * mechanisms pulling against each other.
             *
             * ★ Now a deleted address is held in a PENDING list and filtered out of
             * whatever the block still reports, until the block stops reporting it. The
             * forced re-read is then safe and even useful: it applies the filter at
             * once. */
            for (d = 0; d < gDelCount; d++) {
                Boolean still = false;
                for (i = 0; i < k; i++) {
                    if (gBlock[kWKeyBase + i * 2 + 0] == gDelHi[d] &&
                        gBlock[kWKeyBase + i * 2 + 1] == gDelLo[d]) { still = true; break; }
                }
                /* Keep pending only while the driver still lists it. Once the driver
                 * agrees, the entry has done its job and must go -- a pending list that
                 * never drains would hide a device re-paired later. */
                if (still) { gDelHi[dn] = gDelHi[d]; gDelLo[dn] = gDelLo[d]; dn++; }
            }
            gDelCount = dn;

            for (i = 0; i < k; i++) {
                unsigned long hi = gBlock[kWKeyBase + i * 2 + 0];
                unsigned long lo = gBlock[kWKeyBase + i * 2 + 1];
                if (AddrPendingDelete(hi, lo)) continue;
                gKeyHi[n] = hi;
                gKeyLo[n] = lo;
                /* ⚠ The handed-out mask is indexed by PUBLISHED position, so filtering
                 * shifts it. Rebuilt bit by bit against the new index -- carrying the
                 * old mask across would mislabel a surviving bond as "Paired". */
                if (pub & (1UL << i)) mask |= (1UL << n);
                n++;
            }
            gKeyRows       = n;
            gKeyHandedMask = mask;
            /* ⚠ Track the DRIVER's count, not ours, or change detection stops firing. */
            gLastKeyCount  = k;
            MergeBonded();
            RefreshList();
            changed = true;
        }
    }
    if (st != gLastScanState) {
        gLastScanState = st;
        /* ★ Stamp when "scanning" is first OBSERVED and clear it when it ends, so the
         * watchdog measures the driver's state rather than our own request. */
        if (st == 1) {
            if (gScanStartTick == 0) gScanStartTick = TickCount();
        } else {
            gScanStartTick = 0;
            gScanStalled   = false;
        }
        changed = true;
    }
    return changed;
}

/* ============================ THE STATUS FILE ================================
 *
 * ⚠⚠ WHY THIS EXISTS. On 2026-09-02 the MDD froze while idle with only this panel
 * open. Cursor and keyboard dead, MacsBug NOT triggered, no NMI -- so no exception was
 * raised, which means it was not a crash but a livelock or an interrupts-off spin. A
 * freeze like that leaves NOTHING: the driver's counter block lives in the System heap
 * and dies at restart, and the USB Expert log channel has never produced a byte.
 *
 * ★★ WHAT IT BUYS, and it is one specific thing: it attributes the freeze to an
 * EXECUTION LEVEL. The panel is task level and the driver's counters advance at
 * interrupt level, so the last line written distinguishes:
 *
 *   panel polls advancing, driver counters STALE   -> the driver stopped first
 *   both stop together                             -> the whole machine went at once
 *   file simply ends                               -> task level died first
 *
 * That is not a diagnosis, but it halves the search space, and right now we have
 * literally no information at all.
 *
 * ⚠ SAFE BY CONSTRUCTION:
 *   - Task level only. Called from the event loop, never from a poll or a callback.
 *     The File Manager below task level is this project's oldest hard rule.
 *   - REWRITTEN IN PLACE, never appended, and SetEOF'd -- so it cannot grow without
 *     bound over a long idle period, which is precisely when we need it.
 *   - FlushVol after every write. Without it the buffered line never reaches the
 *     platter and a freeze loses exactly the evidence this was built to keep.
 *   - Every File Manager result is checked; on any error the file is closed and the
 *     feature disables itself rather than retrying into a wedged volume.
 */
/* ★★ SELF-ATTRIBUTING ACTION LOG, and it exists because a test went to waste.
 *
 * The bisect asked for an Option-click and a normal click in ONE boot, then read a
 * single aggregate counter: driver-being-removed 1. Two presses, one teardown, and no
 * way to say which press caused it. The counters cannot attribute what the panel did
 * because they do not know the panel acted.
 *
 * So the panel now records each action against the driver's own Initialize counter,
 * read immediately before and after: "W3>4" means a Walk-only press saw kWInit go 3 to
 * 4 (it caused a teardown), "N4>4" means a Normal press changed nothing. One line, in
 * the status file that already exists, and every future run attributes itself no matter
 * what order the user clicks in. */
#define kWInit 3               /* ⚠ driver's kWInit; see src/bt_probe.c's enum */
static char  gActs[72];
static short gActLen = 0;

/* ★★★ THE TIMELINE, and it exists because the bisect above was measuring the wrong
 * thing for four driver versions.
 *
 * ⚠⚠ WHY THE before>after LOG COULD NEVER SETTLE THIS. ActNote reads the driver's bind
 * counter immediately after DriverStartScan RETURNS. The Expert's unload is not
 * synchronous with the call, so a teardown caused by a press lands AFTER that second
 * read and shows up as the NEXT press's "before". The 3.7 run makes it plain:
 *
 *     acts N1>1 N1>1 N1>1 N1>1 N1>1 N1>1 N2>2 N3>3 N3>3 N4>4 N5>5 N5>5
 *
 * Twelve presses, not one showing a teardown, and yet the count climbed 1 -> 5 in the
 * GAPS. So "X<n>><n>" never meant "X is innocent", it meant "no teardown had completed
 * yet" -- and that is equally true of the W, S and P readings the bisect rests on.
 * Those verdicts are NOT established.
 *
 * ★ THE FIX IS TO STOP ATTRIBUTING TO PRESSES AND ATTRIBUTE TO TIME. The panel polls
 * at roughly 0.85 polls per tick (36878 polls across 43612 ticks in that same run), so
 * simply SAMPLING the bind counter every poll timestamps a teardown to within about a
 * sixtieth of a second. Actions get the same clock. Correlation then becomes something
 * read off the log rather than inferred from a window.
 *
 * This is a panel-only instrument on purpose: the driver's block is adopted across
 * re-binds and kWInit keeps counting, so nothing in the driver needs to change -- no
 * block growth, no kWEnd move, no BTCheck rebuild. */
static char          gTLog[512];
static short         gTLen = 0;
static unsigned long gT0 = 0;                       /* TickCount at panel start */
static unsigned long gLastBinds = 0xFFFFFFFFUL;     /* 0xFFFF.. = never sampled */

static void TLogStr(const char *s)
{
    while (*s && gTLen < (short)(sizeof(gTLog) - 1)) gTLog[gTLen++] = *s++;
    gTLog[gTLen] = 0;
}

static void TLogNum(unsigned long v)
{
    char b[12];
    short n = 0;
    if (v == 0) { TLogStr("0"); return; }
    while (v > 0 && n < 11) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && gTLen < (short)(sizeof(gTLog) - 1)) gTLog[gTLen++] = b[--n];
    gTLog[gTLen] = 0;
}

/* Seconds since the panel opened. Ticks are 1/60 s; seconds keep the log readable and
 * a teardown's latency after a press is what matters, not its exact tick. */
static unsigned long TSec(void)
{
    return (unsigned long)((TickCount() - gT0) / 60UL);
}

/* ⚠ BOTH WRITERS BAIL EARLY RATHER THAN WRAP. A ring that overwrote its oldest entries
 * would quietly destroy the beginning of the run, which is where the idle control
 * phase lives -- and a truncated tail announces itself by simply stopping, which is
 * the failure mode you can actually notice. */
static void TLogAct(char tag)
{
    char t[2];
    if (gTLen > (short)(sizeof(gTLog) - 24)) return;
    t[0] = tag; t[1] = 0;
    TLogStr(" "); TLogStr(t); TLogNum(TSec());
}

static void TLogBind(unsigned long n)
{
    if (gTLen > (short)(sizeof(gTLog) - 24)) return;
    TLogStr(" b"); TLogNum(n); TLogStr("@"); TLogNum(TSec());
}

/* ★ Sampled EVERY loop iteration, which is what makes a teardown attributable to a
 * moment instead of to a press. The first sighting is not a transition -- it is just
 * the count the driver already had when the panel opened. */
static void SampleBinds(void)
{
    unsigned long b;
    if (gBlock == NULL) return;
    b = gBlock[kWInit];
    if (b == gLastBinds) return;
    if (gLastBinds != 0xFFFFFFFFUL) TLogBind(b);
    gLastBinds = b;
}

static void ActNote(char tag, unsigned long before, unsigned long after)
{
    /* ⚠ Bounded: 72 bytes and it simply stops. An action log that grows without limit
     * inside a Str255 status line is how a log becomes a memory smash. */
    char  buf[24];
    short n = 0, i;
    if (gActLen > (short)(sizeof(gActs) - 14)) return;
    buf[n++] = tag;
    if (before > 99) before = 99;
    if (after  > 99) after  = 99;
    if (before >= 10) buf[n++] = (char)('0' + before / 10);
    buf[n++] = (char)('0' + before % 10);
    buf[n++] = '>';
    if (after >= 10) buf[n++] = (char)('0' + after / 10);
    buf[n++] = (char)('0' + after % 10);
    buf[n++] = ' ';
    for (i = 0; i < n; i++) gActs[gActLen++] = buf[i];
    gActs[gActLen] = 0;
}

static short   gStatRef   = 0;         /* open file refnum, 0 = not open        */
static short   gStatVRef  = 0;         /* the volume, for FlushVol              */
static Boolean gStatDead  = false;     /* an error occurred; stop trying        */
static unsigned long gStatLast = 0;    /* TickCount of the last write           */
static unsigned long gPollCount = 0;   /* task-level pulse: OUR liveness        */

#define kStatEveryTicks 900            /* ~15 s */

static void StatusOpen(void)
{
    FSSpec spec;
    short  vRefNum, ref;
    long   dirID;
    OSErr  err;

    if (gStatDead || gStatRef != 0) return;

    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) { gStatDead = true; return; }

    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Panel Status", &spec);
    if (err != noErr && err != fnfErr) { gStatDead = true; return; }
    if (err == fnfErr) {
        /* 'ttro'/'ttxt' so SimpleText opens it read-only with a double click. */
        if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr) {
            gStatDead = true; return;
        }
    }
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) { gStatDead = true; return; }
    gStatRef  = ref;
    gStatVRef = spec.vRefNum;      /* ⚠ needed by FlushVol, which takes a VOLUME */
}

static void StatusClose(void)
{
    if (gStatRef != 0) { (void)FSClose(gStatRef); gStatRef = 0; }
}

static void StatusWrite(Boolean force)
{
    Str255 line;
    long   count;
    unsigned long now = TickCount();

    if (gStatDead) return;
    if (!force && (now - gStatLast) < kStatEveryTicks) return;
    gStatLast = now;

    if (gStatRef == 0) { StatusOpen(); if (gStatRef == 0) return; }

    line[0] = 0;
    /* ⚠⚠ A FOURTH COPY OF THE VERSION, AND IT WENT STALE AND COST REAL CONFUSION.
     *
     * The 7.3 run's status file said "BTPanel 7.2", because bumping bt_panel.r,
     * cpanel/CMakeLists.txt's PANEL_VER and the 'BTcp' signature left this string
     * behind. The log then looked like the WRONG BINARY had been installed, and the
     * only thing that ruled that out was noticing a 7.2 panel could not have found a
     * v8.2 block at all -- it scans for 'ENDS' at word 445 and the block now ends at
     * 460. That is far too subtle a rescue to rely on twice.
     *
     * ⇒ CMake now greps this file for "BTPanel <PANEL_VER>" and fails the build if it
     * disagrees, the same shape as the driver's three-way version guard. */
    PStrCat(line, "BTPanel 19.2  ticks ");
    PStrCatNum(line, (long)now);
    PStrCat(line, "  polls ");
    PStrCatNum(line, (long)gPollCount);

    if (gBlock != NULL) {
        /* ★ The driver's own pulse, read from the shared block. If these stand still
         * while `polls` keeps climbing, the driver stopped and we did not. */
        PStrCat(line, "  intcomp ");
        PStrCatNum(line, (long)gBlock[kWIntComp]);
        PStrCat(line, "  pumps ");
        PStrCatNum(line, (long)gBlock[kWStkPump]);
        PStrCat(line, "  scan ");
        PStrCatNum(line, (long)gBlock[kWScanState]);
        PStrCat(line, "/");
        PStrCatNum(line, (long)gBlock[kWScanCount]);
        PStrCat(line, "  keys ");
        PStrCatNum(line, (long)gBlock[kWKeyCount]);
        PStrCat(line, "  binds ");
        PStrCatNum(line, (long)gBlock[kWInit]);
        /* ★ THE SERVICER, in the one line the user copies. seq/ack apart means the
         * timer is not picking the mailbox up, which is run 47's silent failure made
         * visible; timer standing at 0 means it never started at all. */
        PStrCat(line, "  mbx ");
        PStrCatNum(line, (long)gBlock[kWCmdSeq]);
        PStrCat(line, "/");
        PStrCatNum(line, (long)gBlock[kWCmdAck]);
        PStrCat(line, "  timer ");
        PStrCatNum(line, (long)gBlock[kWTimerRuns]);
    } else {
        PStrCat(line, "  NO DRIVER BLOCK");
    }
    if (gActLen > 0) { PStrCat(line, "  acts "); PStrCat(line, gActs); }
    PStrCat(line, "\r");

    /* ⚠ Rewrite from the start and truncate, so the file never mixes two runs.
     *
     * ★★ THE TIMELINE GETS ITS OWN LINE, and that is not cosmetic. `line` is a Str255
     * and PStrCat clamps at 255 bytes, so appending a growing timeline to it would
     * SILENTLY drop the tail -- losing exactly the late entries a churn run is about.
     * A second write has no such ceiling, and gTLog bounds itself at 512. */
    if (SetFPos(gStatRef, fsFromStart, 0) != noErr) { StatusClose(); gStatDead = true; return; }
    count = line[0];
    if (FSWrite(gStatRef, &count, &line[1]) != noErr)  { StatusClose(); gStatDead = true; return; }
    {
        /* ⚠ COUNTED FROM THE LITERAL, not written down. The first version of this said
         * 12 for an 8-character string and would have written four bytes of whatever
         * followed the literal into the user's status file. */
        static const char kTHdr[] = "timeline";
        long total = count;
        if (gTLen > 0) {
            long n = (long)(sizeof(kTHdr) - 1);      /* 8, excluding the NUL */
            if (FSWrite(gStatRef, &n, kTHdr) != noErr) { StatusClose(); gStatDead = true; return; }
            total += n;
            n = (long)gTLen;
            if (FSWrite(gStatRef, &n, gTLog) != noErr) { StatusClose(); gStatDead = true; return; }
            total += n;
            n = 1;
            if (FSWrite(gStatRef, &n, "\r") != noErr)  { StatusClose(); gStatDead = true; return; }
            total += n;
        }
        if (SetEOF(gStatRef, total) != noErr) { StatusClose(); gStatDead = true; return; }
    }
    /* ⚠⚠ THE POINT OF THE WHOLE EXERCISE. Without FlushVol the line sits in the cache
     * and a freeze loses it -- the one case this was written for. */
    (void)FlushVol(NULL, gStatVRef);
}

static void MakeControls(void)
{
    Rect r;
    Str255 t;

    /* On/Off. Radio buttons in a group box is the AppleTalk control panel idiom and
     * the right one for the era; the user's mockup had it and it stays. */
    /* ⚠⚠ SIZED TO THE TEXT, NOT TO THE BOX. These used to span kBtnL+12 to kBtnR-12 --
     * the full width of the group -- which was invisible only because the label's
     * background fill happened to match the theme grey. The moment anything left the
     * port's back colour set to white, both radios painted a white band clear across
     * the group and over the Bluetooth logo.
     *
     * A control whose rect is far larger than its content is also wrong for hit
     * testing: clicking empty space well to the right of "Off" would toggle it.
     *
     * ⚠ Measured in the SYSTEM font at 12pt, because that is what the Control Manager
     * draws a control title in -- measuring in whatever font happened to be current
     * would size the rect for the wrong glyphs. */
    { short wOn, wOff, wide;
      GrafPtr port; GetPort(&port);
      TextFont(systemFont); TextSize(12);
      PStr(t, "On");  wOn  = StringWidth(t);
      PStr(t, "Off"); wOff = StringWidth(t);
      wide = (short)((wOn > wOff ? wOn : wOff) + 22);   /* 16 glyph + 6 gap/pad */

      SetRect(&r, kBtnL + 12, 48, (short)(kBtnL + 12 + wide), 64);
      PStr(t, "On");
      gOnRadio  = NewControl(gWin, &r, t, true, 1, 0, 1, radioButProc, 0);
      SetRect(&r, kBtnL + 12, 66, (short)(kBtnL + 12 + wide), 82);
      PStr(t, "Off");
      gOffRadio = NewControl(gWin, &r, t, true, 0, 0, 1, radioButProc, 0);

      /* ★★ THE GROUP'S WIDTH FALLS OUT OF ITS CONTENT, computed HERE because this is
       * the only place the radio metrics exist. The box used to be as wide as the
       * button column below it, which the user correctly read as unjustified: two
       * short radios and a 32px logo do not fill 184px. The logo now sits just right
       * of the labels, and the frame closes just right of the logo.
       *
       * ⚠ NEVER NARROWER THAN THE CAPTION. " Bluetooth " is drawn ON the top rule
       * starting at kBtnL+10, so a box sized only to the radios would let the caption
       * run out past its own right edge -- which would look like the frame was broken
       * rather than the box being small. Measured in the same 12pt system font that
       * draws it, while that font is still current, for the same reason the radios
       * are: measuring in whatever font happened to be set would size for the wrong
       * glyphs. If the caption is the wider constraint it wins, leaving a little air
       * to the right of the logo rather than a clipped caption. */
      { short radiosR = (short)(kBtnL + 12 + wide);
        short capMin;
        PStr(t, " Bluetooth ");
        capMin = (short)(kBtnL + 10 + StringWidth(t) + 8);
        gLogoL = (short)(radiosR + 10);
        gGrpR  = (short)(gLogoL + 32 + 10);
        if (gGrpR < capMin) gGrpR = capMin; }

      TextFont(applFont); TextSize(9); }

    /* The button stack, in the order the user asked for. Scan For Devices is
     * separated at the bottom, as Tiger separates it: the others act on the
     * selection, that one starts something new. */
    /* ★★★★ THE BUTTONS ARE SIZED TO THEIR WIDEST LABEL, not to a hardcoded column.
     *
     * The user: "Make the Scan For Devices button only as wide as is needed to fit
     * that text, and then make the other three buttons that same matching width; right
     * now they are all much wider than needed." kBtnR was a fixed 588, which made every
     * button the width of the pane rather than the width of anything in it.
     *
     * ⚠ MEASURED WITH StringWidth IN THE SYSTEM FONT, not counted in characters. A
     * button's text is drawn in the system font, that font is Charcoal here and Chicago
     * elsewhere, and a character count would be wrong on the first machine that differs.
     * StringWidth asks the font that will actually draw it.
     *
     * ⚠ +24 for the push-button end caps and breathing room, which is the conventional
     * padding; the result is clamped so a very long localisation cannot run the buttons
     * off the pane or make them narrower than they are tall. */
    {
        short wNeed, i;
        /* ⚠ EVERY label that shares gBtnW belongs here. "Rename..." is narrower than
         * "Scan For Devices" so it changes nothing today -- but a label left out of
         * this list is one that silently overflows its button the day it is reworded. */
        const char *labels[5] = { "Scan For Devices", "Delete\311", "Disconnect",
                                  "Pair\311", "Rename\311" };
        TextFont(systemFont); TextSize(12);
        wNeed = 0;
        for (i = 0; i < 5; i++) {
            Str255 lt; short lw;
            PStr(lt, labels[i]);
            lw = StringWidth(lt);
            if (lw > wNeed) wNeed = lw;
        }
        gBtnW = (short)(wNeed + 24);
        if (gBtnW < 72)  gBtnW = 72;
        if (gBtnW > (short)(kBtnR - kBtnL)) gBtnW = (short)(kBtnR - kBtnL);
    }

    SetRect(&r, kBtnL, 114, (short)(kBtnL + gBtnW), 134);
    PStr(t, "Delete\311");      gDelete = NewControl(gWin, &r, t, true, 0,0,1, pushButProc, 0);
    SetRect(&r, kBtnL, 142, (short)(kBtnL + gBtnW), 162);
    /* ⚠ NO ELLIPSIS, and that is from the reference implementation rather than taste.
     * The user's Tiger observations (docs/TIGER-UI-REFERENCE.md §2) record that Apple's
     * Disconnect acts IMMEDIATELY with no confirmation dialog. In Mac HI an ellipsis
     * promises further input before anything happens, so "Disconnect..." would be
     * advertising a dialog that must not exist. Delete and Pair keep
     * theirs; both really do open something. */
    PStr(t, "Disconnect");      gDisconnect = NewControl(gWin, &r, t, true, 0,0,1, pushButProc, 0);
    SetRect(&r, kBtnL, 170, (short)(kBtnL + gBtnW), 190);
    /* ★★★★ WAS "Configure...", WHICH HAS BEEN DEAD SINCE THE PANEL EXISTED. It is
     * now the initiator-pairing button, because pairing is the one action that acts
     * on a SELECTED responder and this is the selection-driven slot that did nothing.
     * ⚠ Ellipsis kept: it opens a multi-step interaction -- the user must type the
     * PIN on the device -- which is exactly what Mac HI reserves an ellipsis for.
     * Compare Disconnect, which correctly has none because Tiger's acts at once. */
    PStr(t, "Pair\311");        gConfigure = NewControl(gWin, &r, t, true, 0,0,1, pushButProc, 0);
    /* ★★★★ v15.3: RENAME, directly under Pair and on the same 28px rhythm, because it
     * belongs to the same group -- the four buttons that act on the SELECTED row.
     * ⚠ Ellipsis, like Delete and Pair: it opens a window and asks for something. The
     * comment on Disconnect explains the rule -- an ellipsis promises further input,
     * so omitting it here would be the mirror of Disconnect's error. */
    SetRect(&r, kBtnL, 198, (short)(kBtnL + gBtnW), 218);
    PStr(t, "Rename\311");      gRename = NewControl(gWin, &r, t, true, 0,0,1, pushButProc, 0);
    /* ⚠ SCAN MOVED DOWN 22px to make that room. It keeps a visible gap above it
     * because it is the one button here that does NOT act on the selection. */
    SetRect(&r, kBtnL, 236, (short)(kBtnL + gBtnW), 256);
    /* ★ WAS "Set Up New Device...", AND THE USER IS RIGHT THAT IT LIED.
     * Tiger's button of that name launches the Bluetooth Setup Assistant and
     * genuinely begins configuring a new device. Ours only runs an inquiry --
     * it is the Pair button that starts a setup. Naming it after Tiger's
     * button while doing a third of its job is the kind of mismatch that
     * teaches a user to distrust every other label on the screen.
     * ⚠ NO ELLIPSIS now: a scan acts immediately and returns results here,
     * with no further dialogue, which is exactly when Mac HI omits one.
     * Compare Pair..., which keeps its ellipsis because it opens one. */
    PStr(t, "Scan For Devices");
    gSetup = NewControl(gWin, &r, t, true, 0,0,1, pushButProc, 0);
    /* ⭐ THE DEFAULT BUTTON, so Return or Enter starts a scan -- the user asked for the
     * thick ring. ⚠ Drawn by the Appearance Manager from this tag, never by us: an
     * oval we drew ourselves is exactly what makes a control look home-made, and it
     * would not track the theme. If the call is refused the button simply stays plain
     * rather than half-decorated. */
    if (gSetup != NULL) {
        Boolean isDef = true;
        (void)SetControlData(gSetup, kControlEntireControl,
                             kControlPushButtonDefaultTag,
                             sizeof(isDef), (Ptr)&isDef);
    }

    /* ★ CHASING ARROWS, exactly as the 2003 panel used them -- it called
     * theChasingArrows->StopIdling() on inquiry-complete. kControlChasingArrowsProc
     * is 112 in ControlDefinitions.h, so this is the period-correct indicator rather
     * than a progress bar invented for the purpose. Created hidden; shown while a
     * scan is running. */
    /* ⚠ BESIDE Scan, not below it. The old row (y 240) is where Scan now sits, and the
     * details box below starts at a fixed y=268 -- there is no vertical room left, but
     * there is plenty to the right of a button only gBtnW wide. */
    SetRect(&r, (short)(kBtnL + gBtnW + 12), 238,
               (short)(kBtnL + gBtnW + 46), 254);
    if (r.right > kBtnR) { r.right = kBtnR; r.left = (short)(kBtnR - 34); }
    PStr(t, "");
    gArrows = NewControl(gWin, &r, t, false, 0, 0, 1,
                         kControlChasingArrowsProc, 0);

    /* ⚠ The three selection-dependent buttons start DISABLED, because nothing is
     * selected. A button that looks live and does nothing when clicked teaches the
     * user to distrust the whole panel. */
    HiliteControl(gDelete, 255);
    HiliteControl(gDisconnect, 255);
    HiliteControl(gConfigure, 255);
    HiliteControl(gRename, 255);
}

/* The write-a-command-into-the-block protocol lived here. It is deleted rather than
 * kept as a fallback: run 47 showed it cannot work on an idle radio, and a second way
 * to start an inquiry that silently does nothing is worse than no second way at all.
 * The panel now calls DriverStartScan and gets a return code. */

/* ★★ THE THREE CONFIGURATION COMPONENTS, IN ONE PLACE.
 *
 * These are exactly the three facts the diagnostics pane already prints, and each
 * comes from a DIFFERENT place -- which is why all three are needed and why one
 * cannot stand in for another:
 *
 *   module     gModuleCount, from the USB Name Registry walk -- the HARDWARE
 *   extension  gExtPresent, from the FILE in the Extensions folder
 *   driver     gBlock, from the counter BLOCK in the System heap
 *
 * Extension present with driver inactive is this machine's ordinary state whenever
 * the card is in HID-proxy mode, and it is precisely the combination that confused
 * things earlier -- so "components missing" has to mean all three, checked separately.
 *
 * ⚠ NOT A LATCH. Recomputed on every call from live state, so plugging a module in or
 * the driver binding mid-session re-enables the control. A cached "we were broken at
 * launch" would leave the radio dead for the rest of the session, which is the shape
 * of bug [[feedback_guards_must_not_latch]] describes. */
static Boolean ComponentsOK(void)
{
    return (Boolean)(gHasHardware && gExtPresent && gBlock != NULL);
}

static void SyncButtons(void)
{
    /* ★★ COMPONENTS MISSING => RADIO FORCED OFF AND DISABLED.
     *
     * With no module, no extension or no bound driver there is nothing for On to mean:
     * the radio would be a switch wired to nothing, and this panel's own history says
     * a control that looks live and does nothing teaches the user to distrust
     * everything else on the screen.
     *
     * ⚠ ORDER MATTERS. The radio's value is normally mirrored FROM the driver's block
     * (see the kWRadioOn sync), and that sync cannot run when there is no block -- so
     * forcing the value here cannot fight it. When the components ARE all present this
     * branch does not touch the value at all; it only re-enables the control. Writing
     * Off unconditionally would stamp on the driver's real radio state every time the
     * panel idled.
     *
     * ⚠ HiliteControl(255) also stops FindControl returning the control, so clicks are
     * blocked by the same call that greys it -- the disable is not decorative. */
    {
        Boolean ok = ComponentsOK();
        if (!ok) {
            gRadioOn = false;
            if (gOnRadio)  SetControlValue(gOnRadio,  0);
            if (gOffRadio) SetControlValue(gOffRadio, 1);
        }
        if (gOnRadio)  HiliteControl(gOnRadio,  ok ? 0 : 255);
        if (gOffRadio) HiliteControl(gOffRadio, ok ? 0 : 255);

        /* ⚠ RECORD, DO NOT DRAW. SyncButtons is called from the event loop, from the
         * scan poll and from the watchdog, and only the first of those has a port set
         * up for drawing. The event loop repaints when this changes -- and because
         * gCompState starts at -1, the very first call always counts as a change and
         * the message gets its initial paint. */
        if (gCompState != (short)(ok ? 1 : 0)) {
            gCompState = (short)(ok ? 1 : 0);
            gRadioLogoDirty = true;     /* the logo follows gRadioOn, which just moved */
            gCompMsgDirty   = true;
        }
    }

    /* ⚠ ALL THREE STAY DISABLED, and that is correct rather than lazy. This slice
     * scans and displays; nothing here can delete a bond, disconnect a link or
     * configure a device yet. A button that looks live and does nothing teaches the
     * user to distrust the whole panel, so they stay grey until they work.
     *
     * gSelRow is still tracked, because the description pane uses it. */
    /* ★ DELETE IS LIVE NOW, and only for a row that actually HAS a bond. Enabling it
     * for an "Available" row would offer to forget something we never stored, and the
     * user would rightly read a no-op as a broken button. */
    if (gDelete) {
        /* ★★★★ AND FOR A CARD ROW TOO. Delete was gated on gRow[].paired, which the
         * card rows deliberately set FALSE -- so with our own key store empty there was
         * nothing on screen the button would act on and it was permanently grey. That
         * is exactly what the user hit.
         *
         * ⚠ The two are DIFFERENT DELETES and the click handler routes them apart: a
         * paired row forgets OUR bond, a card row asks the CONTROLLER to forget one of
         * its own. Enabling both here is right; conflating them would not be. */
        /* ★★★★ AND NOW ON ANY ROW, INCLUDING ONE WITH NO PAIRING AT ALL, where it
         * means exactly one thing: get this off my list. It was greyed on those rows
         * because there is no BOND to remove -- true, and it quietly decided that
         * tidying the list was not something the user was allowed to want. It is, and
         * nothing is lost by it: the row comes back the next time a scan finds the
         * device in pairing mode. The click handler has a THIRD route for it that sends
         * no command at all. */
        HiliteControl(gDelete,
                      (gSelRow >= 0 && gSelRow < gRowCount) ? 0 : 255);
        /* ★ RENAME follows the SELECTION and nothing else -- not the radio, not the
         * bond, not whether the device is present. A nickname is a note this Mac keeps
         * about an address; it needs no controller and no live device, and greying it
         * when the radio is off would be inventing a dependency that does not exist. */
        HiliteControl(gRename,
                      (gSelRow >= 0 && gSelRow < gRowCount) ? 0 : 255);
    }
    /* ★★★★ PAIR IS LIVE NOW, for any selected row. Unlike Delete it is deliberately
     * NOT limited to already-paired rows: pairing an unpaired responder is the whole
     * point, and RE-pairing a paired one is exactly what replacing a Tiger-made A1016
     * bond with one made under OS 9 requires.
     *
     * ⚠ It needs the radio on and all components present, for the same reason Set Up
     * New Device does: a pairing needs a live controller, and a button that looks live
     * over a dead stack teaches the user to distrust the whole panel. */
    /* ★★★★★★ AND IT STAYS OPEN ON EVERY ROW EXCEPT TWO, which is NOT the same as
     * limiting it to "Available".
     *
     * ⚠⚠ RESTRICTING PAIR TO Available WOULD REMOVE THE ONLY RECOVERY PATH FROM THE
     * EXACT STATES THIS HARDWARE GETS INTO, and the logs say which:
     *
     *   "Connected, no key"   -- the last run: keys published 0 in our database AND
     *                            keys listed 0 in the module's store, with 1078 HID
     *                            reports flowing. The pairing evaporated and re-pairing
     *                            is the fix. An Available-only rule locks the user out
     *                            of the one button that could repair it.
     *   "Linked, not ready"   -- the A1016 after a pairing that did not take. Retrying
     *                            is the documented workflow and the user has used it.
     *   "Paired, disconnected" -- a bond the PEER has forgotten looks exactly like one
     *                            it still holds. Re-pairing is the only way to find out.
     *
     * ⇒ Pair is greyed only where it could do harm or could not work:
     *
     *   Connected             -- a bond, a live link and an open HID channel. Re-pairing
     *                            renegotiates a keyboard that is working RIGHT NOW,
     *                            which is a way to break the one thing the user has.
     *   Disconnected by you   -- we are actively refusing this device. A pairing cannot
     *                            complete through a refusal, so the button would fail by
     *                            construction. Connect first; the status says so.
     *
     * ⚠ Those two conditions are computed, not read off the status STRING. Matching on
     * displayed text would make the wording of a label load-bearing, and this column's
     * wording has already changed four times this week. */
    if (gConfigure) {
        Boolean pairable = (ComponentsOK() && gRadioOn &&
                            gSelRow >= 0 && gSelRow < gRowCount);
        if (pairable && gBlock != NULL) {
            short   k;
            Boolean live = false;
            /* ⚠⚠⚠ ONLY ON A DRIVER OLDER THAN 13.2, AND THIS NEARLY UNDID A SHIPPED
             * FIX. Panel 13.6 greyed Pair on a refused device, reasoning that a pairing
             * cannot complete through a refusal we are enforcing. That was true --
             * against driver 13.1. Driver 13.2 RELEASES the block as the first act of
             * kCmdPairDevice, deliberately, because the user reported the phone saying
             * "incorrect PIN" when the real cause was our own refusal. Greying the
             * button there would have disabled the fix and restored the symptom in the
             * UI instead of the driver.
             *
             * ⇒ Ask the driver that is actually installed. On 13.2 and later, Pair on a
             * refused device is the supported way to say "I want it back". */
            if (DriverVerBCD() < 1302
                && gBlock[kWBlockActive] != 0
                && gBlock[kWBlockHi] == gRow[gSelRow].addrHi
                && gBlock[kWBlockLo] == gRow[gSelRow].addrLo)
                pairable = false;                       /* refusing it, and it cannot */
            for (k = 0; k < (short)gBlock[kWLiveCount] && k < 2; k++)
                if (gBlock[kWLiveA0Hi + k * 2]     == gRow[gSelRow].addrHi
                 && gBlock[kWLiveA0Hi + k * 2 + 1] == gRow[gSelRow].addrLo)
                    { live = true; break; }
            if (live && gRow[gSelRow].paired && HidChannelOpen())
                pairable = false;                       /* working -- leave it alone */
        }
        HiliteControl(gConfigure, pairable ? 0 : 255);
    }
    /* ★★★★★★ v10.7: THE DISCONNECT BUTTON GOES LIVE.
     *
     * ⚠⚠ IT HAS BEEN ON SCREEN AND DEAD SINCE THE PANEL WAS WRITTEN -- created, hilited
     * to 255 in both places, and with no hit handler anywhere. The comment beside the
     * other disables says exactly why that was wrong to leave: "a button that looks live
     * and does nothing when clicked teaches the user to distrust the whole panel." A
     * button that is permanently grey teaches the same lesson more slowly.
     *
     * It is enabled only when the selected row IS the connected peer, because that is
     * the only case where it can do anything. */
    /* ★★★★ ONE BUTTON, TWO MEANINGS, and the label says which. Disconnect is only
     * useful while a link is up; Connect is only useful while one is refused. A single
     * control that flips is how Tiger does it and it keeps the panel honest -- there is
     * never a live-looking button with nothing to do. */
    {
        Boolean blocked = (gBlock != NULL && gSelRow >= 0 && gSelRow < gRowCount
                           && gBlock[kWBlockActive] != 0
                           && gBlock[kWBlockHi] == gRow[gSelRow].addrHi
                           && gBlock[kWBlockLo] == gRow[gSelRow].addrLo);
        /* ⚠⚠ THIS USED TO TEST kWConnPeerHi/Lo, WHICH IS THE MOST RECENT PEER AND NOT
         * A LIST. With the keyboard and the phone both connected the user selected the
         * phone and got a greyed-out button, because the keyboard had connected last.
         * The driver now publishes the LIVE connections it verified against BTstack's
         * own list, and the button follows those. */
        Boolean liveHere = false;
        if (gBlock != NULL && gSelRow >= 0 && gSelRow < gRowCount) {
            short k;
            for (k = 0; k < (short)gBlock[kWLiveCount] && k < 2; k++) {
                if (gBlock[kWLiveA0Hi + k * 2]     == gRow[gSelRow].addrHi
                 && gBlock[kWLiveA0Hi + k * 2 + 1] == gRow[gSelRow].addrLo) {
                    liveHere = true; break;
                }
            }
        }
        Str255 t;
        if (blocked) PStr(t, "Connect"); else PStr(t, "Disconnect");
        SetControlTitle(gDisconnect, t);
        HiliteControl(gDisconnect,
                      (ComponentsOK() && gRadioOn && (blocked || liveHere)) ? 0 : 255);
    }

    /* ★ Scan For Devices follows the radio. Strictly, an inquiry still works with
     * page and inquiry scan cleared -- "off" stops us being FOUND, not us looking. But
     * a user who has switched Bluetooth off does not expect a scan button to work, and
     * a control panel that contradicts its own switch is worse than one that is
     * slightly conservative about what "off" covers. */
    /* ⚠ AND GREY IT WHILE A SCAN IS RUNNING. An inquiry lasts about ten seconds, and
     * gap_inquiry_start refuses for the whole of it -- so a second click inside that
     * window used to report "start failed, rc -1012" over a perfectly healthy scan.
     * Disabling the button removes the race rather than explaining it after the fact;
     * the message below is the belt to this braces, for the bring-up inquiry that is
     * already running before the panel ever opens. */
    /* ⚠⚠ AND THE DISABLE MUST NOT LATCH EITHER. Greying the button while an inquiry
     * runs removes the "already scanning" race -- but the driver's scanning state is
     * something the panel cannot clear, and run 58 left it stuck at 1 with no inquiry
     * running. The button then stayed dead across a quit and relaunch, because the
     * stuck value lives in the driver.
     *
     * gScanStalled is the watchdog's verdict, and it now releases the button as well as
     * the arrows: after 30 s the user gets a control that at least DOES something and
     * can report what happened, rather than a permanently dead one. */
    /* ⚠ A PENDING MAILBOX REQUEST COUNTS AS SCANNING. Otherwise the button stays live
     * for the 100 ms before the driver picks the request up, and a second click in
     * that window writes a second command that coalesces into the first -- so the
     * user's press does nothing and the button is the thing that lied about it. */
    if (gSetup) HiliteControl(gSetup,
                              (gRadioOn && !ScanRequestPending()
                                        && !(gLastScanState == 1 && !gScanStalled))
                                  ? 0 : 255);
}

/* ★★ DEVICE INFORMATION FOR THE SELECTED ROW.
 *
 * Restored and moved directly under the table. A scan now returns several devices at
 * once and the class-of-device naming cannot tell two of a kind apart -- run 49 had
 * five audio responders -- so the per-device detail has to sit beside the selection,
 * which is also where Extensions Manager puts it. */
static void DrawDeviceInfo(void)
{
    Rect   r, in;
    Str255 t;
    short  y, valX;
    const short pitch = 14;

    SetRect(&r, kListL, kInfoT, kListR, kInfoB);
    FrameGroup(&r, false);
    GroupCaption(&r, " Device Details ");
    in = r; InsetRect(&in, 8, 6);

    TextFont(applFont); TextSize(9);

    if (gSelRow < 0 || gSelRow >= gRowCount) {
        MoveTo(in.left, in.top + 12);
        PStr(t, "No device selected. Click a row above to see its details.");
        DrawString(t);
        return;
    }

    /* One measured value column, same idiom as the pane below: labels are MEASURED,
     * never padded with spaces, because the proportional font makes padding drift. */
    { Str255 a, b, c2, d, e;
      short w, m = 0;
      PStr(a, "Address:");  w = StringWidth(a); if (w > m) m = w;
      PStr(b, "Kind:");     w = StringWidth(b); if (w > m) m = w;
      PStr(c2, "Class:");   w = StringWidth(c2); if (w > m) m = w;
      PStr(d, "Status:");   w = StringWidth(d); if (w > m) m = w;
      PStr(e, "Battery:");  w = StringWidth(e); if (w > m) m = w;
      valX = (short)(in.left + m + 10);

      y = in.top + 12;
      MoveTo(in.left, y);
      TextFace(bold);
      { const char *nm = RowNameOf(&gRow[gSelRow]); PStr(t, nm); DrawString(t); }
      TextFace(0);

      y += pitch;
      MoveTo(in.left, y); DrawString(a);
      MoveTo(valX, y);
      { char addr[24];
        AddrToStr(gRow[gSelRow].addrHi, gRow[gSelRow].addrLo, addr);
        PStr(t, addr); DrawString(t); }

      y += pitch;
      MoveTo(in.left, y); DrawString(b);
      MoveTo(valX, y);
      PStr(t, RowKindOf(&gRow[gSelRow])); DrawString(t);

      y += pitch;
      MoveTo(in.left, y); DrawString(c2);
      MoveTo(valX, y);
      /* ★ THREE STATES HERE, NOT TWO, now that a CoD can outlive the inquiry that
       * produced it. This is the detail pane, so it is the right place to say WHICH of
       * the two "we know the class" cases applies; the list's Kind column shows the
       * same kind either way, which is the point of the fix.
       *
       * ⚠ h is sized for the longest branch: 8 hex characters + a 23-character suffix
       * + terminator = 32, against 64. This project has already had a buffer smashed
       * by a string longer than its array, so the number is counted, not eyeballed. */
      if (gRow[gSelRow].codKnown) {
          char h[64];
          const char *dig = "0123456789ABCDEF";
          unsigned long v = gRow[gSelRow].cod;
          short n = 0;
          h[n++] = '0'; h[n++] = 'x';
          h[n++] = dig[(v >> 20) & 15]; h[n++] = dig[(v >> 16) & 15];
          h[n++] = dig[(v >> 12) & 15]; h[n++] = dig[(v >>  8) & 15];
          h[n++] = dig[(v >>  4) & 15]; h[n++] = dig[ v        & 15];
          if (!gRow[gSelRow].fromScan) {
              const char *s2 = " (from an earlier scan)";
              while (*s2) h[n++] = *s2++;
          }
          h[n] = 0;
          PStr(t, h); DrawString(t);
      } else {
          /* ⚠ Not "unknown class" as if we had asked -- we never saw this device in an
           * inquiry at all, so there is no class to report. */
          PStr(t, "not seen in a scan (listed from its bond)"); DrawString(t);
      }

      y += pitch;
      MoveTo(in.left, y); DrawString(d);
      MoveTo(valX, y);
      if (gRow[gSelRow].handed) {
          PStr(t, "Paired - the controller used this key");
      } else if (gRow[gSelRow].paired) {
          /* ★ THE DISTINCTION THE USER CAUGHT. A key on disk is not a bond: the peer
           * holds the other half and may have discarded it. */
          PStr(t, "Bond on file - not confirmed by the device");
      } else {
          PStr(t, "Available - seen in a scan, not paired");
      }
      DrawString(t);

      /* ★★★ BATTERY, directly below Status (requested 2026-09-21).
       *
       * ⚠ NO GEOMETRY CHANGE WAS NEEDED and none was made. Status sits on baseline 274
       * and this lands on 288 against an inset bottom of 294 -- it fits. The offer to
       * take height from Configuration Details was declined on purpose: that pane's own
       * comment records "at 16 the final line lands on 446 against a 444 limit... the
       * third time this pane has been one line from overflowing", and its content grows
       * with the module count. Shifting it down 14px would have clipped the four-module
       * case to buy room this pane did not need.
       *
       * ⚠ Three states, not two. "Not reported" is not "0%", and neither is "no file" --
       * a device that has never answered must not be shown as flat. */
      y += pitch;
      MoveTo(in.left, y); DrawString(e);
      MoveTo(valX, y);
      {
          short pct = BatLevelFor(gRow[gSelRow].addrHi, gRow[gSelRow].addrLo);
          if (pct >= 0) {
              char bb[32];
              short n = 0;
              if (pct >= 100) { bb[n++] = '1'; bb[n++] = '0'; bb[n++] = '0'; }
              else {
                  if (pct >= 10) bb[n++] = (char)('0' + (pct / 10) % 10);
                  bb[n++] = (char)('0' + pct % 10);
              }
              bb[n++] = '%';
              if (pct <= 5) {
                  const char *w2 = " - very low, replace or recharge";
                  while (*w2) bb[n++] = *w2++;
              }
              bb[n] = 0;
              PStr(t, bb);
          } else {
              /* ⚠ Says WHY, in the one place it can be known: only our own HCI stack
               * asks for a level, so in proxy mode or with no driver there is nothing to
               * report and that is not the device's fault. */
              PStr(t, gBlock != NULL ? "not reported by this device"
                                     : "unavailable - driver not active");
          }
          DrawString(t);
      }
    }
}

static void DrawDescription(void)
{
    Rect   r, in;
    Str255 t;

    SetRect(&r, kListL, kDescT, kListR, kDescB);
    FrameGroup(&r, false);
    GroupCaption(&r, " Configuration Details ");

    in = r; InsetRect(&in, 8, 8);
    TextFont(applFont); TextSize(9);

    /* ★★ FOUR STATUS LINES, one fact each, at a fixed 16px pitch.
     *
     * v0.7 drew three different multi-line explanations at hard-coded offsets and two
     * of them collided on screen -- "A USB Bluetooth adapter is supported today"
     * landed on top of the SCAFFOLDING text. Prose in a fixed-height pane will keep
     * doing that every time the wording changes. One fact per line, computed pitch,
     * and the collision cannot recur.
     *
     * ⚠ "Extension detected" and "Driver active" are NOT redundant, and the reason is
     * where each comes from: the extension line reads the FILE in Extensions, the
     * driver line reads the counter BLOCK. Extension present with driver inactive is
     * this machine's normal state now, and it is exactly the situation that confused
     * things earlier -- so the two lines together say what one could not. */
    {
        short y = in.top + 12;
        /* ⚠ 16 -> 14. Checked arithmetically against the four-module cap, as the last
         * two layout changes were: at 16 the final line lands on 446 against a 444
         * limit. That is the third time this pane has been one line from overflowing,
         * so the numbers are computed rather than eyeballed. */
        const short pitch = 14;

        /* ★ MEASURE THE LABELS, DO NOT PAD THEM WITH SPACES. Panel 0.9 aligned the
         * three values by trailing spaces in the label strings, and the application
         * font is PROPORTIONAL -- so they did not line up and could not. StringWidth
         * of the widest label gives the one column position all three values share. */
        {
            Str255 l1, l2, l3, l4;
            short  valX, w;
            PStr(l1, gModuleCount > 1 ? "Module(s) detected:" : "Module detected:");
            PStr(l2, "Extension detected:");
            PStr(l3, "Driver active:");
            PStr(l4, "Card switch:");
            valX = StringWidth(l1);
            w = StringWidth(l2); if (w > valX) valX = w;
            w = StringWidth(l3); if (w > valX) valX = w;
            w = StringWidth(l4); if (w > valX) valX = w;
            valX = (short)(in.left + valX + 10);

            MoveTo(in.left, y); DrawString(l1);
            if (gModuleCount == 0) {
                MoveTo(valX, y);
                PStr(t, "None"); DrawString(t);
            } else {
                short i;
                /* ⚠ ONE LINE PER MODULE. 0.9 joined them with commas on a single line
                 * and it ran off the pane and behind the button column -- there is no
                 * word wrap in QuickDraw, and a fixed-width pane will always lose the
                 * tail. Each module gets its own line, all starting at the value
                 * column so they read as a list under the label. */
                for (i = 0; i < gModuleCount; i++) {
                    char hex[12];
                    const char *dig = "0123456789ABCDEF";
                    unsigned short v = gHwVendor[i], p = gHwProduct[i];
                    if (i) y += pitch;
                    MoveTo(valX, y);
                    PStr(t, ModuleName(v, p)); DrawString(t);
                    /* The VID:PID stays on every entry: two cards of the same model
                     * would otherwise be indistinguishable, and every log this project
                     * reads is addressed by ID rather than by name. */
                    hex[0] = ' '; hex[1] = '(';
                    hex[2] = dig[(v >> 12) & 15]; hex[3] = dig[(v >> 8) & 15];
                    hex[4] = dig[(v >>  4) & 15]; hex[5] = dig[ v       & 15];
                    hex[6] = ':';
                    hex[7] = dig[(p >> 12) & 15]; hex[8] = dig[(p >> 8) & 15];
                    hex[9] = dig[(p >>  4) & 15]; hex[10]= dig[ p       & 15];
                    hex[11] = 0;
                    PStr(t, hex); DrawString(t);
                    PStr(t, ")"); DrawString(t);
                }
            }

            y += pitch;
            MoveTo(in.left, y); DrawString(l2);
            MoveTo(valX, y);
            if (!gExtPresent)     { PStr(t, "None"); DrawString(t); }
            else if (gExtVers[0]) { PStr(t, "v"); DrawString(t); DrawString(gExtVers); }
            else                  { PStr(t, "yes (version unreadable)"); DrawString(t); }

            y += pitch;
            MoveTo(in.left, y); DrawString(l3);
            MoveTo(valX, y);
            /* ★★★ A BARE "No" HERE NOW READS AS A FAULT, AND IT IS NOT ONE.
             *
             * Reported straight after the "Mac OS is handling Bluetooth" line landed:
             * the panel said the right thing in one place and "Driver active: No" in
             * the other, which together read as a half-broken install.
             *
             * The FACT is right -- with switcher 1.2 idle at boot there really is no
             * driver block, because our driver only binds the card in HCI mode. What
             * was wrong is stating the shipping state in the vocabulary of a failure.
             * "No" is what you print when something should be running and is not;
             * "Not needed" is what you print when nothing should be.
             *
             * ⚠ Still a plain "No" when the extension is ABSENT, because then it
             * genuinely IS a fault and the two cases must not look alike. Same split
             * and deliberately the same test as DrawComponentMsg -- if those two ever
             * disagree about which case this is, one of them is lying. */
            if (gBlock != NULL)                   PStr(t, "Yes");
            /* ⚠⚠ "Not needed just now" READ AS "everything is fine", which is the
             * opposite of useful: the driver is NOT running, so there is no pairing and
             * no media keys. The user said so plainly. It also gave no advice, and this
             * line is the wrong place for advice -- the correct next step depends on
             * WHY the driver is inactive, and the Card switch line immediately below
             * now says which of five states applies. So this states the fact and points
             * at the explanation rather than guessing at one. */
            else if (gHasHardware && gExtPresent)
                                                  PStr(t, "Inactive - see Card switch below");
            else                                  PStr(t, "No");
            DrawString(t);

        /* ★★★★★★ WHAT THE SWITCHER DID THIS START-UP, and it is here because the
         * panel could not previously distinguish two opposite states -- see the note at
         * the kSw enum. "Driver active: No" is true when the card was never switched AND
         * when it was switched and the driver failed to bind, and those need opposite
         * responses from the user. This line says which.
         *
         * ⚠ ORDERED BY WHAT THE USER SHOULD DO ABOUT IT, not by severity. A stale marker
         * is reported before "not attempted", because it is the state that LATCHES: it
         * repeats every boot until the File menu overrides it, and a user who does not
         * know that will reboot forever. */
        y += pitch;
        MoveTo(in.left, y);
        PStr(t, "Card switch:"); DrawString(t);
        MoveTo(valX, y);
        if (gSwBlock == NULL) {
            /* ★★★★ "NOT INSTALLED" WAS WRONG ON EVERY DONGLE-ONLY MACHINE, AND THE USER
             * CAUGHT IT. No 'BTSW' block means one of three things, not one:
             *   1. the switcher extension really is missing;
             *   2. it IS installed and simply never bound, because there is no A1044 on
             *      the bus -- every dongle machine, permanently;
             *   3. it is installed, a card is present, and it failed to initialise.
             * The line asserted (1) for all three. On the FW400 that is flatly false --
             * BTCheck's INSTALLED FILES section reads "USBBluetoothSwitch 1.6" straight
             * off the file -- and it sends the user hunting for a missing extension that
             * is sitting right there, during a pairing that has nothing to do with it.
             *
             * ⇒ Case 2 is distinguishable and worth naming: if the DRIVER bound a
             * non-Apple controller, there is no Apple module in HID proxy for the
             * switcher to claim, because if there were it would have claimed it and left
             * a block. Say so plainly and stop implying a fault.
             *
             * ⚠ Cases 1 and 3 stay on the old wording deliberately. They are genuinely
             * ambiguous from in here, and a line that guesses between them is how this
             * one went wrong in the first place. The switcher's only job is taking the
             * INTERNAL card out of proxy mode; a dongle needs none of it. */
            unsigned long vp = (gBlock != NULL) ? gBlock[kWIfaceVIDPID] : 0;
            if (vp != 0 && ((vp >> 16) & 0xFFFFUL) != 0x05ACUL)
                PStr(t, "Inactive (A1044 module not detected)");
            else
                PStr(t, "switcher extension not installed");
        } else if (gSwBlock[kSwMarkStale] != 0) {
            /* The self-healing decline. The next line the user needs is in the File
             * menu, so name it rather than describing the state. */
            PStr(t, "declined - use File menu, then restart");
        } else if (gSwBlock[kSwSeenProxy] == 0) {
            PStr(t, "card not seen by the switcher");
        } else if (gSwBlock[kSwTried] == 0) {
            PStr(t, "not attempted this start-up");
        } else if (gBlock != NULL) {
            PStr(t, "switched at start-up");
        } else {
            /* ⚠ THE STATE THAT COST TWO REBOOTS: the switch was issued and the driver
             * still did not bind. Naming it is the whole point of this line. */
            Str255 n;
            PStr(t, "switched, driver did not bind (err "); DrawString(t);
            NumToString((long)gSwBlock[kSwImmErr], n); DrawString(n);
            PStr(t, ")");
        }
        DrawString(t);

        /* ★★ SCAN STATUS. §5c requires the panel to be able to say "not
         * acknowledged" rather than spin, because a quiet radio means no USB
         * completions means the driver never services the command. That is a
         * COMPARISON of ack against the sequence we wrote, not a timeout. */
        y += pitch;
        MoveTo(in.left, y);
        PStr(t, "Scan:"); DrawString(t);
        MoveTo(valX, y);
        if (gDeleteTried && gAlertRc != noErr) {
            /* ★ The dialog itself failed. Named, with its code, because "nothing
             * happened" is the one report that cannot be acted on. */
            Str255 n;
            PStr(t, "confirmation dialog FAILED, rc "); DrawString(t);
            NumToString((long)gAlertRc, n); DrawString(n);
        } else if (gDeleteTried && gAlertHit != kAlertStdAlertOKButton) {
            PStr(t, "delete cancelled"); DrawString(t);
        } else if (gDeleteTried) {
            /* ★ Reported until the next action, because a deletion is destructive and
             * the user is entitled to see that it took -- and to see the difference
             * between "removed" and "there was nothing there". */
            if (gDeleteListOnly) {
                PStr(t, "removed from the list - it was not paired"); DrawString(t);
            } else if (gLastDeleteRc == noErr) {
                PStr(t, "bond forgotten"); DrawString(t);
            } else if (gLastDeleteRc == kBTNoSuchBond) {
                PStr(t, "no bond was stored for that address"); DrawString(t);
            } else {
                Str255 n;
                PStr(t, "delete FAILED, rc "); DrawString(t);
                NumToString((long)gLastDeleteRc, n); DrawString(n);
            }
        } else if (gDiscTried && gLastDiscRc != noErr) {
            Str255 n;
            PStr(t, gDiscWasAllow ? "allow FAILED, rc " : "disconnect FAILED, rc ");
            DrawString(t);
            NumToString((long)gLastDiscRc, n); DrawString(n);
        } else if (gDiscTried && gDiscWasAllow) {
            /* ⭐ WHAT IT DID AND WHAT TO DO NEXT. "Connect" clears our refusal; it
             * cannot dial the device, because on this stack the KEYBOARD is the
             * initiator -- it pages us. Saying only "allowed" would leave the user
             * watching a row that has not changed to Connected and concluding, again,
             * that the button does nothing. */
            PStr(t, "allowed - press a key on the device"); DrawString(t);
        } else if (gDiscTried) {
            PStr(t, "disconnected - click Connect to allow it back"); DrawString(t);
        } else if (!MailboxCaughtUp()) {
            /* ★★ CHECKED BEFORE THE STALL, because it is the more specific fact and it
             * names the cause instead of the symptom. A request sitting unserviced is
             * exactly run 47's failure -- seq 9 against ack 2, no inquiry, and at the
             * time nothing on screen said so. If the driver's timer is not running,
             * this line appears within a poll instead of thirty seconds of arrows
             * followed by a generic "never reported completing". */
            /* ⚠ TRIMMED FROM "queued, driver has not picked it up (seq ...": that ran
             * past the pane and out the right-hand side of the window. The label on
             * this line already says "Scan:", so "queued" was saying it twice. The
             * clip in DrawPanes is what GUARANTEES containment -- timer is a tick
             * counter and will grow digits -- but a line that needs clipping on its
             * ordinary path is still a line worth shortening. */
            Str255 n;
            PStr(t, "driver has not picked it up (seq "); DrawString(t);
            NumToString((long)(gBlock ? gBlock[kWCmdSeq] : 0), n); DrawString(n);
            PStr(t, " ack "); DrawString(t);
            NumToString((long)(gBlock ? gBlock[kWCmdAck] : 0), n); DrawString(n);
            PStr(t, ", timer "); DrawString(t);
            NumToString((long)(gBlock ? gBlock[kWTimerRuns] : 0), n); DrawString(n);
            PStr(t, ")"); DrawString(t);
        } else if (gScanStalled) {
            PStr(t, "scan never reported completing - arrows stopped");
            DrawString(t);
        } else if (gLastScanRc == kBTProbeDidCall) {
            PStr(t, "PROBE: entry point called, nothing started"); DrawString(t);
        } else if (gLastRadioRc != noErr) {
            /* ⚠ Checked before everything else: a refused On/Off is the one failure
             * that would otherwise be invisible, because the button springs back and
             * looks like nothing happened. */
            Str255 n;
            PStr(t, "radio change REFUSED, rc "); DrawString(t);
            NumToString((long)gLastRadioRc, n); DrawString(n);
        } else if (!gRadioOn) {
            PStr(t, "off - not discoverable, not connectable"); DrawString(t);
        } else if (gScanTried && gLastScanRc == kBTAlreadyScanning) {
            PStr(t, "a scan was already running\311"); DrawString(t);
        } else if (gScanTried && gLastScanRc != noErr) {
            /* ★ CHECKED FIRST, so a failure can never be masked by the idle text. */
            /* ⚠ "no entry point" was too coarse: it could not tell "the USB Manager
             * offered us no connection at all" from "it offered one and the symbol was
             * not in it", and those need completely different fixes. Split, with the
             * connection count shown, so one more run settles it. */
            if (gLastScanRc == cfragNoSymbolErr && gResolveConns > 0 && gScanTried &&
                gLastScanState == 0 && gResolveRc == noErr) {
                /* The walk-only probe reports "no symbol" by construction. Say so
                 * rather than letting it read as a fault. */
                Str255 n;
                PStr(t, "PROBE: walked "); DrawString(t);
                NumToString((long)gResolveConns, n); DrawString(n);
                PStr(t, " connection(s), no symbol resolved");
                DrawString(t);
            } else if (gLastScanRc == cfragNoLibraryErr) {
                Str255 n;
                PStr(t, "no driver connection offered (walked "); DrawString(t);
                NumToString((long)gResolveConns, n); DrawString(n);
                PStr(t, ")");
            } else if (gLastScanRc == cfragNoSymbolErr) {
                Str255 n;
                PStr(t, "entry point missing in "); DrawString(t);
                NumToString((long)gResolveConns, n); DrawString(n);
                PStr(t, " connection(s)");
            } else if (gLastScanRc == kUSBDeviceBusy) {
                PStr(t, "driver found but none of its copies is bound");
            } else {
                Str255 n;
                PStr(t, "start failed, rc "); DrawString(t);
                NumToString((long)gLastScanRc, n);
                DrawString(n);
                PStr(t, "");
            }
            DrawString(t);
        } else if (!gScanAsked) {
            /* ★ RUN 45's SURPRISE, AND IT IS A PLEASANT ONE. The driver runs its own
             * inquiry during bring-up, so the responder table is ALREADY populated
             * before the panel asks for anything -- run 45 had nine audio devices in
             * it. Saying "idle" over a list of nine real devices would be a lie about
             * where they came from, so name their actual provenance. */
            if (gScanRows > 0) {
                PStr(t, "found during driver start-up"); DrawString(t);
            } else {
                PStr(t, gBlock ? "idle - press Scan For Devices"
                               : "unavailable - no driver");
                DrawString(t);
            }
        } else if (gLastScanState == 1) {
            PStr(t, "scanning\311"); DrawString(t);
        } else {
            char n[8];
            short c = gScanRows;
            n[0] = (char)('0' + (c / 10)); n[1] = (char)('0' + (c % 10)); n[2] = 0;
            PStr(t, "complete, "); DrawString(t);
            PStr(t, (c < 10) ? n + 1 : n); DrawString(t);
            PStr(t, " device(s) found"); DrawString(t);
        }
        }   /* end of the label-alignment block: valX's scope reaches here now,
             * because the scan-status line shares the value column with the
             * other three. */

        /* ⚠ "SCAN AND DISPLAY ONLY" IS DELETED, AND IT HAD BECOME FALSE. The radio
         * buttons drive the real radio and Delete really forgets a bond, so the banner
         * was understating the panel by two working features.
         *
         * ★ Removing it also fixed the pane's height budget. With the advisory line
         * added, four modules landed on 446 against a 444 limit -- 2px over, invisible
         * until someone attached four Bluetooth modules. Dropping a line that had
         * stopped being true was the cheapest of the available fixes, which is a better
         * reason than needing the pixels. */
        /* ⚠⚠ THE LINE ONLY EXISTS IN THE DEGRADED CASE NOW, and the pitch advance
         * moved inside the test with it -- otherwise the normal case spends a line of a
         * pane that has been one line from overflowing three times.
         *
         * ⇒ "Names come from device class, so two of a kind look alike" is DELETED. The
         * user called it a given, and it had also become FALSE: since panel 11.5 the
         * Name column shows the DEVICE'S OWN name when one has been fetched
         * (kWNameBase), falling back to the class only when it has not. An advisory
         * that describes behaviour the panel no longer has is worse than none -- the
         * same rule that caught the bt_switch.c header this week. */
        if (gListDefHdl == NULL) {
            y += pitch;
            MoveTo(in.left, y);
            TextFace(bold);
            PStr(t, "Column view unavailable - showing plain rows.");
            DrawString(t);
            TextFace(0);
        }
        return;
    }
}

/* ★★★★★ THE TWO LOWER PANES, ERASED AND REDRAWN AS ONE -- INSIDE A CLIP.
 *
 * ⚠⚠ THE CLIP IS THE FIX FOR THE TEXT THAT ESCAPED THE BOX. The user photographed
 * "772)" hanging in the window to the RIGHT of Configuration Details, and it had been
 * there since some earlier draw. The cause is an overlong status string:
 *
 *     "queued, driver has not picked it up (seq 5 ack 4, timer 772)"
 *
 * is wider than the pane, so it drew straight through the frame and out the other
 * side. Every redraw since then erased kListL..kListR -- the pane -- and the tail sat
 * OUTSIDE that rectangle, so nothing ever cleaned it up. Two facts, one visible
 * symptom: it escaped, and then it was unreachable.
 *
 * ⚠ AND IT APPEARED ONLY AFTER A SCAN because that string is on the path where the
 * mailbox has not caught up -- exactly the moment a Scan click creates.
 *
 * ⇒ The clip is the structural answer and the string length is the cosmetic one. The
 * string is trimmed below, but a line carrying a tick counter can always grow another
 * digit, so a length that fits today is a coincidence and the clip is the guarantee.
 *
 * ★ Six call sites had the erase-and-redraw pair written out by hand. They are one
 * call now, because a clip that only six of seven places remember is not a clip. */
static void DrawPanes(void)
{
    Rect      d;
    RgnHandle save = NewRgn();

    SetRect(&d, kListL, kInfoT, kListR, kDescB);
    ErasePlatinum(&d);
    /* ⚠ Fail OPEN: no region means draw the way we always did, which is wrong at the
     * edges rather than absent. */
    if (save != NULL) { GetClip(save); ClipRect(&d); }
    DrawDeviceInfo();
    DrawDescription();
    if (save != NULL) { SetClip(save); DisposeRgn(save); }
}

/* ★★ PLATINUM, AND WHY v0.2 WAS WHITE.
 *
 * v0.2 called SetThemeWindowBackground once at startup and then EraseRect in the
 * update handler. The window came up WHITE, and because the theme group frames are
 * drawn as subtle light-grey embossing designed to sit on platinum grey, they were
 * INVISIBLE on white -- which is why the group box border and the pane frames all
 * "disappeared" at the same time. One cause, three reported symptoms.
 *
 * The fix is to set the CURRENT PORT's background to the theme brush immediately
 * before erasing, every time, rather than relying on a window attribute set once.
 *
 * ⚠ AND TO CHECK THE RETURN. v0.2 ignored six OSStatus values and got silence. If
 * the Appearance Manager is not answering, fall back to a plain grey and say so in
 * the description pane rather than drawing white and leaving the user to wonder. */
static Boolean gThemeOK = true;

/* ⚠⚠ v0.3 STILL CAME UP WHITE, and the reason is worth keeping.
 *
 * v0.3 called SetThemeBackground and only fell back if it returned an error. It
 * returned noErr AND DREW NOTHING, so the fallback never ran and the window stayed
 * white. Two rounds lost to trusting a success code.
 *
 * ⇒ Do the painting with plain QuickDraw, ALWAYS. Ask the Appearance Manager for the
 * theme's colour so the panel still tracks the user's theme, but never let it do the
 * erase -- RGBBackColor + EraseRect cannot silently no-op.
 *
 * Corroboration that this is the right layer: DrawThemeListBoxFrame DID draw in
 * v0.3 (the list well has a visible frame in the screenshot) while the two group
 * calls did not -- because a list box frame is a hard dark line and a group box is
 * subtle light-grey embossing, which is invisible on white. Same root cause as the
 * "missing border", and it should resolve once the ground is actually grey. */
static void ErasePlatinum(const Rect *r)
{
    RGBColor c;
    short    depth = 8;
    GDHandle gd = GetMainDevice();

    if (gd != NULL && (*gd)->gdPMap != NULL)
        depth = (*(*gd)->gdPMap)->pixelSize;

    if (GetThemeBrushAsColor(kThemeBrushDialogBackgroundActive, depth, true, &c)
            != noErr) {
        /* Hard-coded platinum. Not theme-aware, but grey -- and the theme frames
         * read against it, which is the whole point. */
        gThemeOK = false;
        c.red = c.green = c.blue = 0xDDDD;
    }
    RGBBackColor(&c);
    EraseRect(r);
}

/* Theme frame with a fallback, for the same reason. */
static void FrameGroup(const Rect *r, Boolean primary)
{
    OSStatus e = primary ? DrawThemePrimaryGroup(r, kThemeStateActive)
                         : DrawThemeSecondaryGroup(r, kThemeStateActive);
    if (e != noErr) { gThemeOK = false; FrameRect(r); }
}

/* ★★ THE BLUETOOTH LOGO, inside the group box to the right of the radios, and it now
 * REFLECTS THE RADIO STATE: colour when the radio is on, greyscale when it is off.
 *
 * ⚠⚠ FACTORED OUT OF DrawFrame FOR A REASON. This used to be inline there, which was
 * fine while the artwork was constant. The moment it depends on gRadioOn it has to be
 * redrawn wherever gRadioOn changes -- and NEITHER radio path redraws the frame:
 * the click handler refreshes only the info pane, and the poll's `changed` flag does
 * the same. Leaving it inline would have shipped a logo that only ever updated when
 * something else happened to repaint the window. Three callers, one function.
 *
 * ⚠ POSITIONED FROM THE MEASURED LABEL WIDTH, not from either edge. It sits at gLogoL,
 * ten pixels right of the radio titles, and the group frame closes ten pixels right of
 * it -- both computed in MakeControls from the real font metrics. Anchoring it to
 * kBtnR, as it was, is what made the box span the whole button column.
 *
 * ⚠ The rect stays strictly inside the group's (kBtnL, 34)..(gGrpR, 86) interior --
 * asserted below rather than trusted, because erasing over the theme frame would chew
 * a notch out of it, and the geometry now depends on a runtime measurement. */
#define kIconRadioOn   210
#define kIconRadioOff  211

/* ★★ "Components missing; check configuration", in the gap under the group box.
 *
 * The group's interior ends at 86 and the first button starts at 114, so 88..112 is
 * free space that belongs to nothing else. 9 pt application font -- the same one the
 * diagnostics panes use -- so it reads as status rather than as a control.
 *
 * ⚠⚠ MEASURED AND WRAPPED, NOT TRUSTED TO FIT. The usable width is kBtnL to the
 * window's right margin: 184 px. The string is 38 characters in a PROPORTIONAL font,
 * close enough to that limit that whether it fits depends on system-font metrics which
 * vary by machine. This pane has already been one line from overflowing three times,
 * and its own comment says prose in a fixed-width pane "will keep doing that every
 * time the wording changes". So it is measured: one line when it genuinely fits, split
 * at the semicolon when it does not. The wording is unchanged either way -- only the
 * line count adapts -- and it degrades rather than clipping mid-word.
 *
 * ⚠ ERASES FIRST, ALWAYS, INCLUDING WHEN THERE IS NOTHING TO DRAW. QuickDraw does not
 * clean up after text that simply stops being redrawn, and this message appears and
 * disappears with the component state -- so without an unconditional erase a stale
 * "Components missing" would sit there after the driver bound, which is the one thing
 * worse than not showing it at all. */
static void GroupCaption(const Rect *r, const char *text)
{
    Str255 t;
    Rect   e;
    short  w;

    TextFont(systemFont); TextSize(12); TextFace(0);
    PStr(t, text);
    w = StringWidth(t);
    /* The notch: erase the rule where the glyphs will sit, then draw straddling it.
     * A 12 pt baseline 4 px below the rule puts the ascent above and the descent
     * below, which is what makes it read as an interruption rather than a label
     * floating over a closed box. */
    SetRect(&e, (short)(r->left + 8), (short)(r->top - 6),
                (short)(r->left + 8 + w), (short)(r->top + 7));
    EraseRect(&e);
    MoveTo((short)(r->left + 10), (short)(r->top + 4));
    DrawString(t);
    TextFont(applFont); TextSize(9);
}

static void DrawComponentMsg(void)
{
    Rect        msg;
    Str255      s;
    const short availR = (short)(kWinW - 12);

    SetRect(&msg, kBtnL, 88, availR, 112);
    ErasePlatinum(&msg);
    if (ComponentsOK()) return;

    TextFont(applFont); TextSize(9); TextFace(0);
    /* ★★★★ "COMPONENTS MISSING" IS NOW THE WRONG MESSAGE FOR THE NORMAL CASE.
     *
     * Switcher 1.2 is idle at boot: it declines the card so OS 9's own HID driver keeps
     * it and a paired keyboard types from startup. On every ordinary boot there is
     * therefore NO driver block -- which is exactly the state this message was written
     * to describe as broken. It is not broken, it is the shipping state, and telling
     * the user to check their configuration on a working machine is worse than saying
     * nothing.
     *
     * ⇒ Distinguish the two. Hardware present + extension present + no block means
     * "idle by design"; anything actually absent keeps the original warning. The test
     * is deliberately the same three facts ComponentsOK() uses, so the two can never
     * disagree about which case this is. */
    if (gHasHardware && gExtPresent) {
        /* ⚠⚠ "Mac OS is handling Bluetooth" WAS TRUE AND MISLEADING, and the user
         * caught why: it reads as reassurance. Bluetooth sounds handled, so a keyboard
         * that types looks like success -- when in fact our driver is not running, the
         * card is running its own on-chip stack, and the two things a user came to this
         * panel for are both unavailable.
         *
         * ⭐ AND THE KEYBOARD REALLY DOES KEEP TYPING IN THIS STATE, which is what made
         * the old wording so easy to believe: the card's HID proxy carries an
         * already-paired device through OS 9's stock driver. Measured 2026-09-17 -- the
         * A1016 typed in BOOT protocol with no media keys while the panel said this.
         *
         * ⇒ Name the CONSEQUENCE, not the actor. "No pairing or media keys" is the
         * difference the user can observe, and it explains the otherwise baffling
         * combination of a working keyboard and a dead Scan button. */
        /* ⭐ THE USER'S WORDING, AND IT IS BETTER THAN MINE because it names the
         * MECHANISM rather than just the loss. In this state the card's HID proxy
         * presents a plain boot-protocol keyboard to OS 9's stock driver; the report
         * protocol -- which is what carries the Consumer page, and therefore the media
         * keys -- needs our driver and its HID channel. That is exactly the observed
         * behaviour: plain keys yes, volume and eject no.
         *
         * ⚠ TWO STRINGS BECAUSE THE COLUMN IS ONLY kBtnR-kBtnL WIDE and the long one is
         * borderline at 9pt. StringWidth decides at draw time rather than my estimating
         * it -- the same lesson as the pairing steps, where a conservative guess
         * produced a worse layout than the problem it avoided. The fallback keeps the
         * half a user can act on. */
        PStr(s, "Boot protocol only; report protocol inactive");
        if (StringWidth(s) > (short)(availR - kBtnL))
            PStr(s, "Boot protocol only - no media keys");
        MoveTo(kBtnL, 99); DrawString(s);
        return;
    }
    PStr(s, "Components missing; check configuration");
    if (StringWidth(s) <= (short)(availR - kBtnL)) {
        MoveTo(kBtnL, 99); DrawString(s);
    } else {
        PStr(s, "Components missing;");
        MoveTo(kBtnL, 97);  DrawString(s);
        PStr(s, "check configuration");
        MoveTo(kBtnL, 108); DrawString(s);
    }
}

static void DrawRadioLogo(void)
{
    Rect ic;
    RGBColor savedFore, savedBack;

    ic.left   = gLogoL;
    ic.right  = (short)(ic.left + 32);
    ic.top    = 46;
    ic.bottom = (short)(ic.top + 32);

    /* ⚠ CLAMP RATHER THAN TRUST. gLogoL and gGrpR come from a runtime string
     * measurement, so an unexpectedly wide system font could in principle push the
     * logo into the frame. Pulling it back inside is silent and correct; drawing over
     * the group's own border is neither. */
    if (ic.right > (short)(gGrpR - 2)) {
        ic.right = (short)(gGrpR - 2);
        ic.left  = (short)(ic.right - 32);
    }
    if (ic.left < (short)(kBtnL + 2)) ic.left = (short)(kBtnL + 2);

    /* ⚠ ERASE FIRST. PlotIconID goes through CopyMask and writes ONLY mask-set
     * pixels, so swapping one icon for another leaves behind anything the outgoing
     * icon drew that the incoming one does not cover. The two masks are identical
     * today -- verified when the resources were generated -- so this is belt and
     * braces; it is here so that replacing the artwork later cannot resurrect a
     * ghosting bug that would be baffling to diagnose. */
    ErasePlatinum(&ic);

    /* ⚠⚠ BLACK ON WHITE FIRST. PlotIconID ends up in CopyMask/CopyBits, and those
     * honour the port's foreground and background colours -- so plotting straight
     * after ErasePlatinum, which leaves RGBBackColor set to the theme grey, blits the
     * icon against the wrong ground and produces the washed-out garble that was once
     * reported. The resources themselves were fine then and are checked now: the
     * generator refuses to emit a member of the wrong length.
     *
     * ⚠⚠ SAVE AND RESTORE BOTH COLOURS EXACTLY. An earlier version set black/white
     * and then restored only the BACKGROUND, and only when gThemeOK -- so on any path
     * where that lookup failed, the port was left with a white ground and every
     * control drawn afterwards painted its label on a white band. Getting the actual
     * values back is unconditional and cannot drift from whatever the theme is. */
    GetForeColor(&savedFore);
    GetBackColor(&savedBack);
    ForeColor(blackColor);
    BackColor(whiteColor);
    /* ⚠ Fall back to the panel's own family at 128 if the state icon will not plot,
     * for the same reason the row icons do: these IDs are new, and a family that
     * failed to Rez would leave a blank patch in the group box with no error
     * anywhere. 128 has shipped since the box had a logo at all. */
    if (PlotIconID(&ic, atNone, ttNone,
                   gRadioOn ? kIconRadioOn : kIconRadioOff) != noErr)
        (void)PlotIconID(&ic, atNone, ttNone, kIconPanelOwn);
    RGBForeColor(&savedFore);
    RGBBackColor(&savedBack);
}

/* ★★★★★★ THE PAIRING STEPS, ON THE PANEL ITSELF.
 *
 * ⚠⚠ THE POWER-CYCLE STEP IS THE ONE EVERYBODY MISSES, AND UNTIL NOW IT WAS ONLY IN
 * THE HELP WINDOW. The user put it plainly: someone who has never opened this panel
 * does not know there IS a Bluetooth Help item, so the single instruction without which
 * an A1016 never starts working was reachable only by people who already knew to look.
 * A device that pairs and then does nothing reads as a broken driver, not a missed step
 * -- which is exactly how it was reported the first time.
 *
 * ⇒ The four steps sit under the button that starts them, where the user is already
 * looking, and the Help window becomes the place for detail rather than the only place
 * for the essentials.
 *
 * ⚠ LINES ARE KEPT UNDER ~34 CHARACTERS AND THE WHOLE BLOCK IS CLIPPED. The column is
 * kBtnR - kBtnL = 184px and Geneva 8 runs about 4.2px per character, so 34 leaves real
 * margin -- but an estimate is not a measurement, and this panel has already had text
 * escape a box and sit there unreachable because nothing clipped it. The clip is the
 * guarantee; the short lines are so the guarantee never has to fire. */
static void DrawPairSteps(void)
{
    /* ⚠⚠ ONE STEP, AT MOST TWO LINES. The first 10pt attempt wrapped every step over
     * three narrow lines and read worse than the unreadable 8pt it replaced -- I had
     * budgeted 26 characters a line because I was guessing at Geneva's width and
     * guessing conservatively. Conservative guessing is still guessing.
     *
     * ⇒ THE FIT IS MEASURED AT DRAW TIME, not estimated here. StringWidth is right
     * there; the widest line in the table is compared against the column, and the whole
     * block drops to 9pt if 10 does not fit. That is the same idiom DrawComponentMsg
     * already uses for its one long string, and it means a wrong estimate costs a point
     * of type size rather than a clipped instruction. Clipping would be the worst
     * outcome available: it hides the failure, because text cut at a frame looks like a
     * layout choice rather than a missing word -- and the words at risk are "and Return"
     * and the whole of step 4. */
    static const char *kSteps[] = {
        "TO PAIR A DEVICE:",
        "",
        /* ⚠⚠ THERE IS NO STEP 4 ANY MORE, and deleting it is the point of driver 13.5.
         * "Switch it off and on AGAIN" was in this panel, in the help window and in the
         * pairing dialog for weeks, described as the keyboard's requirement and measured
         * as if it were one. It was not: this stack had never called hid_host_connect,
         * so a freshly paired keyboard sat authenticated and idle waiting for someone to
         * ask, and the power cycle turned it into a bonded device that reconnects. The
         * driver asks now. MEASURED 2026-09-17: connects attempted 1, rc 0, and the user
         * paired with no power cycle at all.
         *
         * ⇒ An instruction that only existed because of a missing call must come out the
         * moment the call exists, or it becomes folklore -- the kind of step everybody
         * repeats because it was in the manual. */
        "1. Switch the device off and on.",
        "   Its light must blink.",
        "2. Click Scan For Devices.",
        "3. Select it, click Pair, then",
        /* ⚠ "and Return" is not padding: an A1016 does not accept the PIN until Return
         * is pressed, and the help window says so too. */
        "   type 0000 and Return on it.",
        "",
        "Type Cmd-? for Help"
    };
    const short n = (short)(sizeof(kSteps) / sizeof(kSteps[0]));
    const short avail = (short)(kBtnR - kBtnL - 4);
    Rect      box;
    RgnHandle save = NewRgn();
    Str255    t;
    short     i, y = 278, widest = 0, pitch = 13;

    SetRect(&box, kBtnL, 268, kBtnR, kWinH - 6);
    /* ⚠ Erased first: this block is static text with no control behind it, and QuickDraw
     * does not clean up after text that is simply not redrawn. */
    ErasePlatinum(&box);
    /* ⚠ Fail OPEN -- no region means draw it unclipped, which is what every earlier
     * version of every draw site here did. */
    if (save != NULL) { GetClip(save); ClipRect(&box); }

    /* ★ MEASURE FIRST. TextSize must be set before StringWidth means anything. */
    TextFont(applFont); TextSize(10); TextFace(0);
    for (i = 0; i < n; i++) {
        PStr(t, kSteps[i]);
        if (StringWidth(t) > widest) widest = StringWidth(t);
    }
    if (widest > avail) { TextSize(9); pitch = 12; }

    for (i = 0; i < n; i++) {
        if (kSteps[i][0] != 0) {
            MoveTo(kBtnL, y);
            PStr(t, kSteps[i]);
            DrawString(t);
        }
        y = (short)(y + pitch);
    }

    if (save != NULL) { SetClip(save); DisposeRgn(save); }
}

static void DrawFrame(void)
{
    Rect r;
    Str255 t;

    /* ★ 12 pt CHARCOAL, not Geneva. applFont is the APPLICATION font, which on this
     * system is Geneva -- and every stock control panel labels itself in the SYSTEM
     * font, which under Platinum is Charcoal. Using the application font made the panel
     * look like a third-party app rather than part of the OS, which is exactly the
     * "seamless" requirement this project is built around. */
    TextFont(systemFont); TextSize(12);
    MoveTo(kListL + 2, 26);
    PStr(t, "Bluetooth Devices:"); DrawString(t);
    TextFont(applFont); TextSize(9);

    /* ★★ EXTENSIONS MANAGER-STYLE COLUMN HEADERS. v0.7 drew these as plain text on a
     * secondary group, which read as a caption rather than a header row.
     *
     * kThemeListHeaderButton is the Appearance Manager's own list-header kind -- the
     * bevelled sort button Extensions Manager uses across the top of its table -- so
     * one is drawn PER COLUMN, which also gives the column separators for free
     * instead of hand-drawn rules that had to be kept in sync with the cell width. */
    { short c;
      ThemeButtonDrawInfo bi;
      /* ⚠⚠⚠ THE LOOP BOUND IS kNumCols AND MUST NEVER BE A LITERAL AGAIN. This read
       * `c < 4` against a THREE-column table and a THREE-entry title array, and it is
       * the whole of the "overlapping text in the devices list" the user photographed.
       *
       * Removing the ID column took kNumCols from 4 to 3; the titles came down to three
       * with it; the literal 4 here did not. Every pass of the phantom fourth column
       * then read one element off the end of BOTH arrays:
       *
       *   hdr[3]      -- past the titles, so it drew whatever pointer the linker had
       *                  placed next in the data section. In this build that was the
       *                  version literal, which is why the header cell read "Status"
       *                  and "Control panel 12.8 - device scanning" superimposed.
       *   ColWidth(3) -- c is not kNumCols-1, so it returned gColW[3], past the widths.
       *                  That garbage is the cell's width, which is why the bogus
       *                  header ran out over the Bluetooth group box beside it.
       *
       * ⚠ AND IT LANDED ON TOP OF STATUS RATHER THAN BESIDE IT, which is why this read
       * as a text-overlay bug rather than as an extra column: ColLeft(3) sums gColW[0..2]
       * and gColW[2] is 0 -- the last column's width is COMPUTED, not stored -- so the
       * fourth cell started at exactly the third cell's left edge.
       *
       * ⚠ Undefined behaviour, not merely cosmetic: two reads past the end of two
       * arrays, one of them used as a pointer to draw a string. It rendered as garbled
       * text here; there is no rule that says it had to.
       *
       * ⇒ gColTitle is now declared beside gColW with a compile-time count check. */

      bi.state = kThemeStateActive;
      bi.adornment = kThemeAdornmentNone;

      for (c = 0; c < kNumCols; c++) {
          /* ★ The sort column draws pressed. Extensions Manager marks its sort key the
           * same way, and without it the ordering looks arbitrary. */
          bi.value = (c == gSortCol) ? kThemeButtonOn : kThemeButtonOff;
          /* ⚠ SAME GEOMETRY AS THE LDEF, from the same gColW. The header used to lay
           * itself out with its own uniform width, and the moment the LDEF started
           * drawing at real column offsets the two would have drifted apart -- header
           * bevels in one place, text in another. One source of truth. */
          short l = ColLeft(c);
          short rt = (c == kNumCols - 1) ? kListR : (short)(l + ColWidth(c));
          SetRect(&r, l, kListT - 16, rt, kListT);
          if (DrawThemeButton(&r, kThemeListHeaderButton, &bi, NULL, NULL, NULL, 0)
                  != noErr) {
              gThemeOK = false;
              FrameRect(&r);            /* visible beats invisible, as everywhere */
          }
          /* ★ Header titles in the SYSTEM font, like every stock header row. */
          TextFont(systemFont); TextSize(9); TextFace(0);
          MoveTo((short)(l + 6), kListT - 5);
          PStr(t, gColTitle[c]); DrawString(t);

          /* ★★ THE SORT INDICATOR, which Extensions Manager has at the right of its
           * active header and we did not. Without it the pressed bevel is the only cue
           * that a column is the sort key, and it says nothing about DIRECTION -- so a
           * reversed sort looked like the table had simply shuffled itself.
           *
           * A small solid triangle, pointing down for ascending. Hand-drawn rather than
           * a resource: it is six lines of arithmetic and needs no asset. */
          if (c == gSortCol) {
              short ax = (short)(rt - 12), ay = kListT - 11, i;
              RGBColor savedF; GetForeColor(&savedF);
              ForeColor(blackColor);
              for (i = 0; i < 4; i++) {
                  short y = gSortDesc ? (short)(ay + 3 - i) : (short)(ay + i);
                  MoveTo((short)(ax + i), y);
                  LineTo((short)(ax + 6 - i), y);
              }
              RGBForeColor(&savedF);
          }
          TextFont(applFont); TextSize(9);
      }
    }

    /* The list well gets the real theme frame rather than a FrameRect. */
    SetRect(&r, kListL, kListT, kListR, kListB);
    InsetRect(&r, -1, -1);
    DrawThemeListBoxFrame(&r, kThemeStateActive);

    /* The on/off group. */
    SetRect(&r, kBtnL, 34, gGrpR, 86);
    FrameGroup(&r, true);
    /* ★ 12 pt, and sitting ON the group's top rule rather than floating above it.
     *
     * A group-box caption is supposed to interrupt the frame line, which is why the
     * notch is erased first. At 9 pt with a baseline of 32 the whole glyph cleared the
     * rule at y=34, so it read as a stray label above an unlabelled box. A 12 pt
     * baseline of 38 straddles the rule: ascent reaches ~28, descent ~41. */
    /* ★ Charcoal here too, for the same reason -- and measured AFTER the font is set,
     * because Charcoal is wider than Geneva at the same size and a notch erased to
     * Geneva's width would leave the frame rule showing through the caption. */
    TextFont(systemFont); TextSize(12);
    PStr(t, " Bluetooth ");
    { Rect e; short w = StringWidth(t);
      SetRect(&e, kBtnL + 8, 28, (short)(kBtnL + 8 + w), 41); EraseRect(&e); }
    MoveTo(kBtnL + 10, 38); DrawString(t);
    TextFont(applFont); TextSize(9);

    DrawRadioLogo();

    /* ★★ "Components missing; check configuration", under the group box.
     *
     * Sits in the gap between the group's bottom (86) and the first button's top
     * (114), in the same 9 pt application font as the diagnostics panes, so it reads
     * as status rather than as a control.
     *
     * ⚠⚠ MEASURED AND WRAPPED, NOT TRUSTED TO FIT. The available width is kBtnL to
     * the window's right margin -- 184 px -- and this string is 38 characters in a
     * PROPORTIONAL font. That is close enough to the limit that whether it fits
     * depends on the system font's metrics, which vary by machine. This pane has
     * already been one line from overflowing three times, and its own comment says
     * prose in a fixed-width pane "will keep doing that every time the wording
     * changes". So: draw one line when it genuinely fits, and split at the semicolon
     * when it does not. The wording is the user's either way -- only the line count
     * adapts, and it degrades instead of clipping mid-word.
     *
     * ⚠ The area is erased first. This text appears and disappears with the component
     * state, and QuickDraw does not clean up after text that is simply not redrawn --
     * without the erase, a stale message would persist after the driver bound. */
    DrawComponentMsg();

    DrawDeviceInfo();
    DrawDescription();
    DrawPairSteps();
}

/* ---- menus ---------------------------------------------------------------- *
 * ⚠ v0.2 HAD NO MENU BAR AT ALL, which meant the only way out was the close box and
 * there was no About. A stock control panel has Apple / File / Edit / Help and
 * File > Quit works at any time, so this one does too.
 *
 * Built in code rather than from MENU resources: it keeps the menu structure next to
 * the code that dispatches it, and it avoids a resource-plumbing mistake in a file
 * that already has to carry a BNDL and an icon family. */
#define kMApple 128
#define kMFile  129
#define kMEdit  130
/* No kMHelp: the Help Manager supplies the Help menu. See SetUpMenus. */

/* Apple menu items */
#define kAboutItem 1
/* File menu items */
/* ⚠⚠ THESE ARE POSITIONS IN THE File MENU AND THEY MOVE WHEN AN ITEM IS INSERTED.
 *
 * Quit was item 1 until v7.6 put "Hand Bluetooth Back to Mac OS" and a separator
 * above it. A stale kQuitItem would not fail visibly -- picking the new first item
 * would simply QUIT THE PANEL, which reads as "the new command does nothing" and is
 * exactly the kind of silent misbehaviour that costs a hardware run to diagnose.
 * Keep these in the same order as the AppendMenu calls in SetUpMenus. */
#define kPairModeItem 1
#define kHandBackItem 2
/* ★★★★★★ v10.4: RESTORE A SAVED KEY TO THE CARD -- the undo for Delete.
 * ⚠ INSERTED ABOVE THE SEPARATOR, so kQuitItem MOVES FROM 4 TO 5. The comment above
 * says exactly what a stale constant does here: picking the wrong item QUITS THE PANEL
 * and reads as "the new command does nothing". Both numbers updated together. */
#define kRestoreItem  3
#define kQuitItem     5     /* 1 Pair Mode, 2 Hand Back, 3 Restore, 4 sep, 5 Quit */

static void SetUpMenus(void)
{
    MenuHandle m;

    /* 0x14 is the apple glyph in the system font -- the conventional Apple menu
     * title. Written as an escape rather than a literal so the source stays plain
     * ASCII and cannot be mangled by an encoding change. */
    m = NewMenu(kMApple, "\p\024");
    if (m) {
        AppendMenu(m, "\pAbout Bluetooth\311;(-");
        /* Desk accessories, as any well-behaved application offers. */
        AppendResMenu(m, 'DRVR');
        InsertMenu(m, 0);
    }

    m = NewMenu(kMFile, "\pFile");
    if (m) {
        /* ★★★ v7.6: THE LAST SHIP-BLOCKER, AS A MENU ITEM FIRST.
         *
         * Pairing currently needs extension surgery -- install the switcher, reboot,
         * pair, remove the switcher, reboot -- because the switcher switches at every
         * Initialize and nothing switches back. Driver 8.5 can send the documented
         * reverse request (hid2hci's wValue = 1), which would replace the second
         * reboot AND the file removal with one click.
         *
         * ⚠ A MENU ITEM, NOT A BUTTON, ON PURPOSE. Whether this card accepts the
         * reverse request from OS 9 has never been measured -- our own note said it
         * "can only be sent from a host that can see the card, which today means
         * Tiger", written before anything could bind 8204. Until a run says otherwise
         * this is a diagnostic, and a diagnostic does not get prime real estate in a
         * control panel. It becomes a real button, with real wording, once it works.
         *
         * ⚠ Named for what it DOES, not for the plumbing. "Hand Bluetooth Back to
         * Mac OS" is the user-visible effect; the user should not have to know the
         * words HCI or proxy to understand that their keyboard comes back. */
        AppendMenu(m, "\pTurn On Pairing at Restart\311");
        AppendMenu(m, "\pHand Bluetooth Back to Mac OS\311");
        /* ★★★★ v10.4: the undo for Delete Stored Key, and the reason deleting the
         * A1016's Tiger-made bond is a reversible experiment. The driver captured the
         * card's own keys into our database at startup (v12.0); this writes the saved
         * one back with Write_Stored_Link_Key.
         * ⚠ A MENU ITEM rather than a button for the same reason Hand Back is one: it
         * is a deliberate recovery action, not part of the normal flow, and it must not
         * sit next to Delete where a mis-click could reach it. */
        AppendMenu(m, "\pRestore Saved Key to Card\311");
        AppendMenu(m, "\p(-");
        AppendMenu(m, "\pQuit/Q");
        InsertMenu(m, 0);
    }

    /* ⚠ Edit is present and DISABLED, deliberately. The Edit menu exists so that
     * desk accessories opened from the Apple menu have somewhere to send their
     * editing commands -- that is the actual reason stock applications carry one --
     * and the panel itself has no editable text yet, so its items stay grey rather
     * than pretending. */
    m = NewMenu(kMEdit, "\pEdit");
    if (m) {
        AppendMenu(m, "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear");
        InsertMenu(m, 0);
        DisableItem(m, 0);
    }

    /* ⚠ NO HELP MENU OF OUR OWN. v0.3 added one and the menu bar showed TWO: the
     * Help Manager appends a Help menu to every application's bar automatically, so
     * an app that adds its own gets a duplicate. The way to put an item in the OS's
     * Help menu is HMGetHelpMenuHandle (Balloons.h) and append to what it returns.
     *
     * ★★★★ v12.1: THAT CONDITION IS NOW MET -- "worth doing when there is actual help
     * to show" -- and this is the appending, not a second menu.
     *
     * ⚠⚠ REMEMBER THE ITEM NUMBER RATHER THAN ASSUMING ONE. The Help Manager owns the
     * items above ours and how many there are is not ours to predict: it varies with
     * the system version and with what else is installed. CountMItems AFTER the append
     * is the only honest way to know which item is ours, and a hardcoded index would
     * fire on somebody else's command -- the same class of bug as the File menu's
     * kQuitItem, which this project has already had to renumber once. */
    {
        MenuRef hm = NULL;
        if (HMGetHelpMenuHandle(&hm) == noErr && hm != NULL) {
            /* ⭐ "/?" GIVES THE ITEM ITS COMMAND-KEY DISPLAY. AppendMenu treats "/" as
             * the command-key metacharacter, so the item text stays "Bluetooth Help"
             * and the Menu Manager draws the cloverleaf and "?" down the right-hand
             * side, the way every other shortcut in the menu bar is shown.
             *
             * ⚠⚠ THIS IS DISPLAY ONLY, AND THE KEYSTROKE IS STILL HANDLED BY HAND --
             * the two are NOT redundant. "?" is a SHIFTED character on most layouts, so
             * a menu command-key equivalent of "?" does not reliably match what MenuKey
             * is handed; the main loop therefore tests the character itself, and it does
             * so BEFORE calling MenuKey, so this cannot double-fire. See the note at
             * that test. Removing either one breaks something: drop this and the
             * shortcut becomes invisible, drop that and it stops working. */
            AppendMenu(hm, "\pBluetooth Help/?");
            gHelpItem = CountMItems(hm);
        }
    }

    DrawMenuBar();
}


/* ============================ BLUETOOTH HELP ================================
 *
 * ⭐ Requested by the user after asking what the Status column actually meant --
 * which was a fair question that the column alone could not answer, and the same
 * session in which they had to DISCOVER that a freshly paired keyboard must be
 * switched off and on. Both belong somewhere a user can find them.
 *
 * ⚠ APPENDED TO THE SYSTEM'S HELP MENU, NEVER OUR OWN. The note in SetUpMenus
 * records what happens otherwise: v0.3 added a Help menu and the bar showed TWO,
 * because the Help Manager already appends one to every application. That note
 * also said this was "worth doing when there is actual help to show, not before"
 * -- the condition it set has now been met.
 *
 * ⚠ TextEdit rather than a wall of DrawString calls: it wraps, it scrolls, and it
 * is the standard OS 9 way to present more text than fits. A DITL is deliberately
 * avoided for the same reason the About box avoids one
 * [[feedback_os9_null_useritem_breaks_clicks]]. */

static const char *kHelpText =
"WHAT THIS CONTROL PANEL DOES\r"
"\r"
"It lets Mac OS 9 use a Bluetooth keyboard or mouse directly, without Mac OS X. "
"Pairings live in the Bluetooth module itself, so a paired keyboard keeps working "
"from the moment the machine starts.\r"
"\r"
"\r"
"PAIRING A DEVICE\r"
"\r"
"1. Put the device into pairing mode. On an Apple Wireless Keyboard or Mouse, "
"switch it OFF and then ON while it is not paired with anything; its light "
"blinks.\r"
"2. Click Scan For Devices. The device appears with the status Available.\r"
"3. Select it and click Pair. Type 0000 and Return on the DEVICE when asked.\r"
"\r"
"The device should start working within a second or two of the pairing finishing. If it does not, and the status stays at Linked, not ready with its light still blinking, switch it off and on once -- that was a required step in earlier versions of this software and it is still a useful thing to try.\r"
"\r"
"\r"
"WHAT THE STATUS COLUMN MEANS\r"
"\r"
"Available -- the Mac has seen this device but holds no pairing for it. Select it "
"and click Pair to use it.\r"
"\r"
"Paired, disconnected -- the Mac holds a pairing, but the device is not connected "
"right now. It is switched off, asleep, or out of range. This is normal and the "
"device will reconnect on its own; on a keyboard, press a key to wake it.\r"
"\r"
"Connected -- the device is connected and paired. For a phone or similar, this is "
"as good as it gets.\r"
"\r"
"Linked, not ready -- a keyboard or mouse is connected, but the channel that carries "
"its keystrokes is not open, so it will not type or move the pointer yet. It usually "
"clears by itself within a second or two of pairing. If it stays, switch the device "
"off and on once.\r"
"\r"
"Connected, no key -- connected and working, but with no pairing stored on this "
"Mac and none in the Bluetooth module either. It will keep working until the device is switched off, and will then have to be paired again. This is the status to look for if a keyboard that worked yesterday needs pairing every morning.\r"
"\r"
"Waiting for device -- you clicked Connect. The Mac has stopped refusing the device "
"and is now waiting for it to come back, which it does by itself when it is used. "
"Press a key or move the mouse. If nothing happens, the row returns to Paired, "
"disconnected after half a minute.\r"
"\r"
"Disconnected by you -- you clicked Disconnect. The Mac is refusing this device "
"until you select it and click Connect, or restart. A device left like this looks "
"broken, so this is the first thing to check if one stops working.\r"
"\r"
"\r"
"WHAT THE BUTTONS DO\r"
"\r"
"Scan For Devices -- looks for devices in pairing mode nearby, for a few seconds. "
"A device that is already paired to this Mac will not appear; it does not need "
"to.\r"
"\r"
"Pair -- pairs the selected device. You will be asked to type 0000 on the device "
"itself.\r"
"\r"
"Disconnect / Connect -- Disconnect ends the connection AND keeps the device "
"disconnected, so it cannot immediately reconnect. The button then reads Connect, "
"which allows it back. A restart also allows it back.\r"
"\r"
"Connect does not dial the device: it stops this Mac refusing it, and the device then reconnects on its own. On a keyboard or mouse, press a key or move it to wake it up. The status changes to Paired, disconnected as soon as you click Connect, and to Connected when the device comes back.\r"
"\r"
"Delete -- removes the pairing entirely and the device disappears from the list at "
"once. It comes back only when a new scan actually finds it, which for a keyboard or mouse means it has to be in pairing mode: switch it off and on first. It must then be paired again to be used. If the "
"device is in the Bluetooth module's own memory, the warning says so: that memory "
"is shared with Mac OS X, so removing it affects Mac OS X too.\r"
"\r"
"Delete also works on a device that is not paired. There it removes nothing but the row, which is a way to tidy a list full of devices you do not own.\r"
"\r"
"Pair is greyed out on a device that is Connected, because re-pairing would renegotiate something that is already working. On every other status it is available, including Linked, not ready and Connected, no key, where trying again is the fix. Pairing a device you have Disconnected is allowed and releases the disconnection: asking to pair something is a clear statement that you want it back.\r"
"\r"
"\r"
"WHAT THE CONFIGURATION DETAILS MEAN\r"
"\r"
"Module detected -- the Bluetooth hardware this Mac has. On a Power Mac G4 with the "
"factory option this reads Internal Bluetooth Card.\r"
"\r"
"Extension detected -- the version of the Bluetooth software installed in your System "
"Folder. This is read from the file, so it answers even when the software is not "
"running.\r"
"\r"
"Driver active -- whether that software is actually driving the module right now. Yes "
"means everything on this panel works. Inactive means the software is installed and "
"not running, and the Card switch line below says why.\r"
"\r"
"Card switch -- what happened at start-up, and this is the line to read first when "
"nothing works:\r"
"\r"
"   switched at start-up -- normal. This software has the module.\r"
"   not attempted this start-up -- the module was left to Mac OS.\r"
"   declined -- a previous start-up claimed the module and then failed, so this one "
"stood back on purpose. Choose Turn On Pairing at Restart from the File menu and "
"restart to try again.\r"
"   switched, driver did not bind -- the module was handed over and the software did "
"not take it. The number in brackets is the error. Check the extension is installed "
"and expanded.\r"
"   card not seen by the switcher -- the hardware was not found.\r"
"   Inactive (A1044 module not detected) -- normal on a machine with no internal "
"Apple Bluetooth module, such as one using a USB dongle. The switcher only takes the "
"INTERNAL card out of proxy mode; nothing is wrong and nothing needs installing.\r"
"   switcher extension not installed -- one of the two extensions is missing.\r"
"\r"
"Scan -- what the last Scan For Devices did, and any error it met.\r"
"\r"
"\r"
"BOOT PROTOCOL AND REPORT PROTOCOL\r"
"\r"
"If the panel says Boot protocol only; report protocol inactive, this is what it "
"means.\r"
"\r"
"A Bluetooth keyboard can talk to a Mac in two ways. The simple way carries the "
"ordinary keys and nothing else, and the Bluetooth module can do it entirely on its "
"own, without this software. The fuller way carries everything -- including the volume, "
"mute and eject keys -- and needs this software running.\r"
"\r"
"So when you see that message, a keyboard that was already paired KEEPS TYPING, which "
"is why it can look as though nothing is wrong. What you have lost is the volume keys, "
"and the ability to pair anything new: Scan For Devices will do nothing. The Card "
"switch line above says why the software is not running, and what to do about it.\r"
"\r"
"\r"
"FILE MENU\r"
"\r"
"Turn On Pairing at Restart -- forces this software to claim the Bluetooth module at "
"the next start-up. You do not normally need it: the module is claimed automatically "
"every time the machine starts. It exists for one situation, and the Card switch line "
"names that situation -- if it says declined, a previous start-up claimed the module "
"and then failed, so the software now stands back rather than repeating it. This "
"overrides that and tries once more.\r"
"\r"
"Hand Bluetooth Back to Mac OS -- releases the module so Mac OS handles paired "
"devices itself. This software stops until you restart.\r"
"\r"
"Restore Saved Key to Card -- puts back a pairing that was deleted from the "
"module, using a copy kept on this Mac. Use it if deleting a device was a "
"mistake.\r"
"\r"
"\r"
"IF SOMETHING IS NOT WORKING\r"
"\r"
"A keyboard or mouse that pairs and then does nothing: look at the Card switch line "
"in Configuration Details. If it does not say switched at start-up, this software is "
"not driving the module, and that is the problem rather than the device. Otherwise "
"switch the device off and on once.\r"
"\r"
"A device that will not reconnect: press a key or move it. These devices sleep to "
"save their batteries and only reconnect when used.\r"
"\r"
"A device that does nothing at all: check whether it reads Disconnected by you, "
"and click Connect if so.\r";

/* ★★★★ THE ACTION PROC, which is what makes a held scroll arrow REPEAT.
 *
 * ⚠⚠ TrackControl with a NULL action proc calls back exactly ONCE, so the first
 * version scrolled one line per click no matter how long the button was held -- the
 * user reported precisely that. The Control Manager calls this repeatedly for as long
 * as the mouse is down, and the scrolling has to happen HERE rather than after
 * TrackControl returns.
 *
 * ⚠ The TE handle travels through a file-static rather than a refCon because
 * ControlActionProcPtr has no user-data argument; the help window is modal and runs
 * one at a time, so there is exactly one live value and no re-entrancy to worry about. */
static TEHandle          gHelpTE;
static ControlActionUPP  gHelpActionUPP;

/* ★★★★★★ THE SCROLL BAR COUNTS PIXELS NOW, NOT LINES, AND IT HAD TO.
 *
 * ⚠⚠ A STYLED TEXTEDIT RECORD HAS NO SINGLE LINE HEIGHT. The section headings are 12pt
 * over 9pt body, so TEStyleNew sets lineHeight -- and fontAscent -- to -1, meaning
 * "computed per line". Every piece of the old arithmetic multiplied by that field:
 * visLines was viewHeight / lineHeight and every scroll was delta * lineHeight. Left
 * as it was, a -1 line height would have made visLines NEGATIVE and sent each arrow
 * click scrolling the wrong way by one pixel. The mixed sizes and the scroll model are
 * the same change; doing only the first would have shipped a broken scroll bar.
 *
 * ⇒ Value = pixels from the top of the text. TEGetHeight gives the total, which is the
 * one measurement that stays correct however the styles are mixed. */
#define kHelpArrowPx 14          /* one nominal body line */
static short gHelpPagePx = 100;  /* set from the view height */

static pascal void HelpScrollAction(ControlHandle c, short part)
{
    short before = GetControlValue(c), after, delta = 0;
    if (part == 0) return;                 /* mouse left the control */
    switch (part) {
    case kControlUpButtonPart:   delta = -kHelpArrowPx;  break;
    case kControlDownButtonPart: delta =  kHelpArrowPx;  break;
    case kControlPageUpPart:     delta = -gHelpPagePx;   break;
    case kControlPageDownPart:   delta =  gHelpPagePx;   break;
    default: return;
    }
    SetControlValue(c, before + delta);    /* the Control Manager clamps for us */
    after = GetControlValue(c);
    if (after != before && gHelpTE != NULL)
        TEScroll(0, (short)(before - after), gHelpTE);
}

/* ★★★★★ THE ALL-CAPS LINES ARE SECTION HEADINGS, so set them 12pt.
 *
 * ⚠ FOUND BY READING THE TEXT, NOT BY A TABLE OF OFFSETS. A list of character ranges
 * would be correct exactly until the next time the help text is edited, and nothing
 * would report it -- the headings would simply drift onto the wrong lines. Classifying
 * each line by what it contains cannot drift, because it is re-derived from the text
 * that is actually there.
 *
 * ⚠ "Has a letter AND no lowercase letter." Digits, punctuation and blank lines are
 * not headings on their own, or the separator lines would all inflate.
 *
 * ⚠⚠ AND A LINE THAT STARTS WITH A DIGIT IS A NUMBERED STEP, NOT A HEADING. Running
 * this over the real help text -- rather than trusting the rule -- turned up exactly
 * one false positive, and it was the worst possible one:
 *
 *     4. SWITCH THE DEVICE OFF AND ON AGAIN.
 *
 * ⚠ THAT LINE NO LONGER EXISTS -- driver 13.5's outgoing connect deleted the step it
 * described -- and the rule it justifies is kept anyway, deliberately. The rule is
 * "a line opening with a digit is a numbered step, not a section heading", which is
 * true of any numbered list this text ever grows. Deleting a guard because the one
 * case that motivated it went away is how the case comes back unnoticed.
 *
 * At the time, that line was shouted deliberately inside the numbered pairing list,
 * because it was the step everybody missed. At 12pt it would have read as a SECTION BREAK dropped
 * between steps 3 and 4 -- destroying the hierarchy in the one place the headings
 * exist to make clear. "All caps" is what a heading looks like; "all caps and does
 * not open with a number" is what a heading IS in this text. */
static void HelpStyleHeadings(TEHandle te, const char *s)
{
    TextStyle ts;
    long      i = 0, lineStart = 0;

    ts.tsFont = applFont;
    ts.tsFace = 0;
    ts.tsSize = 12;

    for (;;) {
        char c = s[i];
        if (c == '\r' || c == 0) {
            long    j;
            Boolean hasAlpha = false, anyLower = false;
            Boolean numbered = (i > lineStart &&
                                s[lineStart] >= '0' && s[lineStart] <= '9');
            for (j = lineStart; j < i; j++) {
                char ch = s[j];
                if (ch >= 'a' && ch <= 'z') { anyLower = true; break; }
                if (ch >= 'A' && ch <= 'Z') hasAlpha = true;
            }
            if (hasAlpha && !anyLower && !numbered) {
                TESetSelect(lineStart, i, te);
                TESetStyle(doSize, &ts, false, te);
            }
            if (c == 0) break;
            lineStart = i + 1;
        }
        i++;
    }
    /* ⚠ Leave no selection behind: the record is never activated, but a live range
     * would still be the first thing a TEUpdate drew if that ever changed. */
    TESetSelect(0, 0, te);
}

/* The scrollable height, in pixels, of a record whose lines are not a uniform height. */
static short HelpMaxScroll(TEHandle te, const Rect *view)
{
    long total = TEGetHeight((long)(*te)->nLines, 1L, te);
    long ms    = total - (long)(view->bottom - view->top);
    if (ms < 0)     ms = 0;
    if (ms > 30000) ms = 30000;   /* a control value is a short */
    return (short)ms;
}

/* ★★★★ THE HELP WINDOW'S LAYOUT, IN ONE PLACE.
 *
 * ⚠⚠ FOUR RECTANGLES THAT MUST AGREE, AND A RESIZE PATH THAT RECOMPUTES ALL OF THEM.
 * That is the same shape of hazard as the devices-list header, where two tables that
 * had to agree were written out twice and drifted the moment one changed. Here the
 * create path and the grow path would each have had their own copy of the arithmetic.
 * ⇒ One function, called by both. Neither can be edited without the other following. */
#define kHelpStripH 40     /* the button strip across the bottom */
#define kHelpSbW    15     /* scroll-bar width, and the size box's too */
#define kHelpOkW    70     /* Apple's minimum push-button width */
#define kHelpOkH    20

static void HelpLayout(const Rect *port, Rect *view, Rect *sbr,
                       Rect *strip, Rect *okR)
{
    strip->left   = port->left;
    strip->right  = port->right;
    strip->top    = (short)(port->bottom - kHelpStripH);
    strip->bottom = port->bottom;

    /* ⚠ The DEST rect matches the VIEW rect's width so TextEdit wraps to the visible
     * column rather than to something wider that would then need horizontal
     * scrolling -- this window has no horizontal scroll bar and must never want one. */
    *view = *port;
    view->right  = (short)(view->right - kHelpSbW);
    view->bottom = strip->top;
    InsetRect(view, 6, 4);

    /* ⚠ top -1 and right at the port edge put the control's own frame lines just
     * outside the visible port, so only its inner edge shows -- the standard idiom.
     * The bottom now stops at the strip instead of 14px short of the window bottom:
     * it no longer has to dodge the size box, because the size box is in the strip. */
    sbr->left   = (short)(port->right - kHelpSbW);
    sbr->right  = port->right;
    sbr->top    = (short)(port->top - 1);
    sbr->bottom = strip->top;

    /* ⚠ RIGHT EDGE AT -26, NOT -12. The default ring is drawn OUTSIDE the control by
     * 4px, and the size box owns the rightmost 15. -26 leaves the ring clear of the
     * size box by 7px instead of colliding with it. */
    okR->right  = (short)(port->right - 26);
    okR->left   = (short)(okR->right - kHelpOkW);
    okR->top    = (short)(strip->top + 10);
    okR->bottom = (short)(okR->top + kHelpOkH);
}

/* ★★★★ THE SIZE BOX, AND NOTHING ELSE.
 *
 * ⚠⚠ THIS IS THE "EMPTY HORIZONTAL SCROLL BAR" THE USER PHOTOGRAPHED. DrawGrowIcon on
 * a documentProc window does not just draw the size box: it also draws the lines that
 * delimit where the scroll bars would go, along the FULL right and bottom edges. The
 * bottom line made a 15px gutter that reads as an empty horizontal scroll bar.
 *
 * ⚠ And it "vanished once you scrolled" because the old text view extended to within
 * 4px of the window bottom, straight across that gutter -- so the first TEScroll
 * repainted over the line and it never came back. The disappearance was the second
 * symptom of one defect, not a separate mystery.
 *
 * ⇒ Clip to the corner and the delimiting lines have nowhere to land. Same fix, same
 * reason, as claude-os9/client/src/main.c:959, which hit this first. */
static void DrawSizeBoxOnly(WindowPtr w)
{
    RgnHandle save = NewRgn();
    Rect      gb;

    /* ⚠ Fail OPEN. No region means an unclipped grow icon -- ugly, and better than a
     * window with no size box at all. */
    if (save == NULL) { DrawGrowIcon(w); return; }

    GetClip(save);
    gb = w->portRect;
    gb.left = (short)(gb.right  - kHelpSbW - 1);
    gb.top  = (short)(gb.bottom - kHelpSbW - 1);
    ClipRect(&gb);
    DrawGrowIcon(w);
    SetClip(save);
    DisposeRgn(save);
}

/* ⚠⚠ GREY, THEN WHITE AGAIN, ALWAYS. ErasePlatinum LEAVES the port's background set to
 * platinum, and TextEdit paints its background at DRAW time -- TEUpdate and, worse,
 * TEScroll, which fills every newly exposed band with the current background. Leaving
 * the port grey after painting the strip would have put grey bands inside the white
 * text area on the first scroll: exactly the bug the About box's well had twice. The
 * port's background is white at every instant except inside this call. */
static void HelpEraseStrip(const Rect *strip)
{
    RGBColor saveBk;
    GetBackColor(&saveBk);
    ErasePlatinum(strip);
    RGBBackColor(&saveBk);
}

static void ShowHelp(void)
{
    Rect        r, view, sbr, strip, okR;
    WindowPtr   w;
    EventRecord e;
    TEHandle    te;
    ControlHandle sb, okBtn;
    Boolean     go = true;
    short       maxScroll;

    SetRect(&r, 0, 0, 440, 320);
    OffsetRect(&r, (qd.screenBits.bounds.right  - 440) / 2,
                   (qd.screenBits.bounds.bottom - 320) / 2);
    /* ⚠ documentProc, NOT noGrowDocProc. The first version drew a grow icon into a
     * window that had no grow box, so the corner looked broken and could not be
     * dragged -- the user reported the missing size box. A help window is exactly the
     * kind a user wants to make bigger, so give it a real one. */
    w = NewCWindow(NULL, &r, "\pBluetooth Help", true, documentProc,
                   (WindowPtr)-1L, true, 0);
    if (w == NULL) return;
    SetPort(w);
    TextFont(applFont); TextSize(9);

    HelpLayout(&w->portRect, &view, &sbr, &strip, &okR);

    /* ⚠⚠ TEStyleNew, NOT TENew. A plain record has ONE size for the whole text, so the
     * 12pt section headings are not a formatting preference here -- they are the reason
     * this has to be a styled record at all. See HelpScrollAction for what that costs:
     * a styled record reports lineHeight -1, and the scroll arithmetic had to stop
     * being counted in lines. */
    te = TEStyleNew(&view, &view);
    if (te == NULL) { DisposeWindow(w); return; }
    TEAutoView(false, te);
    TEInsert(kHelpText, (long)strlen(kHelpText), te);
    HelpStyleHeadings(te, kHelpText);

    sb = NewControl(w, &sbr, "\p", true, 0, 0, 0, scrollBarProc, 0);

    /* ★ THE OK BUTTON, and it is the DEFAULT one: Return and Enter already dismissed
     * this window, and the ring is what says so before the user tries it. */
    { Str255 lbl; PStr(lbl, "OK");
      okBtn = NewControl(w, &okR, lbl, true, 0, 0, 1, pushButProc, 0); }
    if (okBtn != NULL) {
        Boolean isDef = true;
        (void)SetControlData(okBtn, kControlEntireControl,
                             kControlPushButtonDefaultTag,
                             sizeof(isDef), (Ptr)&isDef);
    }

    /* ⚠⚠ A ROUTINE DESCRIPTOR, NOT A BARE FUNCTION POINTER. The Control Manager is
     * 68K-aware and calls action procs through Mixed Mode; handing it the address of a
     * PowerPC function directly is the classic way to make a control "work" until the
     * moment it is used and then take the machine down. NewControlActionUPP builds the
     * descriptor; it is disposed on the way out. */
    SetMenusForModal(true);
    gHelpActionUPP = NewControlActionUPP(HelpScrollAction);
    gHelpTE     = te;
    gHelpPagePx = (short)(view.bottom - view.top - kHelpArrowPx);
    if (gHelpPagePx < kHelpArrowPx) gHelpPagePx = kHelpArrowPx;
    maxScroll   = HelpMaxScroll(te, &view);
    if (sb != NULL) SetControlMaximum(sb, maxScroll);

    while (go) {
        if (WaitNextEvent(everyEvent, &e, 10L, NULL)) {
            switch (e.what) {
            case updateEvt:
                if ((WindowPtr)e.message == w) {
                    BeginUpdate(w);
                    SetPort(w);
                    /* ⚠ ORDER MATTERS. White first over everything, then the strip
                     * grey on top of it, then the text -- which draws on white,
                     * because HelpEraseStrip gave the background back. */
                    EraseRect(&w->portRect);
                    HelpEraseStrip(&strip);
                    TEUpdate(&view, te);
                    if (sb    != NULL) { Draw1Control(sb); }
                    if (okBtn != NULL) { Draw1Control(okBtn); }
                    DrawSizeBoxOnly(w);
                    EndUpdate(w);
                } else if ((WindowPtr)e.message == gWin) {
                    /* ⚠ The same trail the About box left; both modal loops need it. */
                    RedrawPanelWindow();
                }
                break;
            case mouseDown: {
                WindowPtr hit; short part = FindWindow(e.where, &hit);
                /* ⚠⚠ THE SAME MENU-BAR RULE AS THE ABOUT BOX, and for the same reason:
                 * a menu-bar click the application swallows leaves the Menu Manager
                 * mid-interaction, after which the window will not drag. This window
                 * had the identical omission, so fixing only the About box would have
                 * shipped the same defect under a different menu item. */
                if (part == inMenuBar) {
                    (void)MenuSelect(e.where);
                    HiliteMenu(0);
                    break;
                }
                if (part == inSysWindow) { SystemClick(&e, hit); break; }
                /* ⚠ A click outside this window is ignored rather than beeped: the
                 * window is modal only in the sense that it runs its own loop, and
                 * scolding someone for clicking the panel behind it is noise. */
                if (hit != w) break;
                if (part == inGoAway) {
                    if (TrackGoAway(w, e.where)) go = false;
                } else if (part == inDrag) {
                    DragWindow(w, e.where, &qd.screenBits.bounds);
                } else if (part == inGrow) {
                    /* ⚠ Re-lay the TE view and the scroll bar after a resize, and
                     * RECOMPUTE the maximum: a taller window shows more lines, so a
                     * stale maximum would let the user scroll past the end. */
                    Rect lim; long got;
                    SetRect(&lim, 260, 160, qd.screenBits.bounds.right,
                                            qd.screenBits.bounds.bottom);
                    got = GrowWindow(w, e.where, &lim);
                    if (got != 0) {
                        SizeWindow(w, (short)LoWord(got), (short)HiWord(got), true);
                        SetPort(w);
                        /* ⚠ THE SAME HelpLayout THE CREATE PATH USED. Recomputing the
                         * geometry by hand here is how the two would drift. */
                        HelpLayout(&w->portRect, &view, &sbr, &strip, &okR);
                        (*te)->viewRect = view;
                        (*te)->destRect.left  = view.left;
                        (*te)->destRect.right = view.right;
                        TECalText(te);
                        if (sb != NULL) {
                            MoveControl(sb, sbr.left, sbr.top);
                            SizeControl(sb, (short)(sbr.right - sbr.left),
                                            (short)(sbr.bottom - sbr.top));
                        }
                        /* ⚠ The button MOVES with the bottom edge. A button left at
                         * its old coordinates after a resize is off-window or floating
                         * in the text -- and it would still be clickable there. */
                        if (okBtn != NULL) MoveControl(okBtn, okR.left, okR.top);
                        gHelpPagePx = (short)(view.bottom - view.top - kHelpArrowPx);
                        if (gHelpPagePx < kHelpArrowPx) gHelpPagePx = kHelpArrowPx;
                        maxScroll = HelpMaxScroll(te, &view);
                        /* ⚠⚠ CLAMPING THE VALUE MUST ALSO MOVE THE TEXT. Making the
                         * window taller shows more text, so the maximum drops; the old
                         * code lowered the scroll bar's value to match and left the
                         * text where it was, after which the thumb and the text
                         * disagreed for the rest of the session and every later scroll
                         * inherited the error. Scroll by exactly what was clamped. */
                        if (sb != NULL) {
                            short cur = GetControlValue(sb);
                            if (cur > maxScroll) {
                                SetControlValue(sb, maxScroll);
                                TEScroll(0, (short)(cur - maxScroll), te);
                            }
                            SetControlMaximum(sb, maxScroll);
                        }
                        InvalRect(&w->portRect);
                    }
                } else if (part == inContent) {
                    Point pt = e.where;
                    ControlHandle c;
                    SetPort(w); GlobalToLocal(&pt);
                    if (FindControl(pt, w, &c) && c == okBtn && okBtn != NULL) {
                        if (TrackControl(okBtn, pt, NULL)) go = false;
                    } else if (FindControl(pt, w, &c) && c == sb) {
                        /* ⚠ The THUMB takes a NULL action proc and the ARROWS and PAGE
                         * areas take the repeating one -- they are genuinely different
                         * gestures. Passing the action proc for a thumb drag would call
                         * it continuously with part = thumb, which is not a direction. */
                        short which = TestControl(sb, pt);
                        if (which == kControlIndicatorPart) {
                            short before = GetControlValue(sb), after;
                            (void)TrackControl(sb, pt, NULL);
                            after = GetControlValue(sb);
                            if (after != before)
                                TEScroll(0, (short)(before - after), te);
                        } else {
                            (void)TrackControl(sb, pt, gHelpActionUPP);
                        }
                    }
                }
                break;
            }
            case activateEvt:
                /* ⚠ The About box needed this and so does this window: a default
                 * button that still looks live on a window that is not is a small
                 * lie, and a loop with no code for losing the foreground is how the
                 * About box crashed on click-away-and-back. */
                if ((WindowPtr)e.message == w) {
                    SetPort(w);
                    if (okBtn != NULL)
                        HiliteControl(okBtn,
                                      (e.modifiers & activeFlag) ? 0 : 255);
                }
                break;
            case keyDown: {
                char ch = (char)(e.message & charCodeMask);
                /* ⚠ OWN THE PORT rather than assuming nobody changed it. RedrawPanelWindow
                 * now restores it, but a handler that draws should not depend on that
                 * from a distance -- the same reasoning that moved the column titles
                 * next to the column widths. */
                SetPort(w);
                if (ch == 0x1B || ch == 13 || ch == 3) {  /* esc, return, enter */
                    /* Flash the default button so the key looks like the click it
                     * stands in for -- the same courtesy the About box does. */
                    if (okBtn != NULL) {
                        HiliteControl(okBtn, kControlButtonPart);
                        Delay(6, NULL);
                        HiliteControl(okBtn, 0);
                    }
                    go = false;
                }
                else if ((e.modifiers & cmdKey) && (ch == 'w' || ch == 'W')) go = false;
                break;
            }
            }
        }
    }
    SetMenusForModal(false);
    gHelpTE = NULL;
    if (gHelpActionUPP != NULL) {
        DisposeControlActionUPP(gHelpActionUPP);
        gHelpActionUPP = NULL;
    }
    TEDispose(te);
    DisposeWindow(w);
}

/* ---- About box ------------------------------------------------------------ *
 * A small window drawn by hand, dismissed by any click or key.
 *
 * ⚠ Deliberately NOT an ALRT/DITL. This project has already been bitten by DITL
 * UserItems with NULL handles swallowing dialog clicks
 * [[feedback_os9_null_useritem_breaks_clicks]], and an About box is not worth
 * re-entering that territory for. */
/* ★★★★ THE MENU BAR DURING A MODAL WINDOW.
 *
 * ⚠⚠ THE BUG THIS FIXES HAD TWO FACES AND ONE CAUSE. The About box's loop saw
 * inMenuBar and simply `break`ed -- so clicking the menu bar did nothing visible,
 * AND LEFT THE MENU MANAGER MID-TRACK, after which the window could not be dragged
 * either. A click in the menu bar is not something an application may decline: the
 * Menu Manager has already begun an interaction and MenuSelect is what finishes it.
 * Swallowing the event leaves that interaction open and the next click lands in a
 * state nothing is expecting -- exactly the "then dragging stops working" report.
 *
 * ⇒ MenuSelect is now ALWAYS called for a menu-bar click, and the menus are greyed
 * while the modal window is up, so the result is honestly nothing rather than
 * mysteriously nothing. That is what every OS 9 application does under a modal
 * window, and it is also the visible cue that the state is deliberate. */
static void SetMenusForModal(Boolean modal)
{
    MenuHandle m;
    /* ⚠ THE APPLE MENU STAYS ENABLED, and the selection is still DISCARDED. Greying
     * it would make the menu bar look dead; enabling it keeps the bar looking normal
     * and lets the title highlight and drop as usual.
     *
     * ⚠ It does NOT open desk accessories, and that is deliberate rather than an
     * omission. Both modal loops take a restricted event mask and answer keyDown
     * themselves -- About closes on any key, Help scrolls -- so a DA opened from here
     * would appear and then never receive a keystroke. A DA you can see but cannot
     * type into is worse than one that does not open
     * [[feedback_no_partial_fixes_that_reproduce_the_symptom]]. The modal window is
     * one OK button away; the DA is one click after that. */
    /* ⚠⚠ THE EDIT MENU IS NOT TOUCHED, AND THAT IS THE WHOLE POINT. SetUpMenus
     * disables it PERMANENTLY (DisableItem(m, 0) right after it is built) because the
     * panel has no editable text and its items would otherwise pretend. A symmetric
     * "grey on the way in, un-grey on the way out" would therefore have ENABLED a menu
     * that was never meant to be enabled: open the About box once and Edit turns black
     * for the rest of the session, with Undo/Cut/Copy/Paste selectable and inert.
     *
     * ⇒ Only the menu that is genuinely enabled outside a modal gets toggled. Restoring
     * state means restoring the state that was there, not the state a matching pair of
     * calls happens to produce. */
    if ((m = GetMenuHandle(kMFile)) != NULL) { if (modal) DisableItem(m, 0); else EnableItem(m, 0); }
    DrawMenuBar();
}

/* ★★★★ REDRAW THE MAIN PANEL, callable from a modal loop.
 *
 * ⚠⚠ THIS IS THE FIX FOR "dragging the About window leaves a trail". A modal loop
 * that answers only ITS OWN updateEvt swallows everyone else's: DragWindow correctly
 * invalidates the area the window used to cover, the panel behind dutifully gets an
 * update event -- and our loop dropped it on the floor, so nothing repainted until the
 * modal window went away and the main loop got its events back.
 *
 * ⚠ Other APPLICATIONS' windows repaint themselves, because WaitNextEvent keeps giving
 * them time. It is only our own second window that had nobody to draw it. */
static void RedrawPanelWindow(void)
{
    /* ⚠⚠ RESTORE THE CALLER'S PORT. This is called FROM the modal loops, and it used to
     * leave the port set to gWin with gWin's background painted platinum. The next
     * thing the Help loop did without a SetPort of its own -- a TEScroll, a
     * HiliteControl on the OK button -- would then have drawn into the PANEL. A helper
     * that other event loops call must not change the world behind their backs. */
    GrafPtr savePort;
    if (gWin == NULL) return;
    GetPort(&savePort);
    BeginUpdate(gWin);
    SetPort(gWin);
    ErasePlatinum(&gWin->portRect);
    DrawFrame();
    UpdateControls(gWin, gWin->visRgn);
    if (gList != NULL) {
        LUpdate(gWin->visRgn, gList);
        DrawColumnRules();
    }
    EndUpdate(gWin);
    SetPort(savePort);
}

/* ---- About box: the drawing, separate from the running ---------------------
 *
 * ⚠⚠ THIS SPLIT IS THE FIX FOR TWO REPORTED BUGS AT ONCE. v12.2 drew the whole box
 * ONCE, before its event loop, and answered updateEvt with a bare
 * BeginUpdate/EndUpdate pair commented "content is static". That VALIDATES the
 * update region without drawing anything -- so the moment another window covered
 * the About box and went away, the exposed area stayed blank, exactly as reported.
 * "Static content" describes the TEXT, not the PIXELS: everything QuickDraw draws
 * has to be drawable again on demand.
 *
 * ⇒ Everything now lives in one function the update handler calls, which is the
 * only arrangement that cannot drift out of step with itself. */
/* ---- RENAME, v15.3 ----------------------------------------------------------
 *
 * A small movable-modal window built by hand, for the reason the About box gives:
 * this project has already lost clicks to a DITL UserItem with a NULL handle, and a
 * one-field prompt is not worth re-entering that territory for.
 *
 * ⚠ TEIdle IS NOT OPTIONAL. Without it the insertion point never blinks, and a text
 * field with a frozen caret reads as a disabled control -- the user tries to type,
 * sees nothing move, and concludes the dialog is broken.
 */
/* ⚠ The FRAME, not the text rect. DrawThemeEditTextFrame draws the Platinum sunken
 * border around the rect it is handed, and the focus ring goes outside that again, so
 * the TextEdit rect must sit INSIDE this one or the border clips the first column of
 * text and the caret at the left margin. */
static Rect NicknameFieldFrame(short wide)
{
    Rect r;
    SetRect(&r, 16, 38, (short)(wide - 16), 60);
    return r;
}

static void DrawNicknameContent(WindowPtr w, TEHandle te)
{
    Rect    r = w->portRect, frame;
    Str255  t;
    IconRef icon = NULL;

    SetPort(w);
    /* ★★★★★ PLATINUM, DRAWN BY THE APPEARANCE MANAGER RATHER THAN BY US. The first
     * version framed the field with FrameRect and left the window white -- which is
     * exactly what makes a dialog look hand-made next to the Chooser's. Three calls do
     * the whole job, and they track the user's theme instead of hard-coding one. */
    EraseRect(&r);

    /* ⚠⚠ NOT BOLD, AND THAT WAS A REAL ARTEFACT RATHER THAN A STYLE CHOICE. Charcoal
     * has no bold face at 12pt, so TextFace(bold) makes QuickDraw SYNTHESISE one by
     * drawing every glyph twice, one pixel apart. On screen that reads as smudged or
     * doubled rather than as emphasis -- which is exactly what the user described. Plain
     * is also what Platinum specifies for a dialog's prompt. */
    TextFont(systemFont); TextSize(12);
    TextFace(normal);
    MoveTo(16, 26);
    PStr(t, "Name for this device:");
    DrawString(t);

    frame = NicknameFieldFrame((short)(r.right - r.left));
    (void)DrawThemeEditTextFrame(&frame, kThemeStateActive);
    /* ⭐ THE FOCUS RING is the single most Platinum thing on the Chooser's dialog, and
     * it is honest here: this field is the only thing in the window that takes typing,
     * so it always has the focus. */
    (void)DrawThemeFocusRect(&frame, true);
    if (te != NULL) TEUpdate(&(*te)->viewRect, te);

    /* ⭐ A NOTE ICON, NOT A CAUTION ONE. The Chooser shows a caution triangle because
     * it is warning that something will FAIL. This line tells the user how to undo a
     * rename; presenting that as a warning would be borrowing alarm the message does
     * not carry. Icon Services rather than a resource of our own: the system already
     * has these and they follow the OS's own appearance. */
    if (GetIconRef(kOnSystemDisk, kSystemIconsCreator, kAlertNoteIcon, &icon) == noErr
        && icon != NULL) {
        Rect ir;
        SetRect(&ir, 16, 76, 48, 108);
        (void)PlotIconRef(&ir, kAlignNone, kTransformNone, kIconServicesNormalUsageFlag,
                          icon);
        (void)ReleaseIconRef(icon);
    }

    /* ⚠ TWO LINES, WRAPPED BY HAND. The one-line version ran under both buttons -- the
     * fault the user reported -- and StringWidth on a 12pt system font is not something
     * to guess at across themes, so the break is explicit. */
    MoveTo(60, 90);
    PStr(t, "Leave this empty to go back to the");
    DrawString(t);
    MoveTo(60, 104);
    PStr(t, "device's default name.");
    DrawString(t);

    DrawControls(w);
}

/* true = the user accepted. outName is a C string of at most kAlChars-1 characters. */
static Boolean AskForNickname(const char *current, char *outName)
{
    WindowPtr     w;
    ControlHandle okBtn = NULL, cancelBtn = NULL;
    TEHandle      te;
    Rect          r, fld, frame;
    EventRecord   e;
    Boolean       go = true, accepted = false;
    /* ⚠⚠ 156 TALL, WAS 116, AND THAT 40px IS THE BUG THE USER REPORTED. The buttons sat
     * at y 86..106 and the explanatory line was drawn at y 92 -- straight through them.
     * The layout below gives every element its own band and is measured against the
     * Chooser's: prompt 26, field 38..60, icon and note 76..108, buttons 124..144. */
    short         wide = 372, high = 156;

    SetRect(&r,
            (qd.screenBits.bounds.right  - wide) / 2,
            (qd.screenBits.bounds.bottom - high) / 2,
            (qd.screenBits.bounds.right  + wide) / 2,
            (qd.screenBits.bounds.bottom + high) / 2);
    w = NewCWindow(NULL, &r, "\pRename Device", true, movableDBoxProc,
                   (WindowPtr)-1L, false, 0);
    if (w == NULL) return false;
    SetPort(w);
    TextFont(systemFont); TextSize(12);

    /* ⭐ Platinum grey, so the window matches every other dialog on the system. Set
     * BEFORE the first erase or the first frame drawn is white. */
    (void)SetThemeWindowBackground(w, kThemeBrushDialogBackgroundActive, false);

    frame = NicknameFieldFrame(wide);
    /* ⚠ INSET BY 3: the theme frame is drawn ON the rect above, so text starting at its
     * edge would be clipped by the border. */
    SetRect(&fld, (short)(frame.left + 3), (short)(frame.top + 3),
                  (short)(frame.right - 3), (short)(frame.bottom - 3));
    te = TENew(&fld, &fld);
    if (te == NULL) { DisposeWindow(w); return false; }
    TEAutoView(true, te);
    if (current != NULL && current[0] != 0) {
        short n = 0;
        while (current[n] != 0 && n < kAlChars - 1) n++;
        TEInsert((Ptr)current, (long)n, te);
    }
    TESetSelect(0, 32767, te);        /* ⭐ preselected, so typing replaces it */
    TEActivate(te);

    /* ⚠ BOTTOM RIGHT, Cancel then OK, the Platinum order the Chooser uses. */
    SetRect(&r, (short)(wide - 90), (short)(high - 32), (short)(wide - 16),
               (short)(high - 12));
    { Str255 t; PStr(t, "OK");
      okBtn = NewControl(w, &r, t, true, 0, 0, 1, pushButProc, 0); }
    if (okBtn != NULL) {
        Boolean isDef = true;
        (void)SetControlData(okBtn, kControlEntireControl,
                             kControlPushButtonDefaultTag, sizeof(isDef), (Ptr)&isDef);
    }
    SetRect(&r, (short)(wide - 176), (short)(high - 32), (short)(wide - 102),
               (short)(high - 12));
    { Str255 t; PStr(t, "Cancel");
      cancelBtn = NewControl(w, &r, t, true, 0, 0, 1, pushButProc, 0); }

    SetMenusForModal(true);
    DrawNicknameContent(w, te);

    while (go) {
        TEIdle(te);                    /* ⚠ see the note above: the caret must blink */
        if (WaitNextEvent(mDownMask | keyDownMask | autoKeyMask | updateMask | activMask,
                          &e, 6, NULL)) {
            switch (e.what) {
            case updateEvt:
                if ((WindowPtr)e.message == w) {
                    BeginUpdate(w);
                    DrawNicknameContent(w, te);
                    EndUpdate(w);
                } else if ((WindowPtr)e.message == gWin) {
                    RedrawPanelWindow();
                }
                break;
            case activateEvt:
                if ((WindowPtr)e.message == w) {
                    SetPort(w);
                    if (e.modifiers & activeFlag) TEActivate(te);
                    else                          TEDeactivate(te);
                    if (okBtn     != NULL) HiliteControl(okBtn,     (e.modifiers & activeFlag) ? 0 : 255);
                    if (cancelBtn != NULL) HiliteControl(cancelBtn, (e.modifiers & activeFlag) ? 0 : 255);
                }
                break;
            case keyDown:
            case autoKey: {
                char ch = (char)(e.message & charCodeMask);
                /* ⚠ Cmd-. and Escape both cancel; Return and Enter both accept. Those
                 * are the four keys OS 9 users expect a modal to honour, and a dialog
                 * that ignores Escape is the one people complain about. */
                if ((e.modifiers & cmdKey) && (ch == '.')) { go = false; break; }
                if (ch == 0x1B)                            { go = false; break; }
                if (ch == 0x0D || ch == 0x03) { accepted = true; go = false; break; }
                TEKey(ch, te);
                break;
            }
            case mouseDown: {
                WindowPtr hit  = NULL;
                short     part = FindWindow(e.where, &hit);
                /* ⚠⚠ A MENU-BAR CLICK MUST BE FINISHED, NOT DROPPED -- the About box
                 * learned this the expensive way: swallowing it leaves the Menu Manager
                 * mid-track and the window can no longer be dragged either. */
                if (part == inMenuBar) { (void)MenuSelect(e.where); HiliteMenu(0); break; }
                if (part == inSysWindow) { SystemClick(&e, hit); break; }
                if (hit != w) break;
                if (part == inDrag) { DragWindow(w, e.where, &qd.screenBits.bounds); break; }
                if (part == inContent) {
                    Point p = e.where;
                    ControlHandle c = NULL;
                    SetPort(w);
                    GlobalToLocal(&p);
                    if (FindControl(p, w, &c) != 0 && c != NULL) {
                        if (TrackControl(c, p, NULL) != 0) {
                            if (c == okBtn) { accepted = true; go = false; }
                            else if (c == cancelBtn) { go = false; }
                        }
                        break;
                    }
                    if (PtInRect(p, &fld))
                        TEClick(p, (e.modifiers & shiftKey) != 0, te);
                }
                break;
            }
            }
        }
    }

    if (accepted) {
        CharsHandle h = TEGetText(te);
        long        n = (*te)->teLength;
        long        i;
        if (n > (long)(kAlChars - 1)) n = (long)(kAlChars - 1);
        for (i = 0; i < n; i++) outName[i] = (*h)[i];
        outName[n] = 0;
        /* ⚠ TRIM BOTH ENDS. A name of spaces is indistinguishable from no name on
         * screen, so it must be treated as no name -- otherwise the row shows blank
         * and the only way back is to guess that Rename with an empty field fixes it. */
        { long a = 0, b = n;
          while (a < b && outName[a] == ' ') a++;
          while (b > a && outName[b - 1] == ' ') b--;
          for (i = 0; i < b - a; i++) outName[i] = outName[a + i];
          outName[b - a] = 0; }
    }

    TEDispose(te);
    SetMenusForModal(false);
    DisposeWindow(w);
    if (gWin != NULL) { SetPort(gWin); RedrawPanelWindow(); }
    return accepted;
}

static void DrawAboutContent(WindowPtr w, ControlHandle okBtn)
{
    Rect        box, ir, tr;
    Str255      t;
    const char *desc =
        "Bluetooth support for Mac OS 9. Pair an Apple Wireless Keyboard or Mouse "
        "and use it directly, with no Mac OS X required. Pairings are stored in the "
        "Bluetooth module itself, so a paired keyboard works from start-up.";

    SetPort(w);
    ErasePlatinum(&w->portRect);

    /* ★ APPLE'S ALIGNMENT, from the About AirPort reference the user pointed back to:
     * the icon sits OUTSIDE the text column on the far left, and the product name
     * shares its left edge with the framed box below it. v12.2 had the box starting at
     * the icon's margin instead, so the title looked indented rather than aligned. */
    SetRect(&ir, 18, 16, 50, 48);
    if (PlotIconID(&ir, atNone, ttNone, kIconRadioOn) != noErr)
        FrameRect(&ir);

    /* ⚠ 20pt, not 18. AirPort's title is visibly larger than the body and 18 read as
     * undersized beside it -- the user compared them directly. */
    /* ⚠ 24pt title over a 12pt version line, both Charcoal. The user measured ours
     * against AirPort's twice: 18 was undersized, 20 still was, and the version line
     * at 9 was much too small beside it. AirPort's title reads at roughly double its
     * version line, which is what these two numbers are. */
    /* ⚠ NOT bold. AirPort's name and version lines are both plain Charcoal -- the
     * size carries the emphasis, and bolding on top of 24pt was the last thing making
     * ours read differently from the reference. */
    TextFont(systemFont); TextSize(24); TextFace(0);
    MoveTo(kAboutTextL, 42);  PStr(t, "Bluetooth"); DrawString(t);

    TextSize(12);
    {
        /* ⚠⚠ THE VERSION IS SLICED OUT OF THE STRING scripts/bump-version.py REWRITES,
         * never written a second time. Rewriting this function once already deleted
         * that string and the stamper refused to write -- the guard working. Two copies
         * of a version number is a lie waiting for the next release. */
        static const char *kPanelVer = "Control panel 19.2 - device scanning";
        char v[24]; short i = 0;
        const char *q = kPanelVer + 14;          /* past "Control panel " */
        v[i++] = 'V'; v[i++] = 'e'; v[i++] = 'r'; v[i++] = 's';
        v[i++] = 'i'; v[i++] = 'o'; v[i++] = 'n'; v[i++] = ' ';
        while (*q != ' ' && *q != 0 && i < 22) v[i++] = *q++;
        v[i] = 0;
        MoveTo(kAboutTextL, 62); PStr(t, v); DrawString(t);
    }

    /* ---- the framed description well ---------------------------------------- */
    /* ⚠ 74, not 68: the 24pt title and 12pt version below it need the room, and a
     * box that starts too high clips the version's descenders. */
    SetRect(&box, kAboutTextL, 74, 420, 234);
    /* ⚠⚠ THE BACKGROUND STAYS WHITE UNTIL EVERY STRING IN THE WELL IS DRAWN, and
     * that is the fix for the grey blocks the user saw behind the body text. The well
     * was erased to white and the Platinum grey restored IMMEDIATELY -- so DrawString
     * and TETextBox, which paint their own background behind each glyph run, stamped
     * grey rectangles back into the white box. Erasing a region white does not make
     * text drawn into it later transparent. */
    { RGBColor saveBk; GetBackColor(&saveBk);
      BackColor(whiteColor);
      EraseRect(&box);
      FrameRect(&box);

    /* ⚠ 10pt, not 9. Side by side with About AirPort the body read visibly cramped --
     * Apple sets these boxes in Geneva 10 and the leading follows the size, so one
     * change fixes both the size and the tightness the user saw. The credits at the
     * bottom of the well stay at 9, which is also what AirPort does. */
    /* ⚠ NO LEAD LINE. It read "OS 9 Bluetooth" directly beneath a 24pt title saying
     * "Bluetooth", which is the same thing twice -- the user's call and the right one.
     * AirPort's "AirPort (TM)" is its trademark notice rather than a heading, and we
     * have no trademark to notice.
     * ⇒ The body reclaims the 18px the heading occupied; leaving the offset behind
     * would have left an unexplained gap at the top of the well. */
    TextFont(applFont); TextSize(10); TextFace(0);
    tr = box; InsetRect(&tr, 10, 10);
    TETextBox((Ptr)desc, (long)strlen(desc), &tr, teFlushDefault);

    /* The driver build, where Apple puts its credits, and the one live fact here.
     * ⚠ Back to 9: AirPort's credit lines are smaller than its body text. */
    TextSize(9);
    MoveTo(box.left + 10, box.bottom - 26);
    if (gBlock != NULL) {
        /* ★★★★ DECODE THE BUILD TAG. DO NOT PRINT IT RAW.
         *
         * ⚠⚠ THIS LINE WAS ALREADY LIVE AND ALREADY CORRECT -- it reads the tag out of
         * the counter block inside the DRIVER THAT IS ACTUALLY LOADED, so it has always
         * named the installed extension rather than the one this panel was built
         * beside. What it lacked was a reader. "vD10" looks like a build code nobody
         * can act on, and it looks FROZEN, because the tag only moves when the driver
         * moves and every release for a while has been panel-only. Both readings were
         * wrong, and both came from printing four bytes of an encoding instead of the
         * number they encode.
         *
         * ⭐ THE ENCODING, from scripts/bump-version.py: 'v', major, minor, '0'. The
         * major has exactly ONE BYTE, so 9.9 was the ceiling and majors from 10 up
         * continue into the letters -- 10='A' ... 35='Z' -- which also keeps the tags
         * byte-comparable across the boundary ('9' is 0x39, 'A' is 0x41). So 'vD10' is
         * major 'D' = 13, minor 1 = DRIVER 13.1.
         *
         * ⚠ A tag that does not fit the pattern prints as its four raw bytes rather
         * than as a guess. An unreadable build code is a poor answer; a confidently
         * wrong version number is a worse one. */
        unsigned long v   = gBlock[kWBuild];
        short         ver = DriverVerBCD();     /* ⚠ the one decoder -- see its note */
        char          s[24];
        short         i = 0;

        if (ver > 0) {
            short maj = (short)(ver / 100), min = (short)(ver % 100);
            if (maj >= 10) s[i++] = (char)('0' + maj / 10);
            s[i++] = (char)('0' + maj % 10);
            s[i++] = '.';
            s[i++] = (char)('0' + min);
            s[i]   = 0;
            PStr(t, "Driver extension: version "); DrawString(t);
        } else {
            /* ⚠ A tag that does not fit the pattern prints as its four raw bytes rather
             * than as a guess. An unreadable build code is a poor answer; a confidently
             * wrong version number is a worse one. */
            s[0] = (char)((v >> 24) & 0xFF); s[1] = (char)((v >> 16) & 0xFF);
            s[2] = (char)((v >>  8) & 0xFF); s[3] = (char)(v & 0xFF); s[4] = 0;
            PStr(t, "Driver extension: "); DrawString(t);
        }
        PStr(t, s); DrawString(t);
    } else {
        PStr(t, "Driver extension: not loaded"); DrawString(t);
    }
    MoveTo(box.left + 10, box.bottom - 12);
    PStr(t, "github.com/UnexpectedBomb"); DrawString(t);
      RGBBackColor(&saveBk);   /* ⚠ only NOW, with the well finished */
    }

    /* ⚠ applFont 10, not systemFont 9: AirPort's copyright line is in the same face
     * and size as its body text, not in the system font. */
    TextFont(applFont); TextSize(10); TextFace(0);
    MoveTo(kAboutTextL, 254);
    /* ⚠ NO PERIOD AFTER THE YEAR. It was there because "Free software, no warranty."
     * was a second sentence; with a name in that slot the canonical form is
     * "Copyright © <year> <holder>", which is also how AirPort's line reads. */
    PStr(t, "Copyright \251 2026 UnexpectedBomb"); DrawString(t);

    /* ⚠ The controls last, and via Draw1Control rather than by hand: the Appearance
     * Manager owns the default ring, and anything we drew ourselves would be painted
     * over by the next update anyway. */
    if (okBtn != NULL) Draw1Control(okBtn);
}

static void ShowAbout(void)
{
    Rect          r, ok;
    WindowPtr     w;
    EventRecord   e;
    ControlHandle okBtn = NULL;
    Boolean       go = true;

    SetRect(&r, 0, 0, 440, 292);
    OffsetRect(&r, (qd.screenBits.bounds.right  - 440) / 2,
                   (qd.screenBits.bounds.bottom - 292) / 3);
    w = NewCWindow(NULL, &r, "\pAbout Bluetooth", true, movableDBoxProc,
                   (WindowPtr)-1L, false, 0);
    if (w == NULL) return;
    SetPort(w);

    SetRect(&ok, 350, 258, 420, 278);
    { Str255 t; PStr(t, "OK");
      okBtn = NewControl(w, &ok, t, true, 0, 0, 1, pushButProc, 0); }
    if (okBtn != NULL) {
        Boolean isDef = true;
        (void)SetControlData(okBtn, kControlEntireControl,
                             kControlPushButtonDefaultTag,
                             sizeof(isDef), (Ptr)&isDef);
    }
    SetMenusForModal(true);
    DrawAboutContent(w, okBtn);

    /* ⚠⚠ updateMask AND activMask, and both are handled. v12.2 asked for
     * mDownMask|keyDownMask|updateMask and then did nothing useful with the update --
     * and never saw an activate at all. Clicking away to the Finder and back left this
     * window in a state it had no code to recover from, which is the crash that was
     * reported. A window that can lose the foreground must handle getting it back. */
    while (go) {
        if (WaitNextEvent(mDownMask | keyDownMask | updateMask | activMask,
                          &e, 10, NULL)) {
            switch (e.what) {
            case updateEvt:
                if ((WindowPtr)e.message == w) {
                    BeginUpdate(w);
                    DrawAboutContent(w, okBtn);
                    EndUpdate(w);
                } else if ((WindowPtr)e.message == gWin) {
                    RedrawPanelWindow();   /* ⚠ or dragging leaves a trail */
                }
                break;
            case activateEvt:
                if ((WindowPtr)e.message == w) {
                    SetPort(w);
                    if (okBtn != NULL)
                        HiliteControl(okBtn,
                                      (e.modifiers & activeFlag) ? 0 : 255);
                }
                break;
            case mouseDown: {
                WindowPtr hit = NULL;
                short     part = FindWindow(e.where, &hit);
                /* ⚠ NO BEEP. This is a MOVABLE modal: the user is allowed to click
                 * away and come back, and scolding them for using their own machine
                 * is noise -- the user reported the alert sound as unwanted and they
                 * are right. Clicks elsewhere are simply not ours to act on. */
                /* ⚠⚠ A MENU-BAR CLICK MUST BE FINISHED, NOT DROPPED -- see
                 * SetMenusForModal. MenuSelect completes the interaction the Menu
                 * Manager has already started; HiliteMenu(0) un-highlights the title
                 * afterwards. The menus are greyed, so the selection is 0 and nothing
                 * happens, which is the intent. */
                if (part == inMenuBar) {
                    (void)MenuSelect(e.where);
                    HiliteMenu(0);
                    break;
                }
                /* ⚠ Desk accessories and other system windows get their own clicks. */
                if (part == inSysWindow) { SystemClick(&e, hit); break; }
                if (hit != w) break;
                if (part == inDrag) {
                    DragWindow(w, e.where, &qd.screenBits.bounds);
                } else if (part == inContent) {
                    Point pt = e.where;
                    ControlHandle c = NULL;
                    SetPort(w); GlobalToLocal(&pt);
                    /* ⭐ TrackControl, so the button VISIBLY depresses and so a click
                     * that wanders off it does nothing -- v12.2 closed the window on
                     * any content click at all, which is why there was no feedback: the
                     * button was never actually pressed, the window just vanished. */
                    if (FindControl(pt, w, &c) && c == okBtn) {
                        if (TrackControl(c, pt, NULL) != 0) go = false;
                    }
                }
                break;
            }
            case keyDown: {
                char ch = (char)(e.message & charCodeMask);
                if (ch == 13 || ch == 3 || ch == 0x1B) {
                    /* Flash the default button so the key looks like the click it
                     * stands in for. */
                    if (okBtn != NULL) {
                        HiliteControl(okBtn, kControlButtonPart);
                        Delay(6, NULL);
                        HiliteControl(okBtn, 0);
                    }
                    go = false;
                }
                break;
            }
            }
        }
    }
    SetMenusForModal(false);
    DisposeWindow(w);
    SetPort(gWin);
    InvalRect(&gWin->portRect);
}

/* Returns true if the application should quit. */
static Boolean DoMenu(long sel)
{
    short menu = HiWord(sel), item = LoWord(sel);

    /* ★★★★ THE SYSTEM HELP MENU. ⚠ Matched on the REMEMBERED item number, never a
     * literal: the Help Manager owns the items above ours and their count varies by
     * system version and by what else is installed. gHelpItem is 0 if we never got a
     * menu, and 0 never matches a real item, so an absent Help Manager is simply
     * inert rather than a crash. */
    if (menu == kHMHelpMenuID) {
        if (gHelpItem != 0 && item == gHelpItem) ShowHelp();
        HiliteMenu(0);
        return false;
    }

    switch (menu) {
    case kMApple:
        if (item == kAboutItem) { ShowAbout(); }
        else {
            /* A desk accessory. */
            Str255 nm;
            GetMenuItemText(GetMenuHandle(kMApple), item, nm);
            (void)OpenDeskAcc(nm);
        }
        break;
    case kMFile:
        if (item == kQuitItem) { HiliteMenu(0); return true; }
        /* ★★★★★★ ASK FOR PAIRING MODE ON THE NEXT RESTART.
         *
         * Switcher 1.2 is idle at boot -- it declines the card so OS 9's own HID driver
         * keeps it and a paired keyboard types from startup. This writes the one-shot
         * flag it looks for, so the NEXT boot claims the card and switches it to HCI.
         *
         * ⚠ A restart is genuinely required and the alert says so plainly rather than
         * implying something happens now. The card has to be switched BEFORE anything
         * else binds it, and by the time this panel is running OS 9's HID driver
         * already owns it -- so there is no way to enter pairing mode in this session.
         *
         * ⚠ The panel only WRITES the flag; the switcher consumes and deletes it. If
         * the user changes their mind, picking this again is harmless (the file is
         * simply rewritten) and a boot that fails to switch leaves no flag behind. */
        if (item == kPairModeItem) {
            AlertStdAlertParamRec p;
            SInt16 hit = 0;
            Str255 msg, expl;
            short  ref;

            HiliteMenu(0);
            PStr(msg, "Turn on Bluetooth pairing at the next restart?");
            PStr(expl, "Your Mac needs to restart before you can set up a new Bluetooth "
                       "device. After restarting, open this panel and use Pair. When you "
                       "are finished, choose Hand Bluetooth Back to Mac OS and your "
                       "keyboard and mouse will work again.");
            p.movable       = false;
            p.helpButton    = false;
            p.filterProc    = NULL;
            p.defaultText   = (ConstStringPtr)kAlertDefaultOKText;
            p.cancelText    = (ConstStringPtr)kAlertDefaultCancelText;
            p.otherText     = NULL;
            p.defaultButton = kAlertStdAlertOKButton;
            p.cancelButton  = kAlertStdAlertCancelButton;
            p.position      = kWindowDefaultPosition;
            gAlertRc  = StandardAlert(kAlertNoteAlert, msg, expl, &p, &hit);
            gAlertHit = hit;
            if (gAlertRc == noErr && hit == kAlertStdAlertOKButton) {
                gPairModeRc = fnfErr;
                ref = PairFlagCreate();
                gPairModeRc = (ref != 0) ? noErr : ioErr;
                gPairModeSet = true;
                /* ⚠ REPORT THE OUTCOME. A flag that silently failed to appear would
                 * look exactly like a switcher that ignored it, and the user would
                 * restart for nothing. */
                if (gPairModeRc == noErr) {
                    PStr(msg, "Pairing will be ready after you restart.");
                    PStr(expl, "Restart your Mac now, then open this panel again.");
                } else {
                    PStr(msg, "Could not turn on pairing.");
                    PStr(expl, "The setting could not be saved, so restarting will not "
                               "help. Check that there is room on the startup disk.");
                }
                p.cancelText    = NULL;
                p.cancelButton  = 0;
                p.defaultButton = kAlertStdAlertOKButton;
                (void)StandardAlert(gPairModeRc == noErr ? kAlertNoteAlert
                                                         : kAlertStopAlert,
                                    msg, expl, &p, &hit);
            }
            return false;
        }
        /* ★★★ Hand the card back to Mac OS: send the documented reverse mode switch.
         *
         * ⚠ CONFIRMED FIRST, because this ends Bluetooth for the rest of the session.
         * The card changes USB personality, the driver's device dies, and nothing can
         * be paired again until a restart. That is the POINT -- a proxied keyboard
         * starts working -- but it is not something to do to someone by accident from
         * a menu, and an alert is the honest way to say so. */
        if (item == kRestoreItem) {
            AlertStdAlertParamRec p;
            SInt16 hit = 0;
            Str255 msg, expl;

            HiliteMenu(0);
            /* ⚠ A ROW MUST BE SELECTED. The command restores the key for ONE address,
             * and guessing which would be exactly the "write a wrong key" mistake the
             * driver refuses to make. */
            if (gSelRow < 0 || !ComponentsOK()) {
                PStr(msg, "Select a device first.");
                PStr(expl, "Click the device in the list whose saved key you want to "
                           "put back on the card, then choose this command again.");
                p.movable = false; p.helpButton = false; p.filterProc = NULL;
                p.defaultText = (ConstStringPtr)kAlertDefaultOKText;
                p.cancelText = NULL; p.otherText = NULL;
                p.defaultButton = kAlertStdAlertOKButton;
                p.cancelButton = 0; p.position = kWindowDefaultPosition;
                (void)StandardAlert(kAlertNoteAlert, msg, expl, &p, &hit);
                return false;
            }
            PStr(msg, "Put the saved key back on the card?");
            PStr(expl, "This restores the pairing this device had before it was "
                       "deleted, using the key saved on this Mac. Use it if pairing "
                       "again did not work and you want the device back the way it "
                       "was. If no key was saved for this device, nothing is written.");
            p.movable       = false;
            p.helpButton    = false;
            p.filterProc    = NULL;
            p.defaultText   = (ConstStringPtr)kAlertDefaultOKText;
            p.cancelText    = (ConstStringPtr)kAlertDefaultCancelText;
            p.otherText     = NULL;
            p.defaultButton = kAlertStdAlertOKButton;
            p.cancelButton  = kAlertStdAlertCancelButton;
            p.position      = kWindowDefaultPosition;
            if (StandardAlert(kAlertCautionAlert, msg, expl, &p, &hit) == noErr
                && hit == kAlertStdAlertOKButton) {
                gLastRestoreRc = (gProbeMode != kProbeFull)
                               ? cfragNoSymbolErr
                               : MailboxSend(kCmdRestoreKey,
                                             gRow[gSelRow].addrHi,
                                             gRow[gSelRow].addrLo);
                gRestoreDone = true;
                /* ⚠⚠ SAY SOMETHING. gLastRestoreRc and gRestoreDone were set here and
                 * READ NOWHERE: the alert dismissed and the panel went on as if nothing
                 * had happened. The user clicked a recovery command and had no way to
                 * tell whether it had worked -- and the only place the answer existed
                 * was a log file. That is the dead Disconnect button's lesson again, in
                 * a command that matters more: Restore is what you reach for when
                 * something has already gone wrong. */
                {
                    Str255 m2, e2;
                    SInt16 h2 = 0;
                    AlertStdAlertParamRec p2;
                    if (gLastRestoreRc == noErr) {
                        PStr(m2, "Saved key sent to the module.");
                        PStr(e2, "The device should appear as paired again. If it does "
                                 "not reconnect on its own, switch it off and on.");
                    } else {
                        PStr(m2, "No saved key for this device.");
                        PStr(e2, "Nothing was written. This Mac has no stored key for "
                                 "the selected device, so there is nothing to put back. "
                                 "Pair it again instead.");
                    }
                    p2.movable = false; p2.helpButton = false; p2.filterProc = NULL;
                    p2.defaultText = (ConstStringPtr)kAlertDefaultOKText;
                    p2.cancelText = NULL; p2.otherText = NULL;
                    p2.defaultButton = kAlertStdAlertOKButton;
                    p2.cancelButton = 0; p2.position = kWindowDefaultPosition;
                    (void)StandardAlert(gLastRestoreRc == noErr ? kAlertNoteAlert
                                                                : kAlertCautionAlert,
                                        m2, e2, &p2, &h2);
                }
                gLastKeyCount = -1;     /* force the list to rebuild from the block */
            }
            return false;
        }

        if (item == kHandBackItem) {
            AlertStdAlertParamRec p;
            SInt16 hit = 0;
            Str255 msg, expl;

            HiliteMenu(0);
            if (!ComponentsOK()) {
                PStr(msg, "Bluetooth is not running.");
                PStr(expl, "There is nothing to hand back. The card is already being "
                           "used by Mac OS, or the Bluetooth software is not loaded.");
                p.movable = false; p.helpButton = false; p.filterProc = NULL;
                p.defaultText = (ConstStringPtr)kAlertDefaultOKText;
                p.cancelText = NULL; p.otherText = NULL;
                p.defaultButton = kAlertStdAlertOKButton;
                p.cancelButton = 0; p.position = kWindowDefaultPosition;
                (void)StandardAlert(kAlertNoteAlert, msg, expl, &p, &hit);
                return false;
            }
            PStr(msg, "Hand Bluetooth back to Mac OS?");
            PStr(expl, "Paired keyboards and mice will start working again, handled by "
                       "Mac OS itself. This Bluetooth software stops until you restart, "
                       "so you will not be able to pair anything else until then.");
            p.movable       = false;
            p.helpButton    = false;
            p.filterProc    = NULL;
            p.defaultText   = (ConstStringPtr)kAlertDefaultOKText;
            p.cancelText    = (ConstStringPtr)kAlertDefaultCancelText;
            p.otherText     = NULL;
            p.defaultButton = kAlertStdAlertOKButton;
            p.cancelButton  = kAlertStdAlertCancelButton;
            p.position      = kWindowDefaultPosition;
            gAlertRc  = StandardAlert(kAlertCautionAlert, msg, expl, &p, &hit);
            gAlertHit = hit;
            if (gAlertRc == noErr && hit == kAlertStdAlertOKButton) {
                /* ⚠ The result is the MAILBOX's, not the switch's. A timeout IS the
                 * success case for this request and it is reported by the driver's
                 * completion into kWSwVerdict, not returned here. So do not draw a
                 * verdict from this rc -- read the block. */
                gLastHandBackRc = MailboxSend(kCmdSwitchToProxy, 0, 0);
                gHandBackTried  = true;
            }
            return false;
        }
        break;
    default:
        break;
    }
    HiliteMenu(0);
    return false;
}

int main(void)
{
    Rect        bounds;
    EventRecord evt;
    Str255      title;
    short       part;
    WindowPtr   who;
    Boolean     done = false;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    /* ★ PLATINUM, from the Appearance Manager rather than hand-picked greys, so the
     * panel tracks whatever theme the user actually has instead of guessing. */
    if (RegisterAppearanceClient() != noErr) gThemeOK = false;

    /* ★ RESTORE THE LEARNED DEVICE KINDS BEFORE ANY ROW IS BUILT. Loaded here rather
     * than lazily so the FIRST list the user sees is already named -- the whole point
     * is that a paired phone should not read "Unknown" on launch. */
    CodLoad();

    FindDriverBlock();
    /* Only ask the bus if there is no driver -- if the block exists the driver is
     * loaded and the hardware question is already answered. */
    /* ⚠ Always ask the bus and the Extensions folder, not just when the driver is
     * missing. The four status lines report all three facts every time, and a line
     * that is only populated in the failure case is a line that will read "None"
     * when everything is working. */
    FindBluetoothHardware();
    FindExtensionFile();
    SetUpMenus();

    SetRect(&bounds, 0, 0, kWinW, kWinH);
    OffsetRect(&bounds,
               (qd.screenBits.bounds.right  - kWinW) / 2,
               (qd.screenBits.bounds.bottom - kWinH) / 2);
    PStr(title, "Bluetooth");
    /* ★★★★ NewCWindow, NOT NewWindow. THIS IS WHY PLATINUM FAILED THREE TIMES.
     *
     * MacWindows.h, lines 616 and 644:
     *     WindowRecord  { GrafPort  port; }   <- what NewWindow creates
     *     CWindowRecord { CGrafPort port; }   <- what NewCWindow creates
     *
     * NewWindow makes a basic black-and-white GrafPort. RGBBackColor,
     * SetThemeBackground, SetThemeWindowBackground and every theme brush have
     * NOWHERE TO PUT A COLOUR in one, and EraseRect fills with the B&W background
     * pattern -- white. Every fix attempted so far was at the wrong layer:
     *
     *   v0.2  SetThemeWindowBackground once at startup   -> white
     *   v0.4  SetThemeBackground before each erase       -> white (returned noErr)
     *   v0.6  GetThemeBrushAsColor + RGBBackColor        -> white
     *
     * ⭐ And it explains the asymmetry that should have pointed here two rounds ago:
     * DrawThemeListBoxFrame DID draw, because a hard black frame is expressible in
     * black and white, while the group boxes' subtle grey embossing is not. A
     * symptom that splits along "needs grey / does not need grey" was the port's
     * colour capability all along, not the calls I kept re-plumbing. */
    gWin = NewCWindow(NULL, &bounds, title, true, noGrowDocProc, (WindowPtr)-1L, true, 0);
    if (gWin == NULL) return 1;
    SetPort(gWin);

    /* ★ The timeline's zero. Every action and every bind transition is stamped in
     * seconds from here, so "the idle control phase" is literally the first N seconds
     * of the log and needs no separate marker. */
    gT0 = TickCount();
    (void)SetThemeWindowBackground(gWin, kThemeBrushDialogBackgroundActive, false);

    BuildList();
    MakeControls();

    while (!done) {
        Boolean got = WaitNextEvent(everyEvent, &evt, 10, NULL);

        /* ★ THE POLL. Runs on every pass, event or timeout, which is what makes the
         * list fill live as the driver publishes responders -- the 2003 panel's
         * behaviour reproduced without a callback (docs/SCAN-DESIGN.md §5).
         * ⚠ 10 ticks is WaitNextEvent's own sleep, so this costs nothing extra. */
        /* ★ THE TASK-LEVEL PULSE. Incremented on every pass whether or not anything
         * changed -- that is the whole point: if this stops advancing in the status
         * file, task level died; if it keeps advancing while the driver's counters
         * stand still, the driver died and we did not. */
        gPollCount++;
        StatusWrite(false);           /* rate-limited internally to ~15 s */

        /* ⚠⚠ THE ARROWS MUST BE ABLE TO STOP. A spinner whose only exit is a state the
         * driver may never reach is the UI form of a latching guard -- and it bit for
         * real: the Control-click probe started nothing, so the inquiry state never
         * moved and the arrows span past a minute until the user force-quit.
         *
         * An inquiry is 8 x 1.28s, about ten seconds. Thirty is generous and still
         * finite. On expiry the arrows stop and the pane says the scan never reported
         * completing, which is a fact rather than a guess about why. */
        /* ⚠⚠ TRIGGERED BY THE DRIVER'S STATE, NOT BY OUR HAVING ASKED.
         *
         * The first version keyed on gScanAsked, so a panel launched into a driver
         * ALREADY stuck at "scanning" never ran the watchdog at all -- which is exactly
         * run 58's report: quitting and relaunching did not re-enable the button.
         * gScanStartTick is stamped whenever state 1 is first OBSERVED, whoever
         * started it. */
        if (gLastScanState == 1 && gScanStartTick != 0 && !gScanStalled &&
            (TickCount() - gScanStartTick) > (30UL * 60UL)) {
            if (gArrows) HideControl(gArrows);
            gScanStalled = true;
            gScanAsked   = false;
            SyncButtons();
            SetPort(gWin);
            DrawPanes();
        }

        /* ★★ SAMPLE THE BIND COUNTER FIRST, every iteration, before anything else can
         * consume the loop. This is the churn instrument: a teardown is recorded
         * against the moment it was observed, not against whichever press happened to
         * be in flight. Unconditional and cheap -- one word compared. */
        SampleBinds();

        /* ★ The driver's radio state changed under us -- repaint the logo. Kept OUT of
         * the PollScan branch below and checked on its own, so it cannot be starved by
         * whatever else that branch happens to be doing, and so the order of the two is
         * irrelevant. Cleared unconditionally: a dirty flag that survives its own
         * handler is a latch, and this project has been bitten by two of those. */
        if (gRadioLogoDirty) {
            gRadioLogoDirty = false;
            SetPort(gWin);
            DrawRadioLogo();
        }

        /* ★ The components message, same shape and same reason as the logo above:
         * SyncButtons records that the state moved, the event loop is what has a port
         * and therefore what draws. With the switcher installed this fires in ordinary
         * use -- panel open, card still in proxy, then the switch lands and the driver
         * binds a moment later. */
        if (gCompMsgDirty) {
            gCompMsgDirty = false;
            SetPort(gWin);
            DrawComponentMsg();
        }

        if (PollScan()) {
            /* Inquiry finished: stop the arrows, exactly where the 2003 panel called
             * StopIdling(). gInqState 2 means complete.
             *
             * ⚠ NOT WHILE OUR REQUEST IS STILL PENDING. State 2 belongs to the
             * PREVIOUS inquiry until the driver's timer picks ours up, and hiding on
             * it is precisely what made plain clicks show no arrows in run 3. */
            if (gLastScanState == 2 && !ScanRequestPending() && gArrows)
                HideControl(gArrows);
            /* ⚠ The scan state gates Set Up New Device, so the buttons must follow the
             * poll. Without this the button stays grey after a scan finishes. */
            SyncButtons();
            SetPort(gWin);
            DrawPanes();
        }
        /* The chasing arrows animate from IdleControls, not on their own. */
        if (gArrows && (**gArrows).contrlVis) IdleControls(gWin);

        if (got) {
            switch (evt.what) {
            case mouseDown:
                part = FindWindow(evt.where, &who);
                if (part == inMenuBar) {
                    if (DoMenu(MenuSelect(evt.where))) done = true;
                } else if (part == inGoAway && who == gWin) {
                    if (TrackGoAway(gWin, evt.where)) done = true;
                } else if (part == inDrag && who == gWin) {
                    DragWindow(gWin, evt.where, &qd.screenBits.bounds);
                } else if (part == inContent && who == gWin) {
                    ControlHandle c;
                    Point pt = evt.where;
                    Rect  lr;
                    GlobalToLocal(&pt);

                    /* ★ SORTABLE HEADERS, the Extensions Manager behaviour: click a
                     * header to sort by that column, click it again to reverse. The
                     * active header draws pressed, so the sort key is never a guess. */
                    SetRect(&lr, kListL, (short)(kListT - 16), kListR, kListT);
                    if (PtInRect(pt, &lr)) {
                        /* ★★★ DIVIDER DRAG, tested BEFORE the sort click.
                         *
                         * A header click means one of two things depending on where in
                         * the header it lands, and the divider has to win: a 6px band
                         * either side of a boundary is a resize, everything else is a
                         * sort. Checking sort first would make the dividers
                         * unreachable, since every divide is also inside some column.
                         *
                         * ⚠ MINIMUM WIDTH. Without a floor a column can be dragged to
                         * zero and then can never be grabbed again -- the divider it
                         * would be grabbed by is underneath its neighbour. 34px keeps
                         * a header title partly visible, which is what makes it
                         * recoverable by eye. */
                        short d, hit = -1;
                        for (d = 1; d < kNumCols; d++) {
                            short x = ColLeft(d);
                            if (pt.h >= x - 6 && pt.h <= x + 6) { hit = d; break; }
                        }
                        if (hit > 0) {
                            short  startH = pt.h;
                            short  origW  = gColW[hit - 1];
                            short  lastX  = ColLeft(hit);
                            Point  now;
                            Rect   guide;

                            /* XOR guide line, drawn and undrawn at the same coords so
                             * it never leaves a trace on the window behind it. */
                            PenMode(patXor);
                            SetRect(&guide, lastX, kListT - 16, (short)(lastX + 1),
                                    kListB);
                            PaintRect(&guide);

                            while (StillDown()) {
                                short want, minW = 34, maxW;
                                GetMouse(&now);
                                want = (short)(origW + (now.h - startH));
                                /* ⚠⚠ THE CLAMP, AND MY FIRST VERSION WAS WRONG.
                                 *
                                 * It subtracted minW once per column to the RIGHT of
                                 * the divider and only the widths to the LEFT, which
                                 * let divider 1 reach 249px when the real ceiling is
                                 * 155 -- driving the last column's computed width
                                 * NEGATIVE. A brute-force check over 1200 simulated
                                 * drags found 536 violations before this shipped.
                                 *
                                 * Only ONE column is elastic: the last absorbs the
                                 * remainder. So the ceiling for the dragged column is
                                 * the table width, less the last column's floor, less
                                 * every OTHER explicit width -- whichever side of the
                                 * divider it sits on. */
                                maxW = (short)(kListR - 16 - kListL - 1) - minW;
                                { short i;
                                  for (i = 0; i < kNumCols - 1; i++)
                                      if (i != hit - 1) maxW -= gColW[i]; }
                                if (want < minW) want = minW;
                                if (want > maxW) want = maxW;
                                if (want != gColW[hit - 1]) {
                                    short newX;
                                    gColW[hit - 1] = want;
                                    newX = ColLeft(hit);
                                    if (newX != lastX) {
                                        PaintRect(&guide);      /* erase old */
                                        SetRect(&guide, newX, kListT - 16,
                                                (short)(newX + 1), kListB);
                                        PaintRect(&guide);      /* draw new */
                                        lastX = newX;
                                    }
                                }
                            }
                            PaintRect(&guide);                  /* erase the last */
                            PenMode(patCopy);

                            /* Header and every row have to be redrawn: the LDEF reads
                             * gColW at draw time, so nothing is stale -- it just has to
                             * be asked to draw again. */
                            InvalRect(&gWin->portRect);
                            break;
                        }

                        /* ⚠ Hit-test against the REAL column geometry, not a uniform
                         * width -- otherwise clicking "Status" would sort by "ID" the
                         * moment the columns stopped being equal. */
                        short col = kNumCols - 1, i;
                        for (i = 0; i < kNumCols; i++) {
                            if (pt.h < ColLeft(i) + ColWidth(i)) { col = i; break; }
                        }
                        if (col < 0) col = 0;
                        if (col == gSortCol) gSortDesc = !gSortDesc;
                        else { gSortCol = col; gSortDesc = false; }
                        /* ⚠ Re-sort invalidates the selection, which is an INDEX into
                         * gRow. Clearing it is the honest move: keeping the number
                         * would silently point the info pane at a different device. */
                        gSelRow = -1;
                        if (gList != NULL) { Cell z; z.h = 0; z.v = 0;
                                             LSetSelect(false, z, gList); }
                        SortRows();
                        RefreshList();
                        SyncButtons();
                        InvalRect(&gWin->portRect);
                        break;
                    }

                    SetRect(&lr, kListL, kListT, kListR, kListB);
                    if (PtInRect(pt, &lr) && gList != NULL) {
                        Cell sel;
                        (void)LClick(pt, evt.modifiers, gList);
                        /* ⚠ LClick redraws the affected rows itself, with no update
                         * event -- so the rules, which are drawn ON TOP of the rows,
                         * have to be put back or a freshly selected row loses its
                         * column segments and the table appears to break where you
                         * clicked. */
                        DrawColumnRules();
                        sel.h = 0; sel.v = 0;
                        gSelRow = LGetSelect(true, &sel, gList) ? sel.v : -1;
                        SyncButtons();
                        /* Repaint only the description pane. */
                        DrawPanes();
                    } else if (FindControl(pt, gWin, &c) && TrackControl(c, pt, NULL)) {
                        /* ★★ REAL NOW. These moved and did nothing for five versions.
                         *
                         * ⚠ The control follows the DRIVER, not the click: the radio
                         * is only redrawn once the driver has accepted the change. A
                         * button that moves on click and then silently fails is the
                         * same defect as the scan button that looked inert while
                         * working -- it teaches the user to distrust the panel. */
                        if (c == gOnRadio || c == gOffRadio) {
                            Boolean want = (c == gOnRadio);
                            gLastRadioRc = DriverSetRadio(want);
                            if (gLastRadioRc == noErr) {
                                gRadioOn = want;
                                SetControlValue(gOnRadio,  want ? 1 : 0);
                                SetControlValue(gOffRadio, want ? 0 : 1);
                            } else {
                                /* Put it back where it was and say so in the pane. */
                                SetControlValue(gOnRadio,  gRadioOn ? 1 : 0);
                                SetControlValue(gOffRadio, gRadioOn ? 0 : 1);
                            }
                            SyncButtons();
                            /* ★ The logo follows the radio. Note this is AFTER the
                             * accept/revert above, so it shows the state the DRIVER
                             * settled on -- a refused change leaves the logo where it
                             * was, matching the buttons rather than the click. */
                            DrawRadioLogo();
                            DrawPanes();
                        }
                        /* ★★ THE FIRST BUTTON THAT DOES SOMETHING REAL. Everything
                         * else on this panel is still scaffolding. */
                        /* ★★ DELETE. Destructive, so it CONFIRMS first -- which is also
                         * what the ellipsis in "Delete..." promises, and a button whose
                         * label promises a dialog and then acts immediately is worse
                         * than one that does nothing.
                         *
                         * ⚠ StandardAlert rather than an ALRT/DITL pair: no resources to
                         * keep in sync, and no DITL UserItem to leave without a
                         * SetDialogItem UPP -- the trap that broke dialog clicks on this
                         * project once already. */
                        /* ★★★★ PAIR. Placed BEFORE the Delete branch so the two
                         * selection-driven buttons read in the order the user meets
                         * them: pair a device, then later forget it.
                         *
                         * ⚠ NO CONFIRMATION DIALOG, and that is not laziness. Delete
                         * destroys something the user has; pairing creates something.
                         * Tiger's assistant asks nothing before attempting a pairing
                         * either (docs/TIGER-UI-REFERENCE.md §2) -- it just shows the
                         * PIN. What the user DOES need is the PIN, which this build
                         * cannot generate per-pairing yet, so it states the fixed one.
                         *
                         * ⚠⚠ THE PIN MUST BE ON SCREEN BEFORE THE PAIRING STARTS. A
                         * legacy pairing only completes if a human reads a number and
                         * types it on the device, and the device asks within seconds.
                         * Showing it afterwards would be showing it too late, so this
                         * is a modal alert the user dismisses when ready -- the pairing
                         * is issued on OK, not before. */
                        /* ★★★★ RENAME, v15.3. No controller involved and no command
                         * sent: a nickname is a note this Mac keeps about an address.
                         * ⚠ The row is captured BEFORE the modal runs. The list rebuilds
                         * itself from scans and bonds on a timer, so gSelRow can point at
                         * a different device -- or at nothing -- by the time the user
                         * finishes typing. Reading it afterwards would occasionally
                         * rename the wrong device, which is exactly the kind of fault
                         * that is never reproducible on demand. */
                        if (c == gRename && gSelRow >= 0 && gSelRow < gRowCount) {
                            unsigned long rhi = gRow[gSelRow].addrHi;
                            unsigned long rlo = gRow[gSelRow].addrLo;
                            const char   *cur = AliasFor(rhi, rlo);
                            char          nm[kAlChars];
                            nm[0] = 0;
                            if (AskForNickname(cur, nm)) {
                                AliasSet(rhi, rlo, nm);
                                /* ⚠ REFRESH, DO NOT RE-SORT. SortRows clears gSelRow
                                 * because the selection is an INDEX -- and dropping the
                                 * selection the instant the user acts on a row would
                                 * grey this very button under their cursor. The list
                                 * re-sorts itself on the next rebuild anyway. */
                                RefreshList();
                                SyncButtons();
                                InvalRect(&gWin->portRect);
                            }
                        }
                        else if (c == gConfigure && gSelRow >= 0 && gSelRow < gRowCount) {
                            Str255 msg, expl;
                            AlertStdAlertParamRec p;
                            SInt16 hit = 0;

                            PStr(msg, "Type this passkey on the device:  0000");
                            /* ⭐⭐ THE POWER-CYCLE LINE IS NOT PADDING. MEASURED
                             * 2026-09-16: a freshly paired A1016 authenticates and
                             * encrypts, its light keeps blinking, and it NEVER offers
                             * the HID channel -- incoming connections 0, handshake NOT
                             * RECEIVED -- because it is still in pairing mode and has
                             * not begun behaving as a bonded device. Switching it off
                             * and on produced incoming connections 1, handshake 0 =
                             * success, 51 reports and 44 keystrokes, with nothing else
                             * changed.
                             *
                             * ⚠ The user found that themselves, after a pairing that
                             * looked successful in every status the panel could show.
                             * A required step nobody can guess belongs in the dialog
                             * they are already reading, not in a release note -- this
                             * is the one moment they are looking straight at it. */
                            /* ⚠ 245 CHARACTERS, COUNTED. A Str255 holds 255 and PStr
                             * truncates silently -- the first draft of this text was
                             * 264 and would have cut off mid-sentence, losing the very
                             * instruction it was added to give. Measure before adding
                             * a clause. */
                            PStr(expl, "Click OK, then type 0000 followed by Return on "
                                       "the device. It has only a few seconds, so have "
                                       "it awake first. It should start working within a second or two of the pairing finishing.");
                            p.movable       = false;
                            p.helpButton    = false;
                            p.filterProc    = NULL;
                            p.defaultText   = (ConstStringPtr)kAlertDefaultOKText;
                            p.cancelText    = (ConstStringPtr)kAlertDefaultCancelText;
                            p.otherText     = NULL;
                            p.defaultButton = kAlertStdAlertOKButton;
                            p.cancelButton  = kAlertStdAlertCancelButton;
                            p.position      = kWindowDefaultPosition;
                            /* ⚠ Record the alert's own rc, same reason as Delete's: a
                             * refused alert and a cancelled one must not look alike. */
                            gAlertRc = StandardAlert(kAlertNoteAlert, msg, expl, &p, &hit);
                            gAlertHit = hit;
                            if (gAlertRc == noErr && hit == kAlertStdAlertOKButton) {
                                gLastPairRc = DriverPairDevice(gRow[gSelRow].addrHi,
                                                               gRow[gSelRow].addrLo);
                                gPairTried = true;
                            }
                        }
                        else if (c == gDisconnect && gSelRow >= 0
                                 && gSelRow < gRowCount) {
                            /* ⚠ NO CONFIRMATION, deliberately. Disconnecting is
                             * reversible by the device simply reconnecting, and it
                             * removes nothing. Delete keeps its caution alert because
                             * Delete removes things. */
                            {
                                Boolean blocked =
                                    (gBlock != NULL && gBlock[kWBlockActive] != 0
                                     && gBlock[kWBlockHi] == gRow[gSelRow].addrHi
                                     && gBlock[kWBlockLo] == gRow[gSelRow].addrLo);
                                gLastDiscRc = (gProbeMode != kProbeFull)
                                            ? cfragNoSymbolErr
                                            : MailboxSend(blocked ? kCmdAllowDevice
                                                                  : kCmdDisconnect,
                                                          gRow[gSelRow].addrHi,
                                                          gRow[gSelRow].addrLo);
                                gDiscWasAllow = blocked;
                                /* ⭐ Only for Connect. Disconnect's result is immediate
                                 * and needs no transient. */
                                if (blocked && gLastDiscRc == noErr) {
                                    gWaitHi   = gRow[gSelRow].addrHi;
                                    gWaitLo   = gRow[gSelRow].addrLo;
                                    gWaitTick = TickCount();
                                    gWaiting  = true;
                                }
                            }
                            gDiscTried   = true;
                            gDeleteTried = false;   /* a new action supersedes it */
                        }
                        else if (c == gDelete && gSelRow >= 0 && gSelRow < gRowCount) {
                            Str255 msg, expl;
                            AlertStdAlertParamRec p;
                            SInt16 hit = 0;
                            char addr[24];

                            AddrToStr(gRow[gSelRow].addrHi, gRow[gSelRow].addrLo, addr);
                            /* ★★★★ TWO DIFFERENT DELETES, AND THE DIALOG MUST SAY
                             * WHICH. A paired row forgets OUR bond, which we can
                             * remake. A card row asks the MODULE to forget one of its
                             * own -- that survives a reboot, Tiger shares the same
                             * store, and it is how a working keyboard gets lost. The
                             * user cannot consent to the right thing if both say the
                             * same words. */
                            /* ⚠ ROUTES ON inCard ALONE now. The `&& !paired` meant a
                             * row present in BOTH the card and our database took the
                             * OUR-COPY branch -- see the merge note above. If the card
                             * holds a key for this address, that is the persistent one
                             * the user means, and it is the one needing stern wording. */
                            if (!gRow[gSelRow].paired && !gRow[gSelRow].inCard) {
                                /* ★★★ NO BOND ANYWHERE, so nothing is being unpaired
                                 * and the dialog must not imply otherwise. It still
                                 * asks: the button carries an ellipsis, and one that
                                 * sometimes asks and sometimes does not is worse than
                                 * one that always does. */
                                PStr(msg, "Remove this device from the list?");
                                PStr(expl, "This device is not paired, so no pairing is "
                                           "being removed. The row comes back the next "
                                           "time a scan finds the device in pairing "
                                           "mode.");
                            } else if (DeleteIsOneWay()) {
                                /* ★★★ 15.9: A BOND IS BEING REMOVED AND NOTHING ON THIS
                                 * MAC HAS A COPY OF THE KEY. This branch deliberately
                                 * DISPLACES the two below, including the module wording,
                                 * because the recovery advice is the part the user
                                 * cannot work out for themselves -- a keyboard that
                                 * shows up in every scan as Available and refuses every
                                 * pairing attempt gives no hint that its batteries are
                                 * the answer. The module clause is appended when it
                                 * applies and there is room. */
                                PStr(msg, "Delete this pairing? It cannot be undone.");
                                { Str255 a; short i;
                                  PStr(expl, "No copy of this key exists on this Mac, so "
                                             "Restore cannot put it back. The device will "
                                             "still believe it is paired, and may need its "
                                             "batteries taken out for a few seconds before "
                                             "it will pair again.");
                                  if (gRow[gSelRow].inCard) {
                                      /* ⚠ 45 chars, and the length is deliberate: the
                                       * base text is 196 and the loop caps at 250, so a
                                       * longer clause truncates MID-WORD in the dialog.
                                       * Measured, not estimated. */
                                      PStr(a, " Mac OS X shares this memory and loses "
                                              "it too.");
                                      for (i = 1; i <= a[0] && expl[0] < 250; i++)
                                          expl[++expl[0]] = a[i];
                                  } }
                            } else if (gRow[gSelRow].inCard) {
                                PStr(msg, "Remove this pairing from the module?");
                                { Str255 a; short i;
                                  PStr(expl, "The Bluetooth module's own memory holds a "
                                             "pairing for ");
                                  PStr(a, addr);
                                  for (i = 1; i <= a[0] && expl[0] < 190; i++)
                                      expl[++expl[0]] = a[i];
                                  PStr(a, ". Removing it is PERMANENT and also affects "
                                          "Mac OS X, which shares this module's memory. "
                                          "A device paired there would stop working "
                                          "until it is paired again.");
                                  for (i = 1; i <= a[0] && expl[0] < 250; i++)
                                      expl[++expl[0]] = a[i]; }
                            } else {
                            PStr(msg, "Forget this Bluetooth device?");
                            { Str255 a; short i;
                              PStr(expl, "The pairing for ");
                              PStr(a, addr);
                              for (i = 1; i <= a[0] && expl[0] < 200; i++)
                                  expl[++expl[0]] = a[i];
                              PStr(a, " will be removed. You will have to pair it again "
                                      "to use it with this Mac.");
                              for (i = 1; i <= a[0] && expl[0] < 250; i++)
                                  expl[++expl[0]] = a[i]; }
                            }

                            p.movable       = false;
                            p.helpButton    = false;
                            p.filterProc    = NULL;
                            /* ⚠⚠ THIS RETURNED -50 (paramErr) AND SHOWED NO DIALOG.
                             *
                             * The previous version set defaultButton AND cancelButton
                             * both to kAlertStdAlertCancelButton, wanting Cancel to be
                             * the default so Return could not destroy a bond by reflex.
                             * StandardAlert rejects that outright, and because the old
                             * code discarded its OSStatus the failure was invisible --
                             * the button simply did nothing.
                             *
                             * ★ Standard, valid arrangement instead: OK position is
                             * "Delete" and is the default; Cancel is Cancel. Escape and
                             * Cmd-. therefore cancel safely, which is the real
                             * protection -- and it is what Apple's own delete
                             * confirmations do. I could not have both, and a dialog
                             * that appears beats a safer one that does not. */
                            p.defaultText   = (ConstStringPtr)kAlertDefaultOKText;
                            p.cancelText    = (ConstStringPtr)kAlertDefaultCancelText;
                            p.otherText     = NULL;
                            p.defaultButton = kAlertStdAlertOKButton;
                            p.cancelButton  = kAlertStdAlertCancelButton;
                            p.position      = kWindowDefaultPosition;

                            /* ⚠ RECORD THE ALERT'S OWN RESULT. The button was reported
                             * clickable with no dialog appearing, and the old code
                             * discarded StandardAlert's OSStatus -- so a refused alert
                             * and a cancelled one looked identical, which is why that
                             * report could not be acted on. */
                            gAlertRc = StandardAlert(kAlertCautionAlert, msg, expl,
                                                     &p, &hit);
                            gAlertHit = hit;
                            gDeleteTried = true;
                            gDiscTried   = false;   /* a new action supersedes it */
                            gWaiting     = false;
                            /* ★★★★★ DELETE ENDS THE CONNECTION TOO, AND THAT IS THE
                             * USER'S OWN DEFINITION OF THE TWO BUTTONS: Disconnect ends
                             * the connection and leaves the device visible; Delete does
                             * that AND removes it from the list.
                             *
                             * ⚠⚠ WITHOUT THIS, DELETING A CONNECTED DEVICE COULD NOT
                             * REMOVE ITS ROW, and the reason is not the list code. The
                             * live-connection set is a row source: forgetting the key
                             * leaves the device connected, so it keeps its row and
                             * truthfully reads "Connected, no key". The row was waiting
                             * on a disconnect that nothing had asked for. Reported as
                             * "I would have preferred it to disappear immediately" --
                             * and it is the key deletion that makes this safe to send,
                             * because there is no longer a bond for it to reconnect on.
                             *
                             * ⚠⚠⚠ QUEUED, NOT SENT HERE, AND SENDING IT HERE WOULD HAVE
                             * DONE NOTHING AT ALL. THE MAILBOX IS A SINGLE SLOT:
                             * kWCmd/Arg0/Arg1 with kWCmdSeq written last as the
                             * trigger, and the driver's servicer does
                             * `gCB[kWCmdAck] = seq` -- it acknowledges the CURRENT
                             * sequence number, not one command at a time. Two sends a
                             * few microseconds apart, with the driver's timer yet to
                             * run, means the second overwrites the first and seq jumps
                             * by two: the disconnect is consumed by nothing and vanishes
                             * without an error anywhere. Caught by reading the servicer
                             * before shipping it, not on the hardware.
                             *
                             * ⇒ It goes in a one-deep pending slot that PollScan drains
                             * once MailboxCaughtUp() says the delete has been taken. */
                            if (gAlertRc == noErr && hit == kAlertStdAlertOKButton) {
                                /* ★★★★ ALLOW, NOT DISCONNECT, IF WE ARE ALREADY
                                 * REFUSING IT. Deleting a device the user had first
                                 * disconnected left the refusal standing, so the row
                                 * kept reading "Disconnected by you" about a device
                                 * that had been removed -- a state describing a
                                 * relationship that no longer exists. Disconnecting an
                                 * already-refused device achieves nothing; clearing the
                                 * refusal is what leaves no trace behind.
                                 *
                                 * ⚠ ONE COMMAND EITHER WAY, because the mailbox holds
                                 * one and the key deletion has already claimed this
                                 * round. */
                                {
                                    unsigned long dh = gRow[gSelRow].addrHi;
                                    unsigned long dl = gRow[gSelRow].addrLo;
                                    Boolean blocked = (gBlock != NULL
                                                    && gBlock[kWBlockActive] != 0
                                                    && gBlock[kWBlockHi] == dh
                                                    && gBlock[kWBlockLo] == dl);
                                    PendClear();
                                    /* ⚠ DISCONNECT BLOCKS AS A SIDE EFFECT -- see the
                                     * driver's kCmdDisconnect, which sets the block
                                     * FIRST so the peer cannot re-page in between. So
                                     * ending the link costs us a refusal, and the
                                     * allow that follows is what hands it back. */
                                    if (!blocked) PendQueue(kCmdDisconnect, dh, dl);
                                    PendQueue(kCmdAllowDevice, dh, dl);
                                }
                            }
                            /* ★★★★ ROUTE THEM APART. Same button, same confirmation
                             * shape, three entirely different destinations. */
                            gDeleteListOnly = false;
                            if (gAlertRc == noErr && hit == kAlertStdAlertOKButton
                                && !gRow[gSelRow].paired && !gRow[gSelRow].inCard) {
                                /* ⚠ NO MAILBOX COMMAND. There is no bond to forget, so
                                 * sending one would be a command whose only possible
                                 * answer is "no such bond" -- reported to the user as
                                 * the failure of something they did not ask for. This
                                 * is a local list operation and says so. */
                                gLastDeleteRc   = noErr;
                                gDeleteListOnly = true;
                                PendClear();              /* nothing to disconnect */
                                HideAddr(gRow[gSelRow].addrHi, gRow[gSelRow].addrLo);
                                MergeBonded();
                                RefreshList();
                                gSelRow = -1;
                                if (gList != NULL) { Cell z; z.h = 0; z.v = 0;
                                                     LSetSelect(false, z, gList); }
                            } else if (gAlertRc == noErr && hit == kAlertStdAlertOKButton
                                && gRow[gSelRow].inCard) {
                                gDeleteTried  = true;
                                gLastDeleteRc = DriverDeleteStoredKey(
                                                    gRow[gSelRow].addrHi,
                                                    gRow[gSelRow].addrLo);
                                /* ★★★★ THE ROW GOES NOW -- BUT ONLY ON SUCCESS, and
                                 * that condition is the whole reason this is here
                                 * rather than beside the confirmation. Every source is
                                 * filtered by AddrHidden, so the row can no longer be
                                 * kept alive by a scan result or a page the way the
                                 * user saw it. It returns on the next scan.
                                 *
                                 * ⚠ The old comment here said the row must NOT be
                                 * dropped locally because a card delete has to wait for
                                 * the MODULE to republish -- showing success before the
                                 * controller has answered. That objection is about
                                 * hiding it UNCONDITIONALLY, which is what I wrote
                                 * first. Gated on noErr it does not apply: a refusal
                                 * leaves the row exactly where it was and the Scan line
                                 * names the error. */
                                if (gLastDeleteRc == noErr) {
                                    HideAddr(gRow[gSelRow].addrHi, gRow[gSelRow].addrLo);
                                    MergeBonded();
                                    RefreshList();
                                }
                                /* ⚠ NOT removed from the list locally. Our own delete
                                 * can drop a row at once because the driver has already
                                 * forgotten it; this one has to wait for the CARD to
                                 * republish its store, and pretending otherwise would
                                 * show success before the controller had answered. */
                                gLastKeyCount = -1;
                            } else if (gAlertRc == noErr && hit == kAlertStdAlertOKButton) {
                                gDeleteTried  = true;
                                gLastDeleteRc = DriverDeleteBond(gRow[gSelRow].addrHi,
                                                                 gRow[gSelRow].addrLo);
                                /* ⚠⚠ REMOVE IT FROM OUR OWN MODEL IMMEDIATELY. Forcing
                                 * a re-read was not enough, and the reason is the
                                 * quiet-radio problem in a new place: the block's key
                                 * count is published by BT_KeyPublish inside
                                 * BT_StackPoll, which only runs on a USB COMPLETION. On
                                 * an idle radio nothing publishes, so the panel kept
                                 * showing the deleted device until a scan happened to
                                 * generate traffic -- exactly what was reported.
                                 *
                                 * The driver has already forgotten it, and the panel
                                 * knows the call returned noErr, so waiting to be told
                                 * something we already know is the bug. Drop it locally
                                 * and let the block catch up whenever it next publishes;
                                 * the two agree either way. */
                                if (gLastDeleteRc == noErr) {
                                    short k, n = 0;
                                    unsigned long hi = gRow[gSelRow].addrHi;
                                    unsigned long lo = gRow[gSelRow].addrLo;
                                    /* ★ Remember it, so the next poll's reload filters
                                     * it out of the block's stale list instead of
                                     * putting it straight back on screen. */
                                    if (gDelCount < kMaxKeys &&
                                        !AddrPendingDelete(hi, lo)) {
                                        gDelHi[gDelCount] = hi;
                                        gDelLo[gDelCount] = lo;
                                        gDelCount++;
                                    }
                                    for (k = 0; k < gKeyRows; k++) {
                                        if (gKeyHi[k] == hi && gKeyLo[k] == lo) continue;
                                        gKeyHi[n] = gKeyHi[k];
                                        gKeyLo[n] = gKeyLo[k];
                                        n++;
                                    }
                                    gKeyRows = n;
                                    /* ⚠ The handed-out bitmask is indexed by PUBLISHED
                                     * position, so removing an entry shifts it. Cleared
                                     * rather than re-shuffled: a stale mask would
                                     * mislabel a surviving bond as "Paired", and the
                                     * next publish restores it correctly. */
                                    gKeyHandedMask = 0;
                                    /* ★ And out of every OTHER source as well -- the
                                     * key list was only one of the four. */
                                    HideAddr(hi, lo);
                                    MergeBonded();
                                    RefreshList();
                                }
                                /* Still force a re-read so the next publish is taken. */
                                gLastKeyCount = -1;
                                gSelRow = -1;
                                if (gList != NULL) { Cell z; z.h = 0; z.v = 0;
                                                     LSetSelect(false, z, gList); }
                            }
                            SyncButtons();
                            SetPort(gWin);
                            DrawPanes();
                        }

                        if (c == gSetup) {
                            /* ★★ THE DIRECT CALL. Run 47 proved the block-and-poll
                             * command channel cannot work on an idle radio: seq 9,
                             * ack 2, no inquiry ever ran. This goes straight into the
                             * driver, which hops the work to secondary interrupt level
                             * so it is serialized against the USB completions. */
                            gScanTried  = true;      /* ★ set BEFORE the verdict */
                            gDiscTried  = false;     /* a new action supersedes it */
                            gWaiting    = false;
                            /* ★★★★ THE SCAN IS WHAT LIFTS A DELETION FROM THE LIST.
                             * A deleted device disappears at once and comes back, as
                             * Available, only when a scan finds it again -- which is
                             * the user's own model of Delete, and the reason hiding a
                             * paged row is safe. See AddrHidden. */
                            /* ⚠⚠ THE SCAN NO LONGER CLEARS THE HIDE LIST. It is lifted
                             * per address, in PollScan, by an actual inquiry RESPONSE --
                             * see UnhideAddr. Clearing it here would bring a deleted
                             * device back from the module's store or a stale page
                             * record without it being discoverable at all, which is
                             * looser than the rule the user stated. */
                            gDeleteTried = false;    /* a new action supersedes it */
                            /* Option-click = walk only, for the bisect above.
                             *
                             * ⚠ THE COMBINATION IS TESTED FIRST. These are evaluated
                             * in order, so option+shift would match option alone and
                             * silently give the walk-only probe instead of the direct
                             * call -- a positive control that quietly tests nothing is
                             * worse than none. */
                            gProbeMode =
                                ((evt.modifiers & optionKey) &&
                                 (evt.modifiers & shiftKey)) ? kProbeDirect   :
                                (evt.modifiers & optionKey)  ? kProbeWalkOnly :
                                (evt.modifiers & shiftKey)   ? kProbeNoCall   :
                                (evt.modifiers & controlKey) ? kProbeNoop     :
                                (evt.modifiers & cmdKey)     ? kProbeNoopSIH  :
                                                               kProbeFull;
                            {
                                /* ★ The driver's OWN bind counter, read either side of
                                 * the action. This is what makes the press attributable
                                 * without depending on click order. */
                                unsigned long b = gBlock ? gBlock[kWInit] : 0;
                                char tag = (char)(gProbeMode == kProbeDirect   ? 'D' :
                                                  gProbeMode == kProbeWalkOnly ? 'W' :
                                                  gProbeMode == kProbeNoCall   ? 'S' :
                                                  gProbeMode == kProbeNoop     ? 'P' :
                                                  gProbeMode == kProbeNoopSIH  ? 'C' : 'N');
                                /* ★ Stamp the action BEFORE the call, so its time is the
                                 * time it was asked for. A teardown this press causes
                                 * appears LATER in the timeline as its own b<n>@<s>
                                 * entry, which is the whole point -- see gTLog. */
                                TLogAct(tag);
                                gLastScanRc = DriverStartScan();
                                /* ⚠ Kept for continuity with the older runs, and as a
                                 * standing demonstration of why it could not settle
                                 * this: it will read n>n even when the press does cause
                                 * a teardown, because the unload is not synchronous. */
                                ActNote(tag, b, gBlock ? gBlock[kWInit] : 0);
                                StatusWrite(true);   /* flush it NOW, not in 15 s */
                            }
                            gProbeMode = kProbeFull;
                            /* ★ "Already scanning" counts as running: an inquiry IS in
                             * flight, which is what the click asked for. Treating it as
                             * a failure hid the arrows and cried fault over a race. */
                            /* ★ The sequence our request was given, so the poll can
                             * tell "not started yet" from "already finished". Read
                             * AFTER the send, which is what incremented it. Zero for
                             * the probe modes, which do not use the mailbox. */
                            gScanReqSeq = (gBlock && gProbeMode == kProbeFull)
                                        ? gBlock[kWCmdSeq] : 0;
                            gScanAsked  = (gLastScanRc == noErr ||
                                           gLastScanRc == kBTAlreadyScanning);
                            gLastScanCount = -1;   /* force a refresh on next poll */
                            if (gScanAsked) {
                                gLastScanState = 0xFFFFFFFFUL;  /* re-read, don't trust
                                                                 * the completed state
                                                                 * of the LAST inquiry */
                                gScanStartTick = TickCount();
                                gScanStalled   = false;
                                ShowControl(gArrows);
                                /* ⚠ Clear the SCAN rows only, then re-merge: the bonds
                                 * are still valid and must not vanish because a new
                                 * scan started. Without the re-merge, gRowCount would
                                 * keep describing rows that gScan no longer has. */
                                gScanRows = 0;
                                MergeBonded();
                                RefreshList();
                            }
                            DrawPanes();
                        }
                    }
                }
                break;

            case updateEvt:
                if ((WindowPtr)evt.message == gWin) {
                    BeginUpdate(gWin);
                    SetPort(gWin);
                    ErasePlatinum(&gWin->portRect);
                    DrawFrame();
                    UpdateControls(gWin, gWin->visRgn);
                    if (gList != NULL) {
                        LUpdate(gWin->visRgn, gList);
                        DrawColumnRules();
                    }
                    EndUpdate(gWin);
                }
                break;

            case keyDown:
                /* ⚠ Route command keys through MenuKey rather than testing for 'q'
                 * by hand. v0.2 hand-matched Cmd-Q, which meant the keystroke worked
                 * but the File menu did not flash -- and it would have silently
                 * disagreed with the menu the moment either changed. */
                if (evt.modifiers & cmdKey) {
                    /* ⭐ Cmd-? OPENS HELP, which is what SimpleText does and what the
                     * user asked for. ⚠ It is handled HERE rather than as a menu key
                     * equivalent because "?" is a SHIFTED character on most layouts,
                     * and a command-key equivalent of "?" in the menu would not match
                     * what MenuKey is handed. Checking the character itself works
                     * whichever way the user produced it. */
                    {
                        char ch = (char)(evt.message & charCodeMask);
                        if (ch == '?' || ch == '/') { ShowHelp(); break; }
                    }
                    { long sel = MenuKey((short)(evt.message & charCodeMask));
                    if (HiWord(sel) != 0) { if (DoMenu(sel)) done = true; } }
                } else {
                    /* ⭐ RETURN OR ENTER PRESSES THE DEFAULT BUTTON. The thick ring
                     * SetControlData draws is only a promise; this is the half that
                     * keeps it. A button that looks default and does nothing on Return
                     * is worse than one that never claimed to be.
                     *
                     * ⚠ Only when the button is actually ENABLED. Scan follows the
                     * radio, so with Bluetooth off the ring is still drawn by the
                     * Appearance Manager but the action must not fire -- pressing
                     * Return would otherwise do what a click is refused. */
                    char ch = (char)(evt.message & charCodeMask);
                    if ((ch == 13 || ch == 3) && gSetup != NULL
                        && (**gSetup).contrlHilite == 0) {
                        /* Flash it, so the key press looks like the click it stands in
                         * for, then run the same path the click runs. */
                        HiliteControl(gSetup, kControlButtonPart);
                        Delay(6, NULL);
                        HiliteControl(gSetup, 0);
                        /* ⚠ The same call the click makes, and gProbeMode forced to
                         * kProbeFull: the click derives that from its modifier keys
                         * (option/shift/control select diagnostic probe modes), and a
                         * Return press carries no such intent. Leaving whatever mode a
                         * previous option-click had set would make Return silently run
                         * a DIAGNOSTIC scan. */
                        gProbeMode  = kProbeFull;
                        gScanTried  = true;
                        gDeleteTried = false;
                        gLastScanRc = DriverStartScan();
                    }
                }
                break;
            }
        }
    }

    /* ⚠ A FINAL LINE, THEN CLOSE. A clean quit must be distinguishable from a freeze:
     * without this the file's last line is up to 15 seconds stale and looks exactly
     * like the panel having stopped, which would make every ordinary quit read as a
     * hang. The marker is what tells the two apart. */
    StatusWrite(true);
    if (gStatRef != 0) {
        Str255 bye; long n;
        bye[0] = 0;
        PStrCat(bye, "  QUIT CLEANLY\r");
        n = bye[0];
        if (SetFPos(gStatRef, fsFromLEOF, 0) == noErr) {
            (void)FSWrite(gStatRef, &n, &bye[1]);
            (void)FlushVol(NULL, gStatVRef);
        }
    }
    StatusClose();

    if (gList != NULL) LDispose(gList);
    DisposeWindow(gWin);
    return 0;
}
