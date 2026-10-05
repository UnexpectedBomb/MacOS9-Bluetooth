/*
 *  bt_btstack.c  --  BTstack lifecycle for the OS 9 port.
 *
 *  The only file that both includes BTstack's headers AND is called by the USB
 *  driver. It exists so that bt_probe.c (which includes USB.h) and BTstack's
 *  include world never meet: they disagree about `bool`, which is why the build
 *  needs -DTYPE_BOOL=1, and keeping them apart is cheaper than reconciling them.
 *
 *  ⚠ EVERYTHING HERE RUNS BELOW TASK LEVEL, BT_StackStart INCLUDED.
 *
 *  An earlier version of this comment said BT_StackStart was the exception
 *  "reached from ProbeInitialize", i.e. task level. That was wrong, and the audit
 *  is what caught it: ProbeInitialize only STARTS the configuration chain, and
 *  BT_StackStart is actually called from ConfigDone (bt_probe.c), which is a USL
 *  completion proc and therefore secondary interrupt level. So btstack_memory_init,
 *  btstack_run_loop_init, hci_init, hci_add_event_handler and hci_power_control all
 *  run below task level. Nothing here may assume otherwise -- no allocation, no
 *  File Manager, no Toolbox.
 *
 *  That is safe on measured evidence: the preprocessed audit, re-run with those
 *  five functions as explicit roots, finds no allocator, no File Manager, no
 *  Toolbox call and nothing that can abort. See docs/M2-DESIGN.md §3c. Re-run both
 *  audits after touching this file or btstack_config.h.
 */

#include "btstack_config.h"

#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "hci.h"
#include "hci_transport.h"
#include "btstack_event.h"
#include "l2cap.h"
#include "gap.h"
#include "bluetooth_sdp.h"
#include "bluetooth_psm.h"   /* BLUETOOTH_PSM_HID_CONTROL / _INTERRUPT (M4) */
#include "classic/sdp_client.h"

#include <string.h>          /* memcpy -- pure, and on the audit's safe list */

#include "classic/hid_host.h"     /* v9.5: BTstack's own HID host, finally called */
#include "bt_pump.h"
#include "bt_btstack.h"
#include "bt_linkkey_db.h"
/* ⚠ Dependency-free by design and it must stay that way -- see its header. Included
 * here (a file full of BTstack) rather than the other way round, which is the whole
 * point: the decoder can still be compiled and tested with the HOST compiler. */
#include "bt_bootreport.h"
#include "bt_hidident.h"
#include "bt_inject.h"   /* M5 step 2c: key -> Mac OS event */
#include "bt_keyfile.h"  /* v14.2: BT_BatteryNote -- plain stores, safe from here */

extern const btstack_run_loop_t * btstack_run_loop_os9_get_instance(void);
extern const hci_transport_t    * hci_transport_os9_instance(void);

static btstack_packet_callback_registration_t gHciEventCallback;
static int gStarted;

/* ---- M2b counters, mirrored into the counter block by BT_StackPoll ------- *
 * Plain unsigned long globals, matching the pattern hci_transport_os9.c already
 * uses for gTxCmd/gRxEvent: bt_probe.c owns the block and the word numbering,
 * this file owns the values, and neither includes the other's headers.
 *
 * ⚠ Statuses are stored as 0x100 | status. See the kW* enum in bt_probe.c: a bare
 * 0 cannot be distinguished from "never received", which is the M1
 * kRecInqComplete mistake. Do not "simplify" these back to a bare byte. */
unsigned long gL2Init, gInqState, gInqStartRc, gInqResults, gInqComplStatus;
/* 0x100 | gap_inquiry_stop() -- the recovery attempted after a refused start. */
unsigned long gInqStopRc;

/* ---- M4 step 1: the HID control channel, incoming ------------------------------
 * ⚠ Every one of these exists so a failure names itself. "No keyboard connected" has
 * at least five distinct causes -- the listen was refused, nothing paged us, something
 * paged us on the wrong PSM, we accepted and the channel failed to open, or it opened
 * and closed again -- and a single "it did not work" cannot tell them apart. */
unsigned long gHidListenRc;      /* 0x100 | l2cap_register_service()              */
unsigned long gHidIncoming;      /* incoming connections seen, ANY psm            */
unsigned long gHidAccepted;      /* accepted (psm was HID control)                */
unsigned long gHidDeclined;      /* declined explicitly (wrong psm)               */
unsigned long gHidOpened;        /* control channels actually OPENED              */
unsigned long gHidClosed;        /* ...and later closed                           */
unsigned long gHidOpenStatus;    /* 0x100 | status of the last channel-opened     */
unsigned long gHidLastPsm;       /* psm of the last incoming connection           */
unsigned long gHidCtrlCid;       /* live control-channel cid, 0 = none            */
/* ★★★ THE DEADLOCK, AND WHICH LINK OF IT WE CONTROL.
 *
 * ⚠ The two gates live in bt_pump.h. kHidLevel0 is 0 on this evidence -- see the
 * measurement recorded there: LEVEL_0 broke our half and then the A1016 refused to
 * answer an unencrypted channel, which is worse than the deadlock it cured.
 *
 * v6.7's event ring found a three-link deadlock, every link confirmed in the vendored
 * source rather than inferred:
 *
 *   l2cap.c:2488  an INCOMING connection whose service wants LEVEL_2, on a link with
 *                 no encryption, goes to WAIT_INCOMING_SECURITY_LEVEL_UPDATE, queues
 *                 only a Connection Response PENDING, and does NOT report
 *                 L2CAP_EVENT_INCOMING_CONNECTION to us. That is why M4's "incoming
 *                 connections" read 0 while "L2CAP events seen" read 2: the A1016's
 *                 request arrived and L2CAP swallowed it pending security.
 *   hci.c:8096    authentication waits on BONDING_RECEIVED_REMOTE_FEATURES
 *   hci.c:4704    which is set only from the 0x0B handler
 *                 ... and the A1044 accepts Read_Remote_Supported_Features and never
 *                 completes it while the link is alive. It errors the command out with
 *                 status 2, Unknown Connection Identifier, only AFTER the handle is
 *                 gone -- the ring caught the Disconnection Complete arriving first.
 *
 * Both ends then wait for each other until the peer's supervision timeout kills the
 * link: reason 8, which is the reason every one of these links has died since v6.3.
 *
 * ⇒ We cannot make the card complete that command. We CAN stop L2CAP from waiting on
 * it. At LEVEL_0 the incoming connection is reported to us and answered immediately,
 * which breaks the deadlock at our end and answers the question none of this has yet
 * reached: WILL THE A1016 ACTUALLY TALK TO US?
 *
 * ⚠⚠⚠ THIS IS A MEASUREMENT AND MUST NEVER SHIP AT 1. A keyboard that connects
 * unencrypted is not a keyboard this project can ship -- keystrokes in the clear, and
 * any device in range able to claim the channel. The gate exists so the shipping
 * default is the safe one and the diagnostic cannot leak into a release by someone
 * forgetting a line: set it back to 0 once the run is read.
 *
 * ⚠ Published as gHidLevel and printed by BTCheck, because a run whose security level
 * is ambiguous is a run that cannot be interpreted -- and this project has already
 * lost one to not being able to tell two builds apart. */
/* ⚠ Defined in bt_pump.h, shared with the transport that injects the synthetic
 * completion. Two copies of this number would be two ways to half-disable the
 * probe. */
unsigned long gHidLevel;
unsigned long gHidIntrListenRc, gHidIntrCid, gHidIntrOpened;
unsigned long gHidSetProtoTried, gHidSetProtoRc, gHidHandshake;
unsigned long gHidCtrlDataPkts, gHidIntrDataPkts;
unsigned long gAutoAuthTried, gAutoAuthRc, gAutoAuthHandle;
/* v9.3: the supervision-timeout stretch that unmasks the LMP verdict. */
unsigned long gSuperTimeoutTried, gSuperTimeoutRc, gSuperTimeoutHandle;
/* v9.4: the per-link command queue, and the link policy it exists to carry. */
unsigned char gLinkCmdQ[kLinkCmdSlots];
unsigned long gLinkCmdHead, gLinkCmdCount, gLinkCmdTries, gLinkCmdHandle;
unsigned long gLinkCmdEnq, gLinkCmdSent, gLinkCmdRefused, gLinkCmdDropped;
/* v9.5: BTstack's HID host. ⚠ The descriptor store is required by hid_host_init
 * even though an incoming link never fetches one; 256 against the A1016's real 99. */
unsigned char gHidDescStore[256];
unsigned long gBtstackHostInit, gBhIncoming, gBhCid, gBhAcceptRc;
unsigned long gGapLevel;         /* v9.6: 0x100 | gap_get_security_level() */
/* v9.7: the controller's own Read_Buffer_Size answer. */
unsigned long gBufSizeRc, gAclBufLen, gAclBufNum;
unsigned long gBhOpened, gBhOpenedStatus, gBhOpenedMs;
unsigned long gBhClosed, gBhClosedMs, gBhSetProtoRsp, gBhDescAvail;
unsigned long gBhReports, gBhOtherEvts;
/* ★★★★★★ v13.8: THE HALF-OPEN CHANNEL WATCHDOG -- the reconnect half of the
 * power-cycle deletion. See the long note at bh_stall_timeout. */
unsigned long gBhIncomingMs;      /* when we accepted; the accept->open gap, finally measured */
unsigned long gBhFailedOpens;     /* v13.9: OPENED events carrying a NON-ZERO status          */
unsigned long gBhFailedStatus;    /* v13.9: 0x100 | the last such status (0x69 = L2CAP RTX)   */
unsigned long gBhReconnTried;     /* reconnects attempted after a failed open                 */
unsigned long gBhReconnRc;        /* 0x100 | hid_host_connect rc                              */
/* ★★★★★★ v13.9: "IS A HID CHANNEL OPEN RIGHT NOW", maintained rather than inferred.
 * ⚠⚠ THREE SEPARATE DEFECTS CAME FROM INFERRING THIS, and each inference was wrong in a
 * different state:
 *   gBhCid != 0           -- but gBhCid is ALSO set at INCOMING_CONNECTION, before the
 *                            accept, so it is non-zero after any offer. The v13.5 note
 *                            claims it has "exactly one meaning"; that assignment makes
 *                            it untrue, and the 13.8 teardown used it to disconnect a cid
 *                            that had never opened.
 *   gBhOpened > gBhClosed -- wrong after a close with no counted open (panel 15.5).
 *   openedMs > closedMs   -- wrong after a FAILED open, because gBhOpenedMs is stamped on
 *                            every OPENED event including failures (panel 15.6/15.7: it
 *                            reported "Connected" with HID REPORTS 0).
 * ⇒ One word, set where a channel becomes usable and cleared where it stops being usable,
 * and every reader uses it instead of deriving it. */
unsigned long gBhLive;
static bd_addr_t gBhPeerAddr;     /* peer of the channel we accepted, for the reconnect      */
static btstack_timer_source_t gBhReconnTimer;
static int gBhReconnArmed;
static int gBhAdopt;            /* v15.0: INCOMING's adopt decision, reused for the address */

/* ★★★★★★ v14.1: THE BATTERY PROBE. Does this keyboard ANSWER a GET_REPORT?
 *
 * ⭐ WHY THIS IS WORTH A BOOT. Tiger displays a battery level for this exact keyboard on
 * this exact machine, and its IOAppleBluetoothHIDDriver node names how
 * (docs/source/tiger-ioreg-full-2026-09-09.txt):
 *
 *     BatteryPercent  id=71  type=2 (Feature)  size=1  min=0  max=100
 *     BatteryState    id=48  type=0 (Input)    size=1  min=0  max=2
 *     MaxFeatureReportSize = 1
 *
 * ⚠⚠ AND THE ASSUMPTION THAT HAS HELD THIS UP WAS NEVER TESTED. The note I left myself
 * said BatteryState "arrives UNSOLICITED on the interrupt channel we already read",
 * reasoning from type=0 meaning Input. Measured across four hardware runs -- 56, 37, 51
 * and 4 reports, every one accepted, none of them report 48 -- it is NOT pushed. But an
 * Input report can equally be FETCHED with GET_REPORT, which is very likely what Tiger
 * does, and nobody has ever asked this keyboard. "Not pushed" was read as "not readable"
 * and those are different claims.
 *
 * ⚠ The one real reason for doubt: the A1016 answered SET_REPORT with handshake 0x03,
 * ERR_UNSUPPORTED_REQUEST -- the device saying it takes no SET_REPORT at all, which is
 * why the Caps LED goes out over the interrupt channel instead. GET_REPORT is a
 * DIFFERENT HIDP transaction (0x4 vs 0x5) and a device may support one and not the
 * other, so that inference does not carry. This probe is what settles it.
 *
 * ⚠⚠⚠ NEVER TOUCH REPORT IDS 68 OR 69. The same ExtendedFeatures table that carries 71
 * and 48 also carries FactoryDefault (69) and FullFactoryDefault (68). Those are
 * DESTRUCTIVE. Only the two constants below are ever sent, by name, and this table is
 * never enumerated or swept -- do not add a loop over report IDs here for any reason.
 *
 * ⚠ ONE SHOT EACH, CHAINED, NOT POLLED. Control-channel traffic has knocked this card
 * off the bus three times in this project's history, and BTstack's own header calls
 * GET_REPORT "expensive" and says it should be avoided where possible and used mainly to
 * read initial state. So: one Feature 71 a couple of seconds after the channel opens,
 * and Input 48 only after 71 answers -- two transactions per session, never overlapping,
 * never repeating. A CSM polling this every tick is exactly what must not be built on it. */
#define kBatReportPercent  71UL   /* Feature, 0..100. NOT 69 (FactoryDefault)     */
#define kBatReportState    48UL   /* Input,   0..2.   NOT 68 (FullFactoryDefault) */
#define kBatProbeMs      2000UL   /* after the channel opens; clear of L2CAP's RTX */
#define kBatChainMs       250UL   /* v14.2: Input 48 on its OWN tick -- see the
                                   * response handler; chaining it inline got
                                   * COMMAND_DISALLOWED in 14.1 */

unsigned long gBatSends;          /* hid_host_send_get_report calls made           */
unsigned long gBatResponses;      /* GET_REPORT_RESPONSE events seen               */
unsigned long gBatPctRc;          /* 0x100 | send rc for Feature 71                */
unsigned long gBatPctHs;          /* 0x100 | its handshake status                  */
unsigned long gBatPctLen;         /* its payload length                            */
unsigned long gBatPctVal;         /* its first 4 payload bytes, packed big-endian  */
unsigned long gBatStRc;           /* 0x100 | send rc for Input 48                  */
unsigned long gBatStHs;           /* 0x100 | its handshake status                  */
unsigned long gBatStLen;
unsigned long gBatStVal;
static btstack_timer_source_t gBatTimer;
static int gBatArmed, gBatStateAsked;
unsigned long gBatCid;            /* v15.2: the device currently being asked */
static void bat_probe_timeout(btstack_timer_source_t *ts);
unsigned long gPolicyReadRc, gPolicyReadSends, gPolicyReadBack, gPolicyReads;
unsigned long gPolicyWriteRc, gPolicyWriteSends;
unsigned long gModeChanges, gModeLast, gModeLastMs;
/* ★★★★★ v15.5 PROBE. Tiger drives this same mouse on this same card SMOOTHLY, which
 * retires the hardware as a suspect and makes the difference ours. Tiger's captured
 * trace shows its link sniffing at 20 slots = 12.5 ms. We have permitted sniff since
 * M4 and recorded only the MODE, throwing the INTERVAL away -- so the one number that
 * would explain jerkiness has never been looked at. A sniff interval of 64 slots is
 * 40 ms, i.e. 25 reports/sec, which is exactly what "jerky" looks like.
 *
 * ⚠ SEPARATED BY ROLE. With a keyboard and a mouse connected the card is interleaving
 * two links, and one aggregate number could not say which is slow. */
unsigned long gSniffKbdSlots, gSniffMseSlots, gSniffOtherSlots;
/* Peak mouse report rate: the most reports seen in any one kSniffWinMs window. An
 * AVERAGE is useless here -- a mouse reports only while it moves, and the 19-minute
 * session that prompted this averaged 1.2/sec while feeling jerky in the moments that
 * mattered. The peak is the number that corresponds to what the hand feels. */
#define kRateWinMs 250UL
/* ⚠ These six lived beside the sweep's globals and were caught in its removal (v16.1);
 * the linker found them, not a reading of the diff. They are mouse DECODE state and
 * have nothing to do with paging. */
short gMouseDxMin = 32767, gMouseDxMax = -32768;
short gMouseDyMin = 32767, gMouseDyMax = -32768;
unsigned long gMouseBtnMask, gMouseRidOther;
unsigned long gMouseRatePeak;
/* ★★★★★ v15.6 CALIBRATION. The transport is exonerated: 84 reports/sec at peak, no
 * sniff on any link, every delta accepted by the Cursor Device Manager. What is left is
 * SCALING -- CursorDeviceUnitsPerInch is a guessed 200, copied from Apple's sample and
 * never measured against this device. If the A1015 is really 400 cpi, OS 9 computes the
 * movement speed at half its true value, sits at the bottom of its acceleration curve,
 * and rounds small deltas toward zero: exactly "small precise movements do not respond".
 *
 * ⚠⚠ AND THE OBJECTIVE NUMBER MATTERS MORE THAN USUAL HERE. The user reported v15.5 as
 * "maybe smoother" when v15.5 changed NOTHING -- it was instrument-only. Feel is not a
 * usable oracle for this symptom, so the fix has to be chosen from a measurement.
 *
 * Total absolute counts: swipe a known distance, divide, and that is the true cpi. */
unsigned long gMouseAbsDx, gMouseAbsDy;
static unsigned long gRateWinStart, gRateWinCount;
/* v10.0: the role, never measured before this build. */
unsigned long gRoleChanges, gRoleStatus, gRoleNew, gRoleMs;
/* M5 step 1: the Consumer page, and the relative/absolute question it must settle. */
unsigned long gHidConsumerReports, gHidConsumerSeen, gHidConsumerPadBits;
unsigned long gHidConsumerRing[kHidConsumerRing];
unsigned long gHidConsumerRingCount, gHidConsumerRingLost, gHidConsumerLast;
unsigned long gHidKeyReports;
/* M5: the press/release state. ⚠ ONE instance, because it IS the "what is currently
 * held" memory -- two would each see half the reports and both would be wrong. */
static BTKeyState gKeyState;
unsigned long gInjCalls, gInjEventsSeen;
/* M5 step 4: the Caps Lock LED, written back over the air on state change. */
unsigned long gLedSends, gLedRc, gLedRsp;
/* Mirrored for the block: the caps toggle lives inside BTKeyState, which bt_probe.c
 * deliberately knows nothing about. */
unsigned long gCapsOnMirror;
/* v8.9. ⚠ How fresh the block is. Every other counter in it is only as trustworthy as
 * these two, so they are published FIRST in BT_StackPoll and read FIRST by BTCheck. */
unsigned long gPublishRuns, gPublishMs;
/* v9.0. ⚠ The controller's OWN scan state, read back rather than assumed. See
 * bt_read_scan_enable: bit 1 is page scan, and without it a bonded keyboard cannot
 * reach us no matter what it does. */
unsigned long gScanEnaRc, gScanEnaSends, gScanEnaFirst, gScanEnaLast, gScanEnaReads;
/* v9.2: EVERY Authentication Complete, because the last-value-wins row could not say
 * whether the FIRST one succeeded -- and that is the difference between "auth works
 * and encryption is lost elsewhere" and "auth never ran". */
unsigned long gAuthRing[kAuthRingSlots], gAuthRingMs[kAuthRingSlots], gAuthRingCount;

/* ★★★ A MARKER IN THE BINARY, BECAUSE A HEADER SAYING 1 IS NOT PROOF.
 *
 * v6.8 was STAGED AND RUN with this gate compiled as 0 while bt_pump.h said 1, and it
 * cost a hardware boot. The cause was mundane and will happen again: the gate was
 * flipped to 0 to check the shipping path still built, flipped back to 1, and both
 * edits plus both builds landed inside ONE SECOND -- so make's mtime comparison saw
 * the header as no newer than the objects and the second build was a no-op. The
 * artifact on disk was the one built with 0.
 *
 * Reading the source cannot catch that. Reading the ARTIFACT can, which is the same
 * principle this project already applies to the descriptor records and the version
 * stamps: scripts/verify-probe-gate.py greps the built .ndrv for this string and
 * fails the build if its presence disagrees with the header.
 *
 * ⚠ It must be a real, referenced string or the linker may drop it -- hence the
 * export below rather than a bare static. */
/* ⚠ Encodes EVERY gate, because v6.8 proved a single switch was the wrong shape
 * and a marker that only tracked one of them would let the other go stale
 * unnoticed -- which is the exact failure this marker exists to catch.
 *
 * ⚠⚠ AND IT NEARLY HAPPENED AGAIN AT v8.8. kHidAutoAuth was added as a third gate
 * and this marker still encoded two, so a build with kHidAutoAuth left at 1 -- which
 * authenticates ANY inbound link with no prompt -- produced a marker byte-identical
 * to a correct shipping build. The comment above says a marker tracking one of two
 * gates lets the other go stale; a marker tracking two of three does the same thing,
 * and the third gate is the one with a security consequence.
 *
 * ⇒ The nested #if form was the reason: adding a gate meant doubling eight lines of
 * branch, so it did not get done. Stringifying the gates instead means a new gate
 * costs ONE segment and cannot be branched wrongly. Note the two-step STR macro:
 * one-step stringification would emit the macro's NAME, not its value.
 *
 * LVL stays 0-or-2 rather than a 1/0 flag because 0 and 2 are the security levels
 * actually registered with L2CAP, and the log reader reads it as one. */
#define BT_GATE_STR_(x) #x
#define BT_GATE_STR(x)  BT_GATE_STR_(x)
#if kHidLevel0
#  define kHidLevelMark 0
#else
#  define kHidLevelMark 2      /* 0 or 2, whichever was registered */
#endif
const char kProbeGateMarker[] =
    "OS9BT-GATES-SYNTH" BT_GATE_STR(kHidSynthFeatures)
    "-LVL"              BT_GATE_STR(kHidLevelMark)
    "-AUTH"             BT_GATE_STR(kHidAutoAuth)
    "-SUP"              BT_GATE_STR(kHidLongSupervision)
    "-POL"              BT_GATE_STR(kHidLinkPolicy)
    "-HOST"             BT_GATE_STR(kHidUseBtstackHost);

/* The HID data path's own counters. ⚠ kHidCapBytes is defined ONCE, in bt_pump.h, and
 * shared with the publisher and BTCheck. 16 bytes is deliberate: a boot report is 8,
 * the framed form is 9, and anything much longer means this is not a boot keyboard
 * report at all -- which the FULL received length says. */
unsigned char gHidFirstData[kHidCapBytes];
unsigned long gHidFirstDataLen, gHidFirstDataFull, gHidDataPkts, gHidLastDataLen;
unsigned long gHidDecodeRc, gHidDecodeOk, gHidRollovers;

/* ★ the mouse path, and the report NEITHER decoder claimed. gUnclaimed* exist because
 * the keyboard's fifth framing cost a whole session in which the only evidence was
 * "reports accepted 0" -- a length and four bytes in the block turn that into an
 * answer. See the dispatch in BhRecordReport. */
unsigned long gMouseDecodeOk, gUnclaimedReports, gUnclaimedLen, gUnclaimedB0;

/* ★ the scan filter. gShowAllDevices is read ONCE at task level in ProbeInitialize from
 * `Preferences:Bluetooth Show All Devices` and only READ here, at interrupt level -- the
 * File Manager is not reachable from an inquiry result. gInqFiltered counts what the
 * filter hid, so a scan that legitimately found nothing drivable stays distinguishable
 * from a scan that found nothing at all. */
unsigned long gShowAllDevices, gInqFiltered;
unsigned long gHidLastMods, gHidLastNKeys, gHidLastKey0;

/* v7.0's security instrument. ⚠ Facts only -- every one of these is a raw reading and
 * the interpretation is deliberately left to BTCheck's decision table, because this
 * question has already been answered wrongly three times from confident reasoning. */
unsigned long gSecEvtCount, gSecEvtLevel, gSecEvtStatus, gSecEvtHandle;
unsigned long gSecLvlFirst, gSecLvlLast, gRemFeatFirst, gRemFeatLast, gLiveSamples;
unsigned long gSampledHandle, gSampledHciHandle, gSampledHandleMax, gSampledHciMax;
/* v7.2's initiator pairing. ⚠ Every *Rc is 0x100 | value so "never called" and
 * "returned 0" stay distinguishable -- the ambiguity this project has paid for twice. */
unsigned long gPairAsked, gPairRc, gPairAddrHi, gPairAddrLo;
unsigned long gFirstBadCmdStatus, gBadCmdCount;
unsigned long gPairPath, gLinkWasUp;

/* ★★★★★ TIGER'S PAGING WINDOW, AND WE HAD ONLY BUILT HALF OF IT.
 *
 * v16.0 took Tiger's RESTING page timeout -- 0x2000, 5.12 s -- because our paging
 * deafens the AirPort card in the same Mac, and that cut still stands. But the note
 * beside it records the other half of Tiger's behaviour and we never implemented it:
 * "it raises it to 10.24 s only inside a user-initiated pairing, then restores it
 * immediately".
 *
 * ⚠⚠ AND THE JUSTIFICATION FOR SKIPPING IT IS NOW MEASURED FALSE. That comment argued
 * "a device that is present answers a page in far less than 5 s". 2026-10-04, the
 * A1016 sitting in pairing mode a few feet away:
 *     Create_Connection (0x0405) accepted, send rc 0
 *     bonding completions 1   its status 0x04   = PAGE TIMEOUT
 * A keyboard in pairing mode page-scans slowly to save battery, so 5.12 s is simply
 * too short a window to catch it. BTCheck had already printed the warning -- "Page
 * timeout is Tiger's 0x2000 (5.12 s), not BTstack's 0x6000, for the pairing path that
 * still pages" -- beside the failure, for two runs.
 *
 * ⇒ Do what Tiger does: 10.24 s, but ONLY inside a pairing the user asked for, and put
 * it back immediately afterwards. AirPort keeps priority because the window is bounded
 * and never opens on its own -- there is no sweep any more (deleted v16.1), so nothing
 * pages except an explicit Pair click.
 *
 * ⚠ IT MUST NOT LATCH, and it must not depend on one event to close.
 * GAP_EVENT_DEDICATED_BONDING_COMPLETED arrived for the FIRST time ever on 2026-10-04;
 * before that it read 0 in every log this project ever produced, and the comment at its
 * handler records picking it as a trigger once and being wrong. So it closes the window
 * -- and so does an elapsed-time check that needs no event at all.
 * [[feedback_guards_must_not_latch]] */
#define kPageTimeoutResting  0x2000UL   /* 5.12 s  -- Tiger's resting value   */
#define kPageTimeoutPairing  0x4000UL   /* 10.24 s -- Tiger's pairing window  */
#define kPageWindowMaxMs     30000UL    /* hard stop, ~3x the longest page    */
static unsigned long gPageWinOpenMs;
static int           gPageWinOpen;

static void BT_OpenPagingWindow(void)
{
    gap_set_page_timeout(kPageTimeoutPairing);
    gPageWinOpenMs = (unsigned long)hal_time_ms();
    gPageWinOpen   = 1;
}

/* Safe to call at any time, from any of our contexts, however many times. */
static void BT_ClosePagingWindow(void)
{
    if (!gPageWinOpen) return;
    gPageWinOpen = 0;
    gap_set_page_timeout(kPageTimeoutResting);
}

/* The event-independent half. Called from the HCI event handler, which runs on every
 * event the controller delivers -- and if events have stopped entirely then nothing is
 * transmitting, so a wide page timeout costs AirPort nothing in that case either. */
static void BT_PagingWindowTick(void)
{
    if (!gPageWinOpen) return;
    if (((unsigned long)hal_time_ms() - gPageWinOpenMs) > kPageWindowMaxMs)
        BT_ClosePagingWindow();
}
unsigned long gArmed, gArmedHi, gArmedLo, gArmedFired, gArmedRc;
unsigned long gSuppCmds[2], gSuppCmdsGot;
unsigned long gLocalVer, gLocalMfr;
unsigned long gDelStoredTried, gDelStoredRc, gDelStoredDone, gDelStoredHi, gDelStoredLo;
unsigned long gBadCmdRing[kBadCmdRingLen], gBadCmdIdx;
unsigned long gBondDone, gBondStatus, gWroteKeyTried, gWroteKeyRc, gWroteKeyDone;
unsigned long gHidOutTried, gHidOutRc, gHidOutCid;
unsigned long gJustBonded, gHidOutArmed;
static bd_addr_t gJustBondedAddr;
unsigned long gHandlerEvtRing[kHandlerRingLen], gHandlerEvtIdx, gHandlerEvtTotal;

unsigned long gHidPeerHi, gHidPeerLo;   /* who reached L2CAP (incoming connection) */
unsigned long gConnPeerHi, gConnPeerLo; /* who reached HCI    (connection complete) */
unsigned long gTgtAddrHi, gTgtAddrLo, gTgtCoD, gTgtPick;
unsigned long gHciConnStatus, gHciConnHandle;
/* v12.3: the delete-time disconnect. See BT_DisconnectByAddr. */
unsigned long gDiscReqs, gDiscRc;
unsigned long gSdpIssued, gSdpQueryRc, gSdpAttrBytes, gSdpRecords;
unsigned long gSdpComplete, gSdpStatus;
unsigned long gL2capEvts, gLastUnkEvt;

/* ★★★ Read_Stored_Link_Key, which BTstack does not wrap.
 *
 * OGF 0x03 / OCF 0x0D. BTstack ships only hci_delete_stored_link_key (OCF 0x12), and
 * that it ships ANY of the three is what says the controller-side key store is a real
 * spec feature rather than a CSR extension. Format "B1" is BD_ADDR + Read_All_Flag,
 * the same shape as the Delete wrapper, which is the cross-check that the opcode
 * numbering here is right: BTstack's 0x12 computes to 0x0C12 exactly as the spec says.
 *
 * ⚠ We only READ. Writing a key is the eventual goal for pairing an A1016 from OS 9,
 * but the read is harmless and its Max_Num_Keys answers whether writing is possible at
 * all -- so there is no reason to risk a write before knowing that. */
static const hci_cmd_t bt_read_stored_link_key = {
    HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x0D), "B1"
};

/* ★★★★★ Read_Scan_Enable -- THE ASSUMPTION THIS PROJECT HAS CARRIED SINCE M6.
 *
 * OGF 0x03 / OCF 0x19, no parameters, one byte back: bit 0 inquiry scan, bit 1 PAGE
 * scan. Page scan is the one that matters -- it is what makes a bonded keyboard able
 * to reach us at all.
 *
 * ⚠⚠ WHY IT IS WORTH A COMMAND. "Nothing paged us" has exactly two causes and the
 * block could not tell them apart: the peer never tried, or we were not listening.
 * Everything we had spoke to the first only by assumption. `scan mode requested 3` is
 * gScanMode, a variable WE set when we called gap_connectable_control -- it is our
 * intent, not the controller's state. The command ring records that two 0x0C1A went
 * out but stores opcodes only, not parameters, and BTstack has a path that sends
 * Write_Scan_Enable(0) (hci.c:6202), so two sends are not evidence of two ENABLES.
 *
 * ⇒ Read it back from the card. The v8.8 run spent a boot on a question that reduced
 * to this, and every future "the keyboard did not connect" run reduces to it again.
 *
 * ⚠ RE-READ, not a one-shot, and FIRST plus LAST are both kept. A scan setting that
 * is correct at bring-up and cleared later looks identical to one that was never set
 * if only one sample exists -- the same reason the send gates are sampled twice. */
static const hci_cmd_t bt_read_scan_enable = {
    HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x19), ""
};

/* ★★★★ Write_Stored_Link_Key -- THE COMMAND THE WHOLE GOAL TURNS ON.
 *
 * OGF 0x03 / OCF 0x11. Read_Stored_Link_Key already measured that this controller HAS
 * a store and how big it is: Max_Num_Keys 16, currently holding 2. So putting a key
 * there is the documented route to making the A1044's on-chip proxy stack reconnect a
 * device we paired ourselves -- no CSR vendor command needed, which is what
 * PROXY-FIRST-PLAN §4 spent three runs establishing.
 *
 * ⚠ FORMAT "1BP", AND EVERY CHARACTER IS FROM PRECEDENT RATHER THAN GUESSED.
 * Num_Keys_To_Write is one byte ('1'); the spec then wants BD_ADDR followed by
 * Link_Key per key. BTstack's own hci_link_key_request_reply is "BP" (hci_cmd.c:389),
 * which is exactly that pair for exactly this kind of key -- so "1BP" writes one key
 * in the byte order BTstack already uses successfully for the reply path. 'B' reverses
 * the address onto the wire and 'P' copies the 16 key bytes verbatim (hci_cmd.c:145,
 * 178).
 *
 * ⚠⚠ THIS WRITES TO THE CARD'S PERSISTENT STORE. Unlike every other command this
 * driver has ever sent, it changes state that survives a reboot and that Tiger also
 * depends on -- the store already holds the user's working A1016 pairing. It is
 * therefore sent ONLY for an address we have just successfully bonded with, never
 * speculatively, and never for an address that came from anywhere but our own
 * link-key database. Overwriting a slot the user needs would take away a keyboard
 * that currently works. */
static const hci_cmd_t bt_write_stored_link_key = {
    HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x11), "1BP"
};
unsigned long gStoredKeyRc, gStoredKeyCap;
static int gStoredKeyAsked;
/* ★★ v8.2: set by Delete_Stored_Link_Key's completion to ask for ONE more read.
 *
 * ⚠ A flag rather than a send from inside the event handler, deliberately. The
 * delete's Command Complete arrives at interrupt level, and BT_TryStoredKeyProbe
 * already runs from the pump timer with the send gates checked -- reissuing there
 * reuses a path that is known to work instead of opening a second one. */
static int gStoredKeyReread;

/* ★★ ASK UNTIL IT IS ACCEPTED, AND LATCH ONLY ON SUCCESS.
 *
 * ⚠⚠ THE FIRST VERSION LATCHED BEFORE THE SEND AND THEREFORE NEVER FIRED. It ran from
 * the BTSTACK_EVENT_STATE handler and did:
 *
 *     gStoredKeyAsked = 1;
 *     hci_send_cmd(...);          <- may return ERROR_CODE_COMMAND_DISALLOWED
 *
 * hci_send_cmd refuses when hci_can_send_command_packet_now() is false, which it very
 * plausibly is at the instant BTstack announces HCI_STATE_WORKING, since the bring-up
 * command flow is still unwinding. One refused send and the flag was set forever.
 *
 * Run v6.2 proves it: the A1044 was bound, HCI state 2 = WORKING, 19 commands sent, and
 * the probe still reported NOT ASKED.
 *
 * ⇒ This is [[feedback_guards_must_not_latch]] again, in code I wrote two turns after
 * citing that rule. The flag is now set ONLY when the send is accepted.
 *
 * ⚠ And it is driven from the PUMP TIMER rather than a one-shot event. The timer fired
 * 493 times in that same run, so "retry until accepted" costs nothing and cannot miss
 * its window. */
/* ★★★ v6.5: SAMPLE THE GATES, because "NOT ASKED" happened TWICE and guessing why
 * has now cost two runs.
 *
 * v6.3 bound the A1044 at 8204, reached HCI_STATE_WORKING, fired this timer 3193
 * times with a bail mask of 0 -- and still sent nothing. Our own half of the gate was
 * provably open: kWCmdSent 19 equalled kWCmdComp 19, so gCmdBusy was 0, and gDevice
 * was set. So the refusal is INSIDE BTstack, where we had no counter at all.
 *
 * ⚠ And the probe was not the only thing that went mute. Nothing was ever sent on
 * ACL either (kWAclSent 0), so an inbound L2CAP connection request went unanswered
 * and the link died on a supervision timeout. Two dead directions point at ONE shared
 * gate rather than two coincidences, and hci_can_send_command_packet_now /
 * hci_can_send_acl_classic_packet_now BOTH begin by testing hci_packet_buffer_reserved
 * -- which for an ASYNCHRONOUS transport (ours: we supply can_send_packet_now, so
 * hci_transport_synchronous() is false) stays reserved after every send until
 * HCI_EVENT_TRANSPORT_PACKET_SENT is processed. hci_run() also returns early on that
 * same flag, so a stuck reservation silences the entire stack exactly as observed.
 *
 * ⚠⚠ THAT IS A HYPOTHESIS AND THIS CODE MUST BE ABLE TO REFUTE IT. The state is
 * sampled alongside the gates precisely so the alternative -- that hci_get_state()
 * is not WORKING at timer time and the guard below rejects before any gate is
 * consulted -- is distinguishable rather than assumed. If bit 0 reads TRUE here then
 * BTstack was willing and the fault is ours, and the theory above is dead.
 *
 * FIRST and LAST are both kept: a gate that was bad only briefly would look fine in
 * a last-sample-wins word, and a gate that went bad later would look fine in a
 * first-sample-wins one. Neither alone can be trusted. */
unsigned long gSendRc, gGateFirst, gGateLast, gAclSlots;
static int gHciReady;          /* set the instant hci_init() returns; see BT_StackStart */
/* v6.6's auth-path instrument. gRlkAddr holds ADDRESSES ONLY -- see the 0x15 case. */
unsigned long gRlkEvents, gRlkKeys, gRlkAddr[4][2];
unsigned long gRlkCaptured;   /* v12.0: keys copied into the DB (never the block) */
unsigned long gRlkPasses, gStoredKeyCapFirst;
static unsigned long gRlkPassSeen;   /* which pass gRlkAddr currently describes */
unsigned long gConnReqs, gConnReqHi, gConnReqLo, gConnReqCoD, gConnReqType;
unsigned long gPagedCount, gPagedAddr[kPagedSlots][3];
unsigned long gPagedForgot;   /* v12.5: pager records dropped by a Delete */

/* ★★★★★★ v12.8: THE "STAY DISCONNECTED" STATE.
 *
 * ⚠⚠ THE USER'S OBJECTION, AND IT IS CORRECT: "Why would I want the phone to reconnect
 * instantly after I click disconnect? That doesn't make any sense." v12.5's Disconnect
 * really did drop the link -- the phone's own screen showed it -- and the phone re-paged
 * within a second, so the button's entire effect was undone before it could be seen. A
 * control whose result is reversed faster than a human can look at it is not a control.
 *
 * BTstack accepts inbound connections itself, so making Disconnect mean something needs
 * a state: one address we refuse until the user says otherwise.
 *
 * ⚠ ONE SLOT, NOT A LIST, and deliberately. A blocked device looks BROKEN -- you press
 * keys and nothing happens -- so this is the "can't tell the state by looking" hazard
 * the user has already been bitten by twice. One slot means the panel can always say
 * exactly which device is blocked, there is never a second one to forget about, and
 * clicking Disconnect on anything else releases the previous one.
 *
 * ⚠ AND IT IS DELIBERATELY NOT PERSISTED. It lives in RAM and dies with the driver, so
 * a restart always clears it. A persistent block is an excellent way to lose a keyboard
 * and never work out why. */
unsigned long gBlockHi, gBlockLo, gBlockActive;
unsigned long gBlockDrops;    /* reconnections refused because of it */

/* ★★★★ v12.9: THE LIVE CONNECTIONS, AS A LIST. Published so the panel can enable
 * Disconnect on the row the user actually selected rather than on whichever device
 * happened to connect last. Two slots covers a keyboard and a mouse or phone, which is
 * the whole realistic case for this card. */
/* ⚠ Up here, not next to the registry it belongs to: BT_SampleLiveConns below
 * needs it, and that function comes first in this file. */
#define kHidDevSlots 2
#define kLiveSlots 2
unsigned long gLiveCount, gLiveAddr[kLiveSlots][2];

/* ★★★★★★ v13.0: ASK THE DEVICE ITS OWN NAME.
 *
 * The user: the Name column should say "Pixel 7 Pro" and "A1016 Keyboard", not "Phone"
 * and "Bluetooth Keyboard" -- the Kind column already carries the generic part, so the
 * name column was spending itself on a duplicate of its neighbour.
 *
 * ⚠⚠ ASKED OVER AN EXISTING LINK, AND THAT IS NOT A STYLE CHOICE. Remote_Name_Request
 * on a device that is not connected does an IMPLICIT PAGE, and this card rejects
 * Create_Connection with 0x12 -- the same rejection that means we can never be the side
 * that opens a link. So the only reliable moment to ask is while the device is already
 * connected, which is exactly when Connection Complete fires. Ask once per address per
 * session; the panel remembers the answer in its prefs file so a device that is away
 * still shows its real name.
 *
 * ⚠ 24 characters, not 248. The HCI event carries up to 248 bytes and the block cannot
 * afford that per device; 24 covers "Apple Wireless Keyboard" and "Pixel 7 Pro" with
 * room to spare, and the panel is what displays it. */
#define kNameSlots 2
#define kNameChars 24
unsigned long gNameCount;
unsigned long gNameAddr[kNameSlots][2];
unsigned char gNameText[kNameSlots][kNameChars];
unsigned long gNameReqs, gNameOks, gNameDeferred;
/* v13.1: the stuck-inquiry recovery, and whether it worked. */
unsigned long gInqRetries, gInqRecovered;

static int NameSlotFor(unsigned long hi, unsigned long lo)
{
    unsigned int i;
    for (i = 0; i < gNameCount && i < kNameSlots; i++)
        if (gNameAddr[i][0] == hi && gNameAddr[i][1] == lo) return (int)i;
    return -1;
}

void BT_RequestNameIfUnknown(unsigned long hi, unsigned long lo)
{
    bd_addr_t a;
    if (NameSlotFor(hi, lo) >= 0) return;          /* already asked and answered */
    if (gNameCount >= kNameSlots) return;
    /* ⚠⚠⚠ NEVER WHILE AN INQUIRY IS RUNNING, AND THIS COST A WORKING STACK.
     *
     * v13.0 asked for the name from Connection Complete without considering that a
     * device can connect WHILE A SCAN IS IN PROGRESS -- which is exactly what a paired
     * keyboard does the moment you touch it. Issuing Remote_Name_Request then makes the
     * controller abandon the inquiry, and NO Inquiry Complete event follows. BTstack's
     * inquiry_state is only returned to IDLE by that event, so it stuck at ACTIVE, and
     * from then on every gap_inquiry_start returned 12 = COMMAND_DISALLOWED.
     *
     * ⇒ The user saw "the Scan function continues to scan indefinitely" and "devices are
     * not pairing anymore", which look like two faults and were one: after the first
     * collision, discovery never worked again in that session. Measured, not guessed:
     * gap_inquiry_start rc 12 in the log, with the timer firing 26743 times and the
     * mailbox fully caught up, so nothing else was wrong.
     *
     * ⚠ gInqState is OUR flag, set only on a successful start and cleared on Inquiry
     * Complete -- see BT_ScanStart. Deferring the name to the next connection costs
     * nothing: names are cosmetic and the device will connect again. */
    if (gInqState == 1) { gNameDeferred++; return; }
    a[0] = (uint8_t)((hi >> 16) & 0xFF); a[1] = (uint8_t)((hi >> 8) & 0xFF);
    a[2] = (uint8_t)( hi        & 0xFF); a[3] = (uint8_t)((lo >> 16) & 0xFF);
    a[4] = (uint8_t)((lo >>  8) & 0xFF); a[5] = (uint8_t)( lo        & 0xFF);
    gNameReqs++;
    /* page_scan_repetition_mode 0 and clock_offset 0: both are hints that only matter
     * when PAGING, and we are asking over a link that already exists. */
    (void)gap_remote_name_request(a, 0, 0);
}

/* Defined with the registry, far below; BT_SampleLiveConns needs it here. */
static int HidDevAddrAt(short i, unsigned long *hi, unsigned long *lo);

void BT_SampleLiveConns(void)
{
    unsigned int i;
    gLiveCount = 0;
    for (i = 0; i < kLiveSlots; i++) { gLiveAddr[i][0] = 0; gLiveAddr[i][1] = 0; }
    /* ⚠ Walks OUR published pager/peer knowledge against BTstack's connection list
     * rather than iterating BTstack's internals: the two candidate addresses we could
     * possibly care about are the last peer and each known pager, and asking about a
     * handful of addresses is cheaper and far less fragile than reaching into another
     * library's list structure from a driver. */
    /* ★★★★★★ v15.2: THE OPEN HID CHANNELS COME FIRST, AND THIS IS THE FIX.
     *
     * The two sources below are "the last peer" and "each device that PAGED US". While
     * every connection was incoming that was a complete list. 15.1's sweep made OUTGOING
     * the normal path -- the run showed `incoming connections 0` with two channels open
     * -- so nothing had paged us, the list came back without the keyboard in it, and the
     * panel correctly reported what it had been told: "Paired, disconnected" for a
     * keyboard that was working perfectly.
     *
     * The registry knows every open HID channel whichever direction opened it, which is
     * exactly the fact this list is supposed to carry. Still verified against BTstack's
     * own connection list rather than trusted as a shadow, same as the others. */
    {
        short r;
        for (r = 0; r < kHidDevSlots && gLiveCount < kLiveSlots; r++) {
            unsigned long hi, lo;
            unsigned int j; int dup = 0;
            if (!HidDevAddrAt(r, &hi, &lo)) continue;
            for (j = 0; j < gLiveCount; j++)
                if (gLiveAddr[j][0] == hi && gLiveAddr[j][1] == lo) { dup = 1; break; }
            if (dup) continue;
            if (BT_ConnHandleForAddr(hi, lo) == 0) continue;
            gLiveAddr[gLiveCount][0] = hi;
            gLiveAddr[gLiveCount][1] = lo;
            gLiveCount++;
        }
    }
    if (gConnPeerHi || gConnPeerLo) {
        if (BT_ConnHandleForAddr(gConnPeerHi, gConnPeerLo) != 0
            && gLiveCount < kLiveSlots) {
            gLiveAddr[gLiveCount][0] = gConnPeerHi;
            gLiveAddr[gLiveCount][1] = gConnPeerLo;
            gLiveCount++;
        }
    }
    for (i = 0; i < gPagedCount && i < kPagedSlots && gLiveCount < kLiveSlots; i++) {
        unsigned long hi = gPagedAddr[i][0], lo = gPagedAddr[i][1];
        unsigned int j; int dup = 0;
        if (hi == 0 && lo == 0) continue;
        for (j = 0; j < gLiveCount; j++)
            if (gLiveAddr[j][0] == hi && gLiveAddr[j][1] == lo) { dup = 1; break; }
        if (dup) continue;
        if (BT_ConnHandleForAddr(hi, lo) == 0) continue;
        gLiveAddr[gLiveCount][0] = hi;
        gLiveAddr[gLiveCount][1] = lo;
        gLiveCount++;
    }
}

void BT_SetBlocked(unsigned long hi, unsigned long lo, int on)
{
    if (on) { gBlockHi = hi; gBlockLo = lo; gBlockActive = 1; }
    else    { gBlockHi = 0;  gBlockLo = 0;  gBlockActive = 0; }
}

int BT_IsBlocked(unsigned long hi, unsigned long lo)
{
    return gBlockActive && gBlockHi == hi && gBlockLo == lo;
}

/* ★★★★ v12.5: FORGET ONE PAGER RECORD.
 *
 * ⚠ THIS IS WHAT MAKES "Delete" ACTUALLY REMOVE A DEVICE FROM THE LIST. The panel
 * builds rows from four sources, and the pager list is one of them: a device that has
 * paged us this session is shown even with no bond anywhere, because -- as the note on
 * that builder says -- a keyboard which has lost its host PAGES rather than advertising
 * and would otherwise be in no list at all. Delete removed the bonds and the row stayed,
 * reading "Available", which is honest and is NOT what the user asked for.
 *
 * The user's model, and it is Tiger's: Disconnect ends the link and keeps the device
 * visible; Delete does that AND removes it from the list. Only clearing this record can
 * deliver the second half.
 *
 * ⭐ AND IT IS NOT PERMANENT, which is what keeps the keyboard case working: this drops
 * the RECORD of a past page, not the ability to page. The moment the device pages again
 * it is re-added by the handler above and the row returns. "Removed until it next makes
 * itself known" is exactly the semantics wanted. */
int BT_ForgetPager(unsigned long hi, unsigned long lo)
{
    unsigned int i, j;
    for (i = 0; i < gPagedCount && i < kPagedSlots; i++) {
        if (gPagedAddr[i][0] != hi || gPagedAddr[i][1] != lo) continue;
        /* Compact rather than blank: the panel walks slots 0..gPagedCount-1, so a
         * blanked slot in the middle would end the walk early and hide later pagers. */
        for (j = i + 1; j < gPagedCount && j < kPagedSlots; j++) {
            gPagedAddr[j - 1][0] = gPagedAddr[j][0];
            gPagedAddr[j - 1][1] = gPagedAddr[j][1];
            gPagedAddr[j - 1][2] = gPagedAddr[j][2];
        }
        if (gPagedCount > 0) gPagedCount--;
        gPagedAddr[gPagedCount][0] = 0;
        gPagedAddr[gPagedCount][1] = 0;
        gPagedAddr[gPagedCount][2] = 0;
        gPagedForgot++;
        return 1;
    }
    return 0;
}

unsigned long gAuthComps, gLkNotifs, gLastCmdStatus;

extern int BT_CanSendCommand(void);
extern int BT_CanSendACL(void);

static unsigned long BT_SampleGates(void)
{
    unsigned long b = 0;
    if (hci_can_send_command_packet_now())    b |= 0x01;
    if (hci_is_packet_buffer_reserved())      b |= 0x02;
    if (hci_can_send_acl_classic_packet_now())b |= 0x04;
    if (BT_CanSendCommand())                  b |= 0x08;
    if (BT_CanSendACL())                      b |= 0x10;
    /* ⚠ A "was sampled at all" marker, because every other bit CAN legitimately be 0
     * and HCI_STATE_OFF is 0 too -- so without this an all-zero word could not be told
     * from "this function never ran". That is the same NOT-ASKED-versus-status-zero
     * ambiguity that made the last run unreadable; do not remove it. */
    b |= 0x80;
    /* State in the high byte, so "the guard rejected before any gate mattered" is
     * readable from the same word rather than inferred from another row. */
    return ((unsigned long)hci_get_state() << 8) | b;
}

/* ⚠⚠ v6.6: SAMPLED UNCONDITIONALLY, not from inside the probe.
 *
 * In v6.5 this lived in BT_TryStoredKeyProbe, after the `if (gStoredKeyAsked) return`.
 * The probe succeeded on that run, latched, and the sampling stopped with it -- so
 * gGateLast described the healthy instant of the successful send and NOT the end of
 * the session. BTCheck then printed "can_send_command_now yes" over a stack whose very
 * last event was a Command Status granting zero command credit.
 *
 * ⇒ A diagnostic that stops when the thing it was written for succeeds is a
 * diagnostic that lies about everything afterwards. Split out and called on every
 * timer firing. */
void BT_SampleSendGates(void)
{
    unsigned long g;
    if (!gHciReady) return;      /* hci_stack is not born yet -- see BT_StackStart */
    g = BT_SampleGates();
    gGateLast = g;
    if (gGateFirst == 0) gGateFirst = g;
    gAclSlots = (unsigned long)
                hci_number_free_acl_slots_for_connection_type(BD_ADDR_TYPE_ACL);

    /* ★★★ v7.0: THE TWO PREDICATES, SAMPLED WHILE A LINK IS ACTUALLY UP.
     *
     * These separate the two branches of gap_request_security_level that emit
     * nothing, which GAP_EVENT_SECURITY_LEVEL's absence alone cannot:
     *
     *   gap_security_level(handle)         the `current_level` that function compares
     *                                     against. Verified in hci.c: LEVEL_0 unless
     *                                     the link is encrypted AND authenticated AND
     *                                     the key is long enough. If this reads >= 2
     *                                     with encryption 0, the comparison is not
     *                                     what we think it is.
     *   hci_remote_features_available()    whether our synthetic 0x0B actually left
     *                                     BONDING_RECEIVED_REMOTE_FEATURES set at the
     *                                     moment it was needed. hci_connection_init
     *                                     zeroes bonding_flags "on create AND ON
     *                                     RECONNECT", so a flag set before a re-init
     *                                     would be silently wiped -- and this project
     *                                     has been caught by exactly that shape of
     *                                     latch-then-clear before.
     *
     * ⚠ ONLY WHILE gLastConnHandle IS LIVE. The transport clears it on Disconnection
     * Complete, so these never sample a dead handle -- reading LEVEL_0 off a
     * connection that no longer exists would look identical to reading it off a live
     * one that failed, and that ambiguity is the whole thing being measured.
     *
     * ⚠ FIRST and LAST both kept, for the reason the gate rows already are: a value
     * that was briefly right would vanish from a last-wins word, and one that went
     * wrong later would vanish from a first-wins one. */
    /* ⚠⚠ v7.1: RECORD THE HANDLE WE SAW, NOT JUST A TALLY OF HAVING SEEN ONE.
     *
     * v7.0 gated all of this on `gLastConnHandle != 0` and published only a COUNT.
     * The run came back with live-link samples 0 while the link was demonstrably
     * still up -- ACL links up 1, links down 0, disconnections 0 -- and the synthetic
     * injection had fired once, which REQUIRES that same handle to have been
     * non-zero. Two facts that cannot both describe one variable, and nothing in the
     * block could say which reading was misleading: a count of zero looks identical
     * whether the handle was never set, was cleared early, or the timer only ever
     * looked before the link existed.
     *
     * ⇒ A counter that can only say "it did not happen" cannot distinguish the
     * reasons it did not happen. So the OBSERVATION is recorded rather than a tally
     * of it, and 0-with-750-firings is now a different reading from
     * 0x2E-with-750-firings instead of the same one.
     *
     * ⚠ BOTH handles, from both files, because the cross-file question is itself a
     * candidate: gLastConnHandle is written in hci_transport_os9.c and gHciConnHandle
     * in this file. If those ever disagree, that disagreement IS the finding. */
    gSampledHandle    = gLastConnHandle;
    gSampledHciHandle = gHciConnHandle;
    if (gLastConnHandle > gSampledHandleMax) gSampledHandleMax = gLastConnHandle;
    if (gHciConnHandle  > gSampledHciMax)    gSampledHciMax    = gHciConnHandle;

    /* Sample off whichever handle is live. Preferring neither would mean a cross-file
     * mismatch costs another run to notice. */
    {
        unsigned long h = gLastConnHandle ? gLastConnHandle : gHciConnHandle;
        if (h != 0) {
            unsigned long s = 0x100UL | (unsigned long)
                              gap_security_level((hci_con_handle_t)h);
            unsigned long f = 0x100UL |
                              (hci_remote_features_available(
                                   (hci_con_handle_t)h) ? 1UL : 0UL);
            gSecLvlLast = s;  if (gSecLvlFirst == 0)  gSecLvlFirst = s;
            gRemFeatLast = f; if (gRemFeatFirst == 0) gRemFeatFirst = f;
            gLiveSamples++;
        }
    }
}

/* ★★★★★ REFRESH THE COUNTER BLOCK ON A QUIET RADIO, which nothing did.
 *
 * ⚠⚠ THIS IS THE OTHER HALF OF A DEFECT THAT WAS ONLY EVER HALF FIXED, and it made
 * the v8.8 run unreadable in a way that looked exactly like a hardware answer.
 *
 * BT_StackPoll is the ONLY writer that mirrors the glue's globals into the block, and
 * it is called only from the packet handler -- so only when a USB completion arrives.
 * The comment at the M6b command channel already recorded the consequence, measured on
 * run 47: "after bring-up the interrupt-IN read settles into a proper blocking wait --
 * so on an idle radio nothing ever calls it." The fix then replaced the panel's COMMAND
 * channel with FindSymbol and left the PUBLISH direction where it was, noting
 * "Publishing stays; commanding is gone". Publishing was the half that stayed broken.
 *
 * ⇒ On a run where nothing connects, every counter in the block FREEZES at bring-up,
 * and BTCheck then prints those frozen values under headings that say "last". The v8.8
 * log reported `timer firings 3` and `packet buffer RESERVED: last yes` after 136
 * seconds of uptime, which reads as a dead pump and a wedged send path. Both were
 * artefacts: the timer had fired about 1360 times and nothing had sampled the gates
 * into the block since the third firing. The tell was that the v90 run -- a different
 * driver, months of runs apart -- reported the IDENTICAL 3 and 40. Two runs agreeing
 * to the digit is not a stall, it is a constant.
 *
 * The timer already fires every 100 ms whether or not the radio is busy, so it is the
 * right clock for this. ⚠ Same execution level as the existing callers: the packet
 * handler and this timer are both secondary interrupt level, so nothing moves between
 * levels -- but the audit was re-run anyway, per the standing rule. */
/* ★★★★★★ v9.4: THE PER-LINK COMMAND QUEUE, AND WHY A GUARD WAS NOT ENOUGH.
 *
 * ⚠⚠ THE DEFECT THIS REPLACES WAS MINE, TWICE OVER. v9.3 added a supervision-timeout
 * write to the Connection Complete handler, immediately before the authentication
 * request already living there. Two commands, one command credit: the auth send came
 * back 0x0C COMMAND_DISALLOWED and never went out at all. `inbound links authed 4,
 * its send rc 12`. The diagnostic starved the experiment it shipped alongside -- and
 * I had already guarded BT_TryScanEnableProbe against exactly this, then added an
 * unguarded second command in the very handler the guard was protecting.
 *
 * v9.4 needs FOUR commands per link (supervision, policy read, policy write, auth).
 * A burst of four against one credit cannot work, and a per-command guard would just
 * be four ways to lose a different one. So the shape changes: Connection Complete
 * ENQUEUES, and the 100 ms pump timer DRAINS ONE PER TICK when the controller will
 * actually take it. `command credit granted 0` has been a standing symptom since v8.8;
 * this addresses it as a class instead of case by case.
 *
 * ⚠ A refused send does NOT dequeue -- it retries on the next tick, which is what
 * makes this a queue rather than four hopeful sends. ⚠⚠ But it is BOUNDED PER ENTRY
 * (kLinkCmdMaxTries), because an unbounded retry on a link that has already died would
 * spin for the life of the driver, and a latching-or-spinning guard is the prime
 * suspect for its own symptom ([[feedback_guards_must_not_latch]]). Drops are counted,
 * so a queue that cannot drain says so instead of going quiet.
 *
 * ⚠ Cleared on Disconnection Complete: a command addressed to a dead handle can only
 * come back 0x02, which is the exact ambiguity v9.2 was built to remove.
 *
 * ⚠ Execution level unchanged -- enqueue runs in the packet handler and drain in the
 * pump timer, both secondary interrupt, both serialized by the OS. Plain array stores,
 * no allocation, nothing to lock. */
void BT_LinkCmdEnqueue(unsigned char what, unsigned long handle)
{
    /* ⚠⚠ A NEW LINK MUST NOT INHERIT THE OLD LINK'S QUEUE, and the first draft of
     * this function let it. gLinkCmdHandle was assigned on every enqueue, so if a
     * second link came up while entries for the first were still queued, those
     * entries would be SENT AGAINST THE NEW HANDLE -- a supervision write and an
     * authentication aimed at the wrong link, reported as if they were the right one.
     *
     * In practice the disconnect reset makes the overlap unlikely: a link goes down
     * before the next comes up and the queue is cleared. "Unlikely" is exactly how
     * every last-value-wins defect in this block happened, and the whole point of
     * this queue is that a command's target is unambiguous. Commands for a link that
     * has been superseded are moot, so drop them and count it. */
    if (gLinkCmdHandle != 0 && gLinkCmdHandle != handle && gLinkCmdCount != 0) {
        gLinkCmdDropped += gLinkCmdCount;
        BT_LinkCmdReset();
    }
    if (gLinkCmdCount >= kLinkCmdSlots) { gLinkCmdDropped++; return; }
    gLinkCmdQ[(gLinkCmdHead + gLinkCmdCount) % kLinkCmdSlots] = what;
    gLinkCmdCount++;
    gLinkCmdHandle = handle;
    gLinkCmdEnq++;
    /* ⚠ gLinkCmdTries is NOT reset here. It belongs to the entry at the HEAD, and a
     * later enqueue must not hand the head entry a fresh retry budget -- that would
     * turn a bounded retry into an unbounded one whenever a link enqueues again,
     * which is the latching shape [[feedback_guards_must_not_latch]] warns about. */
}

void BT_LinkCmdReset(void)
{
    gLinkCmdHead = 0;
    gLinkCmdCount = 0;
    gLinkCmdTries = 0;
    gLinkCmdHandle = 0;
}

/* Drained from the pump timer, ONE per tick. Returns nothing: every outcome is a
 * counter, because a queue whose failures are invisible is worse than no queue. */
void BT_LinkCmdPump(void)
{
    unsigned char what;
    unsigned long rc;

    if (gLinkCmdCount == 0) return;
    if (!gHciReady) return;
    if (gLinkCmdHandle == 0) { BT_LinkCmdReset(); return; }

    what = gLinkCmdQ[gLinkCmdHead];

    switch (what) {
    case kLinkCmdSupervision:
        rc = (unsigned long)(hci_send_cmd(&hci_write_link_supervision_timeout,
                 (hci_con_handle_t)gLinkCmdHandle, 0xFA00) & 0xFF);
        if (rc == 0) { gSuperTimeoutTried++; gSuperTimeoutHandle = gLinkCmdHandle; }
        gSuperTimeoutRc = 0x100UL | rc;
        break;
    case kLinkCmdPolicyRead:
        rc = (unsigned long)(hci_send_cmd(&hci_read_link_policy_settings,
                 (hci_con_handle_t)gLinkCmdHandle) & 0xFF);
        if (rc == 0) gPolicyReadSends++;
        gPolicyReadRc = 0x100UL | rc;
        break;
    case kLinkCmdPolicyWrite:
        /* ⚠ SNIFF ONLY (0x0004), deliberately NOT role switch. Role switch was
         * eliminated by reading at v9.3 and enabling it here would put the variable
         * straight back in. BTstack's gap.h calls role-switch|sniff the common value;
         * we want the single-variable version of it. */
        rc = (unsigned long)(hci_send_cmd(&hci_write_link_policy_settings,
                 (hci_con_handle_t)gLinkCmdHandle,
                 (uint16_t)LM_LINK_POLICY_ENABLE_SNIFF_MODE) & 0xFF);
        if (rc == 0) gPolicyWriteSends++;
        gPolicyWriteRc = 0x100UL | rc;
        break;
    case kLinkCmdAuth:
        rc = (unsigned long)(hci_send_cmd(&hci_authentication_requested,
                 (hci_con_handle_t)gLinkCmdHandle) & 0xFF);
        if (rc == 0) { gAutoAuthTried++; gAutoAuthHandle = gLinkCmdHandle; }
        gAutoAuthRc = 0x100UL | rc;
        break;
    default:
        rc = 0;                  /* unknown id: drop it rather than spin */
        break;
    }

    if (rc == 0) {
        gLinkCmdHead = (gLinkCmdHead + 1) % kLinkCmdSlots;
        gLinkCmdCount--;
        gLinkCmdTries = 0;
        gLinkCmdSent++;
    } else {
        gLinkCmdRefused++;
        gLinkCmdTries++;
        if (gLinkCmdTries >= kLinkCmdMaxTries) {
            gLinkCmdHead = (gLinkCmdHead + 1) % kLinkCmdSlots;
            gLinkCmdCount--;
            gLinkCmdTries = 0;
            gLinkCmdDropped++;
        }
    }
}

/* ★★★★★ ARE WE ACTUALLY PAGE-SCANNING? See bt_read_scan_enable for why this exists.
 *
 * ⚠ Every 50th timer firing, so about every 5 seconds: often enough to catch a
 * setting that gets cleared mid-run, rare enough that it cannot crowd out a real
 * command. ⚠ NOT LATCHING and NOT BOUNDED -- a refused send simply retries on the next
 * eligible firing, which is the shape [[feedback_guards_must_not_latch]] requires. The
 * refusal is still visible: gScanEnaRc records the last send's return code. */
void BT_TryScanEnableProbe(void)
{
    static unsigned long tick;
    uint8_t rc;

    if (!gHciReady) return;
    if (hci_get_state() != HCI_STATE_WORKING) return;
    /* ⚠⚠ NEVER WHILE A LINK IS LIVE, and this guard is not tidiness -- without it the
     * diagnostic could break the experiment it was added to serve. A command every
     * 5 seconds can land on the same command credit as the Authentication Requested
     * that v8.8 fires from Connection Complete, and THAT send is not retried: a
     * refusal there loses the whole run. Once a link exists the question this probe
     * answers is already answered -- we were plainly reachable -- so there is nothing
     * to lose by standing down and a run to lose by not. */
    if (gLastConnHandle != 0) return;
    if ((tick++ % 50) != 0) return;
    rc = hci_send_cmd(&bt_read_scan_enable);
    gScanEnaRc = 0x100UL | (unsigned long)rc;
    if (rc == ERROR_CODE_SUCCESS) gScanEnaSends++;
}

void BT_PublishFromTimer(void)
{
    if (!gHciReady) return;      /* hci_stack is not born yet -- see BT_StackStart */
    gPublishMs = hal_time_ms();  /* ⚠ BEFORE the poll, so the word it publishes is the
                                  * time of THIS refresh and not the previous one. */
    gPublishRuns++;
    BT_StackPoll((unsigned long)hci_get_state());
}

void BT_TryStoredKeyProbe(void)
{
    bd_addr_t any;
    uint8_t rc;

    if (gStoredKeyAsked && !gStoredKeyReread) return;
    if (!gHciReady) return;      /* hci_stack is not born yet -- see BT_StackStart */
    if (hci_get_state() != HCI_STATE_WORKING) return;
    memset(any, 0, sizeof(any));
    /* Read_All_Flag = 1: report the whole store, so the address is ignored. */
    rc = hci_send_cmd(&bt_read_stored_link_key, any, 1);
    /* ⚠ 0x100 | rc, the block's convention: a plain 0 could not be told from
     * "never reached", and that exact ambiguity is what made the last run unreadable. */
    gSendRc = 0x100UL | (unsigned long)rc;
    if (rc == ERROR_CODE_SUCCESS) {
        gStoredKeyAsked  = 1;
        gStoredKeyReread = 0;   /* ⚠ CLEARED ONLY ON A SUCCESSFUL SEND, so a refusal
                                 * retries on the next timer instead of dropping the
                                 * re-read silently. Same shape, and same reason, as
                                 * gStoredKeyAsked above. */
        gRlkPasses++;
        /* ★★★ CLEAR THE ANSWER HERE, AT ISSUE TIME -- NOT IN THE 0x15 HANDLER.
         *
         * ⚠⚠ The first version of this cleared gRlkAddr when a Return_Link_Keys event
         * arrived carrying a new pass number, and that is WRONG IN THE ONE CASE THAT
         * MATTERS MOST: a store the delete has emptied sends NO 0x15 EVENT AT ALL.
         * Num_Keys_Read comes back 0, no event follows, the clear never runs, and the
         * panel goes on listing an address the card no longer holds -- the exact stale
         * row this whole mechanism exists to remove, made permanent.
         *
         * Clearing at issue time cannot depend on event ordering, which the spec
         * leaves free between the Command Complete and the 0x15 events. Nothing from
         * this read can have arrived yet, so it is unambiguous. The cost is that the
         * card rows are empty for the fraction of a second until the answer lands,
         * which is honest: we genuinely do not know them yet. */
        gRlkKeys = 0;
        memset(gRlkAddr, 0, sizeof(gRlkAddr));
    }
}
unsigned long gScanMode;      /* 1 = connectable asked, 3 = + discoverable asked */
/* ⚠ Starts 1 because bring-up enables both scans -- the radio IS on before the panel
 * ever asks. A default of 0 would make the panel draw "Off" over a live radio. */
unsigned long gRadioOn = 1;
unsigned long gDiscReason, gDiscHandle, gDiscCount;
unsigned long gIoCapReqs, gUserConfReqs, gPinReqs, gAuthComplete;
unsigned long gSspAuto, gLinkKeyReqs;
unsigned long gSimplePairing, gEncryptChange, gEncryptOn;
unsigned long gSdpRetries, gSdpRetryRc, gSdpNotReady;
unsigned long gSdpEventsAny, gSdpLastEvt;
unsigned long gConnOks, gSdpOks, gAllocFails;
unsigned long gInqPeriph, gInqPhones, gInqComputers, gInqOther;
unsigned long gInqAudio;      /* major class 0x04 -- headphones, speakers, car kits */
unsigned long gInqPeriphAddrHi, gInqPeriphAddrLo, gInqPeriphCoD;
unsigned long gRespStored, gRespDisplaced;
unsigned long gSecReqs;

/* ---- M3.7: legacy PIN pairing ---------------------------------------------- *
 * The PIN the host offers for legacy (pre-Bluetooth-2.1) pairing. The user types
 * this on the keyboard, then Return.
 *
 * ⚠ ONE named constant on purpose. With no GUI the PIN has to be fixed and
 * documented, and "0000" is the near-universal default that era hardware accepts.
 * The control panel's job is to replace this with a per-pairing value it displays
 * (docs/M3-DESIGN.md §5b-bis), so keeping it in one place is what makes that a
 * small change rather than a hunt. */
/* ⚠⚠ THIS MUST HAVE STATIC STORAGE DURATION. gap_pin_code_response_binary does
 *     hci_stack->gap_pairing_input.gap_pairing_pin = pin_data;
 * i.e. it RETAINS THE POINTER and does not copy the bytes -- the PIN is read later,
 * when hci_run reaches GAP_PAIRING_STATE_SEND_PIN. A string literal is fine because
 * it lives in the fragment's constant data for the life of the driver. Passing a
 * stack buffer would dangle, and the failure would be an intermittently wrong PIN
 * rather than a crash, which is the worst kind to debug on hardware with no
 * debugger. Checked in vendor/btstack/src/hci.c, not assumed. */
#define kBTLegacyPin "0000"

/* Safe at interrupt level, and verified rather than assumed: gap_pin_code_response
 * only assigns a few fields and calls gap_pairing_set_state_and_run. No allocation,
 * no File Manager, no blocking -- the same shape as the gap_ssp_* and
 * gap_request_security_level calls this handler already makes. */

unsigned long gPinAnswered;   /* PIN requests we actually answered              */
unsigned long gPinRespRc;     /* 0x100 | gap_pin_code_response return           */
unsigned long gPinAddrHi;     /* who asked, high 3 bytes                        */
unsigned long gPinAddrLo;     /* ... and low 3                                  */

/* The peer we selected, kept by value. gHaveTarget gates the retry. */
static bd_addr_t gTgtAddr;
static int       gHaveTarget;

/* ---- what the inquiry found --------------------------------------------- *
 * A fixed four-entry table, deliberately not a pool: this runs below task level
 * where there is no allocator, and four is enough to choose sensibly from while
 * costing 40 bytes of BSS. Overflow is counted (gInqResults keeps climbing) but
 * not stored, so a crowded RF environment cannot overrun anything. */
/* ⚠ WAS 4, AND THAT WAS TOO SMALL TO BE HONEST. Real runs see 12-15 responders in a
 * populated room -- which is why the per-class tallies had to exist at all, since the
 * table was silently dropping most of what answered. The control panel's scan list
 * reads this table, so a cap of 4 would show the user four devices and quietly discard
 * the rest, including quite possibly the one they were looking for. */
#define BT_MAX_RESPONDERS 16
static bd_addr_t gRespAddr[BT_MAX_RESPONDERS];
/* clock offset << 16 | page-scan repetition mode << 8. See the inquiry handler. */
uint32_t         gRespParm[BT_MAX_RESPONDERS];
static uint32_t  gRespCoD[BT_MAX_RESPONDERS];
static int       gRespCount;

/* Why a particular responder was chosen, recorded alongside the index so the run
 * is interpretable after the fact instead of "it connected to something".
 * The values are the Bluetooth major device class numbers, which is why
 * PERIPHERAL is 5 and PHONE is 2 rather than 1 and 2. */
#define BT_PICK_FALLBACK    0
#define BT_PICK_COMPUTER    1
#define BT_PICK_PHONE       2
#define BT_PICK_AUDIO       4     /* Audio/Video: headphones, speakers, car kits */
#define BT_PICK_PERIPHERAL  5

static void sdp_query_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size);
static void retry_sdp_query_now_encrypted(void);

/* Major device class lives in bits 8..12 of the 24-bit class of device. */
static uint8_t cod_major(uint32_t cod)
{
    return (uint8_t)((cod >> 8) & 0x1Fu);
}

/* ======================= M6: THE SCAN CHANNEL ==============================
 * Two entry points the driver's command handler calls on the control panel's
 * behalf. They live here, next to the responder table, rather than reaching into
 * it from bt_probe.c -- the table's layout is this file's business.
 * docs/SCAN-DESIGN.md.
 */

/* Start an inquiry on demand. ⚠ Longer than the 5.1 s used at startup: the panel's
 * user is watching a list fill and has chosen to wait, whereas the startup inquiry
 * must not make the machine look hung. 8 units of 1.28 s is about 10.2 s, which is
 * also the standard inquiry length most stacks use. */
unsigned long BT_ScanStart(void)
{
    gRespCount    = 0;          /* a new scan starts from an empty list */
    gRespStored   = 0;
    gInqResults   = 0;
    gInqPeriph = gInqPhones = gInqComputers = gInqAudio = gInqOther = 0;
    gInqComplStatus = 0;
    /* ⚠⚠ SET gInqState ONLY ON SUCCESS. It used to be set BEFORE the call, so a
     * REFUSED start left the driver reporting "scanning" with no inquiry running to
     * ever complete it. The panel greys Set Up New Device on that state, so the button
     * went permanently dead -- and relaunching the panel could not help, because the
     * stuck value lives in the DRIVER. Run 58: inquiry 1, gap_inquiry_start rc 12,
     * Inquiry Complete NOT RECEIVED, button disabled for good.
     *
     * That is a latching state, which is the shape this project has been bitten by
     * repeatedly -- and this time the latch was in the reporting rather than a guard. */
    {
        int rc = gap_inquiry_start(8);
        gInqStartRc = 0x100UL | (unsigned long)rc;
        if (rc == 0) {
            gInqState = 1;
        } else if (rc == 12) {
            /* ⚠⚠ AND RETRY ONCE AFTER CANCELLING, which this never did. The cancel
             * below has always been correct and has always been USELESS on its own:
             * it clears the stuck state and returns, so the click the user actually
             * made is thrown away and the button appears dead. They then click again,
             * it works, and the first press looks like a bug in the scan.
             *
             * ⚠ The retry is bounded to ONE attempt, deliberately. If the cancel did
             * not clear the state there is something wrong the driver cannot fix by
             * asking harder, and a loop here would be a loop inside the mailbox
             * servicer at interrupt level. One retry turns a wasted click into a
             * working one; a second would only hide a real fault. */
            /* ★ ERROR_CODE_COMMAND_DISALLOWED: BTstack thinks an inquiry is still
             * active. Cancel it so the NEXT press can succeed, instead of leaving the
             * user with a control panel that refuses forever. gap_inquiry_stop is
             * asynchronous (W2_CANCEL -> W4_CANCELLED -> IDLE), so this press still
             * fails; it is the recovery for the one after it. */
            gInqStopRc = 0x100UL | (unsigned long)gap_inquiry_stop();
            gInqRetries++;
            rc = gap_inquiry_start(8);
            gInqStartRc = 0x100UL | (unsigned long)rc;
            if (rc == 0) { gInqState = 1; gInqRecovered++; }
        }
    }
    return gInqStartRc;
}

/* Publish the responder table into the shared counter block, four words per entry:
 *   +0 address high 3 bytes      +1 address low 3 bytes
 *   +2 class of device           +3 clock offset << 16 | page-scan rep mode << 8
 * Returns how many entries were written. */
short BT_ScanPublish(unsigned long *blk, short base, short maxEntries)
{
    short i, n = (short)gRespCount;
    if (blk == 0) return 0;
    if (n > maxEntries) n = maxEntries;
    for (i = 0; i < n; i++) {
        blk[base + i * 4 + 0] = ((unsigned long)gRespAddr[i][0] << 16)
                              | ((unsigned long)gRespAddr[i][1] << 8)
                              |  (unsigned long)gRespAddr[i][2];
        blk[base + i * 4 + 1] = ((unsigned long)gRespAddr[i][3] << 16)
                              | ((unsigned long)gRespAddr[i][4] << 8)
                              |  (unsigned long)gRespAddr[i][5];
        blk[base + i * 4 + 2] = (unsigned long)gRespCoD[i];
        blk[base + i * 4 + 3] = (unsigned long)gRespParm[i];
    }
    return n;
}

/* ---- choose a peer and start an SDP query against it --------------------- *
 * Preference order is deliberate and is about making the result MEAN something:
 *
 *   PERIPHERAL (5) first, because that is a keyboard or mouse -- the actual
 *     target of this whole project, and the one device whose SDP record we
 *     ultimately need to parse at M4.
 *   PHONE (2), then COMPUTER (1), because those reliably answer an SDP browse
 *     without demanding authentication first, which keeps this test inside M2
 *     instead of accidentally depending on M3 pairing.
 *   Anything at all, as a last resort, with the reason recorded as FALLBACK.
 *
 * ⚠ We do NOT simply take the first responder. In a normal room the first
 * responder may be a neighbour's car or headphones, and the run would then be
 * ambiguous through no fault of the code -- which this project's test discipline
 * explicitly forbids. Recording the chosen address, its class and the reason
 * makes the outcome readable even when the choice was poor. */
static void start_sdp_query_on_best_responder(void)
{
    static const uint8_t prefs[3] = { BT_PICK_PERIPHERAL, BT_PICK_PHONE,
                                      BT_PICK_COMPUTER };
    int chosen = -1;
    uint8_t why = BT_PICK_FALLBACK;
    int i, p;

    for (p = 0; p < 3 && chosen < 0; p++) {
        for (i = 0; i < gRespCount; i++) {
            if (cod_major(gRespCoD[i]) == prefs[p]) { chosen = i; why = prefs[p]; break; }
        }
    }
    if (chosen < 0) {
        if (gRespCount == 0) return;        /* nothing answered; counters say so */
        chosen = 0;
        why = BT_PICK_FALLBACK;
    }

    gTgtAddrHi = ((unsigned long)gRespAddr[chosen][0] << 16)
               | ((unsigned long)gRespAddr[chosen][1] << 8)
               |  (unsigned long)gRespAddr[chosen][2];
    gTgtAddrLo = ((unsigned long)gRespAddr[chosen][3] << 16)
               | ((unsigned long)gRespAddr[chosen][4] << 8)
               |  (unsigned long)gRespAddr[chosen][5];
    gTgtCoD    = gRespCoD[chosen];
    gTgtPick   = ((unsigned long)chosen << 8) | why;

    /* Keep the chosen address in its own variable rather than re-deriving it from an
     * index later. The retry in §7 of docs/M3-DESIGN.md happens in a different event,
     * possibly after the responder table has been overwritten by a later inquiry, and
     * depending on `chosen` still being valid then would be a latent bug. */
    memcpy(gTgtAddr, gRespAddr[chosen], sizeof(bd_addr_t));
    gHaveTarget = 1;

    /* PUBLIC_BROWSE_ROOT (0x1002) asks for every service in the peer's browse
     * group rather than one profile, which is the right probe here: we are
     * proving the L2CAP round trip, not looking for a specific service yet.
     *
     * This one call is what exercises the whole layer. sdp_client opens an L2CAP
     * channel to the SDP PSM (0x0001) for us, which means BTstack must run an ACL
     * connection, L2CAP connection signalling, L2CAP configuration, and then a
     * real request/response over the bulk pipes. Every one of those is untested
     * before this run. */
    gSdpIssued  = 1;
    gSdpQueryRc = 0x100UL | (unsigned long)
        sdp_client_query_uuid16(&sdp_query_handler, gTgtAddr,
                                BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
}

/* ---- the retry that closes M2 ------------------------------------------- *
 * ⚠ WHY A RETRY IS NEEDED AT ALL, from run 18.
 *
 * The first query is issued at GAP_EVENT_INQUIRY_COMPLETE, which is BEFORE any
 * pairing has happened. The phone declined to serve SDP to an unpaired peer, the
 * query completed with status 0 and ZERO attribute bytes, and pairing then succeeded
 * afterwards on a link nobody was querying any more. So the data was never wrong --
 * it was asked for at the wrong moment.
 *
 * ⚠ BOUNDED, AND NOT BY A LATCH. This project's rule is that a guard must be bounded
 * per request rather than latched permanently, because a latching guard becomes the
 * prime suspect for its own symptom. So: at most kSdpMaxRetries attempts, counted, and
 * the count is visible in the block. If the cap is ever hit, that is a finding rather
 * than a silent stop. */
#define kSdpMaxRetries 1

static void retry_sdp_query_now_encrypted(void)
{
    if (!gHaveTarget) return;

    /* ⚠⚠ THE GATE THAT RUN 20 MADE NECESSARY, AND IT COMES FIRST.
     *
     * This retry was added in v1.8 to fix "SDP returns nothing", a symptom that did
     * not exist: the SDP counter had been watching an event BTstack never emits, and
     * once that was corrected run 20 returned 909 attribute bytes across 8 records
     * on the FIRST query. So the retry solved nothing -- and it actively hurt.
     *
     * Run 20's two `86`s were BTSTACK_MEMORY_ALLOC_FAILED, caused by this function:
     * after the peer disconnected, it asked for a SECOND connection while
     * MAX_NR_HCI_CONNECTIONS was 1 and the first object had not been released, so
     * hci.c:8435 synthesised a CONNECTION_COMPLETE carrying that error. A fully
     * successful run therefore displayed two alarming statuses.
     *
     * Rather than delete the retry, gate it on the symptom it was meant to address.
     * It stays useful for the genuine case -- a peer that really does refuse SDP
     * until the link is encrypted -- while never firing on a query that already
     * worked. If bytes have arrived, there is nothing to retry. */
    if (gSdpAttrBytes > 0) return;

    if (gSdpRetries >= kSdpMaxRetries) return;

    /* sdp_client serialises one query at a time. Asking while it is mid-transaction
     * would be refused anyway; recording the refusal separately means a "no bytes"
     * result can never be confused with "we never actually asked". */
    if (!sdp_client_ready()) { gSdpNotReady++; return; }

    gSdpRetries++;
    gSdpRetryRc = 0x100UL | (unsigned long)
        sdp_client_query_uuid16(&sdp_query_handler, gTgtAddr,
                                BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
}

/* ---- SDP results -------------------------------------------------------- *
 * Counting bytes rather than parsing them is the whole point at this milestone.
 * A single SDP_EVENT_QUERY_ATTRIBUTE_BYTE proves the round trip end to end:
 * ACL up, L2CAP channel open and configured, request sent, response parsed. The
 * record structure is an M4 problem and needs the keyboard, not a phone. */
static void sdp_query_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size)
{
    static uint16_t last_record = 0xFFFF;
    uint16_t rec;

    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) return;

    /* ⚠ COUNT EVERY SDP EVENT, WHATEVER IT IS. This catch-all is not padding: it is
     * what makes a wrong expectation self-diagnosing instead of silent. See the note
     * on SDP_EVENT_QUERY_ATTRIBUTE_VALUE below and docs/M3-DESIGN.md §8. */
    gSdpEventsAny++;

    switch (hci_event_packet_get_type(packet)) {

    /* ⚠⚠ IT IS _VALUE (0x94), NOT _BYTE (0x93). THIS COST THREE RUNS.
     *
     * This case read SDP_EVENT_QUERY_ATTRIBUTE_BYTE, which sdp_client.c NEVER emits --
     * `sdp_parser_emit_value_byte()` emits SDP_EVENT_QUERY_ATTRIBUTE_VALUE, and 0x93
     * appears nowhere in the vendored compile set. So `SDP attribute bytes` was
     * structurally incapable of incrementing, and its reading of 0 in runs 17, 18 and
     * 19 was evidence of nothing at all. The retry that v1.8 added was designed against
     * that non-measurement.
     *
     * The parser emits one event PER BYTE, so counting events is genuinely counting
     * attribute bytes and the label stays honest. */
    case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
        gSdpAttrBytes++;
        rec = sdp_event_query_attribute_value_get_record_id(packet);
        if (rec != last_record) { last_record = rec; gSdpRecords++; }
        break;

    case SDP_EVENT_QUERY_COMPLETE:
        {
            uint8_t st = sdp_event_query_complete_get_status(packet);
            gSdpComplete = 1;
            gSdpStatus   = 0x100UL | (unsigned long)st;
            if (st == 0) gSdpOks++;
            if (st == BTSTACK_MEMORY_ALLOC_FAILED) gAllocFails++;
        }
        break;

    default:
        /* First unrecognised SDP event wins, so a surprise is attributable rather
         * than averaged away. */
        if (gSdpLastEvt == 0)
            gSdpLastEvt = (unsigned long)hci_event_packet_get_type(packet);
        break;
    }
    BT_StackPoll((unsigned long)hci_get_state());
}

/* Observer only. BTstack requires a registered handler to deliver events to, and
 * this one exists so that the state transitions show up in the counter block.
 * It must stay cheap: it runs in the same interrupt as the packet that caused it.
 *
 * The interesting transition is BTSTACK_EVENT_STATE reaching HCI_STATE_WORKING,
 * which is the M2 gate: it means BTstack completed its own bring-up over our
 * transport. L2CAP work starts after that, not before. */
/* ★★★★★★ M5 STEP 1: ONE HOME FOR THE DECODE AND ITS INSTRUMENTS.
 *
 * ⚠⚠ WHY THIS IS A FUNCTION AND NOT INLINE, and it cost a hardware run to learn.
 * v10.2 wired the decode into the l2cap packet handler below -- which is DORMANT
 * whenever kHidUseBtstackHost is 1, because BTstack's hid_host owns the channels
 * and delivers reports through HID_SUBEVENT_REPORT instead. The run captured a
 * perfect 11-byte report (A1 01 20 ...), counted 50 of them, and reported the
 * decoder as NOT RECEIVED, because the decoder was never reached. Exactly v9.5's
 * inert gate: code wired into a path that does not execute.
 * [[feedback_control_must_exercise_the_change]]
 *
 * ⇒ Both paths now call this. Whichever one runs, the decode and every counter
 * behind it run with it, and neither can drift from the other.
 */
/* ================================================================================
 *  TWO HID DEVICES AT ONCE -- the A1016 and the A1015 together.
 * ================================================================================
 *
 * ⛔ UNTIL 15.0 THIS STACK COULD HOLD EXACTLY ONE. The A1015 run proved it from the
 * other side: the mouse PAIRED (fresh bond 1, name "Apple Wireless Mouse") and then sat
 * there -- `connects attempted 0`, `incoming connections 1`, all from the keyboard. Three
 * separate things stopped it, and only fixing all three gets a cursor moving:
 *
 *   1. MAX_NR_HID_HOST_CONNECTIONS was 1, so a second hid_host connection could not
 *      exist even if something asked for one;
 *   2. MAX_NR_L2CAP_CHANNELS was 3, and each HID device needs TWO (control + interrupt);
 *   3. nothing ever initiates a connection to a bonded device, and unlike a keyboard --
 *      which pages the host on a keypress -- this mouse never paged us.
 *
 * ⚠⚠ AND A FOURTH, WHICH IS THE ONE THAT WOULD HAVE BEEN A SILENT WRONG ANSWER.
 * gBhCid and gBhPeerAddr are single. With two devices connected, the battery probe sends
 * to gBhCid and the response handler attributes the answer to gBhPeerAddr -- so a reply
 * arriving after the OTHER device connected would have been filed against the wrong
 * device's address. That is the same class of defect as the panel reading kWBatPctVal,
 * and it does not announce itself: you get a plausible percentage on the wrong row.
 *
 * ⭐ THE REGISTRY IS KEYED BY hid_cid, which every HID subevent carries. Roles are NOT
 * assigned from the class of device: they are learned from WHICH DECODER ACCEPTS a
 * report from that cid. The device tells us what it is by what it sends, which needs no
 * CoD lookup and cannot disagree with the data actually arriving.
 *
 * ⚠ gBhCid/gBhPeerAddr are kept as THE KEYBOARD'S, deliberately. Every existing
 * keyboard path -- the Caps LED, the reconnect, the battery attribution -- then behaves
 * exactly as it did when a keyboard was the only possibility, which is the property that
 * makes this change safe to make to a path that took eleven builds to stabilise. */

typedef struct {
    unsigned long cid;          /* 0 = free slot                                  */
    bd_addr_t     addr;
    unsigned long isKeyboard;   /* learned from the first report that decodes     */
    unsigned long isMouse;
    unsigned long reports;
    unsigned char batDone;      /* v15.2: battery chain has been run for this device */
    unsigned long conHandle;    /* v15.5: HCI handle, so Mode_Change can be attributed */
    unsigned long batPct;       /* v15.3: last level THIS device reported, 0 = none   */
    unsigned long batHs;        /* v15.3: 0x100 | its handshake, so "declined" shows  */
} BTHidDev;

static BTHidDev gHidDev[kHidDevSlots];
unsigned long gHidDevOpens, gHidDevFull, gHidDevCloses;

/* ⚠ The registry is static to this file, so the publisher in bt_probe.c cannot read it
 * directly. One accessor rather than eight externs, and it packs the addresses in the
 * project's 3+3 split so BTCheck's RowBDAddr prints them like every other address. */
/* ⚠ SIX WORDS PER SLOT since v15.3 (was four). The caller's array must match; the
 * name says how many so a stale caller is a compile error rather than a silent
 * over-read of the next slot's cid. */
void BT_HidDevSnapshot12(unsigned long *out12)
{
    short i;
    for (i = 0; i < kHidDevSlots; i++) {
        const BTHidDev *d = &gHidDev[i];
        out12[i * 6 + 4] = d->batPct;
        out12[i * 6 + 5] = d->batHs;
    }
    for (i = 0; i < kHidDevSlots; i++) {
        const BTHidDev *d = &gHidDev[i];
        unsigned long *out8 = out12;
        out8[i * 6 + 0] = d->cid;
        out8[i * 6 + 1] = ((unsigned long)d->addr[0] << 16)
                        | ((unsigned long)d->addr[1] << 8) | d->addr[2];
        out8[i * 6 + 2] = ((unsigned long)d->addr[3] << 16)
                        | ((unsigned long)d->addr[4] << 8) | d->addr[5];
        out8[i * 6 + 3] = (d->isKeyboard ? 1UL : 0UL) | (d->isMouse ? 2UL : 0UL);
    }
}

/* ⚠ handle 0 is a real HCI handle value in principle, but we only ever store a handle
 * for a slot we have claimed, so 0 here means "not recorded" and must not match. */
static short HidDevSlotForHandle(unsigned long h)
{
    short i;
    for (i = 0; i < kHidDevSlots; i++)
        if (gHidDev[i].cid != 0 && gHidDev[i].conHandle != 0 && gHidDev[i].conHandle == h)
            return i;
    return -1;
}

static short HidDevSlotForCid(unsigned long cid)
{
    short i;
    if (cid == 0) return -1;
    for (i = 0; i < kHidDevSlots; i++)
        if (gHidDev[i].cid == cid) return i;
    return -1;
}

/* Claim a slot for a newly OPENED connection. ⚠ Idempotent: hid_host can report the
 * same cid opening once per direction, and a second claim must not consume a slot. */
static void HidDevClaim(unsigned long cid, const bd_addr_t addr)
{
    short i;
    if (cid == 0) return;
    if (HidDevSlotForCid(cid) >= 0) return;
    for (i = 0; i < kHidDevSlots; i++) {
        if (gHidDev[i].cid == 0) {
            short k;
            gHidDev[i].cid        = cid;
            for (k = 0; k < 6; k++) gHidDev[i].addr[k] = addr[k];
            gHidDev[i].isKeyboard = 0;
            gHidDev[i].isMouse    = 0;
            gHidDev[i].reports    = 0;
            gHidDev[i].batDone    = 0;   /* a device that returns gets a fresh reading */
            gHidDev[i].conHandle  = 0;
            gHidDev[i].batPct     = 0;
            gHidDev[i].batHs      = 0;
            gHidDevOpens++;
            return;
        }
    }
    /* ⚠ COUNTED, NOT SILENT. A third device is out of scope, but a slot table that
     * quietly drops one is how "my mouse stopped working" becomes unexplainable. */
    gHidDevFull++;
}

static void HidDevRelease(unsigned long cid)
{
    short i = HidDevSlotForCid(cid);
    if (i < 0) return;
    gHidDev[i].cid = 0;
    gHidDevCloses++;
}

/* 3+3, the project-wide split. Returns 0 for an empty slot. */
static int HidDevAddrAt(short i, unsigned long *hi, unsigned long *lo)
{
    if (i < 0 || i >= kHidDevSlots || gHidDev[i].cid == 0) return 0;
    *hi = ((unsigned long)gHidDev[i].addr[0] << 16)
        | ((unsigned long)gHidDev[i].addr[1] << 8)
        |  (unsigned long)gHidDev[i].addr[2];
    *lo = ((unsigned long)gHidDev[i].addr[3] << 16)
        | ((unsigned long)gHidDev[i].addr[4] << 8)
        |  (unsigned long)gHidDev[i].addr[5];
    return 1;
}

static void BhRecordReport(unsigned long cid,
                           const unsigned char *rep, unsigned short n)
{
    /* ★★★★★★ M5 STEP 1: THE REPORT-PROTOCOL DECODER, LIVE.
     *
     * ⚠⚠ THIS USED TO CALL BT_DecodeBootKeyboard, AND v10.0 PROVED THAT WRONG.
     * The moment SET_PROTOCOL(REPORT) started working, every report arrived as
     * eleven bytes -- A1 01 + nine -- and the boot decoder rejected all 93 of them
     * because it accepts only the 8-byte form. The log said "Data arrived and the
     * decoder REJECTED it", which was correct behaviour reported as a fault.
     *
     * BT_DecodeHidReport handles BOTH personalities through one entry point, so
     * this path no longer depends on which protocol the keyboard happens to be in
     * -- boot reports still decode with hasConsumer 0.
     *
     * 0x100 | result, so "the decoder said no" stays distinguishable from "the
     * decoder never ran" -- the ambiguity that has cost two runs. */
    {
        BTHidReport r;
        gHidDecodeRc = 0x100UL |
        (unsigned long)BT_DecodeHidReport(rep, n, &r);

        /* ★★★ THE MOUSE PATH, for the A1015. Tried only when the KEYBOARD decoder has
         * refused the report, which keeps one rule: a report is a mouse report because
         * nothing else claimed it, never because it looked mouse-ish.
         *
         * ⚠ That ordering is what makes it safe. The two decoders accept DISJOINT
         * lengths -- keyboard 8..11, mouse 3..5 -- so neither can claim the other's
         * reports today; but if a future keyboard form ever overlapped, the keyboard
         * would still win, and a keystroke misread as a cursor jump is far worse than
         * the reverse.
         *
         * ⚠⚠ AND WHAT THE MOUSE DECODER REFUSES IS RECORDED, NOT DISCARDED. The A1015's
         * real wire format has never been measured, and the keyboard cost this project a
         * whole session to exactly this: a documented report arrived in an undocumented
         * framing, every branch rejected it, and the log said only "reports accepted 0".
         * So a report neither decoder wanted has its length and first four bytes put in
         * the block, where BTCheck prints them. ONE run then names the real format
         * instead of another round of guessing. */
        if ((gHidDecodeRc & 0xFFUL) == 0) {
            BTMouseReport mo;
            if (BT_DecodeMouseReport(rep, n, &mo)) {
                /* ⭐ THE DEVICE TOLD US WHAT IT IS. Roles are learned here rather than
                 * looked up from a class of device, so the label can never disagree with
                 * the data actually arriving on this cid. */
                short sl = HidDevSlotForCid(cid);
                if (sl >= 0) { gHidDev[sl].isMouse = 1; gHidDev[sl].reports++; }
                gMouseDecodeOk++;
                /* ★ v15.5: peak rate in a sliding window -- see the note at the globals. */
                {
                    unsigned long now = (unsigned long)hal_time_ms();
                    if (now - gRateWinStart >= kRateWinMs) {
                        gRateWinStart = now;
                        gRateWinCount = 0;
                    }
                    gRateWinCount++;
                    if (gRateWinCount > gMouseRatePeak) gMouseRatePeak = gRateWinCount;
                }
                /* ★ KEEP THE READING UNDER MEASUREMENT. The A1 02 framing is now
                 * committed to on three pieces of evidence, not proved. If the deltas
                 * are one byte off, dx/dy will not span both signs while the user moves
                 * the mouse in every direction, and the button mask will pick up bits a
                 * one-button mouse cannot send. That is visible in the log without
                 * anyone having to interpret a raw byte. */
                if (mo.dx < gMouseDxMin) gMouseDxMin = mo.dx;
                if (mo.dx > gMouseDxMax) gMouseDxMax = mo.dx;
                if (mo.dy < gMouseDyMin) gMouseDyMin = mo.dy;
                if (mo.dy > gMouseDyMax) gMouseDyMax = mo.dy;
                gMouseBtnMask |= (unsigned long)mo.buttons;
                gMouseAbsDx += (unsigned long)(mo.dx < 0 ? -mo.dx : mo.dx);
                gMouseAbsDy += (unsigned long)(mo.dy < 0 ? -mo.dy : mo.dy);
                BT_InjectMouse(mo.dx, mo.dy, mo.buttons, mo.wheel);
            } else {
                /* ⚠ THE ASSUMPTION'S OWN ALARM. A 5-byte 0xA1 report whose byte 1 is not
                 * 0x02 means the report-ID reading does not hold for this device. */
                if (n == 5 && rep[0] == 0xA1 && rep[1] != 0x02) gMouseRidOther++;
                gUnclaimedReports++;
                gUnclaimedLen = (unsigned long)n;
                gUnclaimedB0  = (unsigned long)
                    (((n > 0 ? rep[0] : 0) << 24) | ((n > 1 ? rep[1] : 0) << 16)
                   | ((n > 2 ? rep[2] : 0) <<  8) |  (n > 3 ? rep[3] : 0));
            }
        }

        if ((gHidDecodeRc & 0xFFUL) != 0) {
        /* ⭐ A KEYBOARD REPORT, so this cid is the keyboard -- and the single
         * gBhCid/gBhPeerAddr become ITS identity. Every existing keyboard path (the Caps
         * LED, the reconnect, the battery attribution) then keeps working exactly as it
         * did when a keyboard was the only possibility, which is what makes adding a
         * second device safe here. ⚠ Only from a DECODED report: an incoming connection
         * alone does not say what the device is. */
        {
            short sl = HidDevSlotForCid(cid);
            if (sl >= 0) {
                short k;
                gHidDev[sl].isKeyboard = 1;
                gHidDev[sl].reports++;
                gBhCid = cid;
                for (k = 0; k < 6; k++) gBhPeerAddr[k] = gHidDev[sl].addr[k];
            }
        }
        gHidDecodeOk++;
        gHidLastMods  = (unsigned long)r.mods;
        gHidLastNKeys = (unsigned long)r.nKeys;
        gHidLastKey0  = (unsigned long)(r.nKeys > 0 ? r.keys[0] : 0);
        if (r.rollover) gHidRollovers++;
        /* ⚠⚠ A SUCCESSFUL DECODE IS NOT A KEYSTROKE, and in report protocol
         * that distinction is new and load-bearing. An all-zero RELEASE
         * report decodes perfectly with nKeys 0, so gHidDecodeOk goes nonzero
         * the moment the keyboard says anything at all -- and BTCheck would
         * then announce "A REAL KEYSTROKE" over a report in which nothing was
         * held. Count the reports that carried a key, and let the readout
         * claim only what was measured. */
        if (r.nKeys > 0) gHidKeyReports++;

        /* ★★★★★★ M5: TURN THE REPORT INTO KEYSTROKES AND POST THEM.
         *
         * ⚠⚠ THE DIFF MUST RUN ON EVERY DECODED REPORT, INCLUDING EMPTY ONES. A release
         * is precisely a report whose key set shrank, so gating this on r.nKeys > 0
         * would deliver every press and never a single release -- every key would stick
         * down forever, which on a keyboard is worse than delivering nothing at all.
         *
         * ⚠ Secondary interrupt level. BT_KeyEvents is pure arithmetic over a static;
         * BT_InjectKeyEvent writes low memory and calls PostEvent and KeyTranslate,
         * which Apple's own OS 9 USB keyboard driver does from its USB completion
         * routine -- see bt_bootreport.h for the verified call chain.
         * scripts/level-audit.py walks this edge, and until this call existed the audit
         * reported CLEAN over bt_inject.c only because nothing reached it. */
        {
            unsigned short evs[BT_MAX_KEY_EVENTS];
            int k, ne = BT_KeyEvents(&gKeyState, &r, evs, BT_MAX_KEY_EVENTS);
            gInjCalls++;
            gInjEventsSeen += (unsigned long)ne;
            for (k = 0; k < ne; k++) BT_InjectKeyEvent(evs[k]);

            /* ★★★★★ THE CAPS LOCK LED. Measured on hardware 2026-09-15: it never lit,
             * because nothing was ever SENT BACK to the keyboard. The A1016's descriptor
             * declares an 8-bit OUTPUT report -- five LED bits, NumLock first -- and
             * Apple's own driver writes it on state change the same way
             * (USBHIDControlDevice(kHIDSetLEDStateByBits), KeyIn.c:406).
             *
             * ⚠ ON A CHANGE ONLY, never per report. This is a write back over the air to
             * a battery device, and one per keystroke would waste its power and congest
             * the link that took eleven builds to get working.
             *
             * ⚠ Report ID 1: the A1016 declares ONE collection with that ID, so its
             * output report carries it exactly as its input reports do. */
            gCapsOnMirror = (unsigned long)gKeyState.capsOn;
            if (gKeyState.ledsChanged && gBhCid != 0) {
                /* ⚠⚠⚠ STATIC, AND THIS IS NOT STYLE. hid_host_send_set_report does NOT
                 * COPY the report: hid_host.c:1377 stores `connection->report = report`
                 * and sends it LATER from the can-send-now callback. A stack local would
                 * be a dead frame by then and the keyboard would get whatever byte
                 * happened to be there.
                 *
                 * ⇒ EXACTLY THE MIRROR OF THE BUG THAT COST THIS PROJECT THE ACL
                 * INVESTIGATION, where BT_SendACL kept BTstack's pointer across an async
                 * send. Found by READING the function rather than trusting its name --
                 * it returns ERROR_CODE_SUCCESS either way, so the hardware would have
                 * shown a dark or randomly-lit LED and nothing in the log would say why.
                 *
                 * ⚠ One byte, one static: the next LED change overwrites it, and a
                 * change cannot be in flight twice because it is only written when the
                 * state actually toggles. */
                static unsigned char led;
                led = gKeyState.leds;
                gLedSends++;
                /* ⭐ v11.8: THE INTERRUPT CHANNEL, BECAUSE THE CONTROL CHANNEL IS SHUT.
                 *
                 * The A1016 answered SET_REPORT with handshake 0x03,
                 * ERR_UNSUPPORTED_REQUEST -- the device itself saying it does not accept
                 * SET_REPORT at all. That is not our bug and no amount of fixing the
                 * report ID would have helped; the oracle earned its keep.
                 *
                 * HIDP carries output reports two ways, and BTstack's own header says so
                 * at hid_host.h:251: the control channel's SET_REPORT, and a DATA output
                 * report on the INTERRUPT channel. Plenty of Bluetooth keyboards
                 * implement only the second. This is that second route.
                 *
                 * ⚠ Same pointer hazard as before -- hid_host.c stores `connection->
                 * report = report` and sends from a later callback -- so `led` stays
                 * static. Do not make it a local. */
                gLedRc = 0x100UL | (unsigned long)hid_host_send_report(
                    (uint16_t)gBhCid, 1, &led, 1);
            }
        }

        /* ★★★★★ THE CONSUMER PAGE, AND THE ONE THING THE DECODER REFUSES TO
         * GUESS AT.
         *
         * The descriptor marks Eject and Mute RELATIVE and the two volume keys
         * ABSOLUTE. An absolute bit reports held state; a relative bit reports
         * a transition and the device is expected to clear it itself. ⚠ WHETHER
         * THE A1016 ACTUALLY CLEARS THEM IS UNMEASURED -- Tiger's capture shows
         * the four PRESS reports and no releases, and absence from a doc is not
         * absence from the wire.
         *
         * ⇒ It matters because it decides the injection policy, and getting it
         * wrong makes exactly two of the four keys misbehave in a way that
         * looks like a radio fault. So rather than pick one, this records the
         * SEQUENCE of distinct byte-8 values. One keypress of each key, and the
         * ring shows whether a nonzero is followed by a zero.
         *
         * ⚠ Only TRANSITIONS are recorded. The keyboard sends idle reports
         * continuously, so storing every value would fill the ring with zeros
         * before a key was ever pressed and the instrument would measure its
         * own noise -- the same trap as the whole-session first/last gates that
         * cost v8.9 and v9.4 a run each. */
        if (r.hasConsumer) {
            gHidConsumerReports++;
            if (r.consumer != 0) gHidConsumerSeen |= (unsigned long)r.consumer;
            if (r.consumerRaw != r.consumer) gHidConsumerPadBits++;
            /* ★★★★★★ M6: ACT ON A RISING EDGE, NOT ON A SET BIT.
             *
             * ⚠⚠ THE KEYBOARD REPEATS. A held media key produces the same byte 8 in
             * report after report, so treating "bit is set" as a press would fire
             * volume-up dozens of times for one tap.  What we want is the 0->1
             * transition: set NOW and clear BEFORE.
             *
             * ⚠ gHidConsumerLast is the PREVIOUS raw byte 8 and is updated a few lines
             * below, inside this same transition branch -- so the edge must be computed
             * HERE, before that update, or every press would be compared against itself
             * and no edge would ever be seen.
             *
             * ⚠ MEASURED v10.6: volume up, volume down AND mute all set then CLEAR on
             * release, so a rising edge is well defined for them. Eject's release was
             * lost past the old 8-slot ring and is still unmeasured -- if eject LATCHES,
             * its edge fires once and never again, which is part of why the ring is 12
             * slots now. */
            {
                unsigned char rose = (unsigned char)
                    (r.consumer & ~(unsigned char)(gHidConsumerLast & 0x0FUL));
                if (rose != 0) BT_MediaKeyPressed(rose);
            }
            if (gHidConsumerRingCount == 0 ||
            gHidConsumerLast != (unsigned long)r.consumerRaw) {
            if (gHidConsumerRingCount < kHidConsumerRing) {
                gHidConsumerRing[gHidConsumerRingCount] =
                0x100UL | (unsigned long)r.consumerRaw;
                gHidConsumerRingCount++;
            } else {
                gHidConsumerRingLost++;
            }
            gHidConsumerLast = (unsigned long)r.consumerRaw;
            }
        }
        }
    }
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size)
{
    /* ⚠⚠ `channel` USED TO BE DISCARDED HERE, AND THAT WOULD HAVE MADE THE
     * REPORT-PROTOCOL EXPERIMENT UNREADABLE. A HIDP HANDSHAKE arrives on the CONTROL
     * channel and an input report on the INTERRUPT channel; with the cid thrown away
     * they are indistinguishable, and "did the keyboard answer SET_PROTOCOL?" and "did
     * it start sending reports?" are the two separate questions the whole run exists
     * to answer. */

    /* ★★★ L2CAP DATA, WHICH THIS HANDLER USED TO DISCARD IN SILENCE.
     *
     * `if (packet_type != HCI_EVENT_PACKET) return;` was the whole of it, so a HID
     * report arriving on an open channel went straight on the floor. Harmless while no
     * channel had ever opened -- and it would have made the LEVEL_0 probe WORTHLESS,
     * because the one thing that probe exists to answer is whether the A1016 sends us
     * anything, and the answer would have been thrown away unexamined.
     *
     * ⚠ Counted and CAPTURED, not decoded-and-forgotten. The first payload goes into
     * the block verbatim as well as through the decoder, because if the decoder
     * rejects the report the raw bytes are the only way to learn why --
     * bt_bootreport.c has 32 host tests behind it and has never seen real hardware.
     *
     * ⚠ Interrupt level: bounded byte copies into a fixed buffer. No allocation,
     * nothing that can block, nothing that can reach the File Manager. */
    if (packet_type == L2CAP_DATA_PACKET) {
        gHidDataPkts++;
        gHidLastDataLen = (unsigned long)size;
        /* ★★★ WHICH CHANNEL, AND THEREFORE WHICH QUESTION THIS PACKET ANSWERS. */
        if (gHidCtrlCid != 0 && (unsigned long)channel == gHidCtrlCid) {
            gHidCtrlDataPkts++;
            /* ⭐ A HIDP HANDSHAKE is one header byte: (0x0 << 4) | result. So 0x00 is
             * SUCCESSFUL and 0x03 is ERR_UNSUPPORTED -- which is the answer that would
             * mean this keyboard cannot do report protocol at all. 0x100 | byte, the
             * block convention, because 0x00 is a VALID and meaningful value here. */
            if (size >= 1 && gHidHandshake == 0)
                gHidHandshake = 0x100UL | (unsigned long)packet[0];
        } else if (gHidIntrCid != 0 && (unsigned long)channel == gHidIntrCid) {
            gHidIntrDataPkts++;
        }
        if (gHidFirstDataLen == 0 && size > 0) {
            unsigned short i, n = size;
            if (n > kHidCapBytes) n = kHidCapBytes;
            for (i = 0; i < n; i++) gHidFirstData[i] = packet[i];
            gHidFirstDataLen  = (unsigned long)n;
            gHidFirstDataFull = (unsigned long)size;
        }
        /* ★★★★★★ M5 STEP 1: DECODE. ⚠⚠ AND THIS PATH IS DORMANT -- see BhRecordReport.
         * With kHidUseBtstackHost = 1 BTstack's hid_host owns the L2CAP channels and
         * the reports arrive at HID_SUBEVENT_REPORT instead. Kept and calling the same
         * helper so the two cannot drift, and so turning the gate off still decodes. */
        BhRecordReport((unsigned long)channel, packet, size);
        return;
    }

    if (packet_type != HCI_EVENT_PACKET) return;

    /* ★★★ v7.0: A RING OF EVERY EVENT CODE THAT REACHES *US*.
     *
     * ⚠ THIS IS NOT A DUPLICATE OF THE TRANSPORT'S RING, and the distinction is the
     * whole reason it exists. BT_DeliverPacket's ring sees only real CONTROLLER
     * packets -- that is deliberate, so "what did the card say" is unpolluted. But it
     * therefore cannot see anything BTstack SYNTHESISES: GAP_EVENT_SECURITY_LEVEL
     * (0xD8), the L2CAP_EVENT_* family, HCI_EVENT_TRANSPORT_PACKET_SENT. Those are
     * exactly the events that would tell us which branch of
     * gap_request_security_level ran, and for five runs they have been invisible.
     *
     * "L2CAP events seen 1" was the symptom of that blindness: one counter, no codes,
     * no way to know WHICH event it was. */
    {
        unsigned long code = (unsigned long)hci_event_packet_get_type(packet);
        gHandlerEvtRing[gHandlerEvtIdx & (kHandlerRingLen - 1)] =
            (code << 16) | ((unsigned long)(size >= 2 ? packet[1] : 0) << 8)
                         |  (unsigned long)(size >= 3 ? packet[2] : 0);
        gHandlerEvtIdx = (gHandlerEvtIdx + 1) & (kHandlerRingLen - 1);
        gHandlerEvtTotal++;
    }

    /* ★★★ THE DECISIVE EVENT. gap_request_security_level has three outcomes and only
     * one of them is visible from outside:
     *
     *   requested <= current   -> hci_emit_security_level(handle, current, SUCCESS)
     *                             and RETURN. So: this event ARRIVES, with the
     *                             CURRENT level and status 0, and no authentication
     *                             is ever requested.
     *   authentication_active  -> the requested level is bumped and the function
     *                             returns having emitted NOTHING.
     *   otherwise              -> BONDING_SEND_AUTHENTICATE_REQUEST is set, and this
     *                             event arrives LATER carrying the achieved level.
     *
     * ⚠ So its ABSENCE does not identify a single branch -- it narrows to two, and
     * the sampled predicates below are what separate those. Recorded as facts; the
     * verdict belongs to whoever reads them, not to this code. */
    /* ★★★★ 0xD9 -- the initiator pairing's own verdict. This is what
     * gap_dedicated_bonding completes with, success or failure, and it is the single
     * row that says whether pairing an A1016 from OS 9 works. */
    /* ⚠ Event-independent, so a window can never be held open by an event that does
     * not arrive. Costs one subtraction per event. */
    BT_PagingWindowTick();

    if (hci_event_packet_get_type(packet) == GAP_EVENT_DEDICATED_BONDING_COMPLETED) {
        gBondDone++;
        gBondStatus = 0x100UL | (unsigned long)packet[2];
        /* ★ The pairing is over, whatever its verdict: give AirPort the quiet radio
         * back at once rather than waiting out kPageWindowMaxMs. */
        BT_ClosePagingWindow();
        /* ⚠⚠⚠ v13.3 HUNG THE OUTGOING CONNECT HERE AND IT NEVER RAN ONCE.
         * GAP_EVENT_DEDICATED_BONDING_COMPLETED HAS NEVER ARRIVED ON THIS STACK -- this
         * counter has read 0 in every log this project has ever produced, and BTCheck
         * prints "gap_dedicated_bonding was accepted but GAP_EVENT_DEDICATED_BONDING_
         * COMPLETED never arrived" right beside it. I read that line while diagnosing a
         * pairing failure, quoted it, and then chose the same event as the trigger for
         * a new feature. I picked it out of BTstack's API by what it MEANS instead of
         * out of the logs by whether it FIRES.
         *
         * ⇒ The trigger moved to the pair that demonstrably fires on a fresh pair --
         * Link Key Notification then Authentication Complete. Kept as a counter,
         * because "never arrives" is itself worth continuing to measure. */
    }

    if (hci_event_packet_get_type(packet) == GAP_EVENT_SECURITY_LEVEL) {
        gSecEvtCount++;
        gSecEvtLevel  = 0x100UL | (unsigned long)
                        gap_event_security_level_get_security_level(packet);
        gSecEvtStatus = 0x100UL | (unsigned long)
                        gap_event_security_level_get_status(packet);
        gSecEvtHandle = (unsigned long)gap_event_security_level_get_handle(packet);
    }

    switch (hci_event_packet_get_type(packet)) {

    case BTSTACK_EVENT_STATE:
        /* ⚠ THE ONE PLACE L2CAP WORK MAY BEGIN. Reaching HCI_STATE_WORKING is
         * BTstack telling us the controller is configured and commands will be
         * accepted; starting an inquiry before that gets it rejected. Run 15
         * proved we reach this state, so this is the natural trigger and it
         * needs no timer -- which matters, because our run loop only advances
         * when a completion fires. */
        /* ★ ASK ONCE, AS SOON AS COMMANDS ARE ACCEPTED. Max_Num_Keys is the whole
         * answer to whether this controller can hold link keys of its own, and it
         * costs one read. gStoredKeyAsked keeps it to once per bring-up rather than
         * once per arrival at WORKING -- a re-bind rebuilds the stack and re-asks,
         * which is correct, because the answer is a property of the controller and a
         * re-bind may be a DIFFERENT controller. */
        /* ⚠⚠ THE PROBE USED TO BE SENT FROM HERE AND IT NEVER FIRED. Moved to
         * BT_TryStoredKeyProbe, called from the pump timer -- see the note there. */

        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING
            && gInqState == 0) {
            /* ⚠ ROUTE AROUND THE INQUIRY, don't just retry it.
             *
             * Run 16's inquiry was ACCEPTED (Command Status 0) and then produced
             * neither results nor an Inquiry Complete. Until we know why, making
             * the whole L2CAP test depend on inquiry working is a single point of
             * failure -- and the dongle is a counterfeit CSR that the project has
             * already flagged as an unknown on exactly these paths.
             *
             * So we also become discoverable AND connectable, which lets the PEER
             * start the conversation. That is worth more than a second attempt at
             * the same thing: an inbound connection request is by definition an
             * UNSOLICITED event, so it independently tests the one assumption run
             * 16 exposed as never having been proved -- that this transport can
             * deliver an event we did not just ask for. If the phone can find and
             * connect to us while the inquiry still reports nothing, the fault is
             * in the controller's inquiry, not in our transport or L2CAP. */
            gap_connectable_control(1);
            gScanMode = 1;
            gap_discoverable_control(1);
            gScanMode = 3;

            /* ⚠ THIS IS THE RUN 17 FIX, and it is ours, not the dongle's.
             *
             * Run 17 got ACL up to the phone and pushed 1495 bytes of L2CAP
             * traffic, then the link died: Disconnection Complete, reason 0x22 =
             * LMP RESPONSE TIMEOUT, immediately after we sent
             * IO_Capability_Request_Reply. BTstack's `ssp_auto_accept` defaults to
             * 0 (hci.c:5579), and with it off HCI_EVENT_USER_CONFIRMATION_REQUEST
             * is handled by doing NOTHING and waiting for the application to call
             * gap_ssp_confirmation_response(). We never called it, so the phone sat
             * waiting for a confirmation that was never coming and gave up.
             *
             * Auto-accept is not a shortcut here, it is the semantically correct
             * answer: our io capability is SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
             * which selects "Just Works" pairing, and Just Works has no human step
             * to defer to. A driver with no UI and no task level has nothing else
             * it could do.
             *
             * ⚠ SECURITY, AND IT IS A REAL TRADE-OFF: while discoverable, this
             * accepts a pairing from anything that asks, with no confirmation. That
             * is acceptable on a bench for a test build. Before this ships it must
             * be revisited -- the natural answer is to stop being DISCOVERABLE once
             * a keyboard is bonded, staying merely connectable so the known device
             * can return. Being discoverable was only ever a run-16 workaround for
             * an inquiry that has since been proved to work. */
            gap_ssp_set_auto_accept(1);
            gSspAuto = 1;
            /* ⚠ v16.1: nothing is armed here any more. This is where the reconnection
             * sweep started, and the claim in its comment -- "a mouse does not page,
             * which is precisely why the A1015 sat bonded and idle" -- was measured
             * FALSE on 2026-10-02: the A1015 pages, sleeps, and pages again on wake.
             * We listen; the devices call us. See the epitaph further down this file. */

            /* ⚠⚠ THE AUTOMATIC BRING-UP INQUIRY IS OFF, and this is why.
             *
             * It fired on every arrival at HCI_STATE_WORKING -- that is, on every
             * BIND. Run 53 recorded 13 Initialize calls against 12 Finalize in a
             * single session, so this ran up to thirteen times, and the user watched
             * the control panel start scans "on its own": the panel was reporting a
             * real inquiry accurately, it simply was not the one they asked for.
             *
             * It also caused the -1012 report. gap_inquiry_start refuses while an
             * inquiry is active, so for ~5 s after every bind a button press was
             * turned away -- and it made the driver pick a peer and open ACL to
             * whatever answered, unasked.
             *
             * ★ It was run-16 scaffolding: a workaround to prove inquiry worked at
             * all, which it has since done many times over. The control panel is now
             * the only thing that should start a scan, so the driver does not.
             *
             * ⚠ Turning this back on costs the M2b/M3 automatic test chain (peer pick
             * -> ACL -> SDP -> pairing), which is how pairing has been exercised until
             * now. That is the trade being made deliberately: the panel can drive the
             * same path on demand, and an unasked connection to a stranger's phone is
             * not behaviour to ship. Set to 1 to restore the old bench behaviour. */
#define kAutoInquiryOnBringUp 0
#if kAutoInquiryOnBringUp
            gInqState   = 1;
            /* 4 units of 1.28 s is about 5.1 s: long enough for a phone that is
             * already discoverable to answer, short enough that the user is not
             * left wondering whether the machine has hung. */
            gInqStartRc = 0x100UL | (unsigned long)gap_inquiry_start(4);
#endif
        }
        BT_StackPoll((unsigned long)btstack_event_state_get_state(packet));
        break;

    case GAP_EVENT_INQUIRY_RESULT:
        /* ⚠⚠ THE OLD COMMENT HERE SAID THE DIVERGENCE WAS HARMLESS. IT WAS NOT.
         *
         * The table holds 4. Run 20 saw 12 responders and run 24 saw 15. So a device
         * that answers late is counted and then thrown away -- and the one device this
         * whole project exists to find, a keyboard, could answer sixth and be invisible
         * in the readout while `inquiry responses` cheerfully said 15. With no GUI, the
         * block IS the only way to know what is out there, so "counted but not
         * recorded" is a blind spot rather than a tidy trade-off.
         *
         * Two fixes, both cheap:
         *   1. Tally EVERY responder by major device class, whatever the table holds.
         *      "Did a keyboard answer?" is then answerable in a crowded room.
         *   2. Let a PERIPHERAL displace a non-peripheral when the table is full, so
         *      the actual target can never be crowded out by cars and headphones. */
        {
            bd_addr_t raddr;
            uint32_t  rcod;
            uint8_t   rmaj;

            gInqResults++;
            gap_event_inquiry_result_get_bd_addr(packet, raddr);
            rcod = gap_event_inquiry_result_get_class_of_device(packet);
            rmaj = cod_major(rcod);

            /* (1) tally by class, unconditionally */
            switch (rmaj) {
            case BT_PICK_PERIPHERAL:
                gInqPeriph++;
                /* Remember the FIRST peripheral outright, so its address is visible
                 * even if selection logic later changes. */
                if (gInqPeriphCoD == 0) {
                    gInqPeriphAddrHi = ((unsigned long)raddr[0] << 16)
                                     | ((unsigned long)raddr[1] << 8) | raddr[2];
                    gInqPeriphAddrLo = ((unsigned long)raddr[3] << 16)
                                     | ((unsigned long)raddr[4] << 8) | raddr[5];
                    gInqPeriphCoD    = rcod;
                }
                break;
            case BT_PICK_PHONE:    gInqPhones++;    break;
            case BT_PICK_COMPUTER: gInqComputers++; break;
            /* ★ Audio/Video, counted separately from "other" so a pair of headphones
             * answering is a distinct fact rather than a shrug. Until now major class
             * 0x04 fell into the default branch, so the only three classes this code
             * had ever distinguished were the three we happened to own. */
            case BT_PICK_AUDIO:    gInqAudio++;     break;
            default:               gInqOther++;     break;
            }

            /* ★★★ LIST ONLY WHAT WE CAN ACTUALLY DRIVE. The user's call, 2026-09-21:
             *
             *   "our scanner should only pick up devices that it can actually connect
             *    to AND drive, which currently is just the A1016 (and soon the A1015);
             *    all other unsupported device categories should be ignored"
             *
             * It is the same honesty rule the battery gauge follows -- do not show what
             * cannot be delivered. A phone or a pair of headphones in the list is an
             * invitation to pair with something that will then do nothing, and the user
             * has no way to know the difference is ours rather than theirs.
             *
             * DRIVABLE = major class PERIPHERAL *and* at least one of the keyboard
             * (0x40) / pointing-device (0x80) minor bits. A peripheral with neither is a
             * joystick, gamepad or digitizer -- in scope for neither decoder. Those bit
             * positions are the same ones KindName() decodes in the panel and the CSM.
             *
             * ⚠⚠ THE TALLIES ABOVE STAY UNCONDITIONAL, deliberately. "A phone answered"
             * is a diagnostic fact worth having -- it proves the radio and the inquiry
             * work even when nothing drivable is nearby, which is exactly the state that
             * otherwise looks identical to a broken scanner. Count everything; list only
             * what we can drive. Filtering the counters too would have thrown away the
             * evidence that the scan ran at all.
             *
             * ⚠ ESCAPE HATCH, AND IT IS NOT OPTIONAL. project_os9_bluetooth_scope records
             * that the PHONE IS THIS PROJECT'S TEST INSTRUMENT -- "it pairs reliably and
             * is re-pairable at will, which made it the right subject for delete/restore
             * testing" -- because deleting the A1016's bond is a ONE-WAY DOOR that needs
             * a battery pull to undo. Filtering phones away unconditionally would remove
             * the only safely re-pairable device on the bench. So the filter is disabled
             * while `Preferences:Bluetooth Show All Devices` exists: same zero-length
             * marker idiom as the pairing flag, no rebuild needed to test, and invisible
             * to anyone who has not deliberately created it. */
            {
                int drivable = (rmaj == BT_PICK_PERIPHERAL && (rcod & 0xC0UL) != 0);
                if (!drivable && !gShowAllDevices) {
                    gInqFiltered++;
                    BT_StackPoll((unsigned long)hci_get_state());
                    break;
                }
            }

            /* ★ KEEP THE PAGE-SCAN AND CLOCK PARAMETERS. They arrive free in the
             * inquiry result and are throwaway unless stored -- and they are exactly
             * what lets a later Create_Connection find the device quickly instead of
             * doing a blind page. The 2003 control panel in vendor/bt-control-center
             * keeps them in its row record for that reason
             * (docs/SCAN-DESIGN.md §2); we were discarding them. */
            {
                uint32_t rparm = ((uint32_t)gap_event_inquiry_result_get_clock_offset(packet) << 16)
                               | ((uint32_t)gap_event_inquiry_result_get_page_scan_repetition_mode(packet) << 8);

                /* ⚠ DE-DUPLICATE. A controller reports the same responder more than
                 * once during a single inquiry, and a scan list that shows one device
                 * five times is worse than useless. Keyed on address. */
                int k, seen = -1;
                for (k = 0; k < gRespCount; k++)
                    if (memcmp(gRespAddr[k], raddr, sizeof(bd_addr_t)) == 0) { seen = k; break; }

                if (seen >= 0) {
                    gRespCoD[seen]  = rcod;         /* refresh, do not duplicate */
                    gRespParm[seen] = rparm;
                } else if (gRespCount < BT_MAX_RESPONDERS) {
                    memcpy(gRespAddr[gRespCount], raddr, sizeof(bd_addr_t));
                    gRespCoD[gRespCount]  = rcod;
                    gRespParm[gRespCount] = rparm;
                    gRespCount++;
                } else if (rmaj == BT_PICK_PERIPHERAL) {
                    int victim = -1;
                    for (k = 0; k < BT_MAX_RESPONDERS; k++) {
                        if (cod_major(gRespCoD[k]) != BT_PICK_PERIPHERAL) { victim = k; break; }
                    }
                    if (victim >= 0) {
                        memcpy(gRespAddr[victim], raddr, sizeof(bd_addr_t));
                        gRespCoD[victim]  = rcod;
                        gRespParm[victim] = rparm;
                        gRespDisplaced++;
                    }
                }
            }
            gRespStored = (unsigned long)gRespCount;
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case GAP_EVENT_INQUIRY_COMPLETE:
        /* Wait for the inquiry to finish before connecting, rather than stopping
         * it early. An inquiry and a connection attempt overlapping is legal but
         * makes the radio behaviour, and therefore a failure, harder to read. */
        gInqState        = 2;
        gInqComplStatus  = 0x100UL | (unsigned long)
                           gap_event_inquiry_complete_get_status(packet);
        start_sdp_query_on_best_responder();
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    /* ---- M2c: pairing, which run 17 walked into ------------------------- *
     * Recorded, not driven. BTstack answers these itself now that auto-accept is
     * on; we count them so a stalled pairing is visible as a stage rather than as
     * a mystery disconnect. Run 17 had to be diagnosed by hand-decoding the raw
     * `last event bytes`, which is not a thing to repeat. */
    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        gIoCapReqs++;
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        /* ⚠ THIS IS THE EVENT THAT KILLED RUN 17. With ssp_auto_accept at its
         * default of 0, BTstack deliberately does nothing here and waits for the
         * application to answer. We had no answer to give, the peer waited, and
         * the link died with LMP Response Timeout (0x22). */
        gUserConfReqs++;
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case HCI_EVENT_PIN_CODE_REQUEST: {
        /* ★★★ M3.7 LEGACY PIN PAIRING -- and until now this event was COUNTED AND
         * IGNORED, which meant legacy pairing could never complete.
         *
         * BTstack does not answer a PIN request for you. hci.c only sends
         * hci_pin_code_request_negative_reply when AUTH_FLAG_DENY_PIN_CODE_REQUEST is
         * set; otherwise it waits for the application to call gap_pin_code_response.
         * We never called it, so every legacy pairing attempt sat unanswered until the
         * remote gave up. The old comment here even said "a legacy PIN cannot be
         * auto-accepted, it has to be supplied" and then did not supply it.
         *
         * ⚠ THIS MATTERS FOR THE A1016. Everything paired so far used SSP Just Works
         * (gap_ssp_set_auto_accept), which is Bluetooth 2.1+. An era-appropriate Apple
         * keyboard predates SSP and wants legacy pairing, so without this the keyboard
         * that M4 is built around could not have paired at all.
         *
         * How it works from the user's side: the HOST chooses the PIN and the user
         * types it on the keyboard followed by Return. With no GUI yet we use a fixed
         * PIN, which is why kBTLegacyPin is a single named constant -- the control
         * panel replaces it with a displayed, per-pairing value later
         * (docs/M3-DESIGN.md §5b-bis). */
        bd_addr_t paddr;

        gPinReqs++;
        hci_event_pin_code_request_get_bd_addr(packet, paddr);
        gPinAddrHi = ((unsigned long)paddr[0] << 16)
                   | ((unsigned long)paddr[1] << 8)
                   |  (unsigned long)paddr[2];
        gPinAddrLo = ((unsigned long)paddr[3] << 16)
                   | ((unsigned long)paddr[4] << 8)
                   |  (unsigned long)paddr[5];

        /* ⚠ Answer EVERY request. No latching guard: a device may ask more than once
         * in one pairing, and refusing the second ask would be the shape of bug
         * [[feedback_guards_must_not_latch]] warns about. Counted instead. */
        gPinRespRc = 0x100UL | (unsigned long)
                     (gap_pin_code_response(paddr, kBTLegacyPin) & 0xFF);
        gPinAnswered++;
        BT_StackPoll((unsigned long)hci_get_state());
        break;
    }

    case HCI_EVENT_LINK_KEY_REQUEST:
        gLinkKeyReqs++;
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    /* ★★★ WHICH DEVICES THE CARD IS BONDED TO, straight from the controller.
     *
     * 0x15, the answer to Read_Stored_Link_Key with Read_All_Flag = 1. v6.5 asked and
     * got Max_Num_Keys 16 / Num_Keys_Read 2, then counted these events as merely
     * "unsolicited" and discarded the contents -- which is precisely the two
     * addresses needed to identify the peer that paged us eight times.
     *
     * Layout (Core spec): Num_Keys at packet[2], then Num_Keys repeats of
     * { BD_ADDR (6 bytes), Link_Key (16 bytes) } from packet[3]. Stride 22.
     *
     * ⚠⚠ THE LINK KEYS ARE NOT READ, AND MUST NEVER BE. Each address is followed by
     * the 16-byte secret that authenticates that pairing. It has no diagnostic value,
     * and this block is dumped verbatim by BTCheck into a log file on a shared network
     * volume. The address is copied and the key is stepped over -- if a future change
     * needs another field from this event, keep it that way.
     *
     * ⚠ Bounded by BOTH the event's own length and our four slots. Num_Keys is a byte
     * from the controller and the store holds up to 16, so trusting it as a loop
     * bound would walk off the end of a 255-byte event buffer. */
    case HCI_EVENT_RETURN_LINK_KEYS: {
        unsigned int n     = packet[2];
        unsigned int avail = (unsigned int)size > 3u ? ((unsigned int)size - 3u) / 22u : 0u;
        unsigned int i;
        if (n > avail) n = avail;            /* the event's real length wins */
        gRlkEvents++;
        /* ⚠ NOTHING IS CLEARED HERE, DELIBERATELY. The controller may split one read
         * across several 0x15 events, so this handler must APPEND -- clearing per
         * event would keep only the last fragment. BT_TryStoredKeyProbe clears at
         * issue time instead; see the note there for why that placement is required
         * and not merely tidier. */
        gRlkPassSeen = gRlkPasses;     /* which pass these addresses came from */
        for (i = 0; i < n; i++) {
            bd_addr_t a;
            unsigned long slot;
            reverse_bd_addr(&packet[3 + i * 22], a);   /* address for the block */
            /* ★★★★★★ v12.0: CAPTURE THE KEY INTO THE DATABASE -- AND ONLY THERE.
             *
             * ⚠⚠ THE PROHIBITION ABOVE STILL STANDS IN FULL and is not being relaxed:
             * the key must NEVER reach gRlkAddr or any other word of the counter block,
             * because BTCheck dumps that block verbatim into a log file on a SHARED
             * NETWORK VOLUME. Nothing below writes the block.
             *
             * What changes is that the key now also goes somewhere it was always
             * destined to live: the link-key database, whose backing file is
             * `System Folder:Preferences:OS9 Bluetooth Keys` on the LOCAL disk
             * (bt_keyfile.c:85). That file already holds link keys by design -- it is
             * what db_put_link_key persists -- so this adds no new exposure. Block and
             * database are different destinations with different reach, and the comment
             * above conflated them only because nothing had needed the key yet.
             *
             * ⭐ WHY IT IS NEEDED NOW: the user has chosen to delete the A1016's bond
             * and re-pair it from OS 9. That bond was made by Tiger and is the only
             * reason the keyboard works today. Capturing it here is what makes the
             * delete reversible -- without this, "restore the Tiger key" is impossible
             * because nothing on this machine would ever have seen it.
             *
             * ⚠ Type 0 = COMBINATION key. Return_Link_Keys carries no type field, and
             * combination is the only kind a BT 1.2 controller without SSP can hold. */
            if (BT_LinkKeyCapture(a, &packet[3 + i * 22 + 6], 0)) gRlkCaptured++;
            slot = gRlkKeys + i;
            if (slot < 4) {
                gRlkAddr[slot][0] = ((unsigned long)a[0] << 16)
                                  | ((unsigned long)a[1] << 8) | (unsigned long)a[2];
                gRlkAddr[slot][1] = ((unsigned long)a[3] << 16)
                                  | ((unsigned long)a[4] << 8) | (unsigned long)a[5];
            }
        }
        gRlkKeys += n;
        BT_StackPoll((unsigned long)hci_get_state());
        break;
    }

    /* ★★ WHO PAGED, AT PAGE TIME. Connection Complete gives an address but only
     * after the link exists; this one arrives first and carries the class of device,
     * which says whether the peer calls itself a keyboard. */
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
        bd_addr_t who;
        unsigned long hi, lo;
        if (hci_event_remote_name_request_complete_get_status(packet) != 0) break;
        hci_event_remote_name_request_complete_get_bd_addr(packet, who);
        hi = ((unsigned long)who[0] << 16) | ((unsigned long)who[1] << 8) | who[2];
        lo = ((unsigned long)who[3] << 16) | ((unsigned long)who[4] << 8) | who[5];
        if (NameSlotFor(hi, lo) < 0 && gNameCount < kNameSlots) {
            const char *nm = hci_event_remote_name_request_complete_get_remote_name(packet);
            unsigned int k;
            gNameAddr[gNameCount][0] = hi;
            gNameAddr[gNameCount][1] = lo;
            /* ⚠ BOUNDED COPY FROM AN EVENT BUFFER. The name field is up to 248 bytes
             * and is NOT guaranteed NUL-terminated by the controller -- the spec pads
             * with zeros but a short or hostile answer must not walk off the end.
             *
             * ⭐⭐ AND CONVERTED FROM UTF-8, which 14.8 did not do. The Bluetooth spec
             * says this field is UTF-8; OS 9 draws MacRoman. The A1016 calls itself
             * "fw800's keyboard" with a U+2019 apostrophe, so storing the bytes verbatim
             * put "fw800,Aos keyboard" on screen -- E2 80 99 rendered as three glyphs.
             * Converting HERE fixes the counter block, the names file, the control panel
             * and the CSM at once; converting in each consumer would be three chances to
             * forget. BT_Utf8ToMacRoman never grows the string, so the bound still holds.
             *
             * ⚠ Find the source length first: the converter takes a LENGTH, not a
             * terminator, and handing it the full 248-byte field would convert padding. */
            {
                unsigned short srcLen = 0;
                while (srcLen < 248 && nm[srcLen] != 0) srcLen++;
                k = (unsigned int)BT_Utf8ToMacRoman((const unsigned char *)nm, srcLen,
                                                    gNameText[gNameCount],
                                                    (unsigned short)(kNameChars - 1));
            }
            gNameText[gNameCount][k] = 0;
            gNameCount++;
            gNameOks++;
            /* ★ Put it where the CSM can read it. Interrupt-safe: a dirty flag plus
             * BT_DeferRequest, which only ENQUEUES -- the File Manager write happens at
             * task level in BT_NamesFlushIfDirty. ⚠ ASKING for the hop is the point:
             * v14.2 marked the battery dirty and never requested one, and the file was
             * never written. */
            BT_NamesNote();
        }
        break;
    }

    case HCI_EVENT_CONNECTION_REQUEST: {
        bd_addr_t who;
        gConnReqs++;
        hci_event_connection_request_get_bd_addr(packet, who);
        gConnReqHi = ((unsigned long)who[0] << 16)
                   | ((unsigned long)who[1] << 8) | (unsigned long)who[2];
        gConnReqLo = ((unsigned long)who[3] << 16)
                   | ((unsigned long)who[4] << 8) | (unsigned long)who[5];
        gConnReqCoD  = (unsigned long)hci_event_connection_request_get_class_of_device(packet);
        gConnReqType = 0x100UL | (unsigned long)
                       hci_event_connection_request_get_link_type(packet);
        /* ★★★ v8.2: REMEMBER IT, so the panel can offer it as a row to pair with.
         *
         * ⚠ DEDUPED BY ADDRESS. A keyboard looking for its host pages over and over;
         * appending blindly would fill all four slots with one device inside a few
         * seconds and hide everything else. */
        {
            unsigned long i, seen = 0;
            for (i = 0; i < gPagedCount && i < kPagedSlots; i++) {
                if (gPagedAddr[i][0] == gConnReqHi
                    && gPagedAddr[i][1] == gConnReqLo) {
                    gPagedAddr[i][2] = gConnReqCoD;   /* refresh the kind, not the slot */
                    seen = 1;
                    break;
                }
            }
            if (!seen && gPagedCount < kPagedSlots) {
                gPagedAddr[gPagedCount][0] = gConnReqHi;
                gPagedAddr[gPagedCount][1] = gConnReqLo;
                gPagedAddr[gPagedCount][2] = gConnReqCoD;
                gPagedCount++;
            }
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;
    }

    /* 0x18 -- a NEW link key was just negotiated. Zero across eight connection
     * attempts is itself the finding: nothing re-paired, so the controller was
     * expected to use a key it already holds. */
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        gLkNotifs++;
#if kHidOutgoingConnect
        /* ★★★★★★ A NEW BOND EXISTS, AND THIS IS THE EVENT THAT SAYS SO ON THIS STACK.
         * Measured: Link Key Notifications 1 on a fresh pair, in the same run where
         * bonding completions read 0. It carries the address, which Authentication
         * Complete does not -- that one carries a handle.
         *
         * ⚠ LATCH ONLY, DO NOT CONNECT HERE. The key exists but the link is not yet
         * authenticated or encrypted at this point; asking for an L2CAP channel now is
         * asking before the security the channel needs. The connect goes on the next
         * Authentication Complete, which is 800ms later on the measured run. */
        gJustBonded++;
        gHidOutArmed = 1;
        /* ⚠ THE _request_ ACCESSOR ON A _notification_ EVENT IS BTSTACK'S OWN IDIOM,
         * not a slip: both events carry the bd_addr at the same offset and hci.c:4781
         * does exactly this. There is no _notification_ accessor to use. */
        hci_event_link_key_request_get_bd_addr(packet, gJustBondedAddr);
#endif
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    /* ★★★★★ v9.4: MODE CHANGE (0x14) -- the event that would show the keyboard
     * asking for SNIFF, and which this project has never looked at.
     *
     * BTstack handles it (hci.c:4917) but only records conn->connection_mode and
     * logs; nothing is published, so if the A1016 has been requesting sniff on every
     * link since M4 we would never have seen it. Mode 0 = active, 1 = hold,
     * 2 = SNIFF, 3 = park. ⚠ Packed 0x100 | mode so "mode 0, active" is
     * distinguishable from "no mode change ever arrived", which are opposite facts. */
    case HCI_EVENT_MODE_CHANGE:
        gModeChanges++;
        gModeLast   = 0x100UL | (unsigned long)hci_event_mode_change_get_mode(packet);
        gModeLastMs = hal_time_ms();
        /* ★ v15.5: AND THE INTERVAL, which BTstack has always offered and we have
         * always dropped. Slots are 0.625 ms; BTCheck converts. Only sniff (mode 2)
         * carries a meaningful interval. */
        if ((gModeLast & 0xFFUL) == 2) {
            unsigned long iv = (unsigned long)hci_event_mode_change_get_interval(packet);
            hci_con_handle_t h = hci_event_mode_change_get_handle(packet);
            short sl;
            /* Attribute by handle -> cid -> registry role. A link we cannot attribute
             * is recorded separately rather than folded into either device. */
            sl = HidDevSlotForHandle((unsigned long)h);
            if      (sl >= 0 && gHidDev[sl].isMouse)    gSniffMseSlots   = iv;
            else if (sl >= 0 && gHidDev[sl].isKeyboard) gSniffKbdSlots   = iv;
            else                                        gSniffOtherSlots = iv;
        }
        break;

    /* ★★★★★★ v10.0: ROLE_CHANGE, WHICH WE HAVE NEVER ONCE LOOKED AT.
     *
     * Tiger gets this event 215 ms after accepting with role 0x00 and is master for
     * the rest of the session. BTstack defaults to remain-slave (hci.c:5551) and we
     * never handled the event, so for ten builds "are we master or slave" was not
     * merely unknown, it was unmeasurable.
     *
     * ⚠ READ IT AGAINST kWAclOutAct. If the USL moved every byte and we are still
     * SLAVE with no Role_Change, then the packets are sitting in the controller
     * waiting for a master poll that never comes -- and 0x13 is emitted on
     * transmission, not on acceptance, which is the whole shape of this bug.
     *
     * ⚠ ABSENCE IS NOT REFUSAL. If the switch is refused at LMP level the controller
     * reports it HERE with a nonzero status, but a keyboard that simply declines to
     * negotiate may produce nothing at all -- so 0 arrivals means "no switch", NOT
     * "the keyboard said no". Do not report a refusal we did not observe.
     *
     * ⚠ Layout: [0] 0x12, [1] len, [2] status, [3..8] BD_ADDR, [9] new role -- and
     * the event code is 0x12, which is worth stating because our event ring prints
     * codes and 0x12 has never appeared in one. The accessors are used rather than
     * raw offsets so a header change cannot silently shift them; both are pure byte
     * reads (btstack_event.h:640 returns event[2], :658 returns event[9]).
     * Role 0x00 = MASTER, 0x01 = slave -- the same encoding as the accept. */
    case HCI_EVENT_ROLE_CHANGE:
        gRoleChanges++;
        gRoleStatus = 0x100UL | (unsigned long)hci_event_role_change_get_status(packet);
        gRoleNew    = 0x100UL | (unsigned long)hci_event_role_change_get_role(packet);
        gRoleMs     = hal_time_ms();
        break;

    case HCI_EVENT_AUTHENTICATION_COMPLETE:
#if kHidOutgoingConnect
        /* ★★★★★★ THE OUTGOING HID CONNECT, ON AN EVENT THAT DEMONSTRABLY FIRES.
         *
         * ⭐ MEASURED on the fresh pair that needed a power cycle: Auth Complete #0,
         * status 0x00, handle 0x002E, at 132341 ms -- and no HID channel ever followed.
         * The keyboard was authenticated and idle, waiting for someone to ask. Eighty
         * seconds later the user power-cycled it, it re-paged as a bonded device, and
         * the channel opened 271 ms after the next Auth Complete. That gap is the step
         * this is trying to delete.
         *
         * ⚠ ONLY AFTER A FRESH BOND (gHidOutArmed, set by Link Key Notification). Auth
         * Complete also fires on every reconnect of an existing bond -- three times in
         * the measured run -- and on that path the device connects itself within half a
         * second. Firing there would race a working mechanism for no gain, and two
         * connections to the same PSM is how you get a peer to drop both.
         *
         * ⚠⚠⚠ "ON THAT PATH THE DEVICE CONNECTS ITSELF WITHIN HALF A SECOND" IS FALSIFIED,
         * 2026-09-17 23:33. That premise is why the reconnect path was excluded, and it was
         * true of every run available when this was written. It is not always true. On the
         * first COLD RECONNECT from a stored bond (BTCheck v99.68, banked) the keyboard did
         * connect itself -- `incoming connections 2`, `accept rc 0` -- and the channel then
         * STALLED HALF-OPEN: HID_SUBEVENT_CONNECTION_OPENED never fired, so `channels
         * OPENED` stayed 0 while the ACL link sat up for THIRTY-ONE MINUTES. It took a
         * power cycle (closed at 30m51s, a fresh incoming opened 1 s later) to recover.
         *
         * ⇒ So the power-cycle step is deleted for PAIRING, which is what was measured and
         * what Step 4 was, and is NOT deleted for reconnect. Those are different failures
         * wearing the same symptom: pairing = nobody ever asked for the channel; reconnect
         * = someone asked and the channel died in setup.
         *
         * ⚠ AND WIDENING THIS GUARD IS NOT THE FIX -- checked in BTstack's source before
         * proposing it. hid_host_connect opens with
         *     connection = hid_host_get_connection_for_bd_addr(remote_addr);
         *     if (connection){ return ERROR_CODE_COMMAND_DISALLOWED; }
         * (hid_host.c:1254) and a stalled incoming connection IS such an object, so the
         * call would silently no-op exactly when it is needed. The half-open channel has to
         * be torn down first -- hid_host_disconnect on a timeout that fires when a
         * connection is accepted and never opens -- and that is a deliberate piece of work,
         * not a wider condition here.
         *
         * ⚠ ONE SHOT: the latch clears whether or not the call succeeds, so a refusal
         * cannot make this retry on every later authentication. */
        /* ⚠⚠⚠ THE GUARD WAS gBhCid == 0 AND IT COULD NEVER BE TRUE. gBhCid is set in
         * HID_SUBEVENT_INCOMING_CONNECTION -- when a connection is OFFERED, before the
         * accept -- and NOTHING EVER CLEARS IT. So after the very first incoming
         * connection of a session it is non-zero forever, and this connect was blocked
         * every time. The measured run: fresh bonds seen 1, four Auth Completes all
         * status 0x00, incoming connections 3, connects attempted 0.
         *
         * ⇒ "opens minus closes" is what "a channel is open right now" actually means,
         * and I had already written exactly that as HidChannelOpen() in the PANEL this
         * same day. I reached for gBhCid by its name instead of reusing the idiom I had
         * just written, which is the same mistake as choosing an event by what it means
         * rather than by whether it fires. Twice in one feature. */
        /* ⚠ v13.9: `gBhOpened <= gBhClosed` was the third place this file inferred "no
         * channel is open" from counters, and it is wrong in the same states as the other
         * two -- see the note at gBhLive. Uses the maintained flag now. */
        if (gHidOutArmed && !gBhLive
            && hci_event_authentication_complete_get_status(packet) == 0) {
            uint16_t cid = 0;
            gHidOutArmed = 0;
            gHidOutTried++;
            gHidOutRc  = 0x100UL | (unsigned long)
                         hid_host_connect(gJustBondedAddr, HID_PROTOCOL_MODE_REPORT, &cid);
            gHidOutCid = (unsigned long)cid;
        }
#endif
        /* ⚠ LEGACY FLOW ONLY. This is 0x06, emitted in response to
         * HCI_Authentication_Requested. SSP does NOT use it, which is why run 18 --
         * a pairing that demonstrably succeeded and encrypted the link -- reported
         * this row as NOT RECEIVED and looked like a failure. Kept because the A1016
         * keyboard may well use the legacy flow at M3; read it together with
         * SIMPLE_PAIRING_COMPLETE below, never on its own. */
        gAuthComps++;                /* v6.6: the COUNT, so 8 attempts are visible */
        gAuthComplete = 0x100UL | (unsigned long)
                        hci_event_authentication_complete_get_status(packet);
        /* ★★★★★ v9.2: EVERY Auth Complete, not just the last. I proposed this ring
         * two builds ago, built the outbound ACL capture instead, and then could not
         * read my own run: `Auth Completes 2` with one last-value-wins status row
         * cannot say whether the FIRST one succeeded. That matters enormously here --
         * status 0 on the first would mean authentication works and something else
         * loses the encryption, while 0x02 on all of them means it never ran.
         *
         * ⚠ Status AND handle AND time, per slot. The status alone would leave the
         * same ambiguity one level down: 0x02 is Unknown Connection Identifier, so
         * the handle it was asked about and whether the link was still alive at that
         * moment are the whole question. Packed 0x1000000 | (status << 16) | handle,
         * with the ms stamp in its own word -- the marker bit distinguishes "slot
         * never filled" from a genuine status 0 on handle 0. */
        if (gAuthRingCount < kAuthRingSlots) {
            unsigned long st = (unsigned long)
                hci_event_authentication_complete_get_status(packet);
            unsigned long h  = (unsigned long)
                hci_event_authentication_complete_get_connection_handle(packet);
            gAuthRing[gAuthRingCount]   = 0x1000000UL | (st << 16) | (h & 0xFFFFUL);
            gAuthRingMs[gAuthRingCount] = hal_time_ms();
            gAuthRingCount++;
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        /* 0x36 -- THE event SSP actually completes with. */
        gSimplePairing = 0x100UL | (unsigned long)
                         hci_event_simple_pairing_complete_get_status(packet);
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case HCI_EVENT_ENCRYPTION_CHANGE:
    case HCI_EVENT_ENCRYPTION_CHANGE_V2:
        /* ⚠ BOTH CODES, DELIBERATELY. hci.c handles 0x08 and 0x59 in one case, so a
         * controller may legally send either, and this dongle claims HCI version 12
         * (Bluetooth 5.3) while being a counterfeit whose firmware we cannot predict.
         * Matching only 0x08 would mean the retry silently never fires on a V2
         * controller -- a wasted hardware run for no reason.
         *
         * Using the V1 accessors for both is verified, not assumed: V2 appends a
         * key-size byte but keeps the prefix, so status is event[2] and
         * encryption_enabled is event[5] in BOTH (btstack_event.h:443/461 vs
         * :1432/1450).
         *
         * ⚠ THIS IS THE RETRY TRIGGER, and it is the right one.
         *
         * SIMPLE_PAIRING_COMPLETE says the keys were agreed; ENCRYPTION_CHANGE with
         * encryption_enabled nonzero says the link is actually encrypted, which is
         * the condition a peer requires before it will serve SDP. Retrying on
         * pairing-complete instead would race the encryption setup and could ask a
         * moment too early -- which is the exact class of bug being fixed here. */
        gEncryptChange = 0x100UL | (unsigned long)
                         hci_event_encryption_change_get_status(packet);
        gEncryptOn     = (unsigned long)
                         hci_event_encryption_change_get_encryption_enabled(packet);

        if (hci_event_encryption_change_get_status(packet) == 0 && gEncryptOn != 0)
            retry_sdp_query_now_encrypted();

        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        /* ⚠ THE REASON IS THE WHOLE POINT. Run 17's reason 0x22 was only
         * recoverable by hand-decoding the last raw event bytes, and it was the
         * single most informative byte in the run. Never make that implicit again. */
        /* ⚠ v9.4: a queued command addressed to a handle that no longer exists can
         * only come back 0x02 Unknown Connection Identifier, which is precisely the
         * ambiguity the v9.2 ring was built to remove. Drop the queue with the link. */
        BT_LinkCmdReset();
        gDiscCount++;
        gDiscReason = 0x100UL | (unsigned long)
                      hci_event_disconnection_complete_get_reason(packet);
        gDiscHandle = (unsigned long)
                      hci_event_disconnection_complete_get_connection_handle(packet);
        /* ★ Let the ACL reader park. With no link there is nothing to receive, and
         * run 51 measured what polling an idle bulk-IN pipe costs on this stack:
         * 286 refused re-arms and 285 required stall clears, per session, at
         * interrupt level. See AclInCompletion. */
        BT_AclLinkDown();
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case HCI_EVENT_CONNECTION_COMPLETE:
        /* Recorded because it splits a failure cleanly in two: no ACL connection
         * means the problem is below L2CAP (radio, addressing, the peer refusing),
         * while a good ACL handle and no SDP bytes means the problem is L2CAP
         * itself. Without this row those two look identical in the block. */
        {
            uint8_t st = hci_event_connection_complete_get_status(packet);
            gHciConnStatus = 0x100UL | (unsigned long)st;
            gHciConnHandle = (unsigned long)
                             hci_event_connection_complete_get_connection_handle(packet);
            /* Successes accumulate; only the last failure is transient. */
            if (st == 0) gConnOks++;

            /* ★★★ v6.5: WHO. The v6.3 run had an ACL link come up with NO inquiry
             * ever started and no outgoing connection made -- so something in range
             * paged the card while our stack owned it, sent one 12-byte L2CAP
             * signalling packet, and then timed out. The A1016 is the only device
             * bonded to this card and it is actively looking for its host, so it is
             * the obvious candidate -- and "obvious candidate" is exactly what this
             * project keeps being wrong about.
             *
             * ⚠ gHidPeer* could not answer it: that pair is only written from
             * L2CAP_EVENT_INCOMING_CONNECTION, which never fired. Recorded here
             * instead, where the address is present whether or not L2CAP ever gets
             * far enough to have an opinion. Separate words on purpose -- conflating
             * "who reached L2CAP" with "who reached HCI" would destroy the very
             * distinction that makes this run readable. */
            {
                bd_addr_t who;
                hci_event_connection_complete_get_bd_addr(packet, who);
                gConnPeerHi = ((unsigned long)who[0] << 16)
                            | ((unsigned long)who[1] << 8) | (unsigned long)who[2];
                gConnPeerLo = ((unsigned long)who[3] << 16)
                            | ((unsigned long)who[4] << 8) | (unsigned long)who[5];

                /* ★★★★★★ v12.8: REFUSE A BLOCKED PEER, HERE AND NOWHERE ELSE.
                 *
                 * ⚠ Dropped at Connection COMPLETE rather than rejected at Connection
                 * REQUEST, which would be tidier, because BTstack answers the request
                 * itself inside hci.c -- racing it for the accept would be fragile in
                 * exactly the way this driver cannot afford. Letting the link come up
                 * and closing it immediately is a hundred milliseconds of ugliness that
                 * cannot go wrong.
                 *
                 * ⚠⚠ AND IT MUST COME BEFORE THE SECURITY REQUEST BELOW. Asking for
                 * LEVEL_2 on a link we are about to close would start an authentication
                 * against a peer we are refusing -- pointless work, and on this card
                 * authentication is the mechanism that deadlocked for six weeks. Do not
                 * move this down. */
                /* ⭐ v13.0: ask this device its name, over the link that just came
                 * up. ⚠ AFTER the blocked test below would be wrong -- but so would
                 * before it, because we must not chat to a peer we are refusing. It
                 * goes after. See a few lines down. */
                if (st == 0 && BT_IsBlocked(gConnPeerHi, gConnPeerLo)) {
                    gBlockDrops++;
                    gap_disconnect((hci_con_handle_t)gHciConnHandle);
                    break;          /* nothing else happens for a refused peer */
                }
                /* ⭐ Not refused, link is up: this is the one moment we can ask. */
                if (st == 0) BT_RequestNameIfUnknown(gConnPeerHi, gConnPeerLo);

                /* ★★★★★ v9.3: STRETCH THE SUPERVISION TIMEOUT PAST THE LMP TIMEOUT,
                 * because the shorter one has been hiding the verdict all along.
                 *
                 * ⚠⚠ THE MASKING, measured at v9.2. The link lived 20000 ms to the
                 * millisecond and the Authentication Complete arrived at 1706356 --
                 * TWO MILLISECONDS after the Disconnection Complete at 1706354. So
                 * the controller held our Authentication_Requested for the whole
                 * lifetime and closed it out with 0x02, Unknown Connection Identifier,
                 * the instant the link expired.
                 *
                 * That single fact is consistent with TWO opposite worlds and cannot
                 * separate them:
                 *   (a) the controller never started the LMP challenge at all, or
                 *   (b) it did, and the KEYBOARD never answered.
                 * Because the LMP response timeout is 30 s and the link supervision
                 * timeout is 20 s, (b) can never surface: the link always dies first
                 * and 0x22 LMP RESPONSE TIMEOUT is never emitted. Every run in this
                 * project has been reading the wrong timeout.
                 *
                 * ⇒ Write 0xFA00 slots = 40 s, which is longer than the 30 s LMP
                 * timeout, so (b) now reports itself as 0x22 while (a) still reports
                 * 0x02. One command turns an ambiguous run into a decisive one.
                 *
                 * ⚠ BEFORE the authentication request, deliberately -- the timeout has
                 * to be in force when the challenge is issued, not after it.
                 * ⚠ 0xFFFF (40.9 s) is the ceiling; 0xFA00 leaves margin and is a
                 * round 40 s. The keyboard keeps its OWN supervision timeout and may
                 * still drop at 20 s, but the verdict we are reading comes from OUR
                 * controller, and its LMP timer then expires first.
                 * ⚠ DIAGNOSTIC: a 40 s hang on a dead link is not shippable behaviour.
                 * Gated, and it goes to 0 with the others. */
                /* ⚠ v9.4: ENQUEUED, not sent. See BT_LinkCmdEnqueue for why a burst
                 * of commands from this handler starved the auth send at v9.3. Order
                 * matters and is deliberate: supervision FIRST so the window is open
                 * before anything that can time out, then read the policy BEFORE
                 * writing it so the run records what the controller actually had, then
                 * the write, then authentication last. */
                if (st == 0) {
                    unsigned long h = (unsigned long)
                        hci_event_connection_complete_get_connection_handle(packet);
                    if (kHidLongSupervision) BT_LinkCmdEnqueue(kLinkCmdSupervision, h);
                    if (kHidLinkPolicy) {
                        BT_LinkCmdEnqueue(kLinkCmdPolicyRead,  h);
                        BT_LinkCmdEnqueue(kLinkCmdPolicyWrite, h);
                    }
                }

                /* ★★★★ THE ARM FIRES HERE. A device we were asked to pair has just
                 * connected to us, so authenticate THIS link -- the step nothing on
                 * the responder path ever takes. */
                if (gArmed && st == 0
                    && gConnPeerHi == gArmedHi && gConnPeerLo == gArmedLo) {
                    gArmed = 0;              /* fire once; see the note at the arm */
                    gArmedFired++;
                    /* ⚠ v9.4: queued like the rest. gArmedRc now reports the ENQUEUE,
                     * and the send's own rc lands in gAutoAuthRc when the queue
                     * drains it -- one place where a refusal is visible. */
                    gArmedRc = 0x100UL;
                    BT_LinkCmdEnqueue(kLinkCmdAuth, (unsigned long)
                        hci_event_connection_complete_get_connection_handle(packet));
                }
                /* ★★★★★ v8.8: AND OTHERWISE, AUTHENTICATE AN INBOUND LINK ANYWAY.
                 *
                 * The keyboard needs the link ENCRYPTED before it will continue past
                 * its Connection Request -- see the long note at kHidAutoAuth. Nothing
                 * asks us to pair it, so the arm above never fires, and at LEVEL_0
                 * nothing else requests security either. This is the one command that
                 * turns a refused channel into an encrypted one.
                 *
                 * ⚠ ONLY AN ADDRESS THAT PAGED US, which is what the ring test means.
                 * gPagedAddr[] is filled from HCI_EVENT_CONNECTION_REQUEST, and that
                 * event only arrives for a link the PEER initiated -- so a device that
                 * has never paged us can never be authenticated here.
                 *
                 * ⚠⚠ THE RING, NOT gConnReqHi/Lo, AND THE FIRST DRAFT HAD IT WRONG.
                 * gConnReq* is a single LATCHED pair holding the most recent Connection
                 * Request, so two peers paging in quick succession -- request(kbd),
                 * request(phone), complete(kbd) -- leave it naming the phone while the
                 * keyboard's link comes up, and the keyboard's genuinely inbound link
                 * goes unauthenticated. That false negative costs a hardware boot and
                 * the log cannot distinguish it from the keyboard refusing. The paged
                 * ring already holds every address that paged us (four slots), so
                 * testing membership in it removes the ordering dependency for free.
                 *
                 * The cost of the wider test is that an OUTGOING link to a device that
                 * previously paged us also gets authenticated. That is deliberate and
                 * benign -- it is what BT_PairDevice's arm does on purpose, and a HID
                 * host authenticates its keyboard's link whichever end opened it. The
                 * first draft's comment claimed an outgoing link "cannot fire on one",
                 * which was already false for the latched pair: it fires on any peer
                 * whose address happened to be sitting in that pair.
                 *
                 * ⚠ `else if`, so a link the Pair button armed is authenticated once by
                 * the arm and not twice.
                 *
                 * ⚠ NOT BOUNDED, deliberately. Every Connection Complete is a NEW link
                 * and each one genuinely needs authenticating; a cap would silently stop
                 * the feature working after N reconnects, which is the shape
                 * [[feedback_guards_must_not_latch]] warns about. Connection Complete
                 * fires once per link, so this is one command per link, not a storm --
                 * and gAutoAuthTried makes the rate visible either way. */
                else if (kHidAutoAuth && st == 0 && gPagedCount != 0) {
                    unsigned long pi;
                    int paged = 0;
                    for (pi = 0; pi < gPagedCount && pi < kPagedSlots; pi++) {
                        if (gPagedAddr[pi][0] == gConnPeerHi
                            && gPagedAddr[pi][1] == gConnPeerLo) { paged = 1; break; }
                    }
                    if (paged) {
                        BT_LinkCmdEnqueue(kLinkCmdAuth, (unsigned long)
                            hci_event_connection_complete_get_connection_handle(packet));
                    }
                }
            }

            /* ★ Wake the ACL reader. It parks while no link exists, and THIS is the
             * event that starts it again -- which is safe because this event arrives
             * on the INTERRUPT pipe, and that pipe is never disarmed, so the wake-up
             * path cannot be lost. Armed here rather than on the first L2CAP packet
             * because an unarmed bulk-IN pipe would drop that packet silently.
             * ⚠ Only on success: a failed connection has no link to read from. */
            if (st == 0) BT_AclLinkUp();

            /* ⚠⚠ THIS IS WHY RUN 22 NEVER EXERCISED THE LINK KEY STORE.
             *
             * Run 22 stored a key (put_link_key 2, keys stored 1) and then the phone
             * rebooted and PAIRED AGAIN rather than authenticating: `get_link_key 0`
             * and `link key requests 0`, two independent counters agreeing that the
             * controller never asked us for a key.
             *
             * The reason is that nothing REQUIRED it. BTstack's global security level
             * defaults to LEVEL_2, but that level is applied when a service requests
             * security -- and a bare ACL connection carrying no service requests
             * nothing, so no authentication happens and the controller has no reason
             * to issue Link_Key_Request. The peer, seeing a device that demands
             * nothing, was free to start a fresh pairing.
             *
             * Asking explicitly per connection is the fix, and it is NOT test
             * scaffolding: Bluetooth HID requires an authenticated, encrypted link, so
             * a HID host has to do this anyway. M4 would have needed it regardless.
             *
             * LEVEL_2 specifically -- encryption required, MITM protection NOT
             * required. That is the ceiling our io capability can reach:
             * SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT selects "Just Works", which cannot
             * provide MITM protection, and hci.c's
             * hci_ssp_security_level_possible_for_io_cap enforces exactly that. LEVEL_3
             * would be refused.
             *
             * ⚠ REGRESSION WATCH: this makes encryption a precondition of the SDP
             * query rather than something that merely happened first. Runs 20 and 21
             * both reported `encryption enabled 1` alongside their successful SDP, so
             * this matches what already worked -- but if SDP attribute bytes drop to 0
             * in the next run, this line is the first suspect. */
            /* ★★★★★★ v9.8: THIS LINE DEFEATED v9.6, AND IT IS THE WHOLE STALL.
             *
             * ⚠⚠ THE HARDCODED LEVEL_2 ABOVE IGNORED kHidLevel0. v9.6 pulled the real
             * lever -- gap_set_security_level(LEVEL_0) before hid_host_init, so
             * hid_host.c:1180-1181 registers both PSMs at LEVEL_0 -- and then THREE
             * LINES LATER asked for LEVEL_2 on the very handle it had just accepted.
             * The gate said Tiger's shape; this call said the opposite; this call won.
             *
             * ⭐ MEASURED, v9.7 (btcheck-v99.16), and the chain is now closed end to end:
             *
             *   gap_request_security_level(h, LEVEL_2)   <- HERE, gSecReqs 3
             *     -> sets BONDING_SEND_AUTHENTICATE_REQUEST
             *       -> hci.c:8096 refuses to authenticate until
             *          BONDING_RECEIVED_REMOTE_FEATURES
             *         -> hci.c:8117 sends Read_Remote_Supported_Features (0x041B)
             *           -> link up 37472 ms, 0x041B sent 37473 ms  (ONE ms later)
             *              its answer      60117 ms  (22,644 ms -- and status 0x02,
             *                                        the handle was long gone)
             *              link down       46016 ms, reason 8 = SUPERVISION TIMEOUT
             *             -> while that LMP transaction is pending the controller
             *                grants NO command credit (num_cmd_packets 0:
             *                can_send_command_now first yes, last NO, buffer free)
             *                and TRANSMITS NO ACL: 0x13 events 0, packets
             *                acknowledged 0, against 7 sent and 7 USB completions.
             *
             * ⇒ EVERY SYMPTOM OF THE LAST EIGHT BUILDS IS DOWNSTREAM OF THIS ONE CALL.
             * The peer's silence, the 0x69 RTX timeout, the control channel that never
             * opened, SET_PROTOCOL never being reachable, the v9.4 queue's refusals,
             * Authentication_Requested never going out -- all of it is one wedged LMP
             * channel. ⚠ AND THE TRANSPORT IS NOT AT FAULT: 7 writes, 7 completions,
             * status 0, correct bytes, correct handle, correct pipe. I spent v9.7
             * accusing the USB path; the controller simply could never transmit.
             *
             * ⚠ L2CAP HAD ALREADY DONE THE RIGHT THING, which is how we know the
             * registration level was genuinely LEVEL_0: l2cap.c:3253 sends Connection
             * Response PENDING when required_level > LEVEL_0, and our captured
             * responses carry RESULT 0x0000 SUCCESS. So the LEVEL_0 branch was taken
             * and l2cap.c:3259 never fired the query. It came from here instead.
             *
             * ⇒ SO THE GATE MUST REACH THIS SITE TOO. kHidLevel0 means "Tiger's
             * shape": Tiger sets AuthenticationEnable 0 and EncryptionEnable 0 on this
             * same card and the A1016 reconnects in 2-3 s with volume and eject
             * working. One gate, one meaning, in all three places it now touches
             * (gap_set_security_level, hid_host registration, and this request).
             *
             * ⚠ PAIRING IS DELIBERATELY UNAFFECTED. The Pair arm above still enqueues
             * kLinkCmdAuth when the panel has armed it, so an intentional pairing
             * still authenticates -- which it must, to make a link key. Only the
             * ordinary reconnect path stops demanding security it does not need. */
            if (st == 0 && !kHidLevel0) {
                gSecReqs++;
                gap_request_security_level(
                    hci_event_connection_complete_get_connection_handle(packet),
                    LEVEL_2);
            }
            /* ⚠ Not an HCI status at all: hci.c:8435 synthesises this event with
             * BTSTACK_MEMORY_ALLOC_FAILED when the connection POOL is exhausted. */
            if (st == BTSTACK_MEMORY_ALLOC_FAILED) gAllocFails++;
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    /* ★★ M4 STEP 1: ACCEPT AN INCOMING HID CONTROL CHANNEL.
     *
     * docs/M4-DESIGN.md §10 step 1. This is the smallest step that proves L2CAP works
     * in the INCOMING direction, and it doubles as the reconnect test §9f left open: a
     * bonded keyboard pages us, and we find out whether an incoming connection is
     * accepted at all.
     *
     * ⚠ Until now these four events were counted and ignored, because sdp_client owned
     * the only channel and every connection was OUTGOING. */
    case L2CAP_EVENT_INCOMING_CONNECTION:
        gL2capEvts++;
        {
            uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
            uint16_t cid = l2cap_event_incoming_connection_get_local_cid(packet);
            bd_addr_t peer;
            l2cap_event_incoming_connection_get_address(packet, peer);

            gHidIncoming++;
            gHidLastPsm = (unsigned long)psm;
            gHidPeerHi  = ((unsigned long)peer[0] << 16) | ((unsigned long)peer[1] << 8)
                        |  (unsigned long)peer[2];
            gHidPeerLo  = ((unsigned long)peer[3] << 16) | ((unsigned long)peer[4] << 8)
                        |  (unsigned long)peer[5];

            if (psm == BLUETOOTH_PSM_HID_CONTROL) {
                l2cap_accept_connection(cid);
                gHidAccepted++;
            } else {
                /* ⚠ DECLINE EXPLICITLY rather than ignoring. An unanswered incoming
                 * connection leaves the peer waiting on a timeout, and leaves us unable
                 * to tell "we never saw it" from "we saw it and did nothing". */
                l2cap_decline_connection(cid);
                gHidDeclined++;
            }
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case L2CAP_EVENT_CHANNEL_OPENED:
        gL2capEvts++;
        {
            uint8_t  st2 = l2cap_event_channel_opened_get_status(packet);
            uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
            gHidOpenStatus = 0x100UL | (unsigned long)st2;
            if (st2 == 0 && psm == BLUETOOTH_PSM_HID_CONTROL) {
                gHidCtrlCid = (unsigned long)
                              l2cap_event_channel_opened_get_local_cid(packet);
                gHidOpened++;
                /* ★★★★★ THE STEP EVERY PREVIOUS RUN LEFT OUT.
                 *
                 * A HID device that has accepted a control channel is waiting for the
                 * host to speak. We never did -- "4 ACL packets sent and completed"
                 * and then the peer went quiet and the channel died at 0x69. So ask
                 * for a send slot and put SET_PROTOCOL on the wire.
                 *
                 * ⚠ THROUGH CAN_SEND_NOW, NOT l2cap_send FROM HERE. That is BTstack's
                 * sanctioned flow: the packet buffer may not be free at the instant a
                 * channel opens, and a refused send here would look exactly like the
                 * silence we are trying to explain. */
                gHidSetProtoRc = 0x100UL | 0xFEUL;   /* armed, not yet sent */
                l2cap_request_can_send_now_event((uint16_t)gHidCtrlCid);
            }
            if (st2 == 0 && psm == BLUETOOTH_PSM_HID_INTERRUPT) {
                gHidIntrCid = (unsigned long)
                              l2cap_event_channel_opened_get_local_cid(packet);
                gHidIntrOpened++;
            }
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case L2CAP_EVENT_CHANNEL_CLOSED:
        gL2capEvts++;
        {
            uint16_t cid = l2cap_event_channel_closed_get_local_cid(packet);
            if (gHidCtrlCid != 0 && (unsigned long)cid == gHidCtrlCid) {
                gHidCtrlCid = 0;
                gHidClosed++;
            }
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    case L2CAP_EVENT_CAN_SEND_NOW:
        gL2capEvts++;
        /* ★★★★★ SET_PROTOCOL(REPORT) = 0x71, and 0x71 is the whole experiment.
         *
         * ⭐ REPORT protocol, NOT boot. M4 was designed to send 0x70 because it was
         * reproducing what the card's proxy already does -- the flattened 8-byte
         * Keyboard/Keypad-only report. Report protocol is the keyboard's NATIVE form,
         * and it is where the Consumer page (volume, eject) and the battery-strength
         * feature report live. Same keyboard, its other personality.
         *
         * ⚠ ONCE. gHidSetProtoTried both counts and latches: CAN_SEND_NOW can fire for
         * other reasons, and re-sending SET_PROTOCOL every time would be a request
         * storm indistinguishable from a device that keeps refusing.
         * ⚠ Not the latching guard [[feedback_guards_must_not_latch]] warns about --
         * it does not suppress a retry that could succeed, it stops a repeat that
         * carries no new information, and the count shows exactly what happened. */
        if (gHidCtrlCid != 0 && gHidSetProtoTried == 0) {
            unsigned char req = HIDP_SET_PROTOCOL_REPORT;
            gHidSetProtoTried++;
            gHidSetProtoRc = 0x100UL | (unsigned long)
                (l2cap_send((uint16_t)gHidCtrlCid, &req, 1) & 0xFF);
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;

    default:
        /* ⚠ FILTER THE ROUTINE TRAFFIC OUT, or this row is worthless. Command
         * Complete, Command Status, Number Of Completed Packets and our own
         * Transport Packet Sent arrive constantly, so recording every unhandled
         * event would leave this reading 0x0E on any run and tell us nothing.
         * What we want is the FIRST genuinely unexpected code -- a peer's
         * disconnect reason, a pairing request we are not ready for at M2 -- so
         * only surprises are kept, and the first one wins rather than the last. */
        /* ★★★ THE ANSWER, when it comes back. Read_Stored_Link_Key's return
         * parameters are Status(1), Max_Num_Keys(2), Num_Keys_Read(2), little-endian.
         *
         * Max_Num_Keys is what decides the whole OS 9 pairing design: greater than
         * zero means this controller keeps link keys of its own, so a host can put one
         * there with Write_Stored_Link_Key (OCF 0x11) and the card's on-chip stack can
         * then reconnect the device unaided -- which is exactly what the A1044 is
         * observably already doing for a keyboard paired under Tiger. Zero means the
         * standard route does not exist here and it must be a CSR vendor mechanism,
         * which would send us back to Apple's kexts.
         *
         * ⚠ 0x100 | status, the block's convention: a plain 0 could not be told from
         * "never asked", and that ambiguity has already cost this project a run. */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet)
               == HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x0D)) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gStoredKeyRc  = 0x100UL | (unsigned long)rp[0];
            gStoredKeyCap = ((unsigned long)little_endian_read_16(rp, 1) << 16)
                          |  (unsigned long)little_endian_read_16(rp, 3);
            /* ★★ KEEP THE BRING-UP READ. A re-read after a delete overwrites the live
             * value, and "it says 2 now" is worth nothing without "it said 3 before" --
             * the whole point is the TRANSITION, in one boot, with no reboot in
             * between to muddy which read is which. */
            if (gStoredKeyCapFirst == 0) gStoredKeyCapFirst = gStoredKeyCap;
        }

        /* ★★★★★★ v9.7: HOW MANY ACL BUFFERS DOES THE CONTROLLER SAY IT HAS?
         *
         * Read_Buffer_Size (0x1005) returns status(1), ACL_Data_Packet_Length(2),
         * SCO_Data_Packet_Length(1), Total_Num_ACL_Data_Packets(2),
         * Total_Num_SCO_Data_Packets(2). BTstack sends this at init and sizes its
         * outstanding-packet accounting from the answer.
         *
         * ⚠ IT MATTERS FOR THE 0x13 QUESTION. If the controller claims N ACL buffers
         * and never returns any with Number_Of_Completed_Packets, BTstack will send N
         * and then go quiet for ever -- which is a different failure from "the
         * controller discarded the packet", and the two are indistinguishable without
         * this number beside the 0x13 count. Tiger sees one 0x13 per send on this same
         * card, so whatever this says, the acknowledgement path works on the hardware.
         *
         * ⚠ 0x10000 marker | status << 8 packed with the ACL length, and the packet
         * count in its own word. A failed read must not read as "length 0". */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet)
               == HCI_OPCODE_HCI_READ_BUFFER_SIZE) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gBufSizeRc  = 0x10000UL | ((unsigned long)rp[0] << 8)
                        | (unsigned long)0;
            gAclBufLen  = (unsigned long)little_endian_read_16(rp, 1);
            gAclBufNum  = (unsigned long)little_endian_read_16(rp, 4);
        }

        /* ★★★★★★ v9.4: THE LINK POLICY THE CONTROLLER ACTUALLY HAD.
         *
         * Read_Link_Policy_Settings returns Status(1), Handle(2), Settings(2), so the
         * settings are at rp[3..4] little-endian. Bit 2 (0x0004) is SNIFF.
         *
         * ⚠ THIS IS THE MEASUREMENT THE RUN TURNS ON, and it is read BEFORE the write
         * in the queue precisely so the log records what the controller had rather
         * than what we set. If sniff was already permitted, the hypothesis dies here
         * and no second boot is needed -- which is the Scan_Enable lesson applied
         * before the fact instead of after.
         *
         * ⚠ 0x10000 marker | status << 8 | settings, so a failed read cannot pass as
         * "policy 0". Same packing, same reason, as the Scan_Enable word. */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet)
               == HCI_OPCODE_HCI_READ_LINK_POLICY_SETTINGS) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gPolicyReads++;
            gPolicyReadBack = 0x10000UL | ((unsigned long)rp[0] << 8)
                            | (unsigned long)little_endian_read_16(rp, 3);
        }

        /* ★★★★★ AND THE SCAN STATE THE CONTROLLER ACTUALLY HAS. One byte: bit 0
         * inquiry scan, bit 1 PAGE scan. See bt_read_scan_enable.
         *
         * ⚠ 0x100 | value on BOTH samples, so "read it and it was 0" is distinguishable
         * from "never read it" -- 0 is the interesting value here, which is exactly the
         * case a bare 0 could not report. FIRST is kept because a setting cleared
         * mid-run and one never set are the same single sample. */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet)
               == HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x19)) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gScanEnaReads++;
            /* ⚠⚠ THE COMPLETION STATUS IS PACKED IN, and the first draft dropped it.
             * rp[0] is the status and rp[1] the Scan_Enable byte, but on a nonzero
             * status rp[1] is not a Scan_Enable value at all -- and a 1.2 controller
             * answering "unknown command" would have left BTCheck printing a confident
             * "PAGE SCAN IS OFF" from a garbage byte. Reading 0x00 is the interesting
             * outcome here, so it must not be confusable with a failed read.
             *
             * bit 16 = we got an answer, bits 8..15 = status, bits 0..7 = the byte.
             * Same packing idiom as kWDelStoredDone. */
            gScanEnaLast = 0x10000UL | ((unsigned long)rp[0] << 8)
                                     |  (unsigned long)rp[1];
            if (gScanEnaFirst == 0) gScanEnaFirst = gScanEnaLast;
        }

        /* ★★ COMMAND CREDIT, which the v6.5 run made worth reading explicitly.
         *
         * Command Status is code 0x0F: status at [2], Num_HCI_Command_Packets at [3],
         * opcode at [4..5]. hci.c sets num_cmd_packets straight from that byte, and
         * hci_can_send_command_packet_now() returns false while it is zero -- so a
         * controller that stops granting credit silences every later command.
         *
         * ⭐ v6.5's last event bytes were 0F 04 00 00 1B 04: a Command Status for
         * Read_Remote_Supported_Features with credit ZERO. That had to be decoded by
         * hand out of a raw byte dump to be noticed at all, which is exactly the kind
         * of thing that gets missed. Recorded as one word from now on.
         *
         * ⚠ LAST one wins, deliberately: the question is what the credit is NOW, not
         * what it was at some healthier moment earlier in the session. */
        /* ★★★★ THE CARD'S BLUETOOTH VERSION -- and it decides which devices can
         * EVER pair with it, which is a question we have been answering by vintage
         * rather than by measurement.
         *
         * Read_Local_Version_Information (0x1001) returns Status(1), HCI_Version(1),
         * HCI_Revision(2), LMP_Version(1), Manufacturer(2), LMP_Subversion(2).
         * LMP_Version is the decisive byte:
         *     0 = 1.0b   1 = 1.1   2 = 1.2   3 = 2.0+EDR
         *     4 = 2.1+EDR  <- the first with SECURE SIMPLE PAIRING
         *     6 = 4.0      <- the first with BLE
         *
         * ⚠ BTstack CANNOT tell us this: hci.h has hci_version and lmp_version
         * COMMENTED OUT of its struct and keeps only the manufacturer, so there is no
         * accessor to call. Captured here instead.
         *
         * ⚠⚠ WHY IT MATTERS RIGHT NOW: a modern keyboard or headset requires SSP, and
         * a card below LMP 4 cannot do SSP at all. Choosing a test device on my guess
         * about a 2003 module's vintage is exactly the kind of inference this project
         * keeps having to retract -- so the byte gets read. */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet) == 0x1001
            && size >= 14) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gLocalVer = 0x1000000UL                          /* "captured" marker  */
                      | ((unsigned long)rp[1] << 16)          /* HCI_Version        */
                      | ((unsigned long)rp[4] << 8)           /* LMP_Version        */
                      |  (unsigned long)rp[0];                /* Status             */
            gLocalMfr = (unsigned long)rp[5] | ((unsigned long)rp[6] << 8);
        }

        /* ★★★★ THE CONTROLLER'S OWN SUPPORTED-COMMANDS BITMAP, first 8 octets.
         *
         * Create_Connection has now been rejected 0x12 with BTstack's parameters AND
         * with Apple's (packet type 0x330E, role switch 0) -- six theories refuted.
         * The remaining possibility is that this card, in a personality reached by a
         * vendor mode switch out of HID-proxy, simply DOES NOT IMPLEMENT the command.
         *
         * That is not speculation about firmware: this same card already answers
         * 0x0C52 Write_Extended_Inquiry_Response with status 0x01 Unknown HCI Command,
         * so its command set is demonstrably incomplete. And it TOLD US which commands
         * it has, at bring-up, in the Command Complete for Read_Local_Supported_
         * Commands (0x1002) -- 64 octets of bitmap that we have never once looked at.
         *
         * ⚠ THE BYTES ARE CAPTURED VERBATIM, and that is deliberate. BTstack's own
         * SUPPORTED_HCI_COMMANDS table does not include Create_Connection, so its
         * offset is not available from the vendored source and I would be recalling it
         * from the Core spec. Recording the octets means the finding does not rest on
         * my memory of a bit position: BTCheck decodes octet 0 and prints the raw hex
         * beside it, so a wrong offset is visible rather than silently believed. */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet) == 0x1002
            && size >= 14) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            int w, k;
            gSuppCmdsGot = 1;
            for (w = 0; w < 2; w++) {
                unsigned long v = 0;
                for (k = 0; k < 4; k++) v = (v << 8) | (unsigned long)rp[1 + w * 4 + k];
                gSuppCmds[w] = v;
            }
        }

        /* ★★★★ Delete_Stored_Link_Key's answer: Status(1), Num_Keys_Deleted(2). */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet)
               == HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x12)) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gDelStoredDone = 0x100UL
                           | ((unsigned long)little_endian_read_16(rp, 1) << 8)
                           |  (unsigned long)rp[0];
            /* ★★★ AND READ THE STORE BACK, so the same boot shows the count fall and
             * the panel drops the row. Before this the only confirmation a delete had
             * worked was the NEXT boot's bring-up read, which cost a whole hardware
             * cycle to learn one number. */
            gStoredKeyReread = 1;
        }

        /* ★★★★ Write_Stored_Link_Key's answer. Return parameters are Status(1) then
         * Num_Keys_Written(1) -- so a status of 0 with 1 key written is the goal step
         * having actually landed in the controller's store. */
        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_COMPLETE
            && hci_event_command_complete_get_command_opcode(packet)
               == HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x11)) {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            gWroteKeyDone = 0x100UL | ((unsigned long)rp[1] << 8) | (unsigned long)rp[0];
        }

        if (hci_event_packet_get_type(packet) == HCI_EVENT_COMMAND_STATUS && size >= 6) {
            gLastCmdStatus = ((unsigned long)little_endian_read_16(packet, 4) << 16)
                           | ((unsigned long)packet[3] << 8)
                           |  (unsigned long)packet[2];
            /* ★★★★ AND THE FIRST *FAILING* ONE, WITH ITS OPCODE.
             *
             * The v7.3 run put TWO Command Status events carrying 0x12 in the event
             * ring -- so the controller rejected a command outright, at command time.
             * But the ring stores only (code, len, byte2), and byte2 of a Command
             * Status is the STATUS; the opcode lives at [4..5]. And kWLastCmdStatus
             * holds the LAST one, which by then was a healthy 0x041B. So the log
             * proved a command was rejected and could not say WHICH.
             *
             * ⚠ FIRST failure wins, not last: a later healthy status must not erase
             * the one that matters, which is exactly how the 0x12s were lost. */
            /* ⚠⚠ A RING, NOT ONE SLOT -- and v7.4 proved why. "First failure wins"
             * was chosen so a later HEALTHY status could not erase the interesting
             * one, and it promptly let a benign EARLIER one do exactly that damage:
             * the first rejection was 0x0C52 Write_Extended_Inquiry_Response with
             * status 0x01 Unknown HCI Command -- a bring-up command this card simply
             * does not implement, harmless -- and it hid the two 0x12s that matter.
             * rejections read 4 and the log could name only the useless one.
             *
             * ⇒ Same lesson as "last opcode" and "first sample": a single slot cannot
             * represent a sequence. Four slots, all of them kept. */
            if (packet[2] != 0) {
                unsigned long i = gBadCmdIdx & (kBadCmdRingLen - 1);
                gBadCmdRing[i] = gLastCmdStatus;
                gBadCmdIdx = (gBadCmdIdx + 1) & (kBadCmdRingLen - 1);
                gBadCmdCount++;
                if (gFirstBadCmdStatus == 0)
                    gFirstBadCmdStatus = gLastCmdStatus;
            }
        }

        {
            uint8_t t = hci_event_packet_get_type(packet);
            if (t != HCI_EVENT_COMMAND_COMPLETE
             && t != HCI_EVENT_COMMAND_STATUS
             && t != HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS
             && t != HCI_EVENT_TRANSPORT_PACKET_SENT
             && gLastUnkEvt == 0) {
                gLastUnkEvt = (unsigned long)t;
            }
        }
        BT_StackPoll((unsigned long)hci_get_state());
        break;
    }
}

/* ★★★★★★ v13.8 -- TEAR DOWN A HID CHANNEL THAT WAS ACCEPTED AND NEVER OPENED.
 *
 * ⚠⚠ THE FAILURE THIS EXISTS FOR, measured 2026-09-17 23:33 (BTCheck v99.68, banked).
 * On the first COLD RECONNECT from a stored bond the keyboard connected itself --
 * `incoming connections 2`, `accept rc 0` -- and the channel then stalled half-open:
 * HID_SUBEVENT_CONNECTION_OPENED never fired, `channels OPENED` stayed 0, and the ACL
 * link sat up for THIRTY-ONE MINUTES until the user power-cycled the keyboard (closed at
 * 30m51s, a fresh incoming opened 1 s later). "Linked, not ready" is exactly that state.
 *
 * ⚠ v13.5's outgoing connect cannot cover this and widening its arm would not help. It
 * is gated on a FRESH bond (gHidOutArmed, set at Link Key Notification) which never
 * fires on a reconnect -- but more fundamentally, hid_host_connect opens with
 *     connection = hid_host_get_connection_for_bd_addr(remote_addr);
 *     if (connection){ return ERROR_CODE_COMMAND_DISALLOWED; }
 * (hid_host.c:1254) and the stalled connection IS such an object. The call would
 * silently no-op precisely when it is needed. The slot has to be freed first, which is
 * what this does.
 *
 * ⚠⚠ WHY A BTstack TIMER IS SAFE HERE, because btstack_run_loop_os9.c's header says it
 * is not: "Timers only advance when a completion happens to fire. On a quiet link a
 * protocol timeout will expire late or not at all... the fix is a Time Manager task that
 * does nothing but call BT_Pump()." THAT TASK WAS SUBSEQUENTLY BUILT -- it is
 * bt_pump_timer_sih in bt_probe.c, it fires every 100 ms, and its own comment records
 * that it exists because "BTstack's timers only ever advanced when a USB completion
 * happened, so every timeout in the stack was unreliable". The run-loop caveat is stale;
 * timers are reliable now, including on a link as quiet as this one. Verified by reading
 * both, not by assuming either.
 *
 * ⚠ THE TIMEOUT IS DELIBERATELY GENEROUS. The measured healthy latency on this stack is
 * 271 ms from Auth Complete to channel open (see the note at the v13.5 connect), and
 * this waits 10 s -- roughly 37x that -- because tearing down a merely-slow channel
 * would be a regression and waiting an extra few seconds is invisible next to 31
 * minutes. gBhIncomingMs is recorded so the NEXT log reports the real accept->open
 * distribution and this can be tightened against data instead of a safety factor.
 *
 * ⚠ BOUNDED AT kBhMaxTears. Left unbounded this is a disconnect/reconnect loop with the
 * radio in the middle of it. Three attempts, then it stops and the user is no worse off
 * than before this existed.
 *
 * ⚠ AND IT USES THE TIMESTAMPS, NOT THE COUNTS. `gBhOpened <= gBhClosed` is the idiom
 * this file and the panel both used for "no channel is open", and it is WRONG in exactly
 * the state this watchdog runs in: a channel accepted and killed before OPENED fires
 * bumps `closed` without ever bumping `opened`, so after one of those the counters sit
 * equal and the test reads "no channel open" while one is. That defect shipped in panel
 * 15.5 and was fixed in 15.6 by comparing kWBhOpenedMs/kWBhClosedMs instead; the same
 * fix belongs here. See the note in HidChannelOpen. */
/* ★★★★★★ v13.9 -- THE WATCHDOG IS GONE. IT WAS REDUNDANT AND IT RACED L2CAP.
 *
 * ⚠⚠ 13.8 armed a 10 s timer at the accept and tore the channel down with
 * hid_host_disconnect if nothing opened. Measured 2026-09-18 15:00 (BTCheck v99.69,
 * banked): `accept->open gap 10047 ms`, `half-open teardowns 1`, `open status 105`.
 * 105 is 0x69, L2CAP_CONNECTION_RESPONSE_RESULT_RTX_TIMEOUT -- the peer never answered
 * an L2CAP signalling request -- and BTstack's own L2CAP_RTX_TIMEOUT_MS is **10000**,
 * the identical period I had chosen. Two timers with the same timeout fired together,
 * so that run could not attribute its own result. Choosing a constant that collides with
 * the stack's is how a run is made unreadable before it is even booted.
 *
 * ⇒ AND THE WATCHDOG WAS NEVER NEEDED. L2CAP already times the channel out at 10 s and
 * hid_host already finalizes the connection on that failure (hid_host.c:688-689,
 * hid_emit_connected_event then hid_host_finalize_connection), which frees the slot by
 * itself. Nothing had to be torn down. The ONLY thing missing was that nothing retried
 * afterwards -- which is also what the 2026-09-17 23:33 run shows, where the 31 minutes
 * were nobody retrying rather than a channel held hostage. Same root cause both nights;
 * 13.8 added a redundant teardown on top of it and changed the failure's shape.
 *
 * ⇒ SO THE TRIGGER MOVES TO THE EVENT THAT ACTUALLY FIRES. 13.8 armed the reconnect off
 * HID_SUBEVENT_CONNECTION_CLOSED and measured `reconnects attempted 0`, because a
 * connection that fails during SETUP never closes -- it surfaces as OPENED with a
 * non-zero status and is finalized. That code path was read while checking the
 * emit/finalize ordering and was not joined up to the handler choice. It is now. */
#define kBhReconnMs    250UL
#define kBhMaxReconn      3UL

/* ★ v14.1: ask for the battery percentage, once, a couple of seconds after the channel
 * came up. Deliberately NOT sent from the OPENED handler itself: SET_PROTOCOL and the
 * descriptor exchange are still settling there, and a control transaction stacked on top
 * of those is how this card has been knocked off the bus before. */
/* ★ v14.2: the second half of the chain, on its own tick. See the long note at the
 * response handler for why this cannot be called from inside the response event. */
static void bat_state_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    gBatArmed = 0;
    if (gBatCid == 0)            return;
    if (gBatStRc != 0)           return;      /* asked once already */
    gBatSends++;
    gBatStRc = 0x100UL | (unsigned long)
               hid_host_send_get_report((uint16_t)gBatCid,
                                        HID_REPORT_TYPE_INPUT,
                                        (uint16_t)kBatReportState);
}

/* ★★★★★ v15.2: ASK EVERY CONNECTED DEVICE, NOT JUST THE KEYBOARD.
 *
 * The chain below was written when gBhCid was the only channel there could be, so the
 * A1015 has never once been asked for its level -- which is exactly what the user saw.
 * Rather than duplicate the chain per device, it is RETARGETED: gBatCid names the device
 * being asked, this scan picks the next connected device that has not been asked, and
 * the answer is filed against whoever actually replied.
 *
 * ⚠ A DEVICE THAT NEVER ANSWERS MUST NOT BLOCK THE OTHER ONE. A strict chain would stall
 * forever on a silent device, so this scan also times the outstanding probe out. That is
 * not hypothetical: the A1015 may well decline these reports, and "the mouse said no" and
 * "the mouse was never asked" have to stay distinguishable. */
#define kBatNextMs   3000UL
#define kBatGiveUpMs 6000UL

static btstack_timer_source_t gBatNextTimer;
static unsigned long gBatNextArmed, gBatStartedMs;
unsigned long gBatTargets, gBatGiveUps;

static void BatArmNext(void);

static void bat_next_timeout(btstack_timer_source_t *ts)
{
    short i;
    int   pending = 0;
    (void)ts;
    gBatNextArmed = 0;

    if (gBatCid != 0) {
        unsigned long now = (unsigned long)hal_time_ms();
        if (now - gBatStartedMs < kBatGiveUpMs) { BatArmNext(); return; }
        /* outstanding too long: write it off and move on */
        { short sl = HidDevSlotForCid(gBatCid);
          if (sl >= 0) gHidDev[sl].batDone = 1; }
        gBatGiveUps++;
        gBatCid = 0;
    }

    for (i = 0; i < kHidDevSlots; i++) {
        if (gHidDev[i].cid == 0 || gHidDev[i].batDone) continue;
        /* start this one: the chain's own "asked already" latches reset per device */
        gBatCid        = gHidDev[i].cid;
        gBatStartedMs  = (unsigned long)hal_time_ms();
        gBatPctRc      = 0;
        gBatStRc       = 0;
        gBatStateAsked = 0;
        gBatTargets++;
        if (gBatArmed) btstack_run_loop_remove_timer(&gBatTimer);
        btstack_run_loop_set_timer_handler(&gBatTimer, &bat_probe_timeout);
        btstack_run_loop_set_timer(&gBatTimer, kBatProbeMs);
        btstack_run_loop_add_timer(&gBatTimer);
        gBatArmed = 1;
        pending = 1;
        break;
    }
    if (!pending)
        for (i = 0; i < kHidDevSlots; i++)
            if (gHidDev[i].cid != 0 && !gHidDev[i].batDone) { pending = 1; break; }
    if (pending || gBatCid != 0) BatArmNext();
}

static void BatArmNext(void)
{
    if (gBatNextArmed) return;
    gBatNextArmed = 1;
    btstack_run_loop_set_timer_handler(&gBatNextTimer, &bat_next_timeout);
    btstack_run_loop_set_timer(&gBatNextTimer, kBatNextMs);
    btstack_run_loop_add_timer(&gBatNextTimer);
}

static void bat_probe_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    gBatArmed = 0;
    if (gBatCid == 0)            return;      /* channel went away; ask nothing */
    if (gBatPctRc != 0)          return;      /* already asked this device */
    gBatSends++;
    gBatPctRc = 0x100UL | (unsigned long)
                hid_host_send_get_report((uint16_t)gBatCid,
                                         HID_REPORT_TYPE_FEATURE,
                                         (uint16_t)kBatReportPercent);
}

/* ★★★★★ CONNECT TO A BONDED DEVICE THAT IS NOT CONNECTED (15.0).
 *
 * ⛔ THIS IS WHY THE MOUSE DID NOTHING. A keyboard PAGES the host on a keypress, so it
 * reconnects itself and nothing here ever had to. The A1015 does not: the run showed it
 * bonded ("fresh bonds seen 1", name "Apple Wireless Mouse") with `connects attempted 0`
 * and `incoming connections 1`, that one being the keyboard. Nobody was ever going to
 * ask it. A stack that only ever ACCEPTS connections works for exactly the class of
 * device that initiates them.
 *
 * ⚠ BOUNDED, AND THAT IS NOT OPTIONAL. This runs on a timer against every bonded
 * address, and an unbounded sweep against a device that is switched off is a page attempt
 * every few seconds forever -- which congests the radio the keyboard is sharing, and this
 * project has already spent builds on a link that would not stay up. One budget per
 * address per session.
 *
 * ⚠ SKIP WHAT IS ALREADY CONNECTED, by address, against the registry. Asking hid_host to
 * connect something it already holds returns an error and burns a retry for no reason.
 *
 * ⚠ A timer, not a call from an event handler, for the same reason bh_reconnect_timeout
 * is: BTstack emits events BEFORE it finishes its own state transitions, so connecting
 * from inside one hits a live object and comes back COMMAND_DISALLOWED. That mistake has
 * been made twice on this project already. */
/* ★ v15.0 CADENCE. The first build of this had ONE interval and a budget of three,
 * which meant the search was over nine seconds after HCI reached WORKING. An Apple
 * wireless mouse is asleep at that moment -- it wakes when the user clicks it, which
 * on a machine that is still finishing its startup items is a minute or more later.
 * A sweep that has already given up answers nothing and costs a reboot to discover.
 *
 * So: a fast burst for a device that is already awake, then a slow patrol that keeps
 * running for as long as something bonded is missing. The rate, not the total, is
 * what keeps this from being a retry storm -- one connect per pass, always. */
/* ★★★★★★ THE RECONNECTION SWEEP IS GONE (v16.1), AND ITS PREMISE WAS FALSE.
 *
 * It existed for one stated reason -- "a mouse does not page the host, so nobody will
 * ever ask the A1015 to come back" -- and that was never measured. It could not be
 * measured while the sweep ran, because the sweep always connected the mouse first:
 * a working workaround hiding the question it was built to answer.
 *
 * v16.0 turned it off and asked. MEASURED 2026-10-02 on the MDD FW800:
 *   - both devices connected by PAGING US (incoming 2, slots claimed 2)
 *   - the mouse drove the cursor normally (240 reports, 235 moves)
 *   - and the A1015 then SLEPT and came back on its own when woken
 * ⇒ the A1015 pages. The premise was simply wrong, and ~150 lines existed to work
 * around a problem that did not exist.
 *
 * ⚠⚠ IT WAS ALSO ACTIVELY HARMFUL. Paging is continuous 2.4 GHz transmission inches
 * from the AirPort Extreme card, which has no coexistence hardware (BTCOEXIST clear).
 * Measured: the AirPort receiver went deaf ~15 s of every ~30 s while this ran --
 * beacons 10/s -> 0-2/s -- and unplugging the Bluetooth adapter removed it entirely.
 * Deleting this is not only tidying; it is the fix.
 *
 * What replaces it: nothing. Devices page us, which is exactly how Tiger behaves --
 * its capture shows a keyboard paging Tiger 13 s after a link dropped, and Tiger never
 * paging to reconnect at all. The incoming path is untouched and always was.
 *
 * ⚠ The block words 740..743 and 772 are RETIRED IN PLACE rather than renumbered. A
 * hole costs nothing; renumbering a table four binaries agree on is what overran the
 * System heap in 14.6. See the kWCount note in bt_probe.c. */

static void bh_reconnect_timeout(btstack_timer_source_t *ts)
{
    uint16_t cid = 0;
    (void)ts;
    gBhReconnArmed = 0;
    if (gBhLive) return;                        /* a channel is up; nothing to do */
    if (gBhReconnTried >= kBhMaxReconn) return; /* bounded: never a retry storm    */
    gBhReconnTried++;
    /* ⚠ The finalize has run by now -- that is the whole reason this is a timer and not
     * a call from inside the event handler. BTstack emits the failure BEFORE
     * hid_host_finalize_connection frees the slot (hid_host.c:688-689), so a connect
     * from the handler would hit a live object and come back COMMAND_DISALLOWED. */
    gBhReconnRc = 0x100UL | (unsigned long)
                  hid_host_connect(gBhPeerAddr, HID_PROTOCOL_MODE_REPORT, &cid);
}


/* ★★★★★★ v9.5: THE CANONICAL HID HOST'S EVENT HANDLER.
 *
 * Everything hid_host.c reports comes through here as HCI_EVENT_HID_META with a
 * subevent byte. This replaces our hand-rolled L2CAP_EVENT_INCOMING_CONNECTION /
 * CHANNEL_OPENED / DATA_PACKET path entirely -- and with it eleven versions of our
 * own decisions about MTU, accept timing, security level and SET_PROTOCOL.
 *
 * ⚠ EVERY BRANCH COUNTS SOMETHING. A canonical implementation that fails silently
 * would be worse than our own, because we would have no instrument inside it. These
 * counters are the whole reason this is a diagnosable change rather than a rewrite.
 *
 * ⚠ Interrupt level, like the rest: BTstack calls this from its packet handling,
 * which we drive from the USB completions and the pump timer. Plain stores only. */
static void hid_host_evt_handler(uint8_t type, uint16_t ch, uint8_t *packet,
                                 uint16_t size)
{
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) return;

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
    case HID_SUBEVENT_INCOMING_CONNECTION:
        /* ⭐ THE ACCEPT, AND THE PROTOCOL MODE IS THE POINT. hid_host does not accept
         * PSM 0x11 by itself -- it asks us, which is where a host chooses BOOT or
         * REPORT. REPORT is where the A1016's byte 8 lives (Eject, Mute, Volume) per
         * docs/A1016-REPORT-DESCRIPTOR.md, so this single argument is what v8.6's
         * hand-rolled SET_PROTOCOL(0x71) existed to achieve. */
        gBhIncoming++;
        /* ★★★★★★ v15.0: ONE IDENTITY, TWO DEVICES. gBhCid / gBhPeerAddr / gBhLive are
         * THE KEYBOARD'S, and four separate consumers read them as "the connection":
         * the Caps LED write, bh_reconnect_timeout, the battery probe, and now the
         * sweep's collision guard. When a keyboard was the only possibility, assigning
         * them from whatever connected was correct. With a mouse on the radio it is a
         * regression of the working half of the product, so every assignment below is
         * now gated on "nobody live is already wearing this identity". Role learning in
         * BhRecordReport still has the last word, because only a decoded report proves
         * what a device actually is. */
        {
            unsigned long inCid =
                (unsigned long)hid_subevent_incoming_connection_get_hid_cid(packet);
            /* ⚠ Accept the cid THAT IS CONNECTING. This used to accept gBhCid, which was
             * the same thing only because there could never be a second device. */
            gBhAcceptRc = 0x100UL | (unsigned long)
                hid_host_accept_connection((uint16_t)inCid, HID_PROTOCOL_MODE_REPORT);
            /* ⚠ ONE decision, used for the cid AND the address below -- computed before
             * gBhCid moves, or the second test would be asking about the new value. */
            gBhAdopt = BT_IdentityAdopt(gBhCid, HidDevSlotForCid(gBhCid) >= 0, inCid);
            if (gBhAdopt) gBhCid = inCid;
        }
        /* ★ v13.8: remember WHO and WHEN, then start the half-open watchdog. The address
         * is needed for the reconnect after a teardown, and gBhIncomingMs is the
         * measurement this project did not have: no accept-at timestamp existed, so the
         * healthy accept->open gap was not recoverable from any banked log. */
        /* ★ v13.9: remember WHO and WHEN, and nothing else. No watchdog is armed here --
         * L2CAP runs its own 10 s RTX timer on the channel and hid_host finalizes on its
         * expiry, so there is nothing for us to time or tear down. gBhIncomingMs is kept
         * purely as the measurement: paired with kWBhOpenedMs it gives the accept->open
         * gap, which no log carried before 13.8. */
        /* ⚠ Only if this connection is the one wearing the identity -- otherwise a mouse
         * paging us would redirect the keyboard's reconnect at itself. */
        if (gBhAdopt)
            hid_subevent_incoming_connection_get_address(packet, gBhPeerAddr);
        gBhIncomingMs = hal_time_ms();
        break;

    case HID_SUBEVENT_CONNECTION_OPENED:
        /* ⚠ 0x100 | status, the block's convention: status 0 here is SUCCESS and a
         * bare 0 could not be told from "never opened". */
        gBhOpenedStatus = 0x100UL | (unsigned long)
            hid_subevent_connection_opened_get_status(packet);
        if (hid_subevent_connection_opened_get_status(packet) == 0) {
            /* ★ CLAIM A SLOT. This event is the one place a connection becomes usable
             * whichever direction opened it -- the note below says exactly that, and it
             * is as true for the registry as it was for gBhCid. The ROLE is not set
             * here: it is learned from the first report that decodes. */
            {
                bd_addr_t who;
                unsigned long oc = (unsigned long)
                                   hid_subevent_connection_opened_get_hid_cid(packet);
                short osl;
                hid_subevent_connection_opened_get_bd_addr(packet, who);
                HidDevClaim(oc, who);
                /* ★ v15.5: keep the HCI handle so a Mode_Change can be attributed to a
                 * DEVICE. Without it the sniff interval is a number with no owner, and
                 * with two links that is the same as not having it. */
                osl = HidDevSlotForCid(oc);
                if (osl >= 0)
                    gHidDev[osl].conHandle = (unsigned long)
                        hid_subevent_connection_opened_get_con_handle(packet);
            }
            gBhOpened++;
            /* ⚠⚠⚠ SET gBhCid HERE, AND THIS IS THE CAPS LED BUG v13.5 CREATED.
             *
             * gBhCid was only ever set on the INCOMING path, in
             * HID_SUBEVENT_INCOMING_CONNECTION. The moment v13.5's outgoing connect
             * started working, connections existed that nothing had ever recorded a cid
             * for -- and the Caps LED path sends its report `if (gKeyState.ledsChanged
             * && gBhCid != 0)`, so it silently stopped sending. Capitalisation still
             * worked because that is the keyboard's own doing; only the light we drive
             * went out. Reported within minutes of the feature working.
             *
             * ⇒ THE OPENED EVENT IS THE ONE PLACE A CONNECTION BECOMES USABLE, whichever
             * direction opened it, so it is the only correct place to record its
             * identity. Setting it in the outgoing path instead would have fixed today's
             * symptom and left the same hole for the next way a channel can be created.
             *
             * ⚠ This is the third consumer of gBhCid to be wrong about it in two days:
             * the connect guard read it as "is a channel open", the LED read it as "is
             * there a connection", and neither was maintained to mean either. It now has
             * exactly one meaning -- the cid of the open channel -- set where a channel
             * opens and cleared where one closes. */
            {
                unsigned long opCid =
                    (unsigned long)hid_subevent_connection_opened_get_hid_cid(packet);
                if (BT_IdentityAdopt(gBhCid, HidDevSlotForCid(gBhCid) >= 0, opCid)) {
                    bd_addr_t opAddr;
                    short k;
                    gBhCid = opCid;
                    /* ★★★★★★ AND THE ADDRESS WITH IT, v15.2. The note above says the
                     * OPENED event is the one place a connection becomes usable
                     * "whichever direction opened it" -- and then set only the CID here,
                     * leaving gBhPeerAddr written solely on the INCOMING path. That was
                     * invisible while every connection was incoming. 15.1's sweep made
                     * OUTGOING the normal path (`incoming connections 0`, two channels
                     * opened), so gBhPeerAddr stayed 00:00:00:00:00:00 until the user
                     * happened to type -- and the battery record, which is keyed on it,
                     * went into the file under the zero address. The CSM could join it
                     * to no name and showed a third, nameless "Bluetooth device 64%".
                     *
                     * ⇒ The same hole as the Caps LED bug, in the variable next to it,
                     * opened by the same kind of change. Fixed where the comment already
                     * said it should be. */
                    hid_subevent_connection_opened_get_bd_addr(packet, opAddr);
                    for (k = 0; k < 6; k++) gBhPeerAddr[k] = opAddr[k];
                }
            }
        }
        gBhOpenedMs = hal_time_ms();
        /* ★★★★★★ v13.9: THIS EVENT IS BOTH OUTCOMES, AND 13.8 ONLY HANDLED ONE.
         * A status of 0 means a channel is up. Anything else means the setup FAILED and
         * hid_host has already finalized the connection -- there will be no CLOSED event
         * to follow, which is exactly why 13.8's reconnect (armed off CLOSED) measured
         * `reconnects attempted 0` while the keyboard sat dead. */
        if (hid_subevent_connection_opened_get_status(packet) == 0) {
            gBhLive = 1;
            /* ★ v15.2: hand the new channel to the battery SCHEDULER, which asks
             * every connected device in turn. v14.1 armed the probe directly here and
             * latched on gBatPctRc, so exactly one device per session was ever asked --
             * always the keyboard, because it connected first. */
            BatArmNext();
        } else {
            gBhFailedOpens++;
            gBhFailedStatus = gBhOpenedStatus;
            gBhLive = 0;
            /* Retry, from a timer so the finalize has run first. 250 ms is clear of
             * anything in the stack: the collision that made 13.8 unreadable was picking
             * L2CAP's own 10 s RTX period, so this deliberately is not near it. */
            if (gBhReconnArmed) btstack_run_loop_remove_timer(&gBhReconnTimer);
            btstack_run_loop_set_timer_handler(&gBhReconnTimer, &bh_reconnect_timeout);
            btstack_run_loop_set_timer(&gBhReconnTimer, kBhReconnMs);
            btstack_run_loop_add_timer(&gBhReconnTimer);
            gBhReconnArmed = 1;
        }
        break;

    case HID_SUBEVENT_CONNECTION_CLOSED:
        /* ★ Free the slot, or a device that disconnects and returns would find the
         * table full and be dropped -- with gHidDevFull as the only evidence. */
        {
        unsigned long clCid =
            (unsigned long)hid_subevent_connection_closed_get_hid_cid(packet);
        HidDevRelease(clCid);
        gBhClosed++;
        gBhClosedMs = hal_time_ms();
        /* ⚠⚠ THE SECOND DEVICE MUST NOT CLEAR THE FIRST ONE'S STATE. An A1015 sleeps
         * every few minutes; with this ungated, each nap set gBhCid = 0 (killing the
         * Caps LED, which gates on it) and gBhLive = 0, which then sent
         * bh_reconnect_timeout to page a keyboard that was never disconnected -- a
         * connect into a live object, COMMAND_DISALLOWED, and the keyboard's bounded
         * retry budget spent on nothing. */
        if (!BT_IdentityClear(clCid, gBhCid)) break;
        }
        /* ★ v13.9: a channel that was up has gone away. NOT retried from here, and that
         * is deliberate: a close the keyboard chose is its business -- going to sleep,
         * walking out of range -- and chasing it would fight the device. The retry
         * belongs only to a SETUP failure, which arrives as OPENED-with-status and never
         * reaches this case at all. */
        gBhLive = 0;
        /* ⚠⚠ AND CLEAR gBhCid, which nothing has ever done. This is not tidying: the
         * Caps LED path sends its report `if (gKeyState.ledsChanged && gBhCid != 0)`,
         * using the same stale value as a stand-in for "there is a connection". After a
         * disconnect that test stayed true and the LED write went to a cid that no
         * longer exists. Same variable, same wrong assumption, a second consumer. */
        gBhCid = 0;
        break;

    case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
        /* ⭐⭐ THE HIDP HANDSHAKE, which no run has ever reached. 0x00 = SUCCESSFUL,
         * and it would mean the keyboard accepted REPORT protocol -- the Consumer page
         * unlocked. Anything else names the refusal. */
        gBhSetProtoRsp = 0x100UL | (unsigned long)
            hid_subevent_set_protocol_response_get_handshake_status(packet);
        break;

    /* ★★★★★★ v14.1: THE ANSWER, OR THE REFUSAL. This event is the whole probe.
     *
     * handshake 0x00 = SUCCESSFUL and the payload carries the value. 0x03 is
     * ERR_UNSUPPORTED_REQUEST -- the device declining, which is what SET_REPORT already
     * got and would close the battery question honestly. Anything else is named in
     * BTCheck rather than guessed at here.
     *
     * ⚠ The payload is captured as RAW BYTES with its length, not interpreted. Tiger
     * says Feature 71 is one byte 0..100, but this project has twice been burned by
     * trusting a declaration over what a device actually answers -- the whole reason
     * this probe exists. Decode it once a real payload has been seen.
     *
     * ⚠ CHAINED, NOT PARALLEL: Input 48 is asked for only after Feature 71 has answered,
     * so two control transactions are never in flight together. gBatStateAsked makes it
     * once per session. */
    case HID_SUBEVENT_GET_REPORT_RESPONSE: {
        unsigned long  hs  = (unsigned long)
                             hid_subevent_get_report_response_get_handshake_status(packet);
        unsigned short rlen = hid_subevent_get_report_response_get_report_len(packet);
        const unsigned char *rp = hid_subevent_get_report_response_get_report(packet);
        unsigned long  packed = 0;
        unsigned short i;

        gBatResponses++;
        for (i = 0; i < 4 && i < rlen; i++)
            packed = (packed << 8) | (unsigned long)rp[i];

        if (!gBatStateAsked) {          /* this is Feature 71's answer */
            /* ⚠ RECORD THE REFUSAL TOO. hs != 0 skips the publish below, and without
             * this the slot would read exactly like a device that was never asked. */
            { short rsl = HidDevSlotForCid((unsigned long)
                            hid_subevent_get_report_response_get_hid_cid(packet));
              if (rsl >= 0) gHidDev[rsl].batHs = 0x100UL | hs; }
            gBatPctHs  = 0x100UL | hs;
            gBatPctLen = (unsigned long)rlen;
            gBatPctVal = packed;

            /* ⭐⭐ v14.2: PUBLISH IT, keyed by the peer's address, so a Control Strip
             * Module can show a per-device level. Plain stores only here; the file is
             * written later at task level from BT_BatteryFlushIfDirty. The payload goes
             * in WHOLE and BT_BatteryNote takes the LAST byte -- see the note there. */
            if (hs == 0x00UL && rlen > 0) {
                /* ⚠⚠ TOP 3 BYTES THEN LOW 3 -- THE PROJECT-WIDE SPLIT, AND v14.4 GOT
                 * THIS WRONG. It published 2+4 (bytes[0..1], bytes[2..5]) while every
                 * other address in this codebase is 3+3: gInqPeriphAddr* above,
                 * gRlkAddr into kWRlkA0Hi ("top 3 bytes then low 3", bt_probe.c), the
                 * link-key database, and therefore the panel's Device Kinds file, which
                 * simply copies the block's words.
                 *
                 * ⇒ The battery file and the Device Kinds file keyed the SAME device
                 * under DIFFERENT encodings, so a reader joining them on the address
                 * pair matched NOTHING: the A1016 appeared twice in the CSM's menu, once
                 * named with no level and once with a level but no name. Nothing logged,
                 * nothing crashed.
                 *
                 * ⚠ CAUGHT ONLY BY READING THE REAL FILE. The 28 bytes off the G4
                 * decoded to hi=0x0000000A for 00-0a-95-xx-xx-xx -- unambiguously 2+4.
                 * Both sides of the join had been read and both looked right in
                 * isolation; the defect lived in the gap between them. The file's format
                 * version is bumped to 2 alongside this, because the LAYOUT is unchanged
                 * and only the MEANING of these two fields moved -- which is precisely
                 * what a reader cannot detect for itself. */
                /* ★ v15.2: KEYED TO THE DEVICE THAT ANSWERED, not to "the peer".
                 * The reply carries its own cid, so the registry gives the exact
                 * address of whoever replied. gBhPeerAddr was a stand-in for that back
                 * when one device was the only possibility; with two connected it is a
                 * guess, and with an outgoing-only session it was a zero. Ask the
                 * answer who it came from. */
                const unsigned char *ba = gBhPeerAddr;
                short bsl = HidDevSlotForCid((unsigned long)
                              hid_subevent_get_report_response_get_hid_cid(packet));
                if (bsl >= 0) {
                    ba = gHidDev[bsl].addr;
                    /* ★ v15.3: AND KEEP IT PER DEVICE, so the log can say which device
                     * is at which level. Until now the block carried only the LAST
                     * probe's bytes: with two devices answering, "Feature 71 bytes
                     * 0x474C" could not be attributed to either one without guessing
                     * the scheduler's order. The CSM had the answer and the log did not.
                     * ⚠ LAST byte, the same rule BT_BatteryNote applies -- the payload
                     * is [report id, percent]. */
                    gHidDev[bsl].batPct = (unsigned long)rp[rlen - 1];
                    gHidDev[bsl].batHs  = 0x100UL | hs;
                }
                {
                unsigned long ahi = ((unsigned long)ba[0] << 16)
                                  | ((unsigned long)ba[1] << 8)
                                  |  (unsigned long)ba[2];
                unsigned long alo = ((unsigned long)ba[3] << 16)
                                  | ((unsigned long)ba[4] << 8)
                                  |  (unsigned long)ba[5];
                BT_BatteryNote(ahi, alo, rp, rlen, (unsigned long)hal_time_ms());
                }
            }

            /* ⚠⚠ v14.2: ASK FOR Input 48 FROM A TIMER, NOT FROM HERE. v14.1 called
             * hid_host_send_get_report on this line and measured send rc 0x010C,
             * ERROR_CODE_COMMAND_DISALLOWED, because that call requires
             * state == HID_HOST_CONNECTION_ESTABLISHED and BTstack is still in
             * W4_GET_REPORT_RESPONSE when it emits this event -- hid_host.c:785-797
             * emits and does not reset the state first.
             *
             * ⇒ EXACTLY THE MISTAKE WRITTEN UP THREE DAYS EARLIER for the reconnect
             * path, in this same file: "BTstack emits the event BEFORE
             * hid_host_finalize_connection frees the slot, hence the 250 ms timer".
             * Calling back into BTstack from inside its own event is the pattern, and
             * recognising it once did not stop me repeating it. */
            gBatStateAsked = 1;
            if (gBatCid != 0) {
                if (gBatArmed) btstack_run_loop_remove_timer(&gBatTimer);
                btstack_run_loop_set_timer_handler(&gBatTimer, &bat_state_timeout);
                btstack_run_loop_set_timer(&gBatTimer, kBatChainMs);
                btstack_run_loop_add_timer(&gBatTimer);
                gBatArmed = 1;
            }
        } else {                        /* Input 48's answer -- the chain's last step */
            gBatStHs  = 0x100UL | hs;
            gBatStLen = (unsigned long)rlen;
            gBatStVal = packed;
            /* ★ v15.2: this device is answered for. Release it and let the scheduler
             * pick up the next connected one; from a TIMER, never from inside this
             * event, which is the rule the whole chain exists to respect. */
            { short sl = HidDevSlotForCid((unsigned long)
                           hid_subevent_get_report_response_get_hid_cid(packet));
              if (sl >= 0) gHidDev[sl].batDone = 1; }
            gBatCid = 0;
            BatArmNext();
        }
        break;
    }

    case HID_SUBEVENT_SET_REPORT_RESPONSE:
        /* ⭐⭐ THE CAPS LED ORACLE, AND THE REASON gLedRc ALONE IS NOT ONE.
         *
         * gLedRc records what hid_host_send_set_report RETURNED, and that call does
         * almost nothing: a connection lookup, an MTU check, five field stores. It
         * answers ERROR_CODE_SUCCESS long before a byte reaches the keyboard. Reading
         * it as "the LED was set" is the same mistake as the beep test, which proved
         * the Sound Manager scales its own output and was taken as proof the hardware
         * level moved.
         *
         * THIS is the keyboard's own answer, and it is specific enough to name the
         * defect rather than just report one (btstack_hid.h:75):
         *      0x100  SUCCESSFUL           -- the LED byte was accepted
         *      0x101  NOT_READY            -- busy; retransmit
         *      0x102  ERR_INVALID_REPORT_ID -- ⭐ report ID 1 is WRONG for OUTPUT.
         *                                    The input reports are ID 1, and I chose 1
         *                                    for output on that symmetry alone; this is
         *                                    the code that would prove it a guess.
         *      0x103  ERR_UNSUPPORTED_REQUEST -- no SET_REPORT on this device
         *      0x104  ERR_INVALID_PARAMETER
         *      0x10E  ERR_UNKNOWN   0x10F  ERR_FATAL
         *
         * ⚠ If this word stays 0 while kWLedSends is nonzero, the request left but
         * nothing came back -- look at the control channel, not at the report ID. */
        gLedRsp = 0x100UL | (unsigned long)
            hid_subevent_set_report_response_get_handshake_status(packet);
        break;

    case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
        gBhDescAvail++;
        break;

    case HID_SUBEVENT_REPORT:
        /* ★★★★★ A REAL HID REPORT, AND THIS IS THE PATH THAT ACTUALLY RUNS.
         *
         * ⚠⚠ THE COMMENT HERE USED TO SAY the capture was "into the SAME buffer our own
         * decoder used, so BTCheck's existing byte dump and boot-report decode read it
         * unchanged". The byte dump did. THE DECODE DID NOT: BTCheck reads the block
         * word kWHidDecodeRc, and that word is written only where the decoder runs --
         * which was the dormant l2cap handler. So v10.2 captured 50 perfect 11-byte
         * reports and printed the decoder as NOT RECEIVED. An assumption about a call
         * graph, stated in a comment, which expired without anyone noticing. */
        gBhReports++;
        {
            const uint8_t *r = hid_subevent_report_get_report(packet);
            unsigned short n = hid_subevent_report_get_report_len(packet);
            unsigned short i;
            unsigned short cap = n;
            gHidFirstDataFull = (unsigned long)n;
            if (cap > kHidCapBytes) cap = kHidCapBytes;
            if (gHidDataPkts == 0) {
                for (i = 0; i < cap; i++) gHidFirstData[i] = r[i];
                gHidFirstDataLen = (unsigned long)cap;
            }
            gHidLastDataLen = (unsigned long)cap;
            gHidDataPkts++;
            /* ⭐ DECODE THE FULL REPORT, not the capped copy. kHidCapBytes is 16 and a
             * report is 11, so they coincide today -- but handing the decoder a
             * truncated length would silently reject every report the moment a device
             * sent more than 16 bytes, and the capture cap exists for the LOG, not for
             * the decode. Keeping them separate is the difference between a display
             * limit and a functional one. */
            BhRecordReport((unsigned long)hid_subevent_report_get_hid_cid(packet), r, n);
        }
        break;

    default:
        gBhOtherEvts++;
        break;
    }
}

void BT_StackStart(void)
{
    if (gStarted) return;        /* ConfigDone can run again after a bus reset */
    gStarted = 1;

    /* Order matters and is BTstack's, not ours: memory pools, then the run loop,
     * then HCI on top of the transport. */
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_os9_get_instance());

    /* ⚠ NO LONGER NULL. Up to v1.10 this passed NULL, which hci.c tolerates by
     * guarding every use with `if (!hci_stack->link_key_db) return;` -- safe, but it
     * threw away the link key from every successful pairing, so `link key requests`
     * read 0 in runs 18-21 and each session re-paired from scratch. That is fatal for
     * a keyboard: you cannot re-pair one without a keyboard.
     *
     * ⚠ Every callback behind this pointer runs at SECONDARY INTERRUPT LEVEL, so the
     * implementation is RAM-only. The file half is §6.3-6.4 of docs/M3-DESIGN.md, via
     * the NMInstall defer-to-task-level trampoline. */
    hci_init(hci_transport_os9_instance(), NULL);

    /* ★★★★ APPLE'S OWN CREATE_CONNECTION PARAMETERS FOR THIS CARD, read out of
     * Apple's OS 9 Bluetooth source rather than guessed for a sixth time.
     *
     * vendor/bt-control-center/USB Bluetooth/HCI/HCI_Events.c:59 and
     * BTCC/BluetoothInterface.c:42 both set, for every outgoing connection:
     *
     *     theConnectionData.Packet_Type       = 0x0800;   // DM1
     *     theConnectionData.Allow_Role_Switch = 0;
     *
     * BTstack sends 0xFF1E and 1. Both of the two parameters left standing after five
     * refuted theories are ones Apple sets DIFFERENTLY -- and this card rejected
     * Create_Connection with 0x12 Invalid HCI Command Parameters twice, with a link
     * open and again with none.
     *
     * ⚠ hci_enable_acl_packet_types FILTERS what hci_usable_acl_packet_types returns,
     * and that function then flips the "shall not be used" bits with ^ 0x3306. So
     * DM1-only comes out as 0x330E on the wire, NOT Apple's literal 0x0800 -- DM1
     * permitted, every EDR type forbidden. That is the same INTENT in the modern
     * convention, and it is as close as a public API can get without patching BTstack.
     *
     * ⚠⚠ AND APPLE'S COMMENT MAY NOT MATCH APPLE'S VALUE. Under the Bluetooth 1.1
     * table current when that code was written, 0x0008 is DM1 and 0x0800 is DH3 -- so
     * the "// DM1" comment sits beside a value that is not DM1 in either convention I
     * can check. What is trustworthy is the BYTE THEY SEND, not their gloss on it; the
     * Create_Connection capture already records ours, so the two are comparable in the
     * next log rather than argued about here.
     *
     * ⚠ DM1 caps a link at 17 bytes of payload. Irrelevant for a keyboard, and this is
     * a diagnostic step: if it works, widening is a later measurement. */
    hci_enable_acl_packet_types(ACL_PACKET_TYPES_DM1);
    gap_set_allow_role_switch(false);
    /* ★★★★★★ v16.0: TIGER'S PAGE TIMEOUT, AND THIS ONE COSTS BLUETOOTH NOTHING.
     *
     * ⚠⚠ MEASURED 2026-09-29: our reconnection sweep DEAFENS the AirPort Extreme card
     * in this same Mac. BTstack's default page_timeout is 0x6000, "ca. 15 sec"
     * (vendor/btstack/src/hci.c:5569), and a page is CONTINUOUS transmission across
     * 2.4 GHz from inches away. Our sweep then starts another pass 15 s later, so the
     * radio is transmitting ~15.4 s out of every ~30.4 s. The AirPort receiver went
     * deaf for exactly that: beacons 10/s -> 0-2/s, all frames 55/s -> 1-14/s, zero FCS
     * failures, receive-only. Unplugging the adapter removed it completely: 0 of 74 s.
     *
     * There is no hardware arbitration available to fix this -- the card's SPROM
     * boardflags are 0x000A and BTCOEXIST (0x0001) is CLEAR. Tiger coexists by keeping
     * its radio QUIET, and its own captures show page timeout 0x2000 = 5.12 s as the
     * resting value (it raises it to 10.24 s only inside a user-initiated pairing, then
     * restores it immediately).
     *
     * ⚠⚠ THE LINE THAT USED TO FOLLOW THIS ONE WAS WRONG, AND IT COST A HARDWARE RUN.
     * It read: "A device that is present answers a page in far less than 5 s; one that
     * is absent simply fails sooner. So this is a 3x cut in transmit time for no
     * behavioural loss." The A1016 in pairing mode, feet away, answered 0x04 PAGE
     * TIMEOUT on 2026-10-04. A keyboard in pairing mode page-scans slowly to save its
     * battery, so it is exactly the case that needs longer than 5.12 s -- and it is the
     * case this project exists to serve.
     *
     * ⇒ 0x2000 remains the RESTING value, which is the part Tiger's captures support
     * and the part AirPort needs. The other half of Tiger's behaviour -- raising it to
     * 10.24 s inside a user-initiated pairing and restoring it immediately -- now
     * exists too: see BT_OpenPagingWindow / BT_ClosePagingWindow at the top of this
     * file. Nothing pages on its own any more (the sweep went in v16.1), so the radio
     * is still quiet unless the user asked for a pairing. */
    gap_set_page_timeout(kPageTimeoutResting);
    /* ⚠ EVERY hci_* CALL DEREFERENCES hci_stack, WHICH THIS LINE IS THE BIRTH OF.
     *
     * gStarted above is NOT a usable stand-in: it is set before hci_init so that a
     * re-entrant ConfigDone cannot start the stack twice, which leaves a window where
     * gStarted is 1 and hci_stack is not yet valid. The pump timer runs at secondary
     * interrupt level and can fire inside that window, and BT_TryStoredKeyProbe would
     * then read through an uninitialised pointer -- a hard hang with no NMI, which is
     * this project's worst failure mode.
     *
     * ⚠ The window is real but has never been observed to fire: hci_get_state() has
     * been called unguarded from that same handler for many runs. So this closes a
     * latent hazard rather than a known defect -- do not read it as the explanation
     * for anything that has already gone wrong. */
    gHciReady = 1;
    hci_set_link_key_db(bt_linkkey_db_os9_instance());

    /* ⚠ ORDER IS NOT FREE HERE, and it is BTstack's order, not a preference.
     * l2cap_init() registers L2CAP's own ACL and event handlers with hci, so it
     * must follow hci_init(); sdp_client_init() then builds on L2CAP. Calling
     * either before hci_init leaves the handler registrations pointing at an
     * uninitialised hci_stack.
     *
     * Both were linked into the extension from the first M2 build but never
     * initialised, which is why run 15 reached HCI_STATE_WORKING and then had
     * nothing above HCI to do. These two lines are the actual start of L2CAP. */
    l2cap_init();
    gL2Init = 1;
    sdp_client_init();
    gL2Init = 2;

    /* ★★ M4 STEP 1: LISTEN ON THE HID CONTROL PSM.
     *
     * ⚠ SECURITY LEVEL IS LEVEL_2 (encryption required), which is the CORRECT value
     * for a HID host rather than the permissive one. LEVEL_0 would accept anything and
     * would maximise the chance of the first test "working", but a keyboard that
     * connects unencrypted is not a keyboard we could ship -- and if the level turns
     * out to be why a connection is refused, the counters below say so explicitly
     * rather than leaving us to wonder. Lower it only with evidence.
     *
     * ⚠⚠⚠ AND THE PARAGRAPH ABOVE WAS RIGHT ALL ALONG. LEVEL_0 was tried, measured,
     * and is now OFF: it removed the security request entirely, so the link was never
     * encrypted and the A1016 stopped answering us (0x69 RTX timeout after 4 ACL
     * packets it heard and ignored). kHidLevel0 stays 0; kHidSynthFeatures is what
     * actually releases the gate. See bt_pump.h.
     *
     * ⚠⚠⚠⚠ AND THAT CONCLUSION EXPIRED AT v8.8. "kHidLevel0 stays 0" is no longer
     * true and the reasoning behind it was incomplete, so read the two together
     * rather than trusting the older paragraph: the finding above -- LEVEL_0 leaves
     * the link unencrypted and the keyboard will not talk over an unencrypted link --
     * is CORRECT and has been re-measured twice. What was wrong was treating that as
     * a reason to go back to LEVEL_2, because LEVEL_2 never encrypts anything either:
     * hci.c gates authentication on a remote-features read that outlives the link, so
     * it deadlocks before any security happens.
     *
     * ⇒ The third option is the one that works. Stay at LEVEL_0, which is the only
     * level under which the incoming connection is even VISIBLE to us, and encrypt
     * the link OURSELVES with a raw Authentication Requested -- see kHidAutoAuth in
     * bt_pump.h and the fire site in HCI_EVENT_CONNECTION_COMPLETE. Both gates are
     * DIAGNOSTIC and both go to 0 before anything ships, so the LEVEL_2 registration
     * above is still what a shipping build compiles.
     *
     * MTU 672 is L2CAP's default and what Apple's HID driver uses for the control
     * channel; the interrupt channel at PSM 0x13 comes in step 2. */
    gHidLevel = kHidLevel0 ? 0 : 2;
    /* ★★★★★★ v9.5: HAND THE JOB TO BTstack'S OWN HID HOST.
     *
     * ⚠⚠ TWO SERVICES ON ONE PSM WOULD BE A CONFLICT, so when the canonical host is
     * driving, OUR registrations must not happen at all. That is the whole reason
     * this gate wraps the block rather than sitting beside it.
     *
     * WHY. hid_host.c is 1440 lines, has been LINKED INTO EVERY BUILD THIS PROJECT
     * HAS SHIPPED (vendor/btstack-files.txt:45) and was never called; we hand-rolled
     * the same job and have spent eleven versions re-deriving its decisions one
     * command at a time. It differs from ours in exactly the places we have been
     * guessing: MTU 0xffff not 672, gap_get_security_level() rather than a forced
     * level, it does not accept PSM 0x11 immediately, and it knows HID_v1.1.1 §5.2.2
     * (both control AND interrupt channels shall always be opened).
     *
     * ⭐ And hid_host_accept_connection() TAKES THE PROTOCOL MODE. Passing
     * HID_PROTOCOL_MODE_REPORT is the Consumer-page unlock that v8.6's hand-rolled
     * SET_PROTOCOL(0x71) was built to reach -- handled by maintained code that also
     * does the handshake and the fallback.
     *
     * ⚠ MAX_NR_HID_HOST_CONNECTIONS is already 1 in btstack_config.h:103, so
     * btstack_memory_hid_host_connection_get() draws from a STATIC POOL. No malloc,
     * which matters because HAVE_MALLOC is undefined in this build.
     *
     * ⚠ The descriptor store is required by the API even though an INCOMING
     * connection never fetches one over SDP (hid_host.c sets hid_descriptor_status to
     * UNSUPPORTED for incoming links). 256 bytes against the A1016's real 99-byte
     * descriptor, which docs/A1016-REPORT-DESCRIPTOR.md records. */
    /* ⚠⚠ AN `else`, NOT AN EARLY RETURN, AND THE FIRST DRAFT GOT THAT WRONG.
     * Returning here skipped the shared tail of this function -- including
     * hci_power_control(HCI_POWER_ON), which is what starts BTstack's ENTIRE
     * bring-up. The driver would have bound and done nothing at all: no reset, no
     * scan enable, no link. A wasted boot that would have read as a total failure of
     * the canonical HID host rather than as my control-flow bug. Caught by the
     * falsification pass, not by hardware.
     *
     * ⚠ And the callback is registered BEFORE hid_host_init, because init registers
     * the L2CAP services and hid_host's emit paths dereference hid_host_callback. */
    if (kHidUseBtstackHost) {
        /* ★★★★★★ v9.6: TIGER DOES NOT AUTHENTICATE AND DOES NOT ENCRYPT.
         *
         * Measured on the working reference, same card, same keyboard:
         *     defaults read com.apple.Bluetooth
         *       AuthenticationEnable = 0;
         *       EncryptionEnable     = 0;
         * and system_profiler reports "Requires Authentication: No" on the adapter
         * and on every service, while the keyboard reconnects in 2-3 seconds and
         * works completely -- volume and eject included.
         *
         * ⇒ The premise of v8.6 through v9.5 was WRONG. Eight builds tried to
         * authenticate and encrypt this link because the HID profile says a host
         * should. Our own working reference does neither. v8.7's reading of LEVEL_0
         * -- "nothing encrypts the link and the keyboard refuses" -- had the
         * causation backwards: the keyboard was never refusing for want of security.
         *
         * ⚠⚠ AND THIS LINE IS THE LEVER, WHICH WE HAD NEVER PULLED. kHidLevel0 only
         * ever reached OUR l2cap_register_service calls, and with the canonical host
         * driving those do not run at all -- hid_host registers with
         * gap_get_security_level(), which hci.c:5557 defaults to LEVEL_2. So v9.5 as
         * staged would have run the canonical host at LEVEL_2, the OPPOSITE of Tiger,
         * while its gate marker said LVL2 about a gate that no longer controlled
         * anything. One gate now means one thing in both places.
         *
         * ⚠ BEFORE hid_host_init, and that ordering is load-bearing: init registers
         * both L2CAP services and reads the level AT THAT MOMENT
         * (hid_host.c:1180-1181). Setting it afterwards would compile, run, and
         * silently register at the old level. */
        gap_set_security_level(kHidLevel0 ? LEVEL_0 : LEVEL_2);
        hid_host_register_packet_handler(&hid_host_evt_handler);
        hid_host_init(gHidDescStore, (uint16_t)sizeof(gHidDescStore));
        gBtstackHostInit = 1;
        /* ⚠ READ IT BACK, sampled AFTER init, because a gate is an intention and
         * this is what hid_host actually registered with. v9.5 is the whole argument
         * for this row: its marker said LVL2 about a gate that had stopped
         * controlling anything. 0x100 | level so 0 means LEVEL_0 rather than
         * "never sampled". */
        gGapLevel = 0x100UL | (unsigned long)gap_get_security_level();
    } else {
    gHidListenRc = 0x100UL | (unsigned long)
        l2cap_register_service(&hci_event_handler,
                               BLUETOOTH_PSM_HID_CONTROL, 672,
                               kHidLevel0 ? LEVEL_0 : LEVEL_2);
    /* ★★★★ AND THE INTERRUPT CHANNEL, PSM 0x13 -- "step 2" that every previous run
     * left unbuilt. Reports arrive HERE, not on the control channel, so without this
     * listen a keyboard that accepted SET_PROTOCOL would have nowhere to send them and
     * the run could not tell success from silence. Same security level as the control
     * channel, deliberately: a mismatch would make one of the two channels demand
     * authentication the other does not, which is the deadlock in a new place. */
    gHidIntrListenRc = 0x100UL | (unsigned long)
        l2cap_register_service(&hci_event_handler,
                               BLUETOOTH_PSM_HID_INTERRUPT, 672,
                               kHidLevel0 ? LEVEL_0 : LEVEL_2);
    }
    gL2Init = 3;

    gHciEventCallback.callback = &hci_event_handler;
    hci_add_event_handler(&gHciEventCallback);

    /* ⚠ NO link key database is registered, deliberately. hci.c guards every use
     * with `if (!hci_stack->link_key_db) return;`, so this is safe rather than a
     * missing piece. The consequence is user-visible and belongs to M3: link keys
     * are not persisted, so a pairing will not survive a reboot. */

    /* ★★★★★★ v10.0: BECOME THE MASTER ON ACCEPT, WHICH IS WHAT TIGER DOES.
     *
     * ⚠⚠ MEASURED DIFFERENCE, not a theory. Tiger's trace, same card, same keyboard
     * (docs/TIGER-HCI-TRACE.md):
     *
     *     21.793  EVT<  Connection_Request                    (the keyboard pages us)
     *     21.814  CMD>  Accept_Connection_Request <addr> 00   ⭐ ROLE 0x00
     *     22.029  EVT<  Role_Change                           ⭐ and it ACTUALLY HAPPENS
     *
     * Per the spec, role 0x00 means "become the Master for this connection; the LM
     * will perform the role switch" and 0x01 means "remain the Slave". BTstack defaults
     * to ⭐ hci.c:5551 `hci_stack->master_slave_policy = 1` -- REMAIN SLAVE -- and
     * passes it straight into the accept at hci.c:7931. So on every run so far the
     * keyboard has stayed master and we have stayed slave, which is the opposite of
     * the working reference.
     *
     * ⇒ WHY THAT PLAUSIBLY EXPLAINS `0x13 events 0`. A BR/EDR slave may only transmit
     * in a slot where the master has polled it. Our controller accepts the ACL packet
     * over USB, queues it, and can only put it on the air when the A1016 polls us --
     * and Number_Of_Completed_Packets is emitted on TRANSMISSION, not on acceptance.
     * A master that polls us rarely or never leaves the packet queued forever, which
     * is exactly the shape we measure: writes accepted, status 0, nothing acknowledged
     * for the full 20 s the link is up. The A1016 is a battery device that pages its
     * host and clearly expects the host to take mastership, since that is what Apple's
     * own stack does 21 ms after the page.
     *
     * ⚠ IT IS A HYPOTHESIS AND THE INSTRUMENT SAYS SO. We have NEVER handled
     * HCI_EVENT_ROLE_CHANGE at all -- see kWRoleChanges -- so "did the switch happen"
     * has been unmeasurable for ten builds. This run measures it either way.
     *
     * ⚠ I listed "role switch doesn't matter" among the claims I had retracted, and
     * then did nothing about the retraction for several builds. The retraction was the
     * finding; this is acting on it.
     *
     * ⚠ NOT GATED, deliberately: this is what the reference implementation does, so it
     * is intended shipping behaviour rather than a diagnostic. The gate marker stays
     * the same shape, which is itself information -- the diagnostic gates are
     * unchanged from v9.9. Flipping it back is a one-line edit and a rebuild.
     *
     * ⚠ MUST PRECEDE hci_power_control: the policy is read when an incoming connection
     * is accepted, and this is the only place we can be sure runs first. */
    hci_set_master_slave_policy(0);   /* 0 = become master, as Tiger does */

    /* Asks BTstack to run its own initialisation over our transport. Everything
     * after this point is driven by completions calling BT_DeliverPacket and
     * BT_Pump. */
    hci_power_control(HCI_POWER_ON);

    /* One pump so the first command goes out now rather than waiting for the
     * first unsolicited completion to arrive. Without this, bring-up would stall
     * until something happened to interrupt us, which on an idle controller may
     * be never. */
    BT_Pump();
    BT_StackPoll((unsigned long)hci_get_state());
}

/* ★★ THE ON/OFF CONTROL, made real.
 *
 * The panel's radio buttons were scaffolding: they moved and did nothing. This is what
 * they now drive.
 *
 * ⚠ WHAT "OFF" HONESTLY MEANS HERE, because the word invites a bigger claim than the
 * code makes. This does NOT power the radio down. It clears page scan and inquiry
 * scan, so the Mac stops being discoverable and stops accepting incoming connections
 * -- the two things a user actually means by "off". A true power-down is
 * hci_power_control(HCI_POWER_OFF), which tears the stack down and would have to
 * rebuild it to come back; that is a state machine we have never exercised, and
 * introducing it days after an unexplained freeze would be the wrong order of work.
 *
 * These are the same two calls bring-up already makes, so the path is proven. Safe at
 * secondary interrupt level for exactly the reason every other gap_* call here is:
 * they queue an HCI command, they do not block. */
/* ★★★★ M3.7: PAIR WITH A CHOSEN DEVICE, AS THE INITIATOR.
 *
 * ⭐ WHY THIS IS THE RIGHT PATH, after five runs down the wrong one. Everything since
 * v6.6 has driven the RESPONDER case: an already-paired A1016 pages us, and BTstack
 * correctly declines to initiate authentication because the peer is supposed to. That
 * is M4, which PROXY-FIRST-PLAN §3 already demoted to a fallback, and
 * Authentication_Requested never went out because nothing on that path sets
 * BONDING_SEND_AUTHENTICATE_REQUEST for us.
 *
 * gap_dedicated_bonding is the initiator path, and hci.c:2843 is why it should work
 * where five runs did not:
 *
 *     conn->bonding_flags |= BONDING_RECEIVED_REMOTE_FEATURES;
 *     if (conn->bonding_flags & BONDING_DEDICATED){
 *         conn->bonding_flags |= BONDING_SEND_AUTHENTICATE_REQUEST;
 *     }
 *
 * Dedicated bonding sets BONDING_DEDICATED (hci.c:9207), so the remote-features event
 * sets the authenticate flag DIRECTLY -- gap_request_security_level, and both of its
 * branches we could not distinguish, are not involved at all. And our synthetic 0x0B
 * supplies that event on a controller that withholds it.
 *
 * ⚠ AND IT IS NOT NEW MACHINERY. Run 18 already paired successfully as initiator --
 * SSP auto-accept, IO capability request, user confirm request, Encryption Change,
 * Read_Encryption_Key_Size -- against a phone, on the dongle. The pairing engine
 * works; what has never been tried is pointing it at the A1016 through the A1044.
 *
 * ⚠⚠ gap_dedicated_bonding DELETES THE EXISTING LINK KEY FIRST (hci.c:9201,
 * gap_drop_link_key_for_bd_addr). For the A1016 that is the intent -- we are replacing
 * a Tiger-made pairing with one of our own -- but it means a FAILED attempt leaves the
 * device less paired than before, in OUR database. It does NOT touch the card's own
 * store, which is what actually makes the keyboard work in proxy mode, so the user's
 * working keyboard survives a failure here. That asymmetry is the reason this is safe
 * to try at all, and it must not be broken by later "tidying" that also clears the
 * controller's store.
 *
 * ⚠ MITM protection is 0: LEVEL_2 rather than LEVEL_3. An A1016 predates SSP and has
 * no display or keyboard-confirm, so demanding MITM protection would ask for something
 * the device cannot provide -- hci_ssp_security_level_possible_for_io_cap enforces
 * exactly that, and run 18's note in this file already recorded it. */
int BT_PairDevice(unsigned long hi, unsigned long lo)
{
    bd_addr_t addr;

    if (hci_get_state() != HCI_STATE_WORKING) {
        gPairRc = 0x100UL | 0xFEUL;      /* our own code: stack not up */
        return -1;
    }
    addr[0] = (uint8_t)((hi >> 16) & 0xFF);
    addr[1] = (uint8_t)((hi >>  8) & 0xFF);
    addr[2] = (uint8_t)( hi        & 0xFF);
    addr[3] = (uint8_t)((lo >> 16) & 0xFF);
    addr[4] = (uint8_t)((lo >>  8) & 0xFF);
    addr[5] = (uint8_t)( lo        & 0xFF);

    gPairAsked++;
    gPairAddrHi = hi;
    gPairAddrLo = lo;

    /* ★★★★ IF WE ARE ALREADY CONNECTED TO THIS ADDRESS, AUTHENTICATE THAT LINK --
     * DO NOT CREATE A SECOND ONE.
     *
     * v7.5 named the failure precisely: two rejections, both
     *     opcode 0x0405  status 0x12   Create_Connection
     * with parameters that are spec-valid in every field (the packet-type mask was
     * 0xFF1E, BTstack's ordinary value, and it comes from LOCAL features so the
     * synthetic remote ones cannot affect it).
     *
     * ⇒ The command is not malformed, it is INAPPLICABLE. The same run shows
     * ACL links up 2, links down 1 -- a link still open at the end -- and Connection
     * Requests 2, because the A1016 pages us continually. Issuing Create_Connection
     * for an address we already hold a link to is invalid; the spec's preferred answer
     * is 0x0B ACL Connection Already Exists, and this firmware answers 0x12.
     *
     * ⚠ THIS IS A HYPOTHESIS WITH ITS OWN DISCRIMINATOR, not a conclusion. gPairPath
     * records which branch ran and gLinkWasUp records the link state at decision time,
     * so a run where the branch was NOT taken still says why. If authenticating the
     * existing link also fails, the address-already-connected theory is dead and the
     * remaining candidate is Allow_Role_Switch.
     *
     * ⭐ And it is the better architecture regardless. The keyboard connects to US;
     * five runs of responder work failed only because BTstack would not initiate
     * authentication on that link. Doing it explicitly is the missing step, not a
     * workaround for it. */
    /* ★★★★ ARM FOR THE NEXT INBOUND CONNECTION, whatever happens below.
     *
     * The A1016 pages us constantly and answers inquiries -- but it is never
     * connected at the instant the user clicks Pair, which is why v7.6's
     * authenticate-the-existing-link branch has never fired (link open at decision 0,
     * twice). Create_Connection, the only way to make a link ourselves, is rejected.
     *
     * ⇒ So stop requiring a link to exist NOW. Remember the address, and authenticate
     * the moment that device connects to US. The keyboard supplies the link; we supply
     * the authentication that five runs of responder work showed BTstack will not
     * initiate on its own.
     *
     * ⚠ Armed BEFORE the Create_Connection attempt, not instead of it. If the command
     * is ever accepted the arm is harmless -- the resulting Connection Complete is
     * exactly what it waits for. And if it is rejected, the arm is the whole mechanism.
     *
     * ⚠ NOT A LATCH. Cleared when it fires and when the pairing completes, so a failed
     * attempt does not leave the driver authenticating every future connection from
     * that address forever. [[feedback_guards_must_not_latch]]. */
    gArmedHi = hi;
    gArmedLo = lo;
    gArmed   = 1;

    gLinkWasUp = 0x100UL | (gLastConnHandle != 0 ? 1UL : 0UL);
    if (gLastConnHandle != 0 && gConnPeerHi == hi && gConnPeerLo == lo) {
        gPairPath = 1;                       /* authenticate the existing link */
        gPairRc = 0x100UL | (unsigned long)
                  (hci_send_cmd(&hci_authentication_requested,
                                (hci_con_handle_t)gLastConnHandle) & 0xFF);
        return 0;
    }
    /* ★★★★ NO LINK: ARM AND WAIT. DO NOT TRY TO PAGE THE DEVICE.
     *
     * ⚠⚠ THIS USED TO CALL gap_dedicated_bonding, AND THAT CALL WAS ACTIVELY HARMFUL.
     *
     * Every Create_Connection this driver has ever sent to this card has been refused.
     * Before v8.0 it was 0x12 Invalid HCI Command Parameters, cured by the Bluetooth
     * 1.2 packet-type fix. It is now 0x0C Command Disallowed -- and the v93 run showed
     * what that costs, because the refusal does not merely fail:
     *
     *     rejections 3
     *       opcode 0x0405  status 0x0C   Create_Connection
     *       opcode 0x0401  status 0x0C   Inquiry          <== the poison spreads
     *
     * ⇒ The refused attempt leaves an outstanding connection request in the
     * controller, and from then on it refuses to INQUIRE as well. Scanning dies. And
     * Connection Requests stayed at 2 across a whole second attempt in which the user
     * switched the keyboard on -- so a controller with a connection attempt pending
     * very plausibly stops accepting inbound pages too, which would mean this call was
     * suppressing the very page the arm above is waiting for. Clicking Pair was
     * breaking the mechanism meant to answer it.
     *
     * ⭐ AND IT IS NOT NEEDED, WHICH IS MEASURED RATHER THAN ASSUMED. The Pixel paired
     * through the branch above: a bare hci_authentication_requested on a link the peer
     * had already brought up, which produced `legacy PIN requests 1 / ANSWERED 1` and a
     * new key. gap_dedicated_bonding's flags are not what makes legacy PIN pairing run.
     *
     * ⇒ The device supplies the link; we supply the authentication. That is the whole
     * architecture, and paging was never part of it. */
    /* ★★★★ ...EXCEPT THAT "DO NOT PAGE" IS A RULE ABOUT A BLUETOOTH 1.2 CONTROLLER,
     * AND IT WAS BEING APPLIED TO A BLUETOOTH 5.3 ONE.
     *
     * Everything above is still true OF THE A1044. Measured, three sessions, BTCheck
     * v99.63/v99.68/v99.72:  HCI version 2, LMP version 2 -- Bluetooth 1.2. That part
     * refuses Create_Connection and the refusal poisons its inquiry, so arm-and-wait is
     * correct there and is not being changed.
     *
     * ⚠⚠ BUT THE DONGLE IS NOT THAT PART. 2026-10-04, same counters, same reader:
     *       HCI version 12   LMP version 12   manufacturer 0x000A
     * LMP 12 is Bluetooth 5.3. It reports SSP and BLE present, and its supported-
     * commands octet 0 (0xBF) sets the Create_Connection bit. "BT DONGLE10" wears CSR's
     * old 0A12:0001 ID, but the silicon behind it is two decades newer.
     *
     * ⇒ AND ARM-AND-WAIT CANNOT WORK ON A NEW CONTROLLER, WHICH IS WHY THIS MATTERS.
     * The architecture note says "the device supplies the link". The A1016 supplies it
     * only to its BONDED host -- docs/A1016-REPORT-DESCRIPTOR.md, measured by the user
     * on 2026-09-09: it pages its bonded host on a keypress and holds ONE bond per host
     * address. It has been bonded to the A1044's address since it was paired under
     * Tiger, which is exactly why the inbound path has always worked there. A dongle is
     * an address that keyboard has never seen, so nothing will ever page us and the arm
     * waits forever. The user clicked Pair, got the passkey prompt, typed 0000, and
     * nothing happened -- because gPairPath 3 sends NOTHING. That is not a failure of
     * the pairing, it is the absence of one.
     *
     * ⇒ So page it, but ONLY from a controller modern enough to be asked. LMP >= 4 is
     * the Bluetooth 2.1 / SSP line and it cleanly separates the two parts we support:
     * the A1044 at 2 keeps today's behaviour byte for byte, the dongle at 12 gets the
     * initiator path the goal actually needs. ⚠ The A1044 is the priority device and
     * this gate is what keeps it out of the experiment entirely.
     *
     * ★ The arm above STAYS ARMED either way -- it is set before the link check and is
     * not cleared here. If the page is refused, or the keyboard pages us first, the
     * proven inbound path still answers. One mechanism does not replace the other.
     *
     * ⚠ gap_dedicated_bonding DELETES THE EXISTING LINK KEY FIRST (hci.c:9201). For a
     * device we are deliberately pairing afresh that is the intent, not a side effect.
     * It also disconnects once bonded -- which is what we want: the keyboard is then
     * bonded to THIS controller, so from that moment it pages US, and the whole
     * existing inbound HID path takes over unchanged.
     *
     * ⚠ gPairPath 2 names this branch, so a run where it did NOT fire still says so,
     * and gPairRc carries the controller's verdict. Both are NoteKeep'd as of 16.8, so
     * a teardown can no longer erase the answer. */
    {
        unsigned long lmp = (gLocalVer & 0x1000000UL)
                          ? ((gLocalVer >> 8) & 0xFFUL) : 0UL;
        if (lmp >= 4) {
            gPairPath = 2;          /* page it ourselves: modern controller */
            /* ⚠ BEFORE the call, not after: the page starts inside it. */
            BT_OpenPagingWindow();
            gPairRc   = 0x100UL | (unsigned long)
                        (gap_dedicated_bonding(addr, 0) & 0xFF);
            return 0;
        }
    }

    gPairPath = 3;                  /* armed, waiting for the device to page us */
    gPairRc   = 0x100UL;            /* armed successfully; nothing was sent */
    return 0;
}

/* ★★★★ AND THE STEP THE GOAL ACTUALLY NEEDS: put the key we just negotiated into the
 * CARD, so its on-chip proxy stack can reconnect the device with no host at all.
 *
 * ⚠⚠ CALLED ONLY FROM db_put_link_key's success path -- see the note at
 * bt_write_stored_link_key. The address and key come from our own database, having
 * just been produced by a real pairing. Never speculative, never from user input.
 *
 * ⚠ A refused send does NOT latch. gStoredKeyWrote counts attempts and gWroteKeyRc
 * carries the outcome, so a controller that is momentarily busy gets another chance
 * on the next pairing rather than being written off for the session --
 * [[feedback_guards_must_not_latch]]. */
void BT_WriteKeyToController(const unsigned char *addr, const unsigned char *key)
{
    bd_addr_t a;
    int i;
    if (hci_get_state() != HCI_STATE_WORKING) return;
    for (i = 0; i < 6; i++) a[i] = addr[i];
    gWroteKeyTried++;
    gWroteKeyRc = 0x100UL | (unsigned long)
                  (hci_send_cmd(&bt_write_stored_link_key, 1, a, key) & 0xFF);
}

/* ★★★★ DELETE ONE KEY FROM THE CONTROLLER'S OWN STORE (OGF 3 / OCF 0x12).
 *
 * ⚠⚠ THIS MUTATES STATE THAT SURVIVES A REBOOT AND THAT TIGER SHARES. The A1044's
 * store currently holds two keys and one of them is the A1016 pairing that makes the
 * user's keyboard work today. Deleting the wrong one costs them a working keyboard
 * until they boot Tiger and re-pair.
 *
 * ⇒ The panel is what chooses the address, from a row the user selected and confirmed
 * in a dialog that names it. Nothing here picks a target, and Delete_All_Flag is
 * ALWAYS 0 -- the "delete every key" form of this command is deliberately not
 * reachable from anywhere in this driver.
 *
 * ⭐ WHY THIS IS BEING BUILT NOW, and why it is safe to build now: the store holds a
 * key for a Mitsumi-made device the user does not own and cannot identify. Deleting
 * THAT entry costs nothing, and it is the right subject for proving this command works
 * before it is ever pointed at the keyboard's key. Test the destructive command on the
 * entry nobody needs.
 *
 * ⚠ It also tests something never yet established: that the card's store is WRITABLE
 * at all. Read_Stored_Link_Key proved it can be read. Nothing has proved a host can
 * change it, and Write_Stored_Link_Key -- the whole goal -- depends on that. A
 * successful delete is the cheapest possible proof, on the safest possible entry. */
/* ★★★★★★ v12.3: DROP THE LIVE LINK TO ONE PEER.
 *
 * ⚠⚠ THE REASON THIS EXISTS AT ALL: deleting a link key NEVER disconnects anything.
 * A bond is consulted when a link is ESTABLISHED; an established link keeps running
 * regardless. Every delete this project has performed left the keyboard typing, which
 * made a working delete look like a broken one -- twice, in two different sessions.
 *
 * ⚠ Only disconnects if the live connection is actually the address asked for.
 * gConnPeerHi/Lo are written by Connection Complete, so they name the peer on the
 * current link. Disconnecting blind would drop whatever happened to be connected,
 * which on this machine could be a device the user never touched.
 *
 * ⚠ gap_disconnect is asynchronous: it requests the disconnect and the link goes down
 * when the controller says so. Returns 1 if a disconnect was requested.
 * ⚠ Interrupt-level safe -- it queues, it does not block. */
/* ⚠⚠ THIS USED TO COMPARE AGAINST gConnPeerHi/Lo AND gHciConnHandle, WHICH ARE ONE
 * PEER, NOT A LIST. They are written by Connection Complete, so they name whichever
 * device connected MOST RECENTLY. With a keyboard and a phone both connected that is a
 * coin toss: the user selected the phone and got a greyed-out Disconnect because the
 * keyboard had connected last, and a disconnect issued for the phone would have
 * silently done nothing -- or, worse, matched and dropped the wrong device.
 *
 * ⇒ ASK BTSTACK, which actually keeps a connection list. hci_connection_for_bd_addr_and_type
 * answers "is THIS address connected right now, and on what handle" without this driver
 * having to maintain a shadow of state BTstack already owns -- and a shadow of exactly
 * that kind is what produced the bug. */
int BT_ConnHandleForAddr(unsigned long hi, unsigned long lo)
{
    bd_addr_t a;
    hci_connection_t *c;
    a[0] = (uint8_t)((hi >> 16) & 0xFF); a[1] = (uint8_t)((hi >> 8) & 0xFF);
    a[2] = (uint8_t)( hi        & 0xFF); a[3] = (uint8_t)((lo >> 16) & 0xFF);
    a[4] = (uint8_t)((lo >>  8) & 0xFF); a[5] = (uint8_t)( lo        & 0xFF);
    c = hci_connection_for_bd_addr_and_type(a, BD_ADDR_TYPE_ACL);
    if (c == NULL) return 0;
    return (int)c->con_handle;
}

int BT_DisconnectByAddr(unsigned long hi, unsigned long lo)
{
    int h = BT_ConnHandleForAddr(hi, lo);
    if (h == 0) return 0;
    gDiscReqs++;
    gDiscRc = 0x100UL | (unsigned long)(gap_disconnect((hci_con_handle_t)h) & 0xFF);
    return 1;
}

int BT_DeleteStoredKey(unsigned long hi, unsigned long lo)
{
    bd_addr_t addr;
    if (hci_get_state() != HCI_STATE_WORKING) {
        gDelStoredRc = 0x100UL | 0xFEUL;      /* ours: the stack is not up */
        return -1;
    }
    addr[0] = (uint8_t)((hi >> 16) & 0xFF);
    addr[1] = (uint8_t)((hi >>  8) & 0xFF);
    addr[2] = (uint8_t)( hi        & 0xFF);
    addr[3] = (uint8_t)((lo >> 16) & 0xFF);
    addr[4] = (uint8_t)((lo >>  8) & 0xFF);
    addr[5] = (uint8_t)( lo        & 0xFF);

    gDelStoredTried++;
    gDelStoredHi = hi;
    gDelStoredLo = lo;
    /* ⚠ Delete_All_Flag = 0: this address only. Never 1. */
    gDelStoredRc = 0x100UL | (unsigned long)
                   (hci_send_cmd(&hci_delete_stored_link_key, addr, 0) & 0xFF);
    return 0;
}

unsigned long BT_SetRadio(int on)
{
    if (on) {
        gap_connectable_control(1);
        gap_discoverable_control(1);
        gScanMode = 3;
    } else {
        gap_discoverable_control(0);
        gap_connectable_control(0);
        gScanMode = 0;
    }
    gRadioOn = on ? 1UL : 0UL;
    return gRadioOn;
}

