/*
 *  bt_check.c  --  BTCheck: read the Bluetooth probe driver's counter block.
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  The USB Expert's log will not carry our driver's messages. Established over
 *  three hardware runs: the driver loads and its Initialize runs to completion
 *  (the Expert itself logs "calling driver initialize routine" then "driver
 *  initialization completed"), Prober's Status Level is at its maximum of 5, and
 *  four probes emitted through BOTH USBExpertStatus and USBExpertStatusLevel --
 *  two of them beginning with a space so they would survive a length check --
 *  produced nothing, while 45 "Driver -" lines from Apple's own drivers appear
 *  in the same log. The mechanism works. It will not work for us.
 *
 *  So the driver keeps its own counters in a 128-byte block in its data section
 *  and this app finds it by scanning the System heap for the magic. That is
 *  exactly how FWFixCheck reads the FireWire hook's 'S8FX' block, and this file
 *  reuses its log-file and window scaffolding deliberately.
 *
 *  READ-ONLY. It walks the System heap and writes a log file. It touches no
 *  hardware, no registers and no device.
 *
 *  ⚠ THE FILE ON DISK CARRIES THE MAGIC TOO, with the counters zeroed, so a copy
 *  of the extension sitting in a disk cache buffer can match the scan. That is
 *  why "Initialize calls" is reported first and called out: zero counters mean
 *  the block found was the file, not the live driver.
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Events.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Folders.h>
#include <Processes.h>
/* ⚠ Added for DumpInstalledVersions: without it Get1Resource is implicitly declared
 * int, and the Handle it returns arrives through an int -- which happens to survive on
 * 32-bit PowerPC and would have shipped as a silent latent bug. */
#include <Resources.h>
#include <Files.h>
#include <Sound.h>
#include <Components.h>   /* FindNextComponent -- M6 step 1 */
#include <USB.h>
#include <CursorDevices.h>   /* v99.95: read every mouse's resolution/acceleration */
/* ⚠ NEW in v96, and it changes what this app links. The descriptor section reaches the
 * USB Expert's published properties through the Name Registry, because the descriptor
 * calls an application would rather use (USBGetConfigurationDescriptor and friends)
 * are USBServicesLib -- driver-level. Registry reads are read-only and app-legal;
 * fw-regdump has done exactly this since 2026-08. */
#include <NameRegistry.h>
#include <string.h>

#define kLingerTicks   (60 * 60)
/* ⚠ 96 -> 200. Summary() logs to file FIRST and only then stores for the on-screen
 * list, so the log file has always been complete -- but the window silently dropped
 * everything past line 96, and a typical run now emits well over that. Silent
 * truncation in a diagnostic is how a reader stops trusting the instrument. 200 Str255
 * is ~51 KB against a 640 KB preferred size. */
#define kMaxSummary    200
#define kLinesPerPage  22
#define kMaxHits       4

/* ---- Pascal-string helpers (same shapes as fw_fixcheck.c) ---------------- */

static void PStrCat(Str255 dst, const char *src)
{
    short n = dst[0];
    while (*src && n < 255) dst[++n] = (unsigned char)*src++;
    dst[0] = (unsigned char)n;
}

/* ⚠⚠ THE VERSION COMES FROM CMakeLists, and this is why.
 *
 * v46 wrote a log file called BTCheck_v45.log. The number lived in four places -- the
 * CMake target name, and the log filename, window title and banner below -- and three
 * of them were missed on the bump. The user had to rename the file by hand to work out
 * which run it belonged to, which is exactly the kind of ambiguity that has cost this
 * project whole hardware cycles before.
 *
 * CMakeLists now injects kBTCheckVer and names the app from the same string, so the
 * app on disk and every version it prints cannot disagree. The fallback below only
 * fires if someone compiles this file outside the CMake build, and says so loudly
 * rather than quietly claiming a version it is not. */
#ifndef kBTCheckVer
#define kBTCheckVer "?? (built outside CMake)"
#endif

/* Build "<prefix><version><suffix>" as a Pascal string. Used for the log filename,
 * the window title and the banner, so all three move together. */
static void PStrCat(Str255 dst, const char *src);
static void BTCheckName(Str255 out, const char *prefix, const char *suffix)
{
    out[0] = 0;
    PStrCat(out, prefix);
    PStrCat(out, kBTCheckVer);
    if (suffix != NULL) PStrCat(out, suffix);
}

static void PStrCatCh(Str255 dst, char c)
{
    if (dst[0] < 255) { dst[dst[0] + 1] = (unsigned char)c; dst[0]++; }
}

static void PStrCatNum(Str255 dst, long n)
{
    char  buf[16];
    short i = 0;
    if (n < 0) { PStrCatCh(dst, '-'); n = -n; }
    do { buf[i++] = (char)('0' + (n % 10)); n /= 10; } while (n && i < 15);
    while (i > 0) PStrCatCh(dst, buf[--i]);
}

static void PStrCatHexN(Str255 dst, unsigned long v, short digits)
{
    static const char hx[] = "0123456789ABCDEF";
    short i;
    for (i = digits - 1; i >= 0; i--)
        PStrCatCh(dst, hx[(v >> (i * 4)) & 0xF]);
}

static void PStrCatPStr(Str255 dst, const unsigned char *p)
{
    short i;
    for (i = 1; i <= p[0]; i++) PStrCatCh(dst, (char)p[i]);
}

/* Render a four-char code, e.g. 'v004', printably. */
static void PStrCatOSType(Str255 dst, unsigned long v)
{
    short i;
    for (i = 3; i >= 0; i--) {
        unsigned char c = (unsigned char)((v >> (i * 8)) & 0xFF);
        PStrCatCh(dst, (c >= 32 && c < 127) ? (char)c : '.');
    }
}

/* ---- log file, with the same fallback chain as the FireWire probes -------- */

typedef struct {
    short   refNum;
    short   vRefNum;
    Boolean open;
    Str255  where;
    OSErr   lastErr;
} LogFile;

static LogFile gLog;

static Boolean LogTryDir(short vRefNum, long dirID, const char *label)
{
    FSSpec spec;
    OSErr  err;
    Str255 volName;
    Str255 logName;

    gLog.lastErr = noErr;
    BTCheckName(logName, "BTCheck_v", ".log");
    err = FSMakeFSSpec(vRefNum, dirID, logName, &spec);
    if (err != noErr && err != fnfErr) { gLog.lastErr = err; return false; }
    if (spec.name[0] == 0)             { gLog.lastErr = err ? err : paramErr; return false; }

    (void)FSpDelete(&spec);
    err = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
    if (err != noErr && err != dupFNErr) { gLog.lastErr = err; return false; }
    err = FSpOpenDF(&spec, fsWrPerm, &gLog.refNum);
    if (err != noErr) { gLog.lastErr = err; return false; }

    gLog.vRefNum = spec.vRefNum;
    gLog.open    = true;
    SetEOF(gLog.refNum, 0);

    {
        HParamBlockRec pb;
        memset(&pb, 0, sizeof(pb));
        volName[0] = 0;
        pb.volumeParam.ioNamePtr  = volName;
        pb.volumeParam.ioVRefNum  = spec.vRefNum;
        pb.volumeParam.ioVolIndex = 0;
        if (PBHGetVInfoSync(&pb) != noErr) volName[0] = 0;
    }
    gLog.where[0] = 0;
    PStrCat(gLog.where, label);
    PStrCat(gLog.where, " on \"");
    if (volName[0]) PStrCatPStr(gLog.where, volName); else PStrCatCh(gLog.where, '?');
    PStrCatCh(gLog.where, '"');
    return true;
}

static void LogOpen(void)
{
    short vRefNum;
    long  dirID;

    gLog.open    = false;
    gLog.where[0] = 0;
    gLog.lastErr = noErr;

    {
        ProcessSerialNumber psn;
        ProcessInfoRec      info;
        FSSpec              appSpec;
        memset(&info, 0, sizeof(info));
        memset(&appSpec, 0, sizeof(appSpec));
        info.processInfoLength = sizeof(info);
        info.processName       = NULL;
        info.processAppSpec    = &appSpec;
        if (GetCurrentProcess(&psn) == noErr &&
            GetProcessInformation(&psn, &info) == noErr &&
            LogTryDir(appSpec.vRefNum, appSpec.parID, "next to the app"))
            return;
    }
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
                   &vRefNum, &dirID) == noErr &&
        LogTryDir(vRefNum, dirID, "System Folder"))
        return;
    if (LogTryDir(0, fsRtDirID, "startup volume root")) return;

    gLog.where[0] = 0;
    PStrCat(gLog.where, "NOT WRITTEN");
}

static void LogLine(Str255 s)
{
    long          count;
    unsigned char cr = '\r';
    if (!gLog.open) return;
    count = s[0];
    if (count > 0) FSWrite(gLog.refNum, &count, &s[1]);
    count = 1;
    FSWrite(gLog.refNum, &count, &cr);
}

static void LogClose(void)
{
    if (!gLog.open) return;
    FSClose(gLog.refNum);
    FlushVol(NULL, gLog.vRefNum);
    gLog.open = false;
}

static Str255 gSummary[kMaxSummary];
static short  gSummaryCount = 0;

static void Summary(Str255 s)
{
    LogLine(s);
    if (gSummaryCount < kMaxSummary)
        BlockMoveData(s, gSummary[gSummaryCount++], (Size)(s[0] + 1));
}

static void SummaryText(const char *s)
{
    Str255 line;
    line[0] = 0;
    PStrCat(line, s);
    Summary(line);
}

/* One labelled decimal counter, padded so the numbers line up. */
static void Row(const char *label, unsigned long v)
{
    Str255 line;
    short  i;
    line[0] = 0;
    PStrCat(line, "    ");
    PStrCat(line, label);
    for (i = (short)strlen(label); i < 26; i++) PStrCatCh(line, ' ');
    PStrCatNum(line, (long)v);
    Summary(line);
}

/* ★ NAME THE EVENT CODES. A ring of bare hex is a ring nobody reads carefully, and
 * this whole line of investigation exists because "last event hdr 0x040F" had to be
 * hand-decoded out of a byte dump before anyone noticed the command credit was zero.
 *
 * ⚠ Only codes this project has actually seen or is actively hunting. A speculative
 * table would be a list of guesses that reads as authority. */
static const char *EventName(unsigned long code)
{
    switch (code) {
    case 0x03: return "Connection Complete";
    case 0x04: return "Connection Request";
    case 0x05: return "Disconnection Complete";
    case 0x06: return "Authentication Complete";
    case 0x08: return "Encryption Change";
    case 0x0B: return "*** Read_Remote_Supported_Features Complete ***";
    case 0x0C: return "Read Remote Version Complete";
    case 0x0E: return "Command Complete";
    case 0x0F: return "Command Status";
    case 0x13: return "Number Of Completed Packets";
    case 0x15: return "Return Link Keys";
    case 0x16: return "PIN Code Request";
    case 0x17: return "Link Key Request";
    case 0x18: return "Link Key Notification";
    case 0x1B: return "Max Slots Change";
    /* Seen in the v6.7 ring, once per inbound connection, and it came out unnamed --
     * which is a small hole in exactly the tool built to stop hex going unread. */
    case 0x20: return "Page Scan Repetition Mode Change";
    case 0x23: return "Read Remote Extended Features Complete";
    case 0x2F: return "Synchronous Connection Complete";
    case 0x30: return "Read Clock Offset Complete";
    case 0x31: return "Connection Packet Type Changed";
    case 0x36: return "Simple Pairing Complete";
    case 0x38: return "Link Supervision Timeout Changed";
    case 0x3B: return "Enhanced Flush Complete";
    default:   return "";
    }
}

/* One gate, as it read on the FIRST sample and on the LAST. Two columns because a
 * single sample cannot distinguish "was never open" from "closed later", and this
 * project has already drawn the wrong conclusion from a one-shot reading. */
static void RowPair(const char *label, int first, int last)
{
    Str255 line;
    short  i;
    line[0] = 0;
    PStrCat(line, "    ");
    PStrCat(line, label);
    for (i = (short)strlen(label); i < 28; i++) PStrCatCh(line, ' ');
    PStrCat(line, first ? "yes" : "NO ");
    PStrCat(line, "    ");
    PStrCat(line, last  ? "yes" : "NO ");
    Summary(line);
}

static void RowHex(const char *label, unsigned long v, short digits)
{
    Str255 line;
    short  i;
    line[0] = 0;
    PStrCat(line, "    ");
    PStrCat(line, label);
    for (i = (short)strlen(label); i < 26; i++) PStrCatCh(line, ' ');
    PStrCat(line, "0x");
    PStrCatHexN(line, v, digits);
    Summary(line);
}

/* ---- the counter block --------------------------------------------------- *
 * Word indices MUST match the enum in src/bt_probe.c. */

#define kMagic0   0x42545031UL   /* 'BTP1' */

/* ⚠⚠ THE DRIVER VERSION THIS BTCHECK WAS BUILT ALONGSIDE.
 *
 * Run 27 was LOST to a version mismatch: BTCheck v22 ran against a driver still at
 * v1.14, so it truthfully reported "interface-init calls 0" -- of the OLD driver --
 * and the v2.0 interface-match fix was never actually tested. A whole boot for
 * nothing, and it took hand-checking the build tag to notice.
 *
 * This project's own test discipline says to validate a run before analysing it,
 * "expected banner, expected app version", because a wrong app version costs a
 * cycle. So stop relying on remembering: BTCheck now KNOWS which driver it expects
 * and says so loudly when the resident driver differs.
 *
 * ⚠ BUMP THIS EVERY TIME THE DRIVER VERSION CHANGES, in the same commit.
 *
 * ⚠⚠ AND THAT INSTRUCTION WAS NOT ENOUGH -- IT FAILED, TWICE, IN THE SAME LINE.
 *
 * The v93 run printed "WRONG DRIVER. This BTCheck expects 'v810' but found 'v820' ...
 * Do not analyse it" over a log from the CORRECT v8.2 driver. The warning built to
 * stop a version mistake became one: it slandered a good run, and its own comment
 * said 'v650' while the constant said 'v810', so neither half could be trusted.
 *
 * ⇒ A comment telling a human to remember something is not a mechanism. CMakeLists.txt
 * now parses the authoritative tag out of src/bt_probe.c and fails the build if this
 * constant disagrees, so it cannot drift again. Same lesson as the driver's own
 * three-way version guard and the panel's status string. */
#define kExpectedDriverTag 0x76483630UL   /* 'vH60' = driver v17.6 */

/* ⚠⚠ A FIXED HISTORICAL BOUNDARY, DELIBERATELY NOT WRITTEN AS THE SAME LITERAL AS
 * kExpectedDriverTag ABOVE, AND NOT A SPELLING TRICK.
 *
 * These two constants have the same value today and mean entirely different things.
 * kExpectedDriverTag is THE CURRENT DRIVER and scripts/bump-version.py rewrites it on
 * every bump -- it counts that exact literal and refuses to write if the count is not
 * what it expects, which is how it stays honest. This one is the driver at which words
 * 701..706 took their present meanings, it must NOT move when the driver does, and
 * writing it as the same hex literal would either be swept along by the bump or make the
 * script refuse. Spelled from its characters so it is one fixed thing with a name.
 *
 * ⚠ And the first draft of THIS COMMENT quoted that hex literal in prose, which made the
 * script refuse all over again -- the counter does not care whether an occurrence is code
 * or explanation. Do not name the literal here.
 *
 * ⚠ IT EARNS ITS KEEP because the version gate WARNS AND CONTINUES rather than bailing.
 * Driver 13.9 REUSED words 702/703 with new meanings, so a 13.8 block read by this build
 * would print teardown counts under failed-open labels -- shouting "WRONG DRIVER" and
 * then printing wrong numbers anyway is worse than either alone. */
#define kTagFirstWithFailedOpens \
    ((unsigned long)(('v' << 24) | ('D' << 16) | ('9' << 8) | '0'))   /* 13.9 */
/* Same kind of fixed boundary: the driver at which the battery probe words exist. */
#define kTagFirstWithBattery \
    ((unsigned long)(('v' << 24) | ('E' << 16) | ('1' << 8) | '0'))   /* 14.1 */
/* ⚠ Match only the 'v' BYTE, not 'v0'. v1.0 of the driver tags its block 'v100'
 * and the old 0xFFFF0000 mask demanded 'v0', so BTCheck stopped recognising the
 * block the moment the driver reached 1.0 and reported NO BLOCK FOUND on a
 * perfectly healthy driver. That cost a hardware run. The build tag is meant to
 * be READ, never matched beyond identifying it as a tag. */
#define kMagicVer 0x76000000UL   /* 'v...' -- any build tag */
#define kMagicVerMask 0xFF000000UL
#define kMagicEnd 0x454E4453UL   /* 'ENDS' at the terminator word (see kWEnd below;
                                  * 306 to v6.4, 303 to v5.x, 287 to v4.6, 271 to
                                  * v4.3, 191 to v4.1, 159 to v2.3, 127 to v1.11,
                                  * 63 to v1.1, 31 to v0.8).
                                  * ⚠ MOVES WITH THE BLOCK: an older BTCheck cannot
                                  * find a newer block -- it reports NO BLOCK FOUND.
                                  * The top-level CMakeLists now fails the build if
                                  * this file's kWEnd disagrees with the driver's. */

enum {
    kWMagic = 0, kWBuild,
    kWValidate, kWInit, kWFinal, kWNotify, kWNotifyCode,
    kWCfgSteps, kWStageMax, kWCfgStatus, kWImmErr, kWCfgDone,
    kWArmInt, kWIntComp, kWIntStatus, kWActCount,
    kWCmdComp, kWCmdStatus, kWCmdSent, kWLastOpcode,
    kWLastEvent, kWSayCalls, kWIface,
    kWEvt0, kWEvt1,
    kWBusPower, kWFindErr1, kWFindErr2, kWIfaceFound, kWFindAnyTried,
    kWPipes = 31,            /* nibbles: int<<8 | bulkOut<<4 | bulkIn      */
    kWHciBase = 32,          /* the HCI layer's parsed values start here   */
    kWAclArmed = 48, kWAclInComp, kWAclInStatus, kWAclInCount,
    kWAclTotal, kWAclHdr, kWAclSent, kWAclOutComp, kWAclOutStatus, kWImmErrCode,
    kWStkState = 64, kWStkPump, kWStkReenter, kWStkTx, kWStkRx, kWStkRefused,
    kWStkTxDone,
    /* M2b: L2CAP / inquiry / SDP. ⚠ statuses are stored 0x100 | value, so a zero
     * word means NOT RECEIVED and 0x100 means received with status 0. Print them
     * through RowStatus(), never as a bare number. */
    kWL2Init = 71, kWInqState, kWInqStartRc, kWInqResults, kWInqComplStatus,
    kWTgtAddrHi, kWTgtAddrLo, kWTgtCoD, kWTgtPick,
    kWHciConnStatus, kWHciConnHandle,
    kWSdpIssued, kWSdpQueryRc, kWSdpAttrBytes, kWSdpRecords,
    kWSdpComplete, kWSdpStatus,
    kWL2capEvts, kWLastUnkEvt,
    /* Run 16's ambiguity, closed. kWIntArmed vs kWIntComp says whether the reader
     * is armed and WAITING or whether we silently stopped listening. */
    kWIntArmed = 90, kWIntArmErr, kWUnsolEvents, kWScanMode,
    kWDiscReason = 94, kWDiscHandle, kWDiscCount,
    kWIoCapReqs, kWUserConfReqs, kWPinReqs, kWAuthComplete,
    kWSspAuto, kWLinkKeyReqs,
    /* M2d: the SDP retry, and the events SSP ACTUALLY emits. Words 103.. were already
     * free below the terminator, so the block did NOT grow -- an older BTCheck still
     * finds it, it just cannot show these rows. */
    kWSimplePairing = 103, kWEncryptChange, kWEncryptOn,
    kWSdpRetries, kWSdpRetryRc, kWSdpNotReady, kWSdpEventsAny, kWSdpLastEvt,
    /* M2e: successes that a later failure cannot erase, plus pool exhaustion. */
    kWConnOks = 111, kWSdpOks, kWAllocFails,
    /* M3.1 RAM link key store. */
    kWLkGets = 114, kWLkHits, kWLkPuts, kWLkDeletes,
    kWLkStored, kWLkEvicted, kWLkDirty,
    kWSecReqs = 121, kWLkAddrHi, kWLkAddrLo, kWLkType,
    /* M3.2 pipe stall recovery. ⚠ block grew to 160 words, terminator 159. */
    kWIntStallClears = 125, kWAclStallClears,
    kWIntArmErrFull = 128, kWStallClearRc, kWIntStatusFull, kWAclStatusFull,
    /* M3.3/M3.4: the trampoline and the file. */
    kWDeferReqs = 132, kWDeferRuns, kWDeferInstallErrs, kWDeferDropped,
    kWKfLoads, kWKfLoaded, kWKfFlushes, kWKfWritten, kWKfErr, kWKfRejected,
    /* M3.5: what the inquiry SAW, by class -- the table holds only 4. */
    kWInqPeriph = 142, kWInqPhones, kWInqComputers, kWInqOther,
    kWInqPeriphAddrHi, kWInqPeriphAddrLo, kWInqPeriphCoD,
    kWRespStored, kWRespDisplaced,
    /* Phase 1: the INTERFACE init path. */
    kWIfaceInit = 151, kWIfaceInitNum, kWIfaceTriplet, kWIfaceDevClass, kWIfaceVIDPID,
    /* M0.0 the CSR mode switch. ⚠ block grew 160 -> 192, terminator 159 -> 191. */
    kWSwTried = 156, kWSwImmErr, kWSwStatus, kWSwVerdict,
    /* M0.0c: the stall handling from Apple's CSRHIDTransitionDriver. */
    kWSwStalls = 166, kWSwClearRc, kWSwDeferred,
    /* M3.7: legacy PIN pairing -- the answer, not just the ask. */
    kWPinAnswered = 169, kWPinRespRc, kWPinAddrHi, kWPinAddrLo,
    /* M0.4: the one-instance-per-device interlock. */
    kWBoundCount = 173, kWBound0, kWBound1, kWBound2, kWBound3, kWDupDeclined,
    /* M0.5: recoveries abandoned because the pipe's device had gone. */
    kWDeadPipeStops,
    /* M0.6: empty reads -- NOT stalls. See the note at the rows. */
    kWAclEmptyReads, kWIntEmptyReads,
    kWInqAudio,
    /* M0.7: the ACL reader parks while no link exists. */
    kWAclIdleParked, kWAclLinkUps, kWAclLinkDowns,
    /* M0.0b: the full interface walk. Run 34 showed the card has THREE interfaces
     * and every run before v2.5 reported only the first. */
    kWIfScanCount = 160, kWIfScan0, kWIfScan1, kWIfScan2, kWIfScan3, kWIfScanErr,
    /* ⚠⚠ M6: THE SCAN COMMAND CHANNEL, and the reason this file had to change in the
     * same commit as the driver. docs/SCAN-DESIGN.md §5-§6. The block grew from 192
     * words to 272 to hold a 16-entry responder table, which MOVED kMagicEnd from
     * word 191 to word 271 -- and the block is found by matching that terminator.
     * An un-updated BTCheck does not print stale numbers, it prints NO BLOCK FOUND.
     * That has now cost six revisions; it is the single most repeated mistake here. */
    kWCmd = 192, kWCmdArg0, kWCmdArg1, kWCmdSeq, kWCmdAck, kWCmdResult,
    kWTimerRuns = 299, kWMbxServiced, kWMbxStale,
    kWTimerRcTask, kWTimerRcIrq, kWTimerBail,
    kWStoredKeyRc, kWStoredKeyCap,
    /* ⚠⚠ EXPLICIT, AND THEY WERE WRONG FOR A LONG TIME. In the driver these are 198
     * and 199 -- they sit directly after kWCmdResult in the mailbox group at 192. Here
     * they were left to CONTINUE from kWStoredKeyCap, which put them at 307 and 308.
     *
     * That was invisible while kWords was a stale 304: words past 303 were never
     * copied out of the heap, so both rows read 0 from the zeroed static array and the
     * M6b SCAN section printed a plausible "inquiry state 0" in every log it ever
     * produced. Deriving kWords (the right fix) unmasked it, and v6.6's run printed
     * "inquiry state 256 / responders published 409" -- which are exactly kWSendRc
     * (0x100|0) and kWGateFirst ((1<<8)|0x99), the two words that now live at 307/308.
     *
     * ⇒ Two independent defects hid each other, and the M6b rows in every earlier log
     * are an ARTIFACT rather than a measurement. Numbered explicitly now so continuing
     * from the wrong neighbour cannot happen again. */
    kWScanState = 198, kWScanCount = 199,
    /* 16 responders x 4 words: addrHi, addrLo, class of device, page-scan/clock. */
    kWScanBase = 200, kWScanEnd = 263,
    /* M7: addresses we hold link keys for. 8 x 2 words. */
    kWKeyCount = 264, kWKeyBase = 265, kWKeyEnd = 280, kWKeyHandedMask = 281,
    kWRadioOn = 282,
    kWNotifyRemoved = 283, kWNotifySleep = 284, kWNotifyOther = 285,
    kWInqStopRc = 286,
    /* M4 step 1: the incoming HID control channel. */
    kWHidListenRc = 288, kWHidIncoming, kWHidAccepted, kWHidDeclined,
    kWHidOpened, kWHidClosed, kWHidOpenStatus, kWHidLastPsm,
    kWHidCtrlCid, kWHidPeerHi, kWHidPeerLo,
    /* v6.5's send-gate instrument and the HCI-level peer address. ⚠ These push the
     * terminator six words further out, the ninth time it has moved -- see the
     * warning at the head of this enum, which this change is an instance of, not an
     * exception to.
     *
     * ⚠⚠ DO NOT write the word "kWEnd" followed by a number anywhere above its real
     * declaration. CMakeLists.txt matches the FIRST /kWEnd[ ]*=?[ ]*([0-9]+)/ in this
     * file, so a comment mentioning the old value is read as the declaration and the
     * build fails with a mismatch against a number that appears nowhere in the code. */
    kWSendRc = 307, kWGateFirst, kWGateLast, kWAclSlots,
    kWConnPeerHi, kWConnPeerLo,
    /* v6.6: the bonded addresses from HCI_Return_Link_Keys, who paged us at page
     * time, the auth-path counts, and the last 8 opcodes sent. */
    kWRlkEvents = 313, kWRlkKeys,
    kWRlkA0Hi, kWRlkA0Lo, kWRlkA1Hi, kWRlkA1Lo,
    kWRlkA2Hi, kWRlkA2Lo, kWRlkA3Hi, kWRlkA3Lo,
    kWConnReqs = 323, kWConnReqHi, kWConnReqLo, kWConnReqCoD, kWConnReqType,
    kWAuthComps = 328, kWLkNotifs = 330, kWLastCmdStatus = 331,
    kWCmdRing = 332, kWCmdRingIdx = 340,
    /* v6.7: the event ring -- what the CONTROLLER sent back. */
    kWEvtRing = 341, kWEvtRingIdx = 357, kWEvtTotal = 358,
    kWRrsfCount = 359, kWRrsfStatus = 360, kWRrsfHandle = 361,
    /* v6.8: the LEVEL_0 probe and the HID data path. */
    kWHidLevel = 362, kWHidDataPkts = 363, kWHidLastDataLen = 364,
    kWHidFirstLen = 365, kWHidFirstFull = 366,
    kWHidFirstData = 367,
    kWHidDecodeRc = 371, kWHidDecodeOk = 372, kWHidRollovers = 373,
    kWHidLastMods = 374, kWHidLastNKeys = 375, kWHidLastKey0 = 376,
    kWSynthRrsf = 377,
    /* v7.0: which branch of gap_request_security_level ran. */
    kWSecEvtCount = 378, kWSecEvtLevel = 379, kWSecEvtStatus = 380,
    kWSecEvtHandle = 381,
    kWSecLvlFirst = 382, kWSecLvlLast = 383,
    kWRemFeatFirst = 384, kWRemFeatLast = 385, kWLiveSamples = 386,
    kWHandlerRing = 387, kWHandlerIdx = 403, kWHandlerTotal = 404,
    kWSampHandle = 405, kWSampHciHandle = 406, kWSampHandleMax = 407,
    kWSampHciMax = 408, kWTimerAtEnd = 409,
    /* v7.2: initiator pairing and the write into the card's own store. */
    kWPairAsked = 410, kWPairRc = 411, kWPairAddrHi = 412, kWPairAddrLo = 413,
    kWBondDone = 414, kWBondStatus = 415,
    kWWroteKeyTried = 416, kWWroteKeyRc = 417, kWWroteKeyDone = 418,
    kWCreateConn = 419, kWCreateConnLen = 423,
    kWFirstBadCmd = 424, kWBadCmdCount = 425,
    kWBadCmdRing = 426,
    kWPairPath = 430, kWLinkWasUp = 431,
    kWArmedFired = 432, kWArmedRc = 433,
    kWSuppCmds0 = 434, kWSuppCmds1 = 435, kWSuppCmdsGot = 436,
    kWLocalVer = 437, kWLocalMfr = 438, kWCreateConnPatched = 439,
    kWDelStoredTried = 440, kWDelStoredRc = 441, kWDelStoredDone = 442,
    kWDelStoredHi = 443, kWDelStoredLo = 444,
    /* v8.2: the A1016 test -- the store read back after a delete, and every address
     * that PAGED this Mac. ⚠ kWPagedA0Hi has a stride of THREE (hi, lo, CoD). */
    kWStoredKeyCapFirst = 445, kWRlkPasses = 446,
    kWPagedCount = 447, kWPagedA0Hi = 448,
    /* v8.6: the report-protocol experiment. */
    kWHidIntrListenRc = 460, kWHidIntrCid = 461, kWHidIntrOpened = 462,
    kWHidSetProtoTried = 463, kWHidSetProtoRc = 464, kWHidHandshake = 465,
    kWHidCtrlDataPkts = 466, kWHidIntrDataPkts = 467,
    /* v8.7: the ACL capture and the link timestamps. */
    kWAclCapCount = 468, kWAclCap0 = 469, kWAclCapEnd = 484,
    kWLinkUpMs = 485, kWLinkDownMs = 486, kWLinkLifeMs = 487,
    kWRrsfSentMs = 488, kWRrsfDoneMs = 489,
    /* v8.8. ⚠ kWAutoAuthGate is the COMPILED gate, and the readout below needs it:
     * "authed 0" means one thing when the gate is on and something entirely
     * different when it is off, and the shipping build has it off. */
    kWAutoAuthTried = 490, kWAutoAuthRc = 491, kWAutoAuthHandle = 492,
    kWAutoAuthGate = 493,
    /* v8.9. ⚠⚠ FRESHNESS, and it is reported before anything else. Until v8.9 the
     * block was mirrored only from the packet handler, so on a quiet radio it froze at
     * bring-up and this app printed those values under headings saying "last". */
    kWPublishRuns = 494, kWPublishMs = 495,
    /* v9.0. ⚠ The controller's own Scan_Enable, read back. Bit 1 is page scan. */
    kWScanEnaRc = 496, kWScanEnaSends = 497, kWScanEnaReads = 498,
    kWScanEnaFirst = 499, kWScanEnaLast = 500,
    /* v9.1. ⚠ The outbound ACL capture, 24 bytes per slot rather than the inbound
     * 16, because the Connection Response RESULT is at byte 16. */
    kWAclTxCapCount = 501,
    kWAclTxCap0 = 502, kWAclTxCapEnd = 525,
    kWAclTxLen0 = 526, kWAclTxLenEnd = 529,
    /* v9.2: every Authentication Complete, with its handle and time. */
    kWAuthRingCount = 530,
    kWAuthRing0 = 531, kWAuthRingEnd = 534,
    kWAuthRingMs0 = 535, kWAuthRingMsEnd = 538,
    /* v9.3: the supervision stretch that unmasks the LMP verdict. */
    kWSupTimeoutTried = 539, kWSupTimeoutRc = 540,
    kWSupTimeoutHandle = 541, kWSupGate = 542,
    /* v9.4: link policy, the per-link command queue, and MODE CHANGE. */
    kWPolicyGate = 543, kWPolicyReadRc = 544, kWPolicyReadSends = 545,
    kWPolicyReads = 546, kWPolicyReadBack = 547,
    kWPolicyWriteRc = 548, kWPolicyWriteSends = 549,
    kWLinkCmdEnq = 550, kWLinkCmdSent = 551, kWLinkCmdRefused = 552,
    kWLinkCmdDropped = 553, kWLinkCmdDepth = 554,
    kWModeChanges = 555, kWModeLast = 556, kWModeLastMs = 557,
    /* v9.5: BTstack's own HID host. */
    kWBhGate = 558, kWBhInit = 559, kWBhIncoming = 560, kWBhCid = 561,
    kWBhAcceptRc = 562, kWBhOpened = 563, kWBhOpenedStatus = 564,
    kWBhOpenedMs = 565, kWBhClosed = 566, kWBhClosedMs = 567,
    kWBhSetProtoRsp = 568, kWBhDescAvail = 569, kWBhReports = 570,
    kWBhOtherEvts = 571, kWGapLevel = 572,
    /* v9.7: the transport. The 0x13 question, measured rather than eyeballed. */
    kWNumCompEvts = 573, kWNumCompTotal = 574, kWNumCompHandle = 575,
    kWNumCompMs = 576, kWBufSizeRc = 577, kWAclBufLen = 578, kWAclBufNum = 579,
    kWPipeInt = 580, kWPipeOut = 581, kWPipeInB = 582,
    /* v9.9: the raw USBPipeRefs -- the three words that settle the pipe question */
    kWRefInt = 583, kWRefBulkOut = 584, kWRefBulkIn = 585,
    kWPipeOrder = 586, kWPipeChainErr = 587,
    /* v10.0: the role, and what the USL actually moved */
    kWRoleChanges = 588, kWRoleStatus = 589, kWRoleNew = 590, kWRoleMs = 591,
    kWAclOutAct = 592, kWAclOutReq = 593,
    /* M5 step 1: the Consumer page */
    kWHidConsRpts = 594, kWHidConsSeen = 595, kWHidConsPadBits = 596,
    kWHidConsRingCount = 597, kWHidConsRingLost = 598,
    kWHidConsRing = 599, kWHidConsRingEnd = 610, kWHidKeyRpts = 611,
    /* M5 step 2c: injection */
    kWInjInitRuns = 612, kWInjKchrOk = 613, kWInjKchrErr = 614,
    kWInjCalls = 615, kWInjEventsSeen = 616, kWInjEvents = 617, kWInjPosted = 618,
    kWInjNoVk = 619, kWInjNoKchr = 620, kWInjNoChar = 621,
    kWInjLastUsage = 622, kWInjLastVk = 623, kWInjPostErr = 624,
    /* M6: the media keys */
    kWMedPresses = 625, kWMedRuns = 626, kWMedVolUp = 627, kWMedVolDn = 628,
    kWMedMute = 629, kWMedEject = 630, kWMedGetErr = 631, kWMedSetErr = 632,
    kWMedLastVol = 633, kWMedMuted = 634, kWMedDropped = 635,
    /* v11.2: the switcher marker, from the driver side */
    kWMarkRuns = 636, kWMarkCleared = 637, kWMarkErr = 638,
    /* v11.3: the hardware volume write */
    kWMedHwSetErr = 639, kWMedHwReadBack = 640, kWMedDevFound = 641,
    /* v11.4: Caps Lock */
    kWLedSends = 642, kWLedRc = 643, kWCapsOn = 644, kWLedRsp = 645,
    kWMedDevId = 646,
    /* v11.7: Apple's volume trap + the Caps Lock persistence instrument */
    kWMedTrapOk = 647, kWMedTrapCalls = 648, kWMedTrapRc = 649,
    kWInjMapClobber = 650, kWInjMapLastSys = 651, kWInjMapLastOur = 652,
    kWEjectPress = 653, kWEjectRelease = 654, kWEjectWhich = 655,
    kWRlkCaptured = 656, kWLkCaptured = 657, kWRestoreTried = 658,
    kWLkArchived = 659, kWDiscReqs = 660, kWDiscRc = 661,
    kWLinkUp = 662, kWPagedForgot = 663,
    kWBlockHi = 664, kWBlockLo = 665, kWBlockActive = 666, kWBlockDrops = 667,
    kWLiveCount = 668, kWLiveA0Hi = 669,
    kWNameReqs = 673, kWNameOks = 674, kWNameCount = 675, kWNameBase = 676,
    kWNameDeferred = 692, kWInqRetries = 693, kWInqRecovered = 694,
    /* v13.3: the outgoing HID connect -- see kHidOutgoingConnect in src/bt_pump.h */
    kWHidOutGate = 695, kWHidOutTried = 696, kWHidOutRc = 697, kWHidOutCid = 698,
    kWJustBonded = 699, kWKfNoFile = 700,
    /* v13.8: the half-open watchdog. kWBhIncomingMs minus nothing is useless -- it is
     * read AGAINST kWBhOpenedMs to give the accept->open gap, which is the number that
     * should set the stall timeout in place of the current safety factor. */
    /* ⚠ v13.9 REUSED 702/703. Safe only because kExpectedDriverTag pins the driver
     * exactly, so this build never reads a 13.8 block. 706 is new. */
    kWBhIncomingMs = 701, kWBhFailedOpens = 702, kWBhFailedStatus = 703,
    kWBhReconnTried = 704, kWBhReconnRc = 705, kWBhLive = 706,
    /* v14.1: the battery probe -- does the A1016 answer GET_REPORT at all? */
    kWBatSends = 707, kWBatResponses = 708,
    kWBatPctRc = 709, kWBatPctHs = 710, kWBatPctLen = 711, kWBatPctVal = 712,
    kWBatStRc  = 713, kWBatStHs  = 714, kWBatStLen  = 715, kWBatStVal  = 716,
    /* v14.2: the publish a CSM actually consumes */
    kWBatPublished = 717, kWBatFlushes = 718, kWBatFlushErr = 719,
    /* v14.6: the mouse path for the A1015, and the scan filter. kWUnclaimed* are the
     * ones that matter on the first mouse run -- see the readout for why. */
    kWMouseReports = 720, kWMouseDecodeOk = 721, kWMouseMoves = 722,
    kWMouseBtnChanges = 723, kWMouseDropped = 724, kWMouseWheelSeen = 725,
    kWCurCreated = 726, kWCurNewErr = 727,
    kWUnclaimedReports = 728, kWUnclaimedLen = 729, kWUnclaimedB0 = 730,
    kWShowAllDevices = 731, kWInqFiltered = 732,
    /* v14.8: the low-battery warning */
    kWBatWarnings = 733, kWAlertsPosted = 734, kWAlertsDropped = 735,
    kWAlertErr = 736,
    /* v15.0: two HID devices at once */
    kWHidDevOpens = 737, kWHidDevCloses = 738, kWHidDevFull = 739,
    kWSweepRuns = 740, kWSweepConnects = 741, kWSweepSkipped = 742, kWSweepRc = 743,
    kWDev0Cid = 744, kWDev0Hi = 745, kWDev0Lo = 746, kWDev0Role = 747,
    kWDev1Cid = 748, kWDev1Hi = 749, kWDev1Lo = 750, kWDev1Role = 751,
    kWMouseDxMin = 752, kWMouseDxMax = 753, kWMouseDyMin = 754, kWMouseDyMax = 755,
    kWMouseBtnMask = 756, kWMouseRidOther = 757,
    kWCurMoveErr = 758, kWCurBtnErr = 759,
    kWSniffKbd = 766, kWSniffMse = 767, kWSniffOther = 768,
    kWMouseAbsDx = 770, kWMouseAbsDy = 771,
    kWSweepEnabled = 772,
    kWCurAccelErr = 773, kWCurButtonsErr = 774, kWCurUpiErr = 775,
    kWCurAccelUsed = 776, kWCurDevPtr = 777,
    kWMouseRatePeak = 769,
    kWBatTargets = 760, kWBatGiveUps = 761,
    kWDev0Pct = 762, kWDev0BatHs = 763, kWDev1Pct = 764, kWDev1BatHs = 765,
    /* ⚠ v17.2: MOVED IN THE SAME COMMIT AS src/bt_probe.c, which is mandatory --
     * BTCheck locates the block by matching 'ENDS' at kWEnd, so a reader left at 778
     * finds NO BLOCK AT ALL and reports it as "the driver never loaded". */
    kWStallFirstMs = 778, kWStallLastMs = 779, kWFinalizeMs = 780,
    kWIntDepth = 781,
    kWDelivMaxCmdUs = 782, kWDelivMaxUnsolUs = 783, kWDelivLastUs = 784,
    kWCompRing0 = 785, kWCompRingIdx = 793,
    kWEnd = 794
};
#define kPagedSlots 4
/* ⚠ MUST match src/bt_pump.h. 4 slots x 16 bytes, published 4 bytes per word. */
#define kAclCapSlots 4
#define kAclCapBytes 16
/* ⚠ WIDER for the outbound capture -- MUST match src/bt_pump.h. The Connection
 * Response RESULT is at byte 16, one past a 16-byte capture, so the outbound slots
 * are 24 bytes. Shipping 16 would have captured everything except the answer. */
#define kAclTxCapBytes 24
/* ⚠ MUST match src/bt_pump.h. */
#define kAuthRingSlots 4
#define kEvtRingLen     16
#define kHidCapBytes    16
#define kHandlerRingLen 16
#define kWRlkSlots 4

/* ---- USBBluetoothSwitch's separate 'BTSW' block --------------------------------
 * ⚠ MUST MIRROR the enum at the top of src/bt_switch.c. A different extension, a
 * different magic, and a layout that has to be kept in step by hand -- so keep this
 * list SHORT. Everything the switcher records is one word, and that is deliberate. */
enum {
    kSwMagic = 0, kSwBuild,
    kSwValidate, kSwInit, kSwFinal, kSwNotifyC, kSwNotifyCode,
    kSwSeenProxy, kSwSeenOther, kSwOtherVid,
    kSwTried, kSwImmErr, kSwStatus, kSwStalls, kSwClearRc,
    kSwDeferRuns, kSwDeferErr, kSwVerdict,
    /* ⚠ v1.1 of the switcher added kSwStoodDown BEFORE kSwEnd, so the terminator
     * moved. This enum MUST match src/bt_switch.c exactly -- the block is found by
     * matching 'ENDS' at kSwEnd, so a stale index here reports NO 'BTSW' BLOCK on a
     * perfectly healthy switcher. Same trap as the BTP1 block's kWEnd. */
    kSwStoodDown,
    /* ⚠ Switcher 1.2 added three more BEFORE kSwEnd, so the terminator moved again.
     * MUST match src/bt_switch.c exactly -- a stale index reports NO 'BTSW' BLOCK over
     * a healthy switcher. */
    kSwIdleDecline, kSwFlagSeen, kSwFlagErr,
    /* ⚠ AND SWITCHER 1.3 ADDED THREE MORE. Third time this terminator has moved, and
     * the two warnings above did their job -- they are why this was checked rather than
     * assumed. A stale index here reports NO 'BTSW' BLOCK over a healthy switcher. */
    kSwMarkStale, kSwMarkWrote, kSwMarkErr,
    /* ⚠⚠ AND SWITCHER 1.4 ADDED ONE MORE -- fourth move of this terminator. This copy
     * is the one check-block-words.py did NOT cover until now: it compared bt_switch.c
     * against the PANEL only, so this file could drift silently and report NO 'BTSW'
     * BLOCK while the panel read it fine. The script now checks this mirror too. */
    kSwRefRetry,
    /* ⚠ Switcher 1.5 added kSwMarkCleared BEFORE kSwEnd -- FIFTH move of this terminator.
     * check-block-words.py covers this mirror as of 99.67, so a drift here now fails the
     * build instead of printing NO 'BTSW' BLOCK over a healthy switcher. */
    kSwMarkCleared,
    kSwEnd, kSwCount
};
enum {
    kVerdictNone = 0, kVerdictSwitched = 1, kVerdictIgnored = 2,
    kVerdictStalled = 3, kVerdictOther = 4
};
static unsigned long gSw[kSwCount];
static unsigned long gSwAt;
/* ⭐ v99.40: the 'sdev' Component token THIS APP got. The hardware-volume probe runs
 * near the top of the log, long before the driver's block is decoded, so the value is
 * stashed here and compared where the driver's own token is printed. */
static unsigned long gAppSndDevId;
static short         gSwFound;

/* ⚠⚠ DERIVED, NEVER TYPED. This is how many words BestBlock copies out of the heap
 * into gC, so it is the real bound on every c[kW...] read in this file.
 *
 * It was a hand-maintained 304 while the terminator had already moved to 313, and
 * CMakeLists only ever guarded kWEnd -- so the mismatch was invisible. Every read at
 * word 304 or beyond (the whole v6.5 gate instrument, words 307-312) would have run
 * off the end of gC's row and printed whatever the next row held, as confident
 * decimal numbers. It was masked only because that run found no block at all.
 *
 * ⇒ A second number that must equal a first is a defect waiting for someone to update
 * one of them. Deriving it removes the possibility rather than guarding against it. */
#define kWords (kWEnd + 1)

/* Slot numbers inside the HCI section. MUST match the enum in src/hci.h. */
enum {
    kRecResetStatus = 0, kRecHciVersion, kRecManufacturer,
    kRecBdAddrHi, kRecBdAddrLo, kRecAclLen, kRecAclCount,
    kRecSetMaskStatus, kRecScanEnStatus, kRecCmdStatusOpcode,
    kRecInqResponses, kRecInqDevHi, kRecInqDevLo, kRecInqComplete,
    kRecLastOtherEvent
};
#define HCI(c, slot) ((c)[kWHciBase + (slot)])

/* Print a status word stored as 0x100 | status by the driver.
 *
 * ⚠ THE ENCODING IS THE POINT, so decode it rather than printing the raw word. A
 * bare 0 in a status counter is ambiguous between "succeeded" and "the event
 * never arrived", and that exact ambiguity made M1's Inquiry Complete row useless
 * for several runs. Word 0 means NOT RECEIVED; 0x100 means received, status 0. */
static void RowStatus(const char *label, unsigned long w)
{
    Str255 line;
    short  i;
    line[0] = 0;
    PStrCat(line, "    ");
    PStrCat(line, label);
    for (i = (short)strlen(label); i < 26; i++) PStrCatCh(line, ' ');
    if (w == 0) {
        PStrCat(line, "NOT RECEIVED");
    } else {
        PStrCatNum(line, (long)(w & 0xFF));
        if ((w & 0xFF) == 0) PStrCat(line, "  = success");
        else                 PStrCat(line, "  <== NONZERO, this is the failure");
    }
    Summary(line);
}

/* BD_ADDR arrives as two 3-byte halves, high then low. */
static void RowBDAddr(const char *label, unsigned long hi, unsigned long lo)
{
    Str255 line;
    short  i;
    line[0] = 0;
    PStrCat(line, "    ");
    PStrCat(line, label);
    for (i = (short)strlen(label); i < 26; i++) PStrCatCh(line, ' ');
    PStrCatHexN(line, (hi >> 16) & 0xFF, 2); PStrCatCh(line, ':');
    PStrCatHexN(line, (hi >>  8) & 0xFF, 2); PStrCatCh(line, ':');
    PStrCatHexN(line,  hi        & 0xFF, 2); PStrCatCh(line, ':');
    PStrCatHexN(line, (lo >> 16) & 0xFF, 2); PStrCatCh(line, ':');
    PStrCatHexN(line, (lo >>  8) & 0xFF, 2); PStrCatCh(line, ':');
    PStrCatHexN(line,  lo        & 0xFF, 2);
    Summary(line);
}

static short         gBlocks = 0;
static unsigned long gBlockAt[kMaxHits];
static unsigned long gC[kMaxHits][kWords];

/* ⚠ These MUST track the enum in src/bt_probe.c. Driver v1.0 inserted two bulk
 * stages and this table was not updated, so a run that died in the bulk-OUT
 * search was labelled "6 = pipe found, M0.1 complete" -- a confidently wrong
 * label on a failing run. Nine stages now. */
static const char *StageName(unsigned long s)
{
    switch (s) {
    case 0:  return "0 = about to find the interface";
    case 1:  return "1 = interface found, setting configuration";
    case 2:  return "2 = configured, making an interface ref";
    case 3:  return "3 = have the ref, doing SET_INTERFACE";
    case 4:  return "4 = interface set, configuring it";
    case 5:  return "5 = configured, searching for the interrupt-IN pipe";
    case 6:  return "6 = interrupt-IN found, searching for bulk-OUT";
    case 7:  return "7 = bulk-OUT found, searching for bulk-IN";
    default: return "8 = all three pipes found, M0.1 complete";
    }
}

static void ScanSystemHeap(void)
{
    THz            zone = SystemZone();
    unsigned long *p, *lim;
    unsigned long  lo, hi;
    Str255         line;
    short          j;

    SummaryText("=========================================================");
    SummaryText("  SYSTEM HEAP SCAN for the driver's 'BTP1' counter block");
    SummaryText("=========================================================");

    if (zone == NULL) { SummaryText("SystemZone() returned NULL. VOID."); return; }
    lo = (unsigned long)zone;
    hi = (unsigned long)zone->bkLim;
    if (hi <= lo || (hi - lo) > 0x20000000UL) {
        line[0] = 0;
        PStrCat(line, "System zone bounds implausible: 0x");
        PStrCatHexN(line, lo, 8); PStrCat(line, " .. 0x"); PStrCatHexN(line, hi, 8);
        Summary(line);
        SummaryText("Refusing to scan. VOID.");
        return;
    }

    line[0] = 0;
    PStrCat(line, "System heap 0x"); PStrCatHexN(line, lo, 8);
    PStrCat(line, " .. 0x");         PStrCatHexN(line, hi, 8);
    PStrCat(line, "  ");             PStrCatNum(line, (long)((hi - lo) >> 10));
    PStrCat(line, " KB");
    Summary(line);

    /* ⚠⚠⚠ DERIVE THIS FROM kWEnd. A HARDCODED LIMIT IS AN OUT-OF-BOUNDS READ.
     *
     * The loop below tests p[kWEnd], so it touches p + kWEnd*4 bytes -- 1148 with a
     * 288-word block. A hardcoded 256 therefore let the last candidates read up to 896
     * bytes PAST bkLim, on every launch, and it has been wrong since the block passed
     * 64 words. It finally landed somewhere that faulted: BTCheck crashed to MacsBug
     * on a machine whose driver and control panel were both working perfectly.
     *
     * ⚠ THIRD INSTANCE OF THIS EXACT BUG. The driver's EnsureBlock got it right
     * (kWCount * 4 + 16); the control panel had a stale 1024 and was fixed two days
     * ago; this is the same mistake in the third reader. Any scan that reads p[kWEnd]
     * must derive its limit from kWEnd -- never a literal. */
    lim = (unsigned long *)(hi - ((unsigned long)(kWEnd + 1) * 4UL + 16UL));
    for (p = (unsigned long *)((lo + 3) & ~3UL); p < lim; p++) {
        if (p[kWMagic] == kMagic0 &&
            (p[kWBuild] & kMagicVerMask) == kMagicVer &&
            p[kWEnd] == kMagicEnd) {
            if (gBlocks < kMaxHits) {
                gBlockAt[gBlocks] = (unsigned long)p;
                for (j = 0; j < kWords; j++) gC[gBlocks][j] = p[j];
                gBlocks++;
            }
        }
    }

    /* ★★ AND THE SWITCHER'S OWN BLOCK, which is a DIFFERENT MAGIC in the same heap.
     *
     * USBBluetoothSwitch is a separate extension with its own 'BTSW' block, kept
     * separate on purpose: sharing 'BTP1' would mean two independently-built binaries
     * agreeing on a layout, and that agreement is the single most repeated mistake on
     * this project -- ten moves of one terminator and a hand-typed word count that
     * drifted into an out-of-bounds read. Two magics cannot drift into each other.
     *
     * ⚠ Its own limit from kSwEnd, for the same reason as above and not by copying the
     * number: this is the FOURTH reader in this project and the previous three each got
     * a hardcoded limit wrong. */
    {
        unsigned long *q, *qlim =
            (unsigned long *)(hi - ((unsigned long)(kSwEnd + 1) * 4UL + 16UL));
        for (q = (unsigned long *)((lo + 3) & ~3UL); q < qlim; q++) {
            if (q[kSwMagic] == 0x42545357UL &&      /* 'BTSW' */
                (q[kSwBuild] & 0xFFFF0000UL) == 0x73770000UL &&   /* 'sw..' */
                q[kSwEnd]   == kMagicEnd) {
                gSwAt = (unsigned long)q;
                for (j = 0; j < kSwCount; j++) gSw[j] = q[j];
                gSwFound = 1;
                break;                              /* one switcher, one block */
            }
        }
    }
}

static void ReportBlock(short b)
{
    unsigned long missingKeys;
    unsigned long *c = gC[b];
    Str255         line;

    SummaryText("");
    line[0] = 0;
    PStrCat(line, "BTP1 block @0x"); PStrCatHexN(line, gBlockAt[b], 8);
    PStrCat(line, "   build '");     PStrCatOSType(line, c[kWBuild]);
    PStrCatCh(line, '\'');
    Summary(line);

    /* ⚠⚠ VERSION GATE. See kExpectedDriverTag: run 27 was wasted because a new
     * BTCheck measured an old driver and said so only in a four-character tag
     * nobody was checking. Now it shouts. */
    if (c[kWBuild] != kExpectedDriverTag) {
        Str255 w;
        SummaryText("  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        w[0] = 0;
        PStrCat(w, "  !! WRONG DRIVER. This BTCheck expects '");
        PStrCatOSType(w, kExpectedDriverTag);
        PStrCat(w, "' but found '");
        PStrCatOSType(w, c[kWBuild]);
        PStrCatCh(w, '\'');
        Summary(w);
        /* ⚠⚠⚠ WHICH ONE IS STALE? THE OLD TEXT ASSUMED IT WAS ALWAYS THE DRIVER, and
         * on 2026-09-16 that gave the user exactly the wrong instruction. They had just
         * installed driver 13.2 and rebooted; it loaded correctly; BTCheck was the old
         * binary. The banner told them the new extension had NOT loaded and to
         * re-install it -- a second reboot, to fix a driver that was already right.
         *
         * A gate that cannot say WHICH SIDE is behind gives confident advice half the
         * time and confidently wrong advice the other half. The tags are ordered by
         * construction (see majchar() in bump-version.py: '9' < 'A' byte-for-byte), so
         * the comparison that decides this is available for free and always was. */
        if (c[kWBuild] > kExpectedDriverTag) {
            SummaryText("  !! THE DRIVER IS NEWER THAN THIS BTCHECK. The extension");
            SummaryText("  !! loaded fine -- this tool is the stale one.");
            SummaryText("  !! The rows below ARE the running driver and are safe to");
            SummaryText("  !! read: the block is located by matching 'ENDS' at kWEnd,");
            SummaryText("  !! so a block found at all is a block whose layout agrees.");
            SummaryText("  !! FIX: install the matching BTCheck. Do NOT re-install the");
            SummaryText("  !! extension -- it is not the problem.");
        } else {
            SummaryText("  !! The new extension is NOT the one that loaded, so every");
            SummaryText("  !! row below describes the OLD driver. Do not analyse it.");
            SummaryText("  !! FIX: expand the .bin with StuffIt Expander (a raw");
            SummaryText("  !! MacBinary is not type 'ndrv' and the Expert SKIPS it),");
            SummaryText("  !! put the expanded file (named \"USB Bluetooth Support\"");
            SummaryText("     or \"USBBluetoothSupport\" -- the Expert scans only names");
            SummaryText("     beginning USB) into");
            SummaryText("  !! System Folder:Extensions -- it REPLACES the old one, so");
            SummaryText("  !! confirm the Replace prompt, then reboot.");
        }
        SummaryText("  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    }

    /* ★★★★★ IS THIS BLOCK FRESH? BEFORE EVERYTHING, because a stale block does not
     * look stale -- it looks like a hardware answer, and on the v8.8 run it looked
     * like a dead pump timer and a wedged send path.
     *
     * Until v8.9 the block was mirrored ONLY from the packet handler, so a run where
     * nothing connected left every counter frozen at bring-up while the headings below
     * still said "last". The v8.8 log reported `timer firings 3` after 136 seconds of
     * uptime; the timer had in fact fired about 1360 times. The tell was that a run
     * from months earlier, on a different driver, reported the IDENTICAL 3 -- two runs
     * agreeing to the digit are reading a constant, not measuring a stall.
     *
     * The driver now refreshes from its 100 ms timer, so publishes should be roughly
     * ten per second of uptime. A handful means the refresh is not running and NOTHING
     * BELOW THIS LINE CAN BE TRUSTED. */
    SummaryText("  ★★★★★ IS THIS BLOCK FRESH? Read this before any row below");
    Row("    block refreshes",     c[kWPublishRuns]);
    Row("    at (ms)",             c[kWPublishMs]);
    if (c[kWPublishRuns] == 0) {
        /* ⚠ Two causes, and only two: the version gate above already shouts if this
         * is a pre-v8.9 driver, so that one is covered louder elsewhere. What is left
         * is the timer not firing, or hci_init never returning -- BT_PublishFromTimer
         * returns early on !gHciReady. The M2 gate section separates those. */
        SummaryText("    ⚠⚠ NEVER REFRESHED. Either the pump timer is not firing or the");
        SummaryText("    stack never reached hci_init -- check the M2 gate below, which");
        SummaryText("    tells those apart. Every counter here is frozen at whenever");
        SummaryText("    the last USB completion happened, which on a quiet radio is");
        SummaryText("    bring-up. DO NOT read a verdict out of it.");
    } else if (c[kWPublishRuns] < 20) {
        SummaryText("    ⚠ ONLY A HANDFUL. The timer fires 10x a second, so this block");
        SummaryText("    is at most a couple of seconds old -- fine if BTCheck ran");
        SummaryText("    immediately, stale if the machine has been up a while.");
    } else {
        SummaryText("    FRESH. The 100 ms timer is refreshing this block, so the rows");
        SummaryText("    below are current as of the ms stamp above.");
    }

    /* ★ WHICH DEVICE IS THIS BLOCK? Printed first, because with two match rules the
     * driver can bind more than one device and every block used to look alike. */
    if (c[kWIfaceVIDPID] != 0) {
        Str255 l; unsigned long v = c[kWIfaceVIDPID];
        l[0] = 0;
        PStrCat(l, "  BOUND DEVICE  VID:PID ");
        PStrCatHexN(l, (v >> 16) & 0xFFFF, 4); PStrCatCh(l, ':');
        PStrCatHexN(l,  v        & 0xFFFF, 4);
        PStrCat(l, "  devclass 0x");
        PStrCatHexN(l, c[kWIfaceDevClass], 2);
        Summary(l);
        if ((v >> 16) == 0x05AC && (v & 0xFFFF) == 0x1000)
            SummaryText("      *** THIS IS THE A1044 INTERNAL MODULE ***");
        else if ((v >> 16) == 0x0A12)
            SummaryText("      (this is the CSR dongle)");
        else if (v == 0)
            SummaryText("      (VID:PID 0000:0000 -- the bus-powered hub)");
    } else {
        SummaryText("  BOUND DEVICE  not recorded (driver older than v2.3)");
    }

    /* ★★ M0.0 THE MODE SWITCH. Printed high, because on the rule-1 instance it is
     * the ONLY thing that happened -- that instance switches the card and stops.
     *
     * ⚠⚠ THE VERDICT IS INVERTED. BlueZ's usb_switch_csr treats a TIMEOUT as SUCCESS
     * and a clean completion as failure: the card changes personality mid-request and
     * never finishes the handshake. Reading 'last status' the ordinary way would call
     * the working case broken, so the verdict is spelled out in words here rather
     * than left to be remembered. See docs/M0-MODE-SWITCH.md. */
    if (c[kWSwTried] == 0) {
        /* ★ v2.6 sends no switch at all, by design. Say so, rather than leaving a silent
         * gap that reads like the section failed to run -- an absent line has been
         * misread as a result on this project before. */
        SummaryText("");
        /* ⚠ THIS TEXT USED TO ASSERT A v2.6-SPECIFIC DESIGN DECISION, and run 42
         * printed it under v3.0 where that decision was NOT in effect -- on the
         * second of two driver instances, which simply never sent a switch. A
         * readout must describe what the counters SAY, not what some past build
         * intended. State the fact and let the reader draw the conclusion. */
        SummaryText("  M0.0  MODE SWITCH: this instance sent none");
        SummaryText("    Zero attempts recorded in THIS block. With two driver");
        SummaryText("    instances binding the same card, one may switch and the");
        SummaryText("    other not -- check the other block before concluding that");
        SummaryText("    no switch happened at all.");
    } else {
        SummaryText("");
        SummaryText("  M0.0  HID-PROXY -> HCI MODE SWITCH (CSR method)");
        Row("switch attempts",      c[kWSwTried]);
        RowHex("immediate err",     c[kWSwImmErr], 8);
        RowHex("completion status", c[kWSwStatus], 8);
        /* ★★ THE STALL ROWS. Apple's driver expects kIOUSBPipeStalled from this
         * request, clears the stall on pipe zero and retries. Up to v2.8 we filed
         * -6979 under "other error" and left the control pipe stalled, which is a
         * device that can no longer enumerate -- the card vanishing in runs 35/40. */
        Row("pipe stalls seen",     c[kWSwStalls]);
        RowHex("stall clear rc",    c[kWSwClearRc], 8);
        Row("retries issued",       c[kWSwDeferred]);
        SummaryText("    (Apple's driver sends this request SEVEN times, so");
        SummaryText("     'switch attempts' reaching 7 is the intended behaviour)");
        if (c[kWSwStalls] == 0 && c[kWSwTried] > 0) {
            SummaryText("    (no stall seen: this card did not answer the way");
            SummaryText("    Apple's driver expects -- read the verdict below)");
        }
        switch (c[kWSwVerdict]) {
        case 4:
            SummaryText("    VERDICT: STALLED, cleared, retry pending or done.");
            SummaryText("    This is the EXPECTED first answer -- Apple's driver tests");
            SummaryText("    for exactly this and clears pipe zero before retrying.");
            SummaryText("    Check 'stall clear rc' is 0 and 'switch attempts' > 1.");
            break;
        case 1:
            SummaryText("    VERDICT: *** SWITCHED ***  (timed out = SUCCESS here)");
            SummaryText("    The card should now re-enumerate with 0xE0 interfaces.");
            SummaryText("    Look for a SECOND block below whose BOUND DEVICE is not");
            SummaryText("    05AC:1000 -- that is rule 2 binding the switched card.");
            break;
        case 2:
            SummaryText("    VERDICT: IGNORED. The request completed cleanly, which");
            SummaryText("    for this method means the card did NOT switch (BlueZ");
            SummaryText("    calls this EALREADY): already HCI, or not switchable.");
            break;
        case 3:
            SummaryText("    VERDICT: ERROR -- the request failed for another reason.");
            SummaryText("    Read 'immediate err' first: nonzero there means it was");
            SummaryText("    never issued, which is NOT the same as a timeout.");
            break;
        /* ★★★★★ v8.5's REVERSE TRIP, HCI -> HID-PROXY. Codes 5..8, and they exist
         * because the four kWSw* words are SHARED between the two directions -- a
         * reader seeing "SWITCHED" would otherwise have no way to tell which way the
         * card went.
         *
         * ⚠⚠ AND THIS RENDERER'S ABSENCE ALREADY COST A MISREADING. The run that first
         * proved the reverse switch works was read by a BTCheck built before these
         * cases existed, so code 5 fell into the default below and the log announced
         * "started but no completion recorded yet" over a completion status of
         * 0xFFFFE501 -- the documented SUCCESS signature. A diagnostic that reports
         * success as nothing-happened is the failure mode this whole file is shaped to
         * avoid, and it arrived through a stale ARTIFACT rather than stale source: the
         * CMake guard keeps kExpectedDriverTag honest against the driver, but nothing
         * checks that the BTCheck on the share is the one that was built.
         * ⇒ Re-stage BTCheck whenever the driver's block tag moves. */
        case 5:
            SummaryText("    ⭐⭐⭐ VERDICT: *** HANDED BACK TO MAC OS ***");
            SummaryText("    The reverse request (wValue 1) timed out, which for this");
            SummaryText("    method means the card DID change personality. Confirm in");
            SummaryText("    the USB BUS DUMP below: 05AC:1000 present means it is in");
            SummaryText("    HID-proxy mode and OS 9's own USBHIDKeyboardModule can");
            SummaryText("    drive a paired keyboard with nothing of ours involved.");
            SummaryText("    ⇒ No reboot and no moving extensions by hand.");
            break;
        case 6:
            SummaryText("    VERDICT: the card REFUSED to go back. The request");
            SummaryText("    completed cleanly, which for this method means no switch");
            SummaryText("    happened -- so the way back is not available live and a");
            SummaryText("    restart is still needed to return the card to proxy.");
            break;
        case 7:
            SummaryText("    VERDICT: the reverse request ERRORED. Read 'immediate");
            SummaryText("    err' first -- nonzero there means it was never issued,");
            SummaryText("    which is NOT the same as the timeout that means success.");
            break;
        case 8:
            SummaryText("    VERDICT: reverse switch REFUSED -- a request was already");
            SummaryText("    in flight. Nothing was sent and nothing was disturbed;");
            SummaryText("    the shared parameter block is not stomped. Try again.");
            break;
        default:
            SummaryText("    VERDICT: started but no completion recorded yet.");
            /* ⚠ AND SAY SO WHEN THE STATUS CONTRADICTS THAT. A recorded completion
             * status with an unrecognised verdict means THIS BTCheck is older than the
             * driver that wrote the word -- exactly what happened on the first reverse
             * switch run. Print the number rather than implying nothing happened. */
            if (c[kWSwStatus] != 0) {
                Row("    ⚠ but a completion status IS recorded; verdict code",
                    c[kWSwVerdict]);
                SummaryText("    ⇒ THIS BTCheck IS OLDER THAN THE DRIVER. It does not");
                SummaryText("    know that code. Do not read this as a failure -- read");
                SummaryText("    the completion status above and re-stage BTCheck.");
            }
            break;
        }
    }

    /* ★ M0.4. Run 47 produced FOUR blocks and four BTstacks on one dongle, and there
     * was no way to tell which instance any counter belonged to. v3.6 makes the
     * instances share one block and stand down on a device already claimed. ⚠ ONE
     * block in this log is now the expected, healthy reading -- several means the
     * interlock is not working, most likely a build-tag mismatch in EnsureBlock. */
    SummaryText("  M0.4  ONE INSTANCE PER DEVICE");
    Row("devices bound now",    c[kWBoundCount]);
    Row("duplicate binds refused", c[kWDupDeclined]);
    /* ★ M0.5. Run 48 recorded 212 + 691 stall clears with last clear rc -6997
     * kUSBUnknownPipeErr: we were recovering a pipe whose device had gone, ~180
     * futile attempts per session. A nonzero count here is the guard WORKING; the
     * stall-clear counts below should now be small rather than in the hundreds. */
    Row("dead-pipe stops",      c[kWDeadPipeStops]);
    /* ★ M0.6. Run 50: 1708 ACL completions, 1703 "stall clears", last status -6911
     * with ZERO bytes. An idle bulk-IN pipe completes that way, and -6911 is in the
     * stall family, so every idle poll was answered with a pipe-stall clear -- and the
     * dongle dropped off the bus 14 times. These rows separate "nothing to read" from
     * "something is wrong", which the completion status alone cannot do. */
    Row("empty ACL reads",      c[kWAclEmptyReads]);
    Row("empty event reads",    c[kWIntEmptyReads]);
    /* ★ M0.7. Run 51 proved the clears are REQUIRED, not spurious -- every re-arm on
     * an idle pipe is refused until the stall is cleared. So the only way to stop
     * paying is to stop polling a pipe that cannot have data. These rows say whether
     * that is happening: parks should be nonzero and the stall-clear counts above
     * should have collapsed. Clears still high WITH parks nonzero would mean the
     * clears are coming from a live link instead, which is a different question. */
    Row("ACL re-arms parked",   c[kWAclIdleParked]);
    Row("ACL links up",         c[kWAclLinkUps]);
    Row("ACL links down",       c[kWAclLinkDowns]);
    if (c[kWIntStallClears] + c[kWAclStallClears] > 50) {
        SummaryText("    !! stall clears in the dozens or more. As of v3.9 clears");
        SummaryText("    !! happen ONLY on a refused re-arm, so a high count here is");
        SummaryText("    !! real trouble rather than idle polling -- which is what it");
        SummaryText("    !! was in runs 48 to 50.");
    }
    if (gBlocks > 1) {
        SummaryText("    !! MORE THAN ONE BLOCK FOUND. The instances are not sharing");
        SummaryText("    !! one block, so the interlock cannot see across them.");
    }

    SummaryText("  PROOF OF LIFE");
    Row("Initialize calls",     c[kWInit]);
    Row("ValidateHW calls",     c[kWValidate]);
    Row("Finalize calls",       c[kWFinal]);
    Row("Notify calls",         c[kWNotify]);
    Row("bus power offered mA", c[kWBusPower]);
    RowHex("last notification", c[kWNotifyCode], 8);
    /* ⚠⚠ THE ENUM THAT BIT US. This proc receives a USBDriverNotification, NOT the
     * expert-services kNotify* list -- both share the prefix and live in USB.h. The
     * driver compared against kNotifyRemoveDevice (0x01, services enum), which in THIS
     * enum is kNotifySystemSleepRequest, so rude-removal handling had never once fired
     * on a removal. Decoded here so a raw hex code can never be misread again. */
    if (c[kWNotifyCode] == 0x0BUL)
        SummaryText("      0x0B = kNotifyDriverBeingRemoved: the EXPERT unloaded us");
    else if (c[kWNotifyCode] == 0x01UL)
        SummaryText("      0x01 = kNotifySystemSleepRequest (NOT a device removal)");
    else if (c[kWNotifyCode] == 0x08UL)
        SummaryText("      0x08 = kNotifyExpertTerminating");
    Row("  driver-being-removed",  c[kWNotifyRemoved]);
    Row("  sleep request/demand",  c[kWNotifySleep]);
    Row("  other notifications",   c[kWNotifyOther]);
    if (c[kWNotifyRemoved] > 2) {
        SummaryText("      !! the Expert is UNLOADING AND RELOADING this driver. Compare");
        SummaryText("      Initialize calls: each removal costs a full BTstack rebuild,");
        SummaryText("      loses link-key and inquiry state, and opens the window that");
        SummaryText("      made a cached entry point crash on the second scan.");
    }

    if (c[kWInit] == 0) {
        SummaryText("    *** ALL ZERO: this is the FILE's copy of the block, not the");
        SummaryText("    *** live driver. A disk cache buffer matches the magic.");
    }

    SummaryText("  M0.1  pipe discovery");
    Row("ConfigStep entries",   c[kWCfgSteps]);
    line[0] = 0;
    PStrCat(line, "    highest stage reached    ");
    PStrCat(line, StageName(c[kWStageMax]));
    Summary(line);
    Row("last usbStatus",       c[kWCfgStatus]);
    SummaryText("  WHAT INTERFACES THE DEVICE REALLY HAS");
    /* ★★ THE FULL WALK. Run 34 proved the unclaimed A1044 yields THREE bus entries --
     * OS 9 enumerates device-class-0x00 devices per interface, which is why the Apple
     * keyboard yields three and the Dell mouse two. Every run up to v2.4 recorded only
     * the FIRST interface, so 'interface 0 is class 0x03' was one of three all along. */
    if (c[kWIfScanCount] != 0) {
        short i;
        Str255 l;
        Row("interfaces found", c[kWIfScanCount]);
        for (i = 0; i < 4 && (unsigned long)i < c[kWIfScanCount]; i++) {
            unsigned long v = c[kWIfScan0 + i];
            l[0] = 0;
            PStrCat(l, "    iface ");
            PStrCatNum(l, (long)(v & 0xFF));
            PStrCat(l, "  class 0x");
            PStrCatHexN(l, (v >> 24) & 0xFF, 2);
            PStrCat(l, " sub 0x");
            PStrCatHexN(l, (v >> 16) & 0xFF, 2);
            PStrCat(l, " proto 0x");
            PStrCatHexN(l, (v >> 8) & 0xFF, 2);
            if (((v >> 24) & 0xFF) == 0xE0)
                PStrCat(l, "  <== BLUETOOTH HCI");
            else if (((v >> 24) & 0xFF) == 0x03)
                PStrCat(l, "  (HID)");
            else if (((v >> 24) & 0xFF) == 0xFF)
                PStrCat(l, "  (VENDOR-SPECIFIC)");
            Summary(l);
        }
        RowHex("walk ended with", c[kWIfScanErr], 8);
        SummaryText("    (-6987 kUSBNotFound here is the NORMAL end of the walk)");
        if (c[kWIfScanCount] >= 4)
            SummaryText("    !! hit the 4-slot cap -- there may be MORE interfaces.");
    }
    RowHex("exact-triple find err", c[kWFindErr1], 8);
    Row("wildcard find tried",   c[kWFindAnyTried]);
    RowHex("wildcard find err",   c[kWFindErr2], 8);
    if (c[kWFindAnyTried] && c[kWFindErr2] == 0) {
        Str255 l;
        l[0] = 0;
        PStrCat(l, "    interface 0 is class 0x");
        PStrCatHexN(l, (c[kWIfaceFound] >> 24) & 0xFF, 2);
        PStrCat(l, " sub 0x");
        PStrCatHexN(l, (c[kWIfaceFound] >> 16) & 0xFF, 2);
        PStrCat(l, " proto 0x");
        PStrCatHexN(l, (c[kWIfaceFound] >> 8) & 0xFF, 2);
        PStrCat(l, "  ifaceNum ");
        PStrCatNum(l, (long)(c[kWIfaceFound] & 0xFF));
        Summary(l);
        SummaryText("    (Bluetooth HCI transport must be class 0xE0 sub 0x01 proto 0x01)");
    } else if (c[kWFindAnyTried]) {
        SummaryText("    even a WILDCARD interface search failed. The problem is not");
        SummaryText("    the class triple: suspect the bus power row above, or the");
        SummaryText("    device never reached a configured state.");
    }
    /* ⚠⚠ THESE TWO ROWS DO NOT COUNT THE SAME EVENTS, AND THE OLD LABELS IMPLIED THEY
     * DID. The 16.6 dongle run printed "immediate errors 0" beside "immediate errno
     * 0xFFFFE4B5", which reads as a contradiction and invites a wrong conclusion in
     * EITHER direction -- that an error was swallowed, or that a real error is being
     * ignored. In fact kWImmErr is bumped at only two sites in the config chain, while
     * Failed() stamps kWImmErrCode on ANY synchronous rejection anywhere in the driver.
     * So a code with a zero count is normal and means "something was rejected
     * synchronously somewhere, and it was not one of the two counted sites".
     *
     * ⇒ Say what each row actually measures, and DECODE the errno rather than printing
     * a bare number -- a hex value in a log is a question, a decoded field is an answer. */
    Row("immediate errors (2 counted config sites)", c[kWImmErr]);
    RowHex("last synchronous errno, ANY site",       c[kWImmErrCode], 8);
    if (c[kWImmErrCode] != 0) {
        long e = (long)c[kWImmErrCode];
        Str255 l; l[0] = 0;
        PStrCat(l, "      = ");
        PStrCatNum(l, e);
        if      (e == -6987) PStrCat(l, "  kUSBNotFound - a search ended with no (further) match");
        else if (e == -6911) PStrCat(l, "  kUSBNotRespondingErr - pipe stall, no device, or hung");
        else if (e == -6998) PStrCat(l, "  kUSBUnknownDeviceErr - the device REF is stale");
        else                 PStrCat(l, "  (not in this app's decode table)");
        Summary(l);
        if (e == -6987 && c[kWImmErr] == 0 && c[kWStageMax] >= 8)
            SummaryText("      BENIGN HERE: all three pipes were found (stage 8), so this is"
                        " a search that ended normally, not a failure.");
    }
    if (c[kWImmErr] > 0)
        SummaryText("      a USL call was rejected synchronously at a COUNTED site");
    Row("ConfigDone reached",   c[kWCfgDone]);
    Row("interrupt read armed", c[kWArmInt]);

    SummaryText("  M0.2 / M1  transport");
    Row("HCI commands sent",    c[kWCmdSent]);
    RowHex("last opcode",       c[kWLastOpcode], 4);
    Row("cmd completions",      c[kWCmdComp]);
    Row("last cmd status",      c[kWCmdStatus]);
    Row("int completions",      c[kWIntComp]);
    Row("last int status",      c[kWIntStatus]);
    Row("last actCount",        c[kWActCount]);
    RowHex("last event hdr",    c[kWLastEvent], 4);
    line[0] = 0;
    PStrCat(line, "    last event bytes         ");
    PStrCatHexN(line, c[kWEvt0], 8); PStrCatCh(line, ' ');
    PStrCatHexN(line, c[kWEvt1], 8);
    Summary(line);

    SummaryText("  M0.3  ACL DATA PATH (what L2CAP will ride on)");
    {
        Str255 l;
        unsigned long p = c[kWPipes];
        l[0] = 0;
        PStrCat(l, "    pipes open                ");
        PStrCat(l, ((p >> 8) & 0xF) ? "interrupt-IN " : "interrupt-IN:NO ");
        PStrCat(l, ((p >> 4) & 0xF) ? "bulk-OUT "     : "bulk-OUT:NO ");
        PStrCat(l, ( p       & 0xF) ? "bulk-IN"       : "bulk-IN:NO");
        Summary(l);
    }
    Row("ACL reads armed",      c[kWAclArmed]);
    Row("ACL read completions", c[kWAclInComp]);
    Row("last ACL read status", c[kWAclInStatus]);
    Row("last ACL byte count",  c[kWAclInCount]);
    Row("ACL bytes in total",   c[kWAclTotal]);
    RowHex("last ACL header",   c[kWAclHdr], 8);
    Row("ACL packets sent",     c[kWAclSent]);
    Row("ACL write completions", c[kWAclOutComp]);
    Row("last ACL write status", c[kWAclOutStatus]);
    if (c[kWAclArmed] > 0 && c[kWAclInComp] == 0)
        SummaryText("      armed and quiet, which is correct until L2CAP exists");

    /* ⚠ RETIRED SECTION. These words are written ONLY by src/bt_hci_m1.c, which
     * is still compiled but deliberately never started since the M2 switchover --
     * BTstack owns the controller now and keeps identity in its own hci_stack.
     *
     * Printing them unconditionally is what made BTCheck v9 misreport run 15: an
     * all-zero identity block plus a footer saying "the controller has not
     * identified itself yet", on a run that had in fact reached HCI_STATE_WORKING.
     * A successful run whose own summary reads as a failure costs a hardware cycle
     * to untangle, so the block is now printed only when the M1 layer actually ran.
     * Run 11 is the run that proved identity (HCI_Version 12, Manufacturer_ID 10,
     * BD_ADDR 00:1A:7D:xx:xx:xx). */
    if (HCI(c, kRecHciVersion) == 0 && HCI(c, kRecManufacturer) == 0
        && HCI(c, kRecBdAddrHi) == 0 && HCI(c, kRecBdAddrLo) == 0) {
        SummaryText("  M1  HCI IDENTITY - retired, and correctly empty");
        SummaryText("    The M1 layer is compiled but never started: BTstack drives");
        SummaryText("    bring-up since the M2 switchover and keeps identity itself.");
        SummaryText("    Zeros here are EXPECTED and are not a regression. Run 11 is");
        SummaryText("    the run that proved the controller identifies itself.");
        goto m2_section;
    }

    SummaryText("  M1  HCI IDENTITY - the milestone deliverable");
    Row("Reset status",         HCI(c, kRecResetStatus));
    Row("HCI_Version",          HCI(c, kRecHciVersion));
    Row("Manufacturer_ID",      HCI(c, kRecManufacturer));
    if (HCI(c, kRecManufacturer) == 10) {
        SummaryText("      = 10 (0x000A) the firmware CLAIMS Cambridge Silicon Radio.");
        SummaryText("      That is not proof of authenticity: clones claim it too. The");
        SummaryText("      BD_ADDR below and the HCI_Version above are the real test --");
        SummaryText("      a genuine CSR8510 A10 is Bluetooth 4.0, HCI_Version 6.");
    } else if (HCI(c, kRecManufacturer) != 0) {
        SummaryText("      NOT 10, so the firmware does not claim CSR at all. Every");
        SummaryText("      CSR-specific assumption downstream needs rechecking.");
    }
    RowBDAddr("BD_ADDR", HCI(c, kRecBdAddrHi), HCI(c, kRecBdAddrLo));
    SummaryText("      (counterfeit CSR8510 clones all share one burned-in address)");
    Row("ACL packet length",    HCI(c, kRecAclLen));
    Row("total ACL packets",    HCI(c, kRecAclCount));
    Row("SetEventMask status",  HCI(c, kRecSetMaskStatus));
    Row("WriteScanEnable stat", HCI(c, kRecScanEnStatus));
    RowHex("Cmd Status opcode", HCI(c, kRecCmdStatusOpcode), 4);
    Row("Inquiry responses",    HCI(c, kRecInqResponses));
    if (HCI(c, kRecInqResponses) > 0)
        RowBDAddr("first device found", HCI(c, kRecInqDevHi), HCI(c, kRecInqDevLo));
    if (HCI(c, kRecInqComplete) == 0)
        SummaryText("    Inquiry Complete            NOT RECEIVED YET");
    else
        Row("Inquiry Complete stat", HCI(c, kRecInqComplete) & 0xFF);
    RowHex("last other event",  HCI(c, kRecLastOtherEvent), 2);

m2_section:
    SummaryText("  PHASE 1  WHICH INIT PATH RAN, and what the interface says");
    Row("interface-init calls",     c[kWIfaceInit]);
    if (c[kWIfaceInit] == 0) {
        /* ⚠ THIS TEXT USED TO READ AS A FAULT AND IT IS NOT ONE ANY MORE. It said a
         * device-level 0xE0 match "cannot see" the A1044 -- run 26's finding, from when
         * the card was in HID proxy at 05AC:1000 with class 0x00. Both supported
         * controllers now declare 0xE0/0x01/0x01 at DEVICE level (the switched A1044 at
         * 8204 and the CSR dongle), so a device-level match is the EXPECTED path and 0
         * here is the normal, healthy answer. Read the M0.1 rows for whether it worked. */
        SummaryText("      0 = loaded as a DEVICE driver, which is the normal path:");
        SummaryText("      both supported controllers declare 0xE0/01/01 at DEVICE");
        SummaryText("      level, so rule 2a claims them and no interface-init runs.");
        SummaryText("      (Only a controller that hides its class on the interfaces");
        SummaryText("      needs that path -- the A1044 in HID proxy, run 26.)");
    } else {
        Row("  interface number",   c[kWIfaceInitNum]);
        {
            Str255 l; unsigned long t = c[kWIfaceTriplet];
            l[0] = 0;
            PStrCat(l, "      interface triplet        ");
            PStrCatHexN(l, (t >> 16) & 0xFF, 2); PStrCatCh(l, '/');
            PStrCatHexN(l, (t >>  8) & 0xFF, 2); PStrCatCh(l, '/');
            PStrCatHexN(l,  t        & 0xFF, 2);
            Summary(l);
        }
        RowHex("  device class",     c[kWIfaceDevClass], 2);
        {
            Str255 l; unsigned long v = c[kWIfaceVIDPID];
            l[0] = 0;
            PStrCat(l, "      VID:PID                  ");
            PStrCatHexN(l, (v >> 16) & 0xFFFF, 4); PStrCatCh(l, ':');
            PStrCatHexN(l,  v        & 0xFFFF, 4);
            Summary(l);
        }
        if (((c[kWIfaceTriplet] >> 16) & 0xFF) == 0xE0)
            SummaryText("      *** INTERFACE IS 0xE0 = Bluetooth HCI. The right device. ***");
        if ((c[kWIfaceVIDPID] >> 16) == 0x05AC && (c[kWIfaceVIDPID] & 0xFFFF) == 0x1000)
            SummaryText("      *** AND IT IS 05AC:1000 = THE A1044 INTERNAL MODULE ***");
    }

    SummaryText("  M2  BTstack");
    {
        Str255 l;
        unsigned long st = c[kWStkState];
        l[0] = 0;
        PStrCat(l, "    HCI state                 ");
        PStrCatNum(l, (long)st);
        PStrCat(l, "  = ");
        switch (st) {
        case 0:  PStrCat(l, "OFF");                                break;
        case 1:  PStrCat(l, "INITIALIZING");                       break;
        case 2:  PStrCat(l, "WORKING  <== M2 gate reached");       break;
        case 3:  PStrCat(l, "HALTING");                            break;
        case 4:  PStrCat(l, "SLEEPING");                           break;
        case 5:  PStrCat(l, "FALLING ASLEEP");                     break;
        default: PStrCat(l, "unknown");                            break;
        }
        Summary(l);
    }
    Row("run-loop iterations",  c[kWStkPump]);
    Row("nested pumps dropped", c[kWStkReenter]);
    Row("packets sent by stack", c[kWStkTx]);
    Row("packets delivered up", c[kWStkRx]);
    Row("sends refused (busy)", c[kWStkRefused]);
    Row("sends acked to stack", c[kWStkTxDone]);
    if (c[kWStkTx] > 0 && c[kWStkTxDone] == 0)
        SummaryText("      !! sent but never acked: BTstack still holds its packet");
    else if (c[kWStkTx] > c[kWStkTxDone] + 1)
        SummaryText("      more sent than acked; a completion may not be firing");
    if (c[kWStkPump] == 0)
        SummaryText("      pump never ran: BTstack was not started, or ConfigDone");
    else if (c[kWStkState] == 1)
        SummaryText("      still initialising: read packets sent vs delivered");

    SummaryText("  M2b  L2CAP / INQUIRY / SDP - the current milestone");
    {
        Str255 l;
        l[0] = 0;
        PStrCat(l, "    layers initialised        ");
        PStrCatNum(l, (long)c[kWL2Init]);
        switch (c[kWL2Init]) {
        case 0: PStrCat(l, "  = NEITHER (BT_StackStart did not reach them)"); break;
        case 1: PStrCat(l, "  = l2cap_init only"); break;
        default: PStrCat(l, "  = l2cap + sdp_client"); break;
        }
        Summary(l);

        l[0] = 0;
        PStrCat(l, "    inquiry                   ");
        PStrCatNum(l, (long)c[kWInqState]);
        switch (c[kWInqState]) {
        case 0: PStrCat(l, "  = NEVER STARTED (did HCI reach WORKING?)"); break;
        case 1: PStrCat(l, "  = started, no Inquiry Complete yet"); break;
        default: PStrCat(l, "  = complete"); break;
        }
        Summary(l);
    }
    RowStatus("gap_inquiry_start rc",  c[kWInqStartRc]);
    /* ⚠⚠ 12 = ERROR_CODE_COMMAND_DISALLOWED, and until v5.2 it left the driver stuck.
     * BT_ScanStart set gInqState = 1 BEFORE calling, so a REFUSED start reported
     * "scanning" with no inquiry running to ever complete it -- and the control panel
     * greys its scan button on that state, so the button died permanently and survived
     * a panel relaunch because the stuck value is in the DRIVER. Run 58 caught it:
     * inquiry 1, start rc 12, Inquiry Complete NOT RECEIVED. */
    if ((c[kWInqStartRc] & 0xFFUL) == 12UL && c[kWInqStartRc] != 0) {
        SummaryText("      12 = COMMAND DISALLOWED: BTstack had an inquiry active. As");
        SummaryText("      of v5.2 a cancel is attempted so the NEXT press can work --");
        SummaryText("      see the stop rc below. gInqState is no longer set on a");
        SummaryText("      refused start, so this cannot wedge the scan button.");
    }
    RowStatus("gap_inquiry_stop rc",   c[kWInqStopRc]);
    Row("inquiry responses",           c[kWInqResults]);
    Row("  of those, PERIPHERAL",       c[kWInqPeriph]);
    Row("  phones",                     c[kWInqPhones]);
    Row("  computers",                  c[kWInqComputers]);
    /* ★ Audio/Video counted separately as of v3.10. Until then major class 0x04 --
     * headphones, speakers, car kits -- fell into other/unknown, so the only classes
     * distinguished were the three we happened to own. */
    Row("  audio/video",                c[kWInqAudio]);
    Row("  other/unknown",              c[kWInqOther]);
    /* ⚠ The label said "max 4" for four revisions after v4.0 widened the table to
     * 16 for the control panel's list. Run 45 printed "stored in the table (max 4) 9",
     * which reads as a contradiction and invites exactly the wrong conclusion. */
    Row("  stored in the table (max 16)", c[kWRespStored]);
    Row("  peripheral displaced one",   c[kWRespDisplaced]);
    if (c[kWInqPeriph] > 0) {
        RowBDAddr("FIRST peripheral seen", c[kWInqPeriphAddrHi], c[kWInqPeriphAddrLo]);
        RowHex("  its class of device",   c[kWInqPeriphCoD], 6);
        {
            unsigned long kind = (c[kWInqPeriphCoD] >> 6) & 0x03UL;
            if (kind == 1)      SummaryText("      *** IT IS A KEYBOARD (peripheral minor class) ***");
            else if (kind == 2) SummaryText("      it is a POINTING DEVICE (mouse/trackpad)");
            else if (kind == 3) SummaryText("      it is a COMBO keyboard+pointing device");
            else                SummaryText("      peripheral, but neither keyboard nor pointing");
        }
        SummaryText("      ⇒ this device answered a CLASSIC inquiry, so it is NOT BLE-only");
    } else if (c[kWInqResults] > 0) {
        SummaryText("      NO peripheral answered. A BLE-only keyboard (any modern Magic");
        SummaryText("      Keyboard) cannot answer a Classic inquiry, so it will never");
        SummaryText("      appear here no matter how discoverable it is.");
    }
    RowStatus("Inquiry Complete stat", c[kWInqComplStatus]);

    if (c[kWTgtPick] != 0 || c[kWTgtAddrHi] != 0 || c[kWTgtAddrLo] != 0) {
        RowBDAddr("peer chosen", c[kWTgtAddrHi], c[kWTgtAddrLo]);
        RowHex("its class of device", c[kWTgtCoD], 6);
        {
            Str255 l;
            l[0] = 0;
            PStrCat(l, "    why that peer             #");
            PStrCatNum(l, (long)((c[kWTgtPick] >> 8) & 0xFF));
            PStrCat(l, ", ");
            switch (c[kWTgtPick] & 0xFF) {
            case 5:  PStrCat(l, "PERIPHERAL (a keyboard/mouse - the real target)"); break;
            case 2:  PStrCat(l, "PHONE"); break;
            case 1:  PStrCat(l, "COMPUTER"); break;
            default: PStrCat(l, "fallback: no preferred class answered"); break;
            }
            Summary(l);
        }
    } else if (c[kWInqState] == 2) {
        SummaryText("    no peer chosen: nothing answered the inquiry. Make the");
        SummaryText("    phone DISCOVERABLE (not merely powered on) and rerun.");
    } else if (c[kWInqState] == 0) {
        /* ⚠⚠ ZERO IS NOW THE NORMAL RESTING STATE, and saying nothing here would
         * invite reading it as a broken inquiry.
         *
         * As of driver v4.8 the automatic bring-up inquiry is OFF
         * (kAutoInquiryOnBringUp in bt_btstack.c). It used to fire on every bind --
         * 13 times in run 53 -- which made the control panel appear to start scans on
         * its own and refused button presses for ~5 s after each bind. The panel is
         * now the only thing that starts a scan, so an untouched driver legitimately
         * reports no inquiry at all. */
        SummaryText("    inquiry state 0: NO inquiry has run, which is now CORRECT --");
        SummaryText("    the automatic bring-up inquiry was removed in v4.8. Press Set");
        SummaryText("    Up New Device in the control panel to start one.");
    }

    Row("ACL connections OK",       c[kWConnOks]);
    RowStatus("last ACL conn status", c[kWHciConnStatus]);
    /* ★★★★★★ DECODE IT. THIS NUMBER IS THE ANSWER AND IT READ AS NOISE.
     *
     * ⚠⚠ 2026-09-16: a pairing attempt failed five times. The log carried
     * "last ACL conn status 34" and nothing else about it, while the M3.7 narrative a
     * few hundred lines below said "PATH: ARMED AND WAITING -- nothing was sent. Switch
     * the device on now." The user duly power-cycled the keyboard, repeatedly, and got
     * nowhere -- because something HAD been sent, the controller had accepted it
     * (Command Status 0x0405, status 0), and the connection had come back 0x22.
     *
     * 34 is 0x22, LMP RESPONSE TIMEOUT, and this file already decodes exactly that
     * value for the DISCONNECT reason twenty lines away. The knowledge was in the tool
     * and simply not applied where it mattered. A diagnostic that makes the reader
     * convert a decimal to hex and then go looking for the meaning has not diagnosed
     * anything.
     *
     * ⚠ Only the codes that actually distinguish something are named. A table of all 60
     * HCI errors would be padding; these four are the ones that separate "the peer
     * never heard us" from "the peer heard us and gave up", which need opposite
     * responses from the user. */
    {
        unsigned long st = c[kWHciConnStatus] & 0xFFUL;
        if ((c[kWHciConnStatus] & 0x100UL) != 0 && st != 0) {
            switch (st) {
            case 0x04:
                SummaryText("      0x04 = PAGE TIMEOUT. The device never answered at all:");
                SummaryText("      it is off, out of range, or asleep and not listening.");
                break;
            case 0x22:
                SummaryText("      ⭐ 0x22 = LMP RESPONSE TIMEOUT, and this is NOT the same");
                SummaryText("      as 'the device did not hear us'. It ANSWERED the page and");
                SummaryText("      then stopped replying part-way through link setup.");
                SummaryText("      ⚠⚠ THE USUAL CAUSE IS A ONE-SIDED BOND. If the device still");
                SummaryText("      holds a link key for this Mac and this Mac no longer holds");
                SummaryText("      one -- after a Delete, say -- the device tries to resume a");
                SummaryText("      bond we cannot answer, and link setup dies here. Power");
                SummaryText("      cycling does NOT fix that: a device that believes it is");
                SummaryText("      bonded RECONNECTS on power-up, it does not re-advertise.");
                SummaryText("      The device's own pairing has to be cleared on the device.");
                break;
            case 0x05:
                SummaryText("      0x05 = AUTHENTICATION FAILURE. The keys do not match.");
                break;
            case 0x08:
                SummaryText("      0x08 = CONNECTION TIMEOUT (supervision).");
                break;
            default:
                SummaryText("      Non-zero: no link came up. Read the code against the HCI");
                SummaryText("      error table before blaming anything in this stack.");
                break;
            }
        }
    }
    RowHex("ACL handle",               c[kWHciConnHandle], 4);
    Row("L2CAP events seen",           c[kWL2capEvts]);
    Row("SDP query issued",            c[kWSdpIssued]);
    RowStatus("sdp query call rc",     c[kWSdpQueryRc]);
    Row("SDP events (ANY code)",       c[kWSdpEventsAny]);
    RowHex("first surprise SDP evt",   c[kWSdpLastEvt], 2);
    Row("SDP attribute bytes",         c[kWSdpAttrBytes]);
    Row("SDP records seen",            c[kWSdpRecords]);
    Row("SDP queries OK",              c[kWSdpOks]);
    RowStatus("last SDP complete",     c[kWSdpStatus]);
    Row("pool alloc FAILURES",         c[kWAllocFails]);
    if (c[kWAllocFails] > 0) {
        SummaryText("      !! 0x56 BTSTACK_MEMORY_ALLOC_FAILED seen: a static pool ran");
        SummaryText("      dry. Raise the MAX_NR_* in btstack_config.h. NOT a protocol");
        SummaryText("      fault, and it looks exactly like one -- that is the trap.");
    }
    if (c[kWConnOks] > 0 && (c[kWHciConnStatus] & 0xFF) != 0)
        SummaryText("      note: a connection DID succeed earlier; last-status is a later try");
    Row("SDP retries after encrypt", c[kWSdpRetries]);
    RowStatus("retry call rc",         c[kWSdpRetryRc]);
    Row("retries skipped (busy)",      c[kWSdpNotReady]);
    RowHex("first surprise event",     c[kWLastUnkEvt], 2);
    if (c[kWLastUnkEvt] == 0)
        SummaryText("      0x00 = nothing unexpected arrived; routine events are filtered");
    else if (c[kWLastUnkEvt] == 0x05)
        SummaryText("      0x05 = DISCONNECTION COMPLETE: the peer dropped the link");
    else if (c[kWLastUnkEvt] == 0x18 || c[kWLastUnkEvt] == 0x31)
        SummaryText("      a pairing/authentication request -- that is M3, not M2");

    /* The single line that decides the milestone. One attribute byte is proof of
     * the whole chain: ACL up, L2CAP channel opened AND configured, request sent,
     * response received and parsed. */
    if (c[kWSdpAttrBytes] > 0)
        SummaryText("      *** L2CAP ROUND TRIP PROVEN: SDP data came back ***");
    else if (c[kWSdpRetries] == 0 && c[kWEncryptOn] == 0)
        SummaryText("      no retry: the link never became encrypted (see M2c rows)");
    else if (c[kWSdpRetries] == 0 && c[kWEncryptOn] != 0)
        SummaryText("      !! encrypted but NO RETRY -- the trigger did not fire.");
    else if (c[kWSdpEventsAny] > 0 && c[kWSdpAttrBytes] == 0)
        SummaryText("      SDP replied but returned NO records: wrong search UUID");
    else if (c[kWSdpRetries] > 0)
        SummaryText("      !! retried while encrypted and STILL no bytes: SDP itself");
    else if (c[kWHciConnStatus] == 0x100 && c[kWSdpAttrBytes] == 0)
        SummaryText("      !! ACL is up but no SDP bytes: the fault is IN L2CAP.");
    else if (c[kWHciConnStatus] > 0x100)
        SummaryText("      !! the peer refused the ACL connection; below L2CAP.");
    else if (c[kWSdpIssued] == 0 && c[kWInqState] == 2)
        SummaryText("      !! query never issued -- see 'why that peer' above.");

    SummaryText("  M2c  PAIRING - what run 17 walked into");
    Row("SSP auto-accept set",     c[kWSspAuto]);
    Row("IO capability requests",  c[kWIoCapReqs]);
    Row("user confirm requests",   c[kWUserConfReqs]);
    Row("legacy PIN requests",     c[kWPinReqs]);
    /* ★★ M3.7. Until v3.2 the request above was counted and never answered, so legacy
     * pairing could not complete -- BTstack waits for gap_pin_code_response and we
     * never called it. ANSWERED LAGGING REQUESTS IS THE SIGNATURE OF THAT BUG COMING
     * BACK, which is why both are printed side by side rather than just the answer. */
    Row("  of those, ANSWERED",    c[kWPinAnswered]);
    RowStatus("  response rc",     c[kWPinRespRc]);
    if (c[kWPinAddrHi] || c[kWPinAddrLo]) {
        Str255 l; l[0] = 0;
        PStrCat(l, "  PIN asked by            ");
        PStrCatHexN(l, (c[kWPinAddrHi] >> 16) & 0xFF, 2); PStrCatCh(l, ':');
        PStrCatHexN(l, (c[kWPinAddrHi] >>  8) & 0xFF, 2); PStrCatCh(l, ':');
        PStrCatHexN(l,  c[kWPinAddrHi]        & 0xFF, 2); PStrCatCh(l, ':');
        PStrCatHexN(l, (c[kWPinAddrLo] >> 16) & 0xFF, 2); PStrCatCh(l, ':');
        PStrCatHexN(l, (c[kWPinAddrLo] >>  8) & 0xFF, 2); PStrCatCh(l, ':');
        PStrCatHexN(l,  c[kWPinAddrLo]        & 0xFF, 2);
        Summary(l);
    }
    if (c[kWPinReqs] > 0 && c[kWPinAnswered] < c[kWPinReqs]) {
        SummaryText("    !! A PIN WAS ASKED FOR AND NOT ANSWERED. Legacy pairing");
        SummaryText("    !! cannot complete: the remote waits, then gives up.");
    }
    if (c[kWPinAnswered] > 0) {
        SummaryText("    (the host offers a FIXED PIN -- type 0000 then Return");
        SummaryText("     on the keyboard being paired)");
    }
    Row("link key requests",       c[kWLinkKeyReqs]);
    RowStatus("Simple Pairing Complete", c[kWSimplePairing]);
    RowStatus("Encryption Change",     c[kWEncryptChange]);
    Row("encryption enabled",          c[kWEncryptOn]);
    RowStatus("Auth Complete (LEGACY)", c[kWAuthComplete]);
    if (c[kWAuthComplete] == 0 && c[kWSimplePairing] != 0)
        SummaryText("      LEGACY row empty is CORRECT for SSP: 0x06 is not its event");
    if ((c[kWSimplePairing] & 0xFF) == 0 && c[kWSimplePairing] != 0 && c[kWEncryptOn] != 0)
        SummaryText("      *** PAIRED AND ENCRYPTED ***");
    Row("disconnections",          c[kWDiscCount]);
    RowStatus("disconnect reason",  c[kWDiscReason]);
    RowHex("handle that dropped",   c[kWDiscHandle], 4);
    if ((c[kWDiscReason] & 0xFF) == 0x22 && c[kWDiscReason] != 0) {
        SummaryText("      0x22 = LMP RESPONSE TIMEOUT. The peer waited for an answer");
        SummaryText("      we never sent. This is exactly what killed run 17, and if");
        SummaryText("      it recurs WITH auto-accept set, the reply is not going out.");
    } else if ((c[kWDiscReason] & 0xFF) == 0x13) {
        SummaryText("      0x13 = remote user ended the connection -- normal, or the");
        SummaryText("      phone gave up/was dismissed by hand.");
    } else if ((c[kWDiscReason] & 0xFF) == 0x05) {
        SummaryText("      0x05 = AUTHENTICATION FAILURE: pairing was refused.");
    }
    if (c[kWUserConfReqs] > 0 && (c[kWAuthComplete] & 0xFF) == 0 && c[kWAuthComplete] != 0)
        SummaryText("      *** PAIRING SUCCEEDED: confirm requested, auth complete 0 ***");

    /* ★★★ DOES THE CONTROLLER HAVE A KEY STORE OF ITS OWN? This decides how OS 9
     * could ever pair an A1016 through the A1044.
     *
     * Proven 2026-09-06: a keyboard paired under Tiger drove the Open Firmware boot
     * picker and then typed in OS 9, so the CARD holds a link key and reconnects with
     * no host. The question is whether a host can PUT one there, and it needs no
     * vendor command: the spec defines Read/Write/Delete_Stored_Link_Key at OGF 0x03,
     * OCF 0x0D/0x11/0x12. Max_Num_Keys is the answer. */
    SummaryText("  CONTROLLER-SIDE LINK KEY STORE - can a host write one?");
    if (c[kWStoredKeyRc] == 0) {
        SummaryText("    Read_Stored_Link_Key  NOT ASKED (driver older than v5.8?)");
    } else {
        Row("Read_Stored_Link_Key status", c[kWStoredKeyRc] & 0xFF);
        Row("  Max_Num_Keys",          (c[kWStoredKeyCap] >> 16) & 0xFFFF);
        Row("  Num_Keys_Read",          c[kWStoredKeyCap]        & 0xFFFF);
        /* ★★★ v8.2: THE COUNT BEFORE AND AFTER, IN ONE BOOT.
         *
         * ⚠ Until v8.2 the store was read exactly once, at bring-up, so a delete or a
         * write could only ever be confirmed by the NEXT boot's read -- one number per
         * hardware cycle. A delete now re-reads, and the bring-up value is kept beside
         * the live one so the TRANSITION is legible without a reboot to separate them. */
        Row("  reads issued",           c[kWRlkPasses]);
        if (c[kWRlkPasses] > 1 && c[kWStoredKeyCapFirst] != 0) {
            Row("  Num_Keys at BRING-UP", c[kWStoredKeyCapFirst] & 0xFFFF);
            if ((c[kWStoredKeyCapFirst] & 0xFFFFUL) > (c[kWStoredKeyCap] & 0xFFFFUL)) {
                SummaryText("    ⭐⭐⭐⭐ THE COUNT FELL. The card's store really did lose a");
                SummaryText("      key, measured in this same boot -- not inferred from");
                SummaryText("      a command's return status. Compare the bonded");
                SummaryText("      addresses below: the deleted one should be GONE.");
            } else if ((c[kWStoredKeyCapFirst] & 0xFFFFUL)
                       < (c[kWStoredKeyCap] & 0xFFFFUL)) {
                SummaryText("    ⭐ THE COUNT ROSE -- a key was ADDED to the card's store");
                SummaryText("      and read back. That is a pairing made here, proven");
                SummaryText("      by the controller rather than by our own bookkeeping.");
            } else {
                SummaryText("    ⚠ RE-READ, AND THE COUNT DID NOT MOVE. The delete or");
                SummaryText("      write reported success and the store disagrees.");
                SummaryText("      Believe this row, not the command's status.");
            }
        }
        if ((c[kWStoredKeyRc] & 0xFF) != 0) {
            SummaryText("    ⚠ the command FAILED. This controller may not support it.");
        } else if (((c[kWStoredKeyCap] >> 16) & 0xFFFF) != 0) {
            SummaryText("    ⭐ NON-ZERO: this controller HOLDS link keys of its own,");
            SummaryText("      so Write_Stored_Link_Key (OCF 0x11) is the route to");
            SummaryText("      pairing an A1016 from OS 9. No vendor command needed.");
        } else {
            SummaryText("    ⚠ ZERO: no controller-side store. The standard route is");
            SummaryText("      out and it must be a CSR vendor mechanism -- back to");
            SummaryText("      Apple's kexts. See docs/PROXY-FIRST-PLAN.md §4.");
        }
    }

    /* ★★★ v71: WHY THE PROBE DID NOT SEND. Twice now the row above has read NOT
     * ASKED with the stack in HCI_STATE_WORKING and the pump timer firing thousands
     * of times, and both times the block could not say why. These rows are the whole
     * reason v6.5 exists, so print them whether or not the probe eventually fired. */
    SummaryText("  ⭐ WHY - the send gates, sampled in the pump timer");
    if (c[kWGateFirst] == 0) {
        SummaryText("    NOT SAMPLED - driver older than v6.5, or the probe function");
        SummaryText("    never ran at all. Check THE PUMP TIMER rows below.");
    } else {
        unsigned long gf = c[kWGateFirst], gl = c[kWGateLast];
        Row("hci_send_cmd rc",        c[kWSendRc] & 0xFF);
        Row("HCI state at sample",   (gl >> 8) & 0xFF);
        Row("free classic ACL slots", c[kWAclSlots]);
        SummaryText("    gate                       first  last");
        RowPair("can_send_command_now",  (gf & 0x01) != 0, (gl & 0x01) != 0);
        RowPair("packet buffer RESERVED",(gf & 0x02) != 0, (gl & 0x02) != 0);
        RowPair("can_send_acl_classic",  (gf & 0x04) != 0, (gl & 0x04) != 0);
        RowPair("ours: BT_CanSendCommand",(gf & 0x08) != 0,(gl & 0x08) != 0);
        RowPair("ours: BT_CanSendACL",   (gf & 0x10) != 0, (gl & 0x10) != 0);

        /* The discriminator, spelled out so the run cannot be read two ways. */
        if ((gl & 0x01) != 0) {
            SummaryText("    ⚠ BTstack was WILLING to take a command. So the refusal is");
            SummaryText("      NOT a stuck gate -- the state guard rejected first, or the");
            SummaryText("      send itself failed. Read the rc row. THE MUTE-STACK THEORY");
            SummaryText("      IS REFUTED; do not carry it forward.");
        } else if ((gl & 0x02) != 0) {
            SummaryText("    ⭐ THE ANSWER: the packet buffer is STILL RESERVED. For an");
            SummaryText("      async transport BTstack holds it until it processes");
            SummaryText("      HCI_EVENT_TRANSPORT_PACKET_SENT, and hci_run() returns");
            SummaryText("      early on this same flag -- which silences commands AND");
            SummaryText("      ACL together, exactly as the v6.3 run showed. Look at");
            SummaryText("      BT_NotifyPacketSent: an ack that never reaches the stack.");
        } else if ((gl & 0x08) == 0) {
            SummaryText("    ⚠ OUR gate is the one shut: gCmdBusy latched or gDevice is");
            SummaryText("      gone. That is ours to fix, not BTstack's.");
        } else {
            SummaryText("    ⚠ Buffer free and our gate open, yet BTstack still refuses:");
            SummaryText("      that leaves num_cmd_packets == 0, i.e. the CONTROLLER");
            SummaryText("      stopped granting command credit. A controller-side stall.");
        }
        if ((gl & 0x04) == 0 && c[kWAclSlots] == 0) {
            SummaryText("    ⚠ ZERO ACL slots too: the controller's Read_Buffer_Size may");
            SummaryText("      have reported no ACL buffers, which would explain why an");
            SummaryText("      inbound L2CAP request could never be answered.");
        }
    }

    /* ★★★ v73/v6.6: WHICH DEVICES THE CARD IS BONDED TO, by address, from the
     * controller's own store. This is the row that identifies the peer without
     * needing a Tiger boot to look the keyboard's address up by hand. */
    SummaryText("  ⭐ THE CARD'S OWN BONDED ADDRESSES (Return_Link_Keys 0x15)");

    /* ★★★★★★ v12.0: IS THE TIGER KEY SAVED? Read this BEFORE deleting anything.
     *
     * The user has chosen to delete the A1016's bond and re-pair it from OS 9. That
     * bond was made by Tiger and is the only reason the keyboard works at all today --
     * pairing has NEVER completed from OS 9 on this card (put_link_key is 0 in every
     * banked run). So the delete is only reversible if we captured the key first.
     *
     * ⚠ THE KEY ITSELF IS NOT PRINTED AND NEVER WILL BE. This log goes to a shared
     * network volume. These are counts and a verdict, nothing else. */
    SummaryText("  ★★★★ THE PAIRING SAFETY NET -- READ BEFORE DELETING A BOND");
    missingKeys = 0;
    if (c[kWNameDeferred] != 0 || c[kWInqRetries] != 0) {
        SummaryText("  ---- SCAN vs NAME REQUESTS (v13.1)");
        Row("      name asks deferred (scan live)", c[kWNameDeferred]);
        Row("      refused scans cancelled+retried", c[kWInqRetries]);
        Row("        of those, retry SUCCEEDED",     c[kWInqRecovered]);
        SummaryText("      A Remote_Name_Request issued while an inquiry is running");
        SummaryText("      makes the controller abandon the inquiry with NO Inquiry");
        SummaryText("      Complete -- so BTstack's inquiry_state sticks at ACTIVE and");
        SummaryText("      every later scan returns 12 = COMMAND_DISALLOWED. That is the");
        SummaryText("      v13.0 regression. Deferrals here are collisions PREVENTED.");
        if (c[kWInqRetries] != 0 && c[kWInqRecovered] == 0) {
            SummaryText("    ⚠ RETRIES ALL FAILED. Cancelling did not clear the state, so");
            SummaryText("      something is holding the inquiry open that we cannot see.");
        }
    }
    Row("      keys captured from the card", c[kWRlkCaptured]);
    /* ⚠⚠ THIS ROW USED TO READ kWLkCaptured, WHICH IS THE SAME COUNTER AS THE ONE
     * ABOVE. Two rows showing one value, labelled as if they corroborated each other
     * -- the v12.0 run printed "captured 1 / in database 1" while the database
     * actually held TWO and the flush wrote TWO records. gLkStored is the real size.
     * A diagnostic whose two independent-looking rows are one number is worse than a
     * single row, because it manufactures confidence. */
    Row("      keys now in our database",    c[kWLkStored]);
    if (c[kWRestoreTried] != 0)
        Row("      restore commands issued",  c[kWRestoreTried]);

    /* ★★★★★★ THE CHECK THAT ACTUALLY MATTERS: is EVERY key the card holds also in
     * our database? v12.0 asked only "is the count greater than zero", said CAPTURED,
     * and was RIGHT BY LUCK -- it would have said the same if the one captured key
     * belonged to the other device and the keyboard's had been missed. Deleting a bond
     * on that basis is a one-way door. Compare the two lists by address instead. */
    {
        unsigned long i, j;
        for (i = 0; i < (unsigned long)kWRlkSlots; i++) {
            unsigned long hi = c[kWRlkA0Hi + i * 2];
            unsigned long lo = c[kWRlkA0Hi + i * 2 + 1];
            int found = 0;
            if (hi == 0 && lo == 0) continue;
            for (j = 0; j < c[kWKeyCount] && j < 8; j++) {
                if (c[kWKeyBase + j * 2] == hi && c[kWKeyBase + j * 2 + 1] == lo) {
                    found = 1; break;
                }
            }
            if (!found) { RowBDAddr("  ⚠⚠ NOT CAPTURED", hi, lo); missingKeys++; }
        }
        if (c[kWRlkKeys] > (unsigned long)kWRlkSlots) {
            SummaryText("    ⚠ The card holds more keys than there are address slots,");
            SummaryText("      so this comparison covers only the first four. Treat an");
            SummaryText("      all-clear as covering those, not the whole store.");
        }
        if (missingKeys != 0) {
            SummaryText("    ⚠⚠ THE CARD HOLDS A KEY WE HAVE NOT SAVED. Deleting THAT");
            SummaryText("      bond would be a ONE-WAY DOOR. Restore cannot put back a");
            SummaryText("      key that was never captured, and the driver refuses to");
            SummaryText("      write a guess. Do not delete the address named above.");
        }
    }

    /* ⚠⚠ THE ALL-CLEAR MUST NOT PRINT ALONGSIDE A MISS. The v12.1 run printed BOTH
     * "NOT CAPTURED 00:0A:95:xx:xx:xx" and "EVERY KEY THE CARD HOLDS IS SAVED", because
     * this test was independent of the address comparison above. A diagnostic that
     * contradicts itself in adjacent lines is worse than one that says nothing. */
    if (missingKeys != 0) {
        /* the comparison above already printed the verdict and named the address */
    } else if (c[kWRlkCaptured] == 0 && c[kWLkStored] == 0) {
        SummaryText("    ⚠⚠ NOTHING CAPTURED. DO NOT DELETE THE A1016 BOND YET.");
        SummaryText("      The card's key has not been copied anywhere, so deleting it");
        SummaryText("      would be a ONE-WAY DOOR: nothing on this machine would ever");
        SummaryText("      have seen the Tiger key, and the keyboard would stay dead");
        SummaryText("      until pairing works -- which is the thing being tested.");
        SummaryText("      Check the 0x15 rows above: no read, no capture.");
    } else {
        SummaryText("    ⭐⭐ EVERY KEY THE CARD HOLDS IS SAVED. The delete is REVERSIBLE:");
        SummaryText("      database and in System Folder:Preferences:OS9 Bluetooth Keys");
        SummaryText("      on the local disk, and the panel's Restore puts it back.");
        SummaryText("      ⚠ Keep the WIRED keyboard attached for the whole experiment.");
    }
    if (c[kWRlkEvents] == 0) {
        SummaryText("    NO 0x15 EVENT SEEN. Either the driver predates v6.6, or");
        SummaryText("    Read_Stored_Link_Key was never answered -- check the");
        SummaryText("    Max_Num_Keys rows above. Num_Keys_Read > 0 with no 0x15 event");
        SummaryText("    would mean the controller reported keys and never listed them.");
    } else {
        short k;
        Row("0x15 events seen",  c[kWRlkEvents]);
        Row("keys listed",       c[kWRlkKeys]);
        for (k = 0; k < kWRlkSlots; k++) {
            unsigned long hi = c[kWRlkA0Hi + k * 2], lo = c[kWRlkA0Hi + k * 2 + 1];
            if (hi != 0 || lo != 0) RowBDAddr("  bonded", hi, lo);
        }
        if (c[kWRlkKeys] > kWRlkSlots) {
            SummaryText("    ⚠ more keys than address slots -- only the first four are");
            SummaryText("      named. The count above is the real total.");
        }
        SummaryText("    ⚠ ADDRESSES ONLY BY DESIGN. That event carries the 16-byte");
        SummaryText("      link key beside each address; the driver steps over it and");
        SummaryText("      never copies it into the block. Do not add it.");
    }

    /* ★★ WHO PAGED US, now from two independent points in the flow. */
    if (c[kWConnPeerHi] != 0 || c[kWConnPeerLo] != 0 || c[kWConnReqs] != 0) {
        SummaryText("  ⭐ WHO CONNECTED");
        if (c[kWConnReqs] != 0) {
            Row("page requests (0x04)", c[kWConnReqs]);
            RowBDAddr("  paged from", c[kWConnReqHi], c[kWConnReqLo]);
            RowHex("  its class of device", c[kWConnReqCoD], 6);
            Row("  link type",             c[kWConnReqType] & 0xFF);
            /* CoD minor class lives in bits 2..7 of the low byte; 0x40 = keyboard,
             * 0x80 = pointing device, 0xC0 = combo. Major class 0x05 = Peripheral. */
            if (((c[kWConnReqCoD] >> 8) & 0x1F) == 0x05) {
                unsigned long minor = c[kWConnReqCoD] & 0xC0;
                if (minor == 0x40)      SummaryText("      ⭐ PERIPHERAL / KEYBOARD");
                else if (minor == 0x80) SummaryText("      ⭐ PERIPHERAL / POINTING DEVICE");
                else if (minor == 0xC0) SummaryText("      ⭐ PERIPHERAL / KEYBOARD + POINTER");
                else                    SummaryText("      PERIPHERAL, unclassified minor");
            }
        }
        if (c[kWConnPeerHi] != 0 || c[kWConnPeerLo] != 0)
            RowBDAddr("connected (0x03)", c[kWConnPeerHi], c[kWConnPeerLo]);
        SummaryText("    ⇒ Compare against the bonded addresses above. A match means");
        SummaryText("      the card already holds a key for whatever keeps paging us.");
        /* ★★★ v8.2: EVERY paging address, not only the most recent one. These are the
         * rows the control panel now offers, so what is listed here is exactly what
         * the user can select and pair. */
        if (c[kWPagedCount] != 0) {
            int pi;
            Row("  distinct pagers", c[kWPagedCount]);
            for (pi = 0; pi < kPagedSlots; pi++) {
                unsigned long hi  = c[kWPagedA0Hi + pi * 3 + 0];
                unsigned long lo  = c[kWPagedA0Hi + pi * 3 + 1];
                unsigned long cod = c[kWPagedA0Hi + pi * 3 + 2];
                if (hi == 0 && lo == 0) continue;
                RowBDAddr("    pager", hi, lo);
                RowHex("      class of device", cod, 6);
            }
            SummaryText("      ⭐ THESE ARE SELECTABLE IN THE CONTROL PANEL. A keyboard");
            SummaryText("      that has lost its host PAGES rather than advertising, so");
            SummaryText("      it never answers an inquiry -- and once its stored key is");
            SummaryText("      deleted it is in no other list either. This is the only");
            SummaryText("      source that can offer it after the delete.");
        }
    }

    /* ★★ WHERE THE AUTH HANDSHAKE DIES. */
    SummaryText("  ⭐ AUTH PATH - the 8 links that timed out");
    Row("Connection Requests",   c[kWConnReqs]);
    Row("link key requests",     c[kWLinkKeyReqs]);
    Row("Auth Completes",        c[kWAuthComps]);
    Row("Link Key Notifications",c[kWLkNotifs]);
    if (c[kWLastCmdStatus] != 0) {
        RowHex("last Command Status opcode", (c[kWLastCmdStatus] >> 16) & 0xFFFF, 4);
        Row("  its status",                   c[kWLastCmdStatus]        & 0xFF);
        Row("  command credit granted",      (c[kWLastCmdStatus] >> 8)  & 0xFF);
        if (((c[kWLastCmdStatus] >> 8) & 0xFF) == 0) {
            SummaryText("    ⚠⚠ CREDIT ZERO. hci.c sets num_cmd_packets from this byte");
            SummaryText("      and refuses every command while it is 0, so the stack is");
            SummaryText("      mute for commands from here on. This is the state at the");
            SummaryText("      END of the session, which is what makes it meaningful.");
        }
    }
    /* The order is the question, so print the ring oldest-first. */
    if (c[kWCmdRingIdx] != 0 || c[kWCmdRing] != 0) {
        short n;
        SummaryText("    last 8 opcodes SENT, oldest first:");
        for (n = 0; n < 8; n++) {
            unsigned long op = c[kWCmdRing + ((c[kWCmdRingIdx] + n) & 7)];
            if (op == 0) continue;
            RowHex("      opcode", op, 4);
            if (op == 0x0411) SummaryText("        ^ Authentication_Requested");
            else if (op == 0x0409) SummaryText("        ^ Accept_Connection_Request");
            else if (op == 0x040B) SummaryText("        ^ Link_Key_Request_Reply");
            else if (op == 0x040C) SummaryText("        ^ Link_Key_Request_NEGATIVE_Reply");
            else if (op == 0x0413) SummaryText("        ^ Set_Connection_Encryption");
            else if (op == 0x041B) SummaryText("        ^ Read_Remote_Supported_Features");
            else if (op == 0x0406) SummaryText("        ^ Disconnect");
            else if (op == 0x0C0D) SummaryText("        ^ Read_Stored_Link_Key (ours)");
        }
    }

    /* ★★★ AND WHAT CAME BACK. The opcode ring says what we sent; this says what the
     * controller answered, and the pair together is the handshake.
     *
     * ⚠ THE HEADLINE IS THE 0x0B COUNT. hci.c:8096 gates Authentication_Requested on
     * BONDING_RECEIVED_REMOTE_FEATURES, set only from the 0x0B handler -- so a count of
     * zero means the controller accepted Read_Remote_Supported_Features and never
     * completed it, and every stalled link follows from that one absence. A NON-zero
     * count means the event does arrive and the fault is on our side of it, which
     * would move the investigation somewhere completely different. */
    SummaryText("  ⭐ WHAT CAME BACK - controller events, from the transport");
    if (c[kWEvtTotal] == 0) {
        SummaryText("    NO EVENTS RECORDED. Driver older than v6.7, or the transport");
        SummaryText("    never delivered a packet at all -- which the interrupt");
        SummaryText("    completion counters above would already have shown.");
    } else {
        short n;
        Row("events delivered",  c[kWEvtTotal]);
        Row("0x0B arrivals",     c[kWRrsfCount]);
        /* ⚠⚠ FABRICATED EVENTS, NAMED AS SUCH AND PRINTED HERE ON PURPOSE. v6.8's
         * probe injects a Read_Remote_Supported_Features completion the controller
         * never sent, because the A1044 accepts that command and never completes it
         * while the link is alive. Anything downstream that starts working in this
         * build works BECAUSE OF THIS LIE, and a reader who does not see the count
         * next to the real arrivals would draw a false conclusion about the
         * controller. Do not move this row away from the one above it. */
        /* ⚠⚠⚠ THE LABEL HERE USED TO READ "of which SYNTHESISED", AND IT WAS A LIE
         * ABOUT ITS OWN NUMBERS THAT MISLED EVERY READING OF THIS SECTION FOR WEEKS.
         *
         * These are INDEPENDENT counters, not a total and a subset:
         *   gRrsfCount  is incremented at the top of BT_DeliverPacket, the transport's
         *               entry point, so it counts REAL 0x0B events off the wire.
         *   gSynthRrsf  is incremented beside a DIRECT gPacketHandler() call that does
         *               not re-enter BT_DeliverPacket, so it counts injections only.
         *
         * "arrivals 3, of which SYNTHESISED 3" therefore meant 3 REAL events AND 3
         * fabricated ones -- the stack was fed every completion TWICE -- and it read
         * as "the card sent none of them". Every previous run's conclusion that the
         * A1044 withholds this event came from that phrasing. The v99 run settles it:
         * with the gate off, arrivals 3 and NO synthetic row at all. THE CARD SENDS
         * THEM. The 1:1 symmetry in the old logs is just one completion per command
         * from the card, plus one injection per command from us.
         *
         * ⇒ AND THAT REFRAMES WHAT THE GATE EVER DID. It was never filling in a
         * missing event. It was supplying a DUPLICATE with all-zero feature bytes, to
         * make l2cap believe the peer had no SSP and so dodge l2cap.c:2469's Security
         * Mode 4 disconnect. Not "the card is silent" -- "lie about the peer". A far
         * better reason it must never ship, and as of the 8.4 driver it does not.
         *
         * ⚠ Worded to avoid the literal phrase "driver v8" + the number:
         * scripts/bump-version.py expects exactly ONE occurrence of that string in
         * this file (the kExpectedDriverTag comment) and refuses to bump when prose
         * adds a second. A comment is not worth breaking the version guard over. */
        if (c[kWSynthRrsf] != 0) {
            Row("  PLUS fabricated by us", c[kWSynthRrsf]);
            SummaryText("      ⚠⚠ A SEPARATE COUNT, NOT A SUBSET OF THE ARRIVALS ABOVE.");
            SummaryText("      The stack saw both: the card's real completions AND this");
            SummaryText("      many fabricated duplicates carrying all-zero features,");
            SummaryText("      chosen so the SSP bit stays clear and l2cap.c's Security");
            SummaryText("      Mode 4 disconnect is skipped. Nothing downstream of here");
            SummaryText("      is evidence about what the A1044 does unaided, and the");
            SummaryText("      real arrivals above are NOT proof the lie was needed.");
        }
        if (c[kWRrsfCount] != 0) {
            Row("  its status",      c[kWRrsfStatus] & 0xFF);
            RowHex("  for handle",   c[kWRrsfHandle], 4);
            SummaryText("    ⚠ THE EVENT DOES ARRIVE. So the stall is NOT a missing");
            SummaryText("      completion, and the missing-event theory is REFUTED.");
            SummaryText("      A nonzero status means the controller refused the read;");
            SummaryText("      status 0 means it succeeded and BTstack still did not");
            SummaryText("      set BONDING_RECEIVED_REMOTE_FEATURES -- look at whether");
            SummaryText("      the handle above matches the connection that stalled.");
        } else if (c[kWConnReqs] == 0 && c[kWConnOks] == 0) {
            /* ⚠⚠ THE MESSAGE BELOW ASSUMED A CONNECTION HAPPENED, AND ONE RUN PROVED
             * THAT WRONG. With no page request and no ACL link, 0x041B was never sent
             * -- so "accepted and never completed" is a claim about a command that
             * does not exist in this session. A diagnostic asserting a cause that
             * cannot apply is worse than one that says it has no data.
             *
             * ⇒ This is the inconclusive case and it must be labelled as such: not a
             * measurement, not a refutation, just nothing to measure. */
            SummaryText("    ⚠ NO DATA -- NOT A RESULT. Nothing ever connected this");
            SummaryText("      session (Connection Requests 0, ACL links 0), so");
            SummaryText("      Read_Remote_Supported_Features was never sent and there");
            SummaryText("      was nothing for the probe to act on. The peer simply did");
            SummaryText("      not page us. Wake the device -- for an A1016, switching");
            SummaryText("      it OFF and ON makes it page at once instead of waiting");
            SummaryText("      on its own reconnect timer -- and run again.");
        } else {
            /* ⚠ v9.9: THIS TEXT USED TO CALL A ZERO HERE THE FAULT. It said the query
             * "was accepted and NEVER COMPLETED", which was true up to v9.7 -- and
             * became exactly backwards in v9.8, where we stopped SENDING it. The gate
             * below tells the two apart instead of assuming. */
            if (c[kWSecReqs] == 0) {
                SummaryText("    ⭐⭐ ZERO, AND THAT IS THE FIX WORKING, not a fault.");
                SummaryText("      security requests made is also 0, so nothing ever set");
                SummaryText("      BONDING_SEND_AUTHENTICATE_REQUEST and hci.c:8117 never");
                SummaryText("      sent Read_Remote_Supported_Features at all. Confirm it");
                SummaryText("      in the opcode ring: 0x041B should be ABSENT.");
                SummaryText("      ⇒ Up to v9.7 that query wedged the LMP channel for 22 s");
                SummaryText("      and the link died at 8.5 s of supervision timeout.");
            } else {
                SummaryText("    ⭐⭐ ZERO, on a session where a link DID come up, AND we");
                SummaryText("      still requested security. So Read_Remote_Supported_");
                SummaryText("      Features was accepted (Command Status 0) and NEVER");
                SummaryText("      COMPLETED. hci.c gates Authentication_Requested on that");
                SummaryText("      event, so nothing downstream can run: no auth, no");
                SummaryText("      encryption, no L2CAP response, and the peer times out.");
            }
        }
        SummaryText("    last 16 events, oldest first  (code / len / byte2):");
        for (n = 0; n < kEvtRingLen; n++) {
            unsigned long e = c[kWEvtRing + ((c[kWEvtRingIdx] + n) & (kEvtRingLen - 1))];
            unsigned long code = (e >> 16) & 0xFF;
            Str255 line;
            if (e == 0) continue;           /* slot never written */
            line[0] = 0;
            PStrCat(line, "      0x");
            PStrCatHexN(line, code, 2);
            PStrCat(line, "  len ");
            PStrCatNum(line, (long)((e >> 8) & 0xFF));
            PStrCat(line, "  b2 ");
            PStrCatNum(line, (long)(e & 0xFF));
            PStrCat(line, "  ");
            PStrCat(line, EventName(code));
            Summary(line);
        }
    }

    SummaryText("  M3.1  LINK KEY STORE (RAM only - no file yet)");
    Row("keys stored now",         c[kWLkStored]);
    Row("put_link_key (pairings)", c[kWLkPuts]);
    Row("get_link_key (asked)",    c[kWLkGets]);
    Row("  of those, FOUND a key", c[kWLkHits]);
    Row("delete_link_key",         c[kWLkDeletes]);
    Row("evicted (store full)",    c[kWLkEvicted]);
    Row("flush owed (dirty)",      c[kWLkDirty]);
    if (c[kWLkType] != 0) {
        RowBDAddr("key stored for",    c[kWLkAddrHi], c[kWLkAddrLo]);
        Row("its link key type",       c[kWLkType] & 0xFF);
    }
    Row("security requests made",  c[kWSecReqs]);
    if (c[kWLkHits] > 0)
        SummaryText("      *** RECONNECTED WITHOUT RE-PAIRING: a stored key was used ***");
    else if (c[kWLkGets] > 0)
        SummaryText("      the peer ASKED and we had none: normal on a first pairing");
    else if (c[kWLkPuts] > 0 && c[kWSecReqs] == 0)
        SummaryText("      key stored, but NO security was requested: nothing forced");
    else if (c[kWLkPuts] > 0)
        SummaryText("      key stored, not yet asked for. Reconnect to exercise it.");
    if (c[kWLkEvicted] > 0)
        SummaryText("      !! store was FULL and the oldest key was dropped.");
    SummaryText("      (RAM only: this survives a session, NOT a reboot - M3 6.4)");

    SummaryText("  M3.3/M3.4  PERSISTENCE - task level and the key file");
    Row("defer requests (from IRQ)", c[kWDeferReqs]);
    Row("  responses RUN at task",   c[kWDeferRuns]);
    Row("  NMInstall errors",        c[kWDeferInstallErrs]);
    Row("  coalesced away",          c[kWDeferDropped]);
    Row("key file load attempts",    c[kWKfLoads]);
    Row("  records LOADED",          c[kWKfLoaded]);
    /* ★★★★★★ v13.7: THE ROW THAT MAKES THE ONE ABOVE READABLE. "load attempts 1,
     * records LOADED 0" appeared in three consecutive sessions and could not be acted
     * on, because the file being ABSENT and the file being EMPTY produced identical
     * output -- the open-failure path returned without counting anything. */
    Row("  opens that FAILED",       c[kWKfNoFile]);
    if (c[kWKfLoaded] == 0 && c[kWKfNoFile] > 0) {
        SummaryText("    ⚠⚠ THE FILE COULD NOT BE OPENED. On a first-ever run that is");
        SummaryText("      normal. On any run after one that reported records written,");
        SummaryText("      it means a pairing was saved and then could not be read back");
        SummaryText("      -- which loses the bond on every reboot and leaves the DEVICE");
        SummaryText("      holding a key this Mac does not have. Clearing that needs a");
        SummaryText("      BATTERY PULL on the device; a power cycle cannot, because a");
        SummaryText("      device that believes it is bonded reconnects rather than");
        SummaryText("      re-advertising.");
    } else if (c[kWKfLoaded] == 0 && c[kWKfFlushes] > 0) {
        SummaryText("    ⚠ The file OPENED and held nothing, which is a DIFFERENT fault");
        SummaryText("      from not finding it: the write side is the suspect.");
    }
    Row("key file flushes",          c[kWKfFlushes]);
    Row("  records written",         c[kWKfWritten]);
    Row("file errors",               c[kWKfErr]);
    Row("file REJECTED (bad/dongle)", c[kWKfRejected]);
    if (c[kWKfLoaded] > 0 && c[kWLkPuts] == 0)
        SummaryText("      *** A BOND SURVIVED A REBOOT: keys loaded, nothing paired ***");
    else if (c[kWKfLoaded] > 0)
        SummaryText("      keys loaded from disk (and something also paired this run)");
    if (c[kWDeferRuns] > 0)
        SummaryText("      *** TASK LEVEL REACHED from an interrupt trigger ***");
    else if (c[kWDeferReqs] > 0)
        SummaryText("      !! requested but never RAN: no app event loop serviced it");
    if (c[kWKfRejected] > 0)
        SummaryText("      !! file discarded: bad magic, or bonded through ANOTHER dongle");

    SummaryText("  PIPE STALL RECOVERY - run 23's permanent death, now handled");
    Row("int pipe stalls cleared", c[kWIntStallClears]);
    Row("bulk-IN stalls cleared", c[kWAclStallClears]);
    RowHex("last clear rc",        c[kWStallClearRc], 8);
    RowHex("last int status",      c[kWIntStatusFull], 8);
    RowHex("last bulk-IN status",  c[kWAclStatusFull], 8);
    RowHex("re-arm err (full)",    c[kWIntArmErrFull], 8);
    if (c[kWIntStatusFull] == 0xFFFFE501UL || c[kWAclStatusFull] == 0xFFFFE501UL)
        SummaryText("      0xFFFFE501 = -6911 kUSBNotRespondingErr: stall or hung device");
    if (c[kWIntArmErrFull] == 0xFFFFE4BDUL)
        SummaryText("      0xFFFFE4BD = -6979 kUSBPipeStalledError: needs a CLEAR first");
    if (c[kWIntStallClears] > 0 || c[kWAclStallClears] > 0)
        SummaryText("      a stall WAS cleared: recovery ran. Check events still flow.");

    /* ★★★★★ v17.2: THE TEARDOWN EXPERIMENT, AND IT ANSWERS ITSELF.
     *
     * Two explanations fit "32 stalls and one Finalize" and they need opposite fixes.
     * Time separates them, so print the spread and then APPLY THE RULE rather than
     * leaving three raw numbers for someone to reason about at 1am. */
    /* ★ v17.3: which transport shape ran. Printed BEFORE the timing block, because it
     * is what the timing is now being read against. */
    if (c[kWIntDepth] != 0) {
        Row("interrupt reads kept posted", c[kWIntDepth]);
        if (c[kWIntDepth] >= 2)
            SummaryText("      2 = a v17.3-to-17.5 build. That experiment was REVERTED"
                        " in 17.6: the stalls are a symptom, not a cause.");
        else
            SummaryText("      1 = the single-read flow, which is what both devices run.");
    }
    /* ★★★★★ v17.4: the step 17.3 skipped. BT_DeliverPacket runs BTstack INSIDE the
     * interrupt completion; if an unsolicited event is a much longer trip than a
     * command reply, the posted reads time out while we are in it -- and two posted
     * reads are no help, because both wait on this same handler to return. That is the
     * only candidate left that survives depth 2 being inert. */
    if (c[kWDelivMaxCmdUs] != 0 || c[kWDelivMaxUnsolUs] != 0) {
        SummaryText("  ⭐ TIME INSIDE THE COMPLETION (BT_DeliverPacket), microseconds");
        Row("longest for a command reply", c[kWDelivMaxCmdUs]);
        Row("longest for UNSOLICITED",     c[kWDelivMaxUnsolUs]);
        Row("most recent",                 c[kWDelivLastUs]);
        if (c[kWDelivMaxUnsolUs] >= 4 * (c[kWDelivMaxCmdUs] + 1)) {
            SummaryText("      ⇒ UNSOLICITED EVENTS COST FAR MORE TIME. That is the");
            SummaryText("      candidate: we sit at interrupt level long enough for the");
            SummaryText("      posted read to time out, which is why depth 2 did not");
            SummaryText("      help -- both reads wait on this handler. Get the work");
            SummaryText("      out of the completion and onto the pump.");
        } else {
            SummaryText("      ⇒ THE TWO ARE COMPARABLE, so time in the handler is NOT");
            SummaryText("      what singles out unsolicited events. That theory is dead;");
            SummaryText("      do not carry it forward. The correlation is real, so the");
            SummaryText("      difference is in what the CONTROLLER does around an");
            SummaryText("      unprompted event, not in what we do with it.");
        }
        SummaryText("      ⚠ A maximum, not an average: one slow trip is enough to");
        SummaryText("      time out a read, so the worst case is the measurement.");
    }
    /* ★★★★★ v99.104: the completion stream, oldest first. Three inferred mechanisms
     * have been wrong; this is the evidence they were all inferred FROM, unprocessed. */
    if (c[kWCompRing0] != 0 || c[kWCompRing0 + 1] != 0) {
        short k;
        short empties = 0, data = 0;
        SummaryText("  ⭐ THE LAST 8 INTERRUPT COMPLETIONS, oldest first");
        SummaryText("     (status is the low byte: 00 = ok, 01 = -6911 not responding)");
        for (k = 0; k < 8; k++) {
            unsigned long v = c[kWCompRing0 + ((c[kWCompRingIdx] + k) & 7)];
            unsigned long st = (v >> 24) & 0xFFUL;
            unsigned long ac =  v        & 0xFFFFUL;
            Str255 l; l[0] = 0;
            PStrCat(l, "      status 0x"); PStrCatHexN(l, st, 2);
            PStrCat(l, "  bytes "); PStrCatNum(l, (long)ac);
            if (st == 0 && ac > 0)       { PStrCat(l, "   <- an event arrived"); data++; }
            else if (ac == 0 && st != 0) { PStrCat(l, "   <- EMPTY, the one that costs a clear"); empties++; }
            Summary(l);
        }
        {
            Str255 l; l[0] = 0;
            PStrCat(l, "      in this window: "); PStrCatNum(l, (long)data);
            PStrCat(l, " with data, "); PStrCatNum(l, (long)empties);
            PStrCat(l, " empty");
            Summary(l);
        }
        SummaryText("      ⇒ READ THE ORDER. One empty after each event means the");
        SummaryText("      controller terminates every event with a packet we are");
        SummaryText("      treating as a failure -- the fix is in how we classify it,");
        SummaryText("      not in how many reads are posted. Empties CLUSTERED instead");
        SummaryText("      means something else entirely; say so rather than assuming.");
    }
    SummaryText("  ⭐ WHEN did the stalls happen, and when were we pulled?");
    if (c[kWStallFirstMs] == 0 && c[kWFinalizeMs] == 0) {
        SummaryText("      no stalls and no teardown this session -- nothing to decide.");
    } else {
        Row("first stall at (ms)", c[kWStallFirstMs]);
        Row("last  stall at (ms)", c[kWStallLastMs]);
        Row("Finalize  at (ms)",   c[kWFinalizeMs]);
        if (c[kWStallFirstMs] != 0 && c[kWStallLastMs] >= c[kWStallFirstMs])
            Row("  stalls spanned (ms)", c[kWStallLastMs] - c[kWStallFirstMs]);
        if (c[kWFinalizeMs] != 0 && c[kWStallLastMs] != 0
            && c[kWFinalizeMs] >= c[kWStallLastMs])
            Row("  last stall -> Finalize (ms)", c[kWFinalizeMs] - c[kWStallLastMs]);

        /* ⚠⚠⚠ THIS RULE GAVE A CONFIDENT WRONG ANSWER TWICE, AND THE FAULT WAS HERE.
         * kWStallFirstMs is written ONCE and never rewritten; kWStallLastMs tracks the
         * latest. So in a session with more than one teardown the span runs from the
         * FIRST burst to the LAST and reads as "spread" -- which is exactly how a
         * 93 ms burst repeated three times reported as 35 seconds, and sent a whole
         * build (17.3's double-buffer) after a cause that was really a symptom.
         * ⇒ The span only MEANS anything when there was exactly one teardown. Say so
         * instead of ruling on evidence that cannot support a ruling. */
        if (c[kWFinal] > 1) {
            SummaryText("      ⚠ MORE THAN ONE TEARDOWN this session, so the span above");
            SummaryText("      runs from the FIRST burst to the LAST and CANNOT be read");
            SummaryText("      as spread-vs-burst. Divide: ~32 stalls per teardown has");
            SummaryText("      been the signature in every run, which is the count of");
            SummaryText("      re-arm spins that fit in one removal window -- i.e. the");
            SummaryText("      stalls are a SYMPTOM. For a clean reading, use a session");
            SummaryText("      with exactly one Finalize.");
        } else if (c[kWFinalizeMs] == 0) {
            SummaryText("      stalls but NO teardown this session. On its own that");
            SummaryText("      already weakens 'the stalls cause the removal'.");
        } else if (c[kWStallFirstMs] == 0) {
            SummaryText("      a teardown with NO stalls at all -- the removal is");
            SummaryText("      independent of the stall treadmill. Case B, decisively.");
        } else if ((c[kWStallLastMs] - c[kWStallFirstMs]) < 2000UL) {
            SummaryText("      ⇒ CASE B: the stalls are a BURST (< 2 s) and the device");
            SummaryText("      was being removed underneath them. They are a SYMPTOM.");
            SummaryText("      Double-buffering the interrupt read would change nothing;");
            SummaryText("      find out why the Expert dropped the device.");
        } else {
            SummaryText("      ⇒ CASE A: the stalls are SPREAD over the session, so they");
            SummaryText("      accumulated BEFORE the teardown rather than during it.");
            SummaryText("      Only one interrupt read is ever outstanding, so the");
            SummaryText("      re-arm gap is the candidate: keep two posted.");
        }
        SummaryText("      ⚠ ms values are hal_time_ms (Microseconds-based) and the");
        SummaryText("      first-stall stamp is written once and never rewritten, so");
        SummaryText("      the span crosses driver instances deliberately.");
    }

    SummaryText("  IS ANYONE LISTENING? - run 16's ambiguity, resolved");
    Row("interrupt reads armed",   c[kWIntArmed]);
    Row("interrupt completions",   c[kWIntComp]);
    RowStatus("last re-arm error",  c[kWIntArmErr]);
    Row("UNSOLICITED events",      c[kWUnsolEvents]);
    Row("scan mode requested",     c[kWScanMode]);
    if (c[kWIntArmed] == c[kWIntComp] + 1 && c[kWIntArmErr] == 0) {
        SummaryText("      the reader IS armed and waiting, with no arm error. So");
        SummaryText("      any silence above is the CONTROLLER's, not ours.");
    } else if (c[kWIntArmErr] != 0 && c[kWIntStallClears] > 0
               && c[kWUnsolEvents] > 0) {
        /* ⚠⚠ THIS BRANCH EXISTS BECAUSE THE ONE BELOW WAS LYING.
         *
         * Run 46 printed "a re-arm FAILED: we stopped listening" over a log that also
         * showed 381 interrupt completions, 46 unsolicited events, an inquiry that ran
         * to completion and two panel commands serviced. We plainly had NOT stopped
         * listening. The field is the LAST error seen, not a running failure, and
         * since v3.9 a refused re-arm is cleared and retried -- so a non-zero value
         * with clears recorded and events still arriving is the recovery working,
         * which is the ordinary idle-treadmill case and not a fault at all.
         *
         * Same class of mistake as the "stored in the table (max 4)" label: a verdict
         * that outlived the code it described, stated with more confidence than the
         * evidence on the very same page. */
        SummaryText("      a re-arm was refused and RECOVERED: the stall was cleared");
        SummaryText("      and events kept arriving. Idle-treadmill behaviour, not a");
        SummaryText("      fault. The reader is still listening.");
    } else if (c[kWIntArmErr] != 0) {
        SummaryText("      !! a re-arm FAILED with no clear recorded and no unsolicited");
        SummaryText("      events since: we may have stopped listening. Ours, not the");
        SummaryText("      controller's. This is the cause of any missing event.");
    } else if (c[kWIntArmed] <= c[kWIntComp]) {
        SummaryText("      !! armed no more than completed: no read is outstanding,");
        SummaryText("      so nothing can arrive. Ours, not the controller's.");
    }
    if (c[kWUnsolEvents] == 0)
        SummaryText("      0 unsolicited: every event so far was a reply to a command");
    else
        SummaryText("      >0 unsolicited: the transport CAN deliver unprompted events");

    /* ★★ M6: DID THE CONTROL PANEL REACH THE DRIVER?
     *
     * The panel and the driver are an application and an 'ndrv' and cannot call each
     * other, so they trade a sequence number through the shared block: the panel
     * writes arguments first and kWCmdSeq LAST, and the driver copies seq into ack
     * LAST after servicing. That makes every interesting failure legible from here
     * rather than guessable from the UI. docs/SCAN-DESIGN.md §5.
     *
     * ⚠ The failure this section exists to name: BT_StackPoll only runs on a USB
     * completion, so a QUIET RADIO means the command is never serviced. seq > ack is
     * therefore "panel asked, driver never got to it" -- not a panel bug. */
    /* ⚠⚠ THE seq/ack ROWS ARE GONE, and their absence is the finding.
     *
     * They reported a shared-block command channel the panel wrote and BT_StackPoll
     * serviced. Run 47 proved it could not work: BT_StackPoll runs only on a USB
     * completion, and after bring-up the interrupt-IN read blocks, so on an idle radio
     * nothing serviced anything -- seq 9, ack 2, no inquiry. The panel now calls the
     * driver's exported BTScanStart directly (2003 prior art, vendor/bt-control-center)
     * and gets a synchronous return code, so there is no asynchronous handshake left to
     * report. Printing seq/ack now would show 0/0 forever and invite the conclusion
     * that the panel never asked. */
    /* M8: the panel's On/Off switch, which drives page scan and inquiry scan. */
    Row("radio on (1/0)", c[kWRadioOn]);
    if (c[kWRadioOn] == 0) {
        SummaryText("      OFF: not discoverable and not connectable. Nothing can find");
        SummaryText("      or page this Mac, so an absent peer proves nothing.");
    }

    SummaryText("  M6b SCAN - the panel calls the driver DIRECTLY now");
    Row("inquiry state (0/1/2)",    c[kWScanState]);
    Row("responders published",     c[kWScanCount]);

    /* ★★★ THE PUMP TIMER AND THE MAILBOX. ⚠ THIS BLOCK EXISTS BECAUSE ITS ABSENCE
     * WASTED A RUN. v5.5 put these counters in the driver and in the panel's status
     * line but NOT here -- and BTCheck's log is the one artifact copied off the machine
     * every single time. The panel's status file was not copied that run, so the only
     * reading available was "35 run-loop iterations, therefore the timer never ran",
     * inferred rather than reported. */
    SummaryText("  THE PUMP TIMER - does the mailbox get serviced at all");
    Row("timer firings",            c[kWTimerRuns]);
    Row("mailbox cmds serviced",    c[kWMbxServiced]);
    Row("servicer found nothing",   c[kWMbxStale]);
    Row("mailbox seq",              c[kWCmdSeq]);
    Row("mailbox ack",              c[kWCmdAck]);
    /* ★★★★ THE LAST COMMAND AND WHAT THE DRIVER MADE OF IT.
     *
     * ⚠⚠ THESE TWO ROWS WOULD HAVE SAVED THREE HARDWARE RUNS. kCmdDelStoredKey had no
     * `case` in the driver's switch, so every card-side delete fell to `default:` and
     * the driver wrote 0x100 | 0xFF -- "unknown command" -- into kWCmdResult. That word
     * was published in the block the whole time and simply never printed, so the run
     * showed a mailbox that sent and acknowledged commands perfectly while nothing
     * happened, and the failure looked like the CARD refusing a delete. */
    if (c[kWCmd] != 0 || c[kWCmdSeq] != 0) {
        Row("  last command",       c[kWCmd]);
        if ((c[kWCmdResult] & 0x100UL) == 0) {
            SummaryText("      result                 NOT ANSWERED");
        } else if ((c[kWCmdResult] & 0xFFUL) == 0xFFUL) {
            SummaryText("    ⚠⚠ UNKNOWN COMMAND. The driver has no case for the number");
            SummaryText("      above, so it did NOTHING and said so. The panel and the");
            SummaryText("      driver disagree about the command set -- check that both");
            SummaryText("      binaries are the versions you think they are, and that");
            SummaryText("      the driver's switch has a case for that number.");
        } else {
            RowStatus("      result",      c[kWCmdResult]);
        }
    }
    /* ⚠ BRACED. My unbraced version printed the second line unconditionally, so every
     * healthy run would have claimed requests were not being serviced. GCC's
     * misleading-indentation warning caught it; a diagnostic that cries fault on a
     * good run is worse than no diagnostic. */
    if (c[kWCmdSeq] != c[kWCmdAck]) {
        SummaryText("    ⚠ seq AHEAD of ack: requests are NOT being serviced. This is");
        SummaryText("      run 47's failure mode, and it means scanning does nothing.");
    }
    /* The two attempt sites, reported separately. 0 means NEVER ATTEMPTED, which is a
     * different fact from "attempted and returned 0" -- hence the 0x100 flag. */
    if (c[kWTimerRcTask] == 0)
        SummaryText("    task-level start:  NOT ATTEMPTED");
    else
        Row("task-level start rc",  c[kWTimerRcTask] & 0xFF);
    if (c[kWTimerRcIrq] == 0)
        SummaryText("    ConfigDone start:  not attempted (task level had already taken)");
    else
        Row("ConfigDone start rc",  c[kWTimerRcIrq] & 0xFF);
    Row("handler bail mask",        c[kWTimerBail]);
    SummaryText("    bail mask bits: 1 not-live  2 device-gone  4 no-device  8 no-block.");
        SummaryText("    A mask of 0 with 0 firings means the handler NEVER RAN at all.");
    SummaryText("      The panel resolves BTScanStart with FindSymbol and calls it;");
    SummaryText("      the driver hops to secondary interrupt level so the work is");
    SummaryText("      serialized against USB completions. Failures surface as a");
    SummaryText("      return code IN THE PANEL, not as counters here.");
    if (c[kWScanState] == 1)
        SummaryText("      state 1 = inquiry still running at snapshot time.");
    else if (c[kWScanState] == 2)
        SummaryText("      state 2 = inquiry completed.");
    if (c[kWScanCount] > 0) {
        /* Print the table itself. The addresses are the whole point: they are what
         * proves the panel's list is real hardware and not scaffolding. */
        unsigned long n = c[kWScanCount], i;
        if (n > 16) n = 16;
        for (i = 0; i < n; i++) {
            unsigned long *e = &c[kWScanBase + i * 4];
            Str255 line;
            line[0] = 0;
            PStrCat(line, "      #");
            PStrCatNum(line, (long)i);
            PStrCat(line, "  ");
            /* Same byte order as RowBDAddr, which is the order BT_ScanPublish packs. */
            PStrCatHexN(line, (e[0] >> 16) & 0xFF, 2); PStrCatCh(line, ':');
            PStrCatHexN(line, (e[0] >>  8) & 0xFF, 2); PStrCatCh(line, ':');
            PStrCatHexN(line,  e[0]        & 0xFF, 2); PStrCatCh(line, ':');
            PStrCatHexN(line, (e[1] >> 16) & 0xFF, 2); PStrCatCh(line, ':');
            PStrCatHexN(line, (e[1] >>  8) & 0xFF, 2); PStrCatCh(line, ':');
            PStrCatHexN(line,  e[1]        & 0xFF, 2);
            PStrCat(line, "  class ");
            PStrCatHexN(line, e[2], 6);
            PStrCat(line, "  parm ");
            PStrCatHexN(line, e[3], 6);
            Summary(line);
        }
    }

    /* ★ M7: WHICH DEVICES ARE BONDED, by address rather than by count.
     * Run 49: the user's phone paired with the dongle and never appeared in the panel,
     * because a paired phone stops advertising and answers no inquiry (5 responses, 0
     * phones). A bond is now visible without a scan. */
    SummaryText("  M7 BONDED DEVICES - link keys we hold, by address");
    Row("keys published", c[kWKeyCount]);
    if (c[kWKeyCount] == 0) {
        SummaryText("      no bonds stored. Nothing has paired, or the key file was");
        SummaryText("      rejected -- see the M3.3/M3.4 rows above.");
    } else {
        unsigned long n = c[kWKeyCount], i;
        unsigned long mask = c[kWKeyHandedMask];
        if (n > 8) n = 8;
        for (i = 0; i < n; i++)
            RowBDAddr((mask & (1UL << i)) ? "  USED by controller" : "  key on file only",
                      c[kWKeyBase + i * 2], c[kWKeyBase + i * 2 + 1]);
        /* ⚠⚠ THE DISTINCTION v50 DID NOT MAKE, and it mattered.
         *
         * v50 printed every stored key as "bonded" and the panel showed "Paired".
         * Run 50 then reported keys stored 3, put_link_key 0, records LOADED 3 -- every
         * key had come off disk from an earlier session, and one was for a phone whose
         * own UI said pairing had FAILED. A bond is mutual; holding a key says nothing
         * about whether the peer still honours it. */
        SummaryText("      'USED by controller' = the controller asked for that key BY");
        SummaryText("      ADDRESS this session, which only happens when the peer");
        SummaryText("      initiated an authenticated link. That is real evidence.");
        SummaryText("      'key on file only' = read from the key file and never");
        SummaryText("      exercised. The peer may have discarded its half long ago;");
        SummaryText("      compare put_link_key above -- 0 means nothing paired here.");
    }

    /* ★★ M4 STEP 1: THE INCOMING HID CONTROL CHANNEL.
     *
     * docs/M4-DESIGN.md §10 step 1, and also the reconnect test §9f left open. Every
     * row separates one cause of "no keyboard connected" from the others: a refused
     * listen, nothing paging us, a page on the wrong PSM, an accept whose channel
     * failed to open, or one that opened and closed again. */
    SummaryText("  M4  HID CONTROL CHANNEL (PSM 0x11) - incoming");
    /* ⚠⚠ THE SECURITY LEVEL FIRST, BECAUSE EVERY ROW BELOW MEANS SOMETHING DIFFERENT
     * DEPENDING ON IT. At LEVEL_2 an incoming connection is held by l2cap.c pending
     * security and is never reported to us at all -- so "incoming connections 0" is
     * NOT evidence that nothing paged us. That misreading is exactly what v6.7's event
     * ring had to correct. */
    if (c[kWHidLevel] == 0 && c[kWHidDataPkts] == 0 && c[kWHidIncoming] == 0
        && c[kWHidListenRc] == 0) {
        SummaryText("    (security level not published -- driver older than v6.8)");
    } else {
        Row("registered at LEVEL", c[kWHidLevel]);
        if (c[kWHidLevel] == 0) {
            SummaryText("      ⚠⚠ LEVEL_0 -- THE DIAGNOSTIC BUILD, NOT A SHIPPABLE ONE.");
            SummaryText("      L2CAP answers an incoming connection immediately instead");
            SummaryText("      of holding it for encryption, which is what breaks the");
            SummaryText("      three-link deadlock v6.7 found. It also means keystrokes");
            SummaryText("      would travel in the clear and any device in range could");
            SummaryText("      claim the channel. Set kHidLevel0Probe back to 0.");
        } else {
            SummaryText("      LEVEL_2, the shippable value. ⚠ An incoming connection is");
            SummaryText("      then held by l2cap.c pending security and NEVER reported");
            SummaryText("      to us, so 'incoming connections 0' below does not mean");
            SummaryText("      nothing paged us -- compare L2CAP events seen in M2b.");
        }
    }
    RowStatus("l2cap_register_service", c[kWHidListenRc]);
    if (c[kWHidListenRc] == 0) {
        SummaryText("      NOT RECEIVED: the driver never registered the service, so");
        SummaryText("      nothing can page us. Check BT_StackStart reached gL2Init 3.");
    } else if ((c[kWHidListenRc] & 0xFFUL) != 0) {
        SummaryText("      !! the LISTEN was refused. Nothing else below can happen.");
    }
    /* ⚠⚠ RELABELLED v16.1. This counts the OLD hand-rolled L2CAP path, which BTstack's
     * hid_host replaced at v9.5 and which no longer runs -- so it reads 0 forever. It
     * used to share the label "incoming connections" with the LIVE counter further down
     * (kWBhIncoming), and the two sat in one log showing 0 and 2. That nearly inverted a
     * conclusion about whether our devices page us, which is the question the whole
     * AirPort coexistence decision rests on. Two rows may not share a name. */
    Row("incoming conns (legacy L2CAP path, dead)",  c[kWHidIncoming]);
    Row("  accepted (HID ctrl)", c[kWHidAccepted]);
    Row("  declined (other psm)",c[kWHidDeclined]);
    RowHex("last incoming psm",  c[kWHidLastPsm], 4);
    Row("control channels OPEN", c[kWHidOpened]);
    Row("  later closed",        c[kWHidClosed]);
    RowStatus("last open status", c[kWHidOpenStatus]);
    RowHex("live control cid",   c[kWHidCtrlCid], 4);
    if (c[kWHidPeerHi] != 0 || c[kWHidPeerLo] != 0)
        RowBDAddr("  paged us", c[kWHidPeerHi], c[kWHidPeerLo]);
    if (c[kWHidIncoming] == 0 && c[kWHidLevel] == 0) {
        /* ⚠ At LEVEL_0 this row IS meaningful, unlike at LEVEL_2. Nothing is holding
         * the connection back, so zero really does mean nothing reached L2CAP. */
        SummaryText("      0 incoming AT LEVEL_0, where nothing is holding it back. So");
        SummaryText("      the request genuinely never reached L2CAP -- which is a");
        SummaryText("      DIFFERENT problem from the deadlock, and the ACL rows plus");
        SummaryText("      the event ring are where to look next.");
    } else if (c[kWHidIncoming] == 0) {
        SummaryText("      0 incoming, but at LEVEL_2 that is NOT evidence: l2cap.c");
        SummaryText("      holds an incoming connection pending security and never");
        SummaryText("      reports it. Check L2CAP events seen in M2b instead.");
    } else if (c[kWHidAccepted] > 0 && c[kWHidOpened] == 0) {
        /* ⚠ THIS TEXT USED TO BLAME LEVEL_2 UNCONDITIONALLY, and printed that over a
         * LEVEL_0 run where it was simply false. Read the level first. */
        SummaryText("      !! ACCEPTED BUT NEVER OPENED. The peer paged us on HID");
        SummaryText("      control and the channel did not come up.");
        if ((c[kWHidOpenStatus] & 0xFFUL) == 0x69) {
            SummaryText("      ⭐ 0x69 = L2CAP RTX TIMEOUT: we ANSWERED and the peer then");
            SummaryText("        said nothing. Check ACL packets sent in M0.3 -- if that");
            SummaryText("        is non-zero our response reached the wire and the");
            SummaryText("    ⚠⚠ THE EXPLANATION THAT USED TO BE HERE HAS BEEN REFUTED");
        SummaryText("    TWICE. It said the peer went silent because an unencrypted");
        SummaryText("    channel is what a device paired under SSP earns. Both halves");
        SummaryText("    are false: SSP is IMPOSSIBLE on this LMP 2 controller, and the");
        SummaryText("    v8.6 run measured ACL read completions 4 -- the peer sent us");
        SummaryText("    four packets while the channel was failing. IT IS NOT SILENT.");
        SummaryText("    ⇒ Read the raw ACL bytes and the link lifetime above instead");
        SummaryText("    of reasoning from an assumption.");
            SummaryText("        level we asked for.");
        } else if (c[kWHidLevel] != 0) {
            SummaryText("      The service asked for LEVEL_2 (encryption), so an");
            SummaryText("      unencryptable peer is refused HERE and that is deliberate.");
        }
    }

    /* ★★★ THE ONE QUESTION THE LEVEL_0 PROBE EXISTS TO ANSWER. Everything above is
     * plumbing; this is whether a keyboard paired under Tiger will speak to a stack
     * we wrote. */
    /* ★★★★★ v8.6: THE REPORT-PROTOCOL EXPERIMENT -- the whole point of this build.
     *
     * Read in order. Each row answers exactly one question, and the FIRST row that
     * disappoints is the answer; everything after it is meaningless. */
    /* ★★★★★ v8.7: HOW LONG THE LINK LIVED, AND WHAT THE PEER SAID.
     *
     * Three separate theories about this link have now been argued and refuted --
     * "the card withholds the features event", "the deadlock kills it", "the peer goes
     * silent because it wants encryption" -- and every one of them was reasoned without
     * these two facts. */
    /* ★★★★★ v8.8: DID WE ENCRYPT THE INBOUND LINK? Read this FIRST -- everything
     * below it is downstream of the answer.
     *
     * v8.7 established what the keyboard wants: it sends one Connection Request for
     * PSM 0x11 and then nothing, for the full 20-second supervision window, because
     * the HID profile has the Host authenticate and encrypt before the control channel
     * opens. This is the one command that supplies it. */
    /* ★★★★★ v9.0: WERE WE EVEN REACHABLE? This sits immediately before the auto-auth
     * readout because it decides whether "nothing paged us" is a fact about the
     * KEYBOARD or a fact about US, and until v9.0 nothing in the block could say.
     *
     * The v8.8 run reported Connection Requests 0 and the only evidence that we were
     * listening was `scan mode requested 3` -- a variable the driver sets when it asks
     * for scanning, not the controller's answer. This is the controller's answer. */
    SummaryText("  ★★★★★ WERE WE EVEN REACHABLE? Scan_Enable, read from the card");
    Row("    reads sent",           c[kWScanEnaSends]);
    Row("    answers back",         c[kWScanEnaReads]);
    RowStatus("    last send rc",   c[kWScanEnaRc]);
    RowHex("    Scan_Enable first", c[kWScanEnaFirst] & 0xFFUL, 2);
    RowHex("    Scan_Enable last",  c[kWScanEnaLast] & 0xFFUL, 2);
    RowStatus("    its HCI status", 0x100UL | ((c[kWScanEnaLast] >> 8) & 0xFFUL));
    if (c[kWScanEnaReads] == 0) {
        SummaryText("    ⚠ NEVER READ IT BACK, so reachability is still an assumption.");
        SummaryText("    If reads sent is also 0 the probe never ran (HCI not WORKING,");
        SummaryText("    a link was already up, or fewer than 50 timer firings); if it");
        SummaryText("    sent and nothing came back, look at the command credit above.");
    } else if (((c[kWScanEnaLast] >> 8) & 0xFFUL) != 0) {
        /* ⚠ A nonzero status means the byte beside it is NOT a Scan_Enable value, so
         * no verdict may be drawn from it. On a 1.2 controller "unknown command" is
         * the plausible answer, and reading it as 0x00 would say "page scan is off"
         * with total confidence about a command the card never honoured. */
        SummaryText("    ⚠ THE READ FAILED -- nonzero HCI status, so the byte above is");
        SummaryText("    not a Scan_Enable value and says NOTHING about reachability.");
        SummaryText("    Status 0x01 means this controller does not support OCF 0x19,");
        SummaryText("    in which case reachability needs a different instrument.");
    } else {
        /* bit 1 = page scan, bit 0 = inquiry scan. Page scan is the one a bonded
         * keyboard needs; inquiry scan only matters for being FOUND by a new host. */
        unsigned long f = c[kWScanEnaFirst] & 0xFFUL;
        unsigned long l = c[kWScanEnaLast]  & 0xFFUL;
        if ((l & 0x02UL) == 0 && (f & 0x02UL) != 0) {
            SummaryText("    ⚠⚠ PAGE SCAN WAS ON AND IS NOW OFF. Something cleared it");
            SummaryText("    mid-run, so a keyboard that tried late could not reach us.");
            SummaryText("    That is OUR bug -- find what sent Write_Scan_Enable last.");
        } else if ((l & 0x02UL) == 0) {
            SummaryText("    ⚠⚠ PAGE SCAN IS OFF. We were NOT reachable, so a bonded");
            SummaryText("    keyboard could not connect however hard it tried, and");
            SummaryText("    Connection Requests 0 says nothing about the keyboard.");
            SummaryText("    ⇒ This is the whole answer. Fix this before anything else.");
        } else {
            SummaryText("    ⭐ PAGE SCAN IS ON, so we were reachable. If nothing paged");
            SummaryText("    us, that is a fact about the KEYBOARD, not about us -- it");
            SummaryText("    was asleep, out of range, or still in its reconnect");
            SummaryText("    backoff. Cold power-cycle it and rerun.");
        }
    }

    SummaryText("  ★★★★★ DID WE ENCRYPT THE INBOUND LINK? (v8.8)");
    Row("    auto-auth gate",        c[kWAutoAuthGate]);
    Row("    inbound links authed",  c[kWAutoAuthTried]);
    RowStatus("    its send rc",     c[kWAutoAuthRc]);
    RowHex("    for handle",         c[kWAutoAuthHandle], 4);
    Row("    Auth Completes",        c[kWAuthComps]);
    Row("    Encryption Change",     c[kWEncryptChange] & 0xFF);
    Row("    encryption enabled",    c[kWEncryptOn]);
    /* ⚠⚠ THE GATE IS TESTED FIRST, and it has to be. kHidAutoAuth is 0 in the
     * SHIPPING build, so with the gate off "authed 0" is the CORRECT and expected
     * state -- and the first draft of this readout answered it with "that is OUR
     * bug", which is the fourth time a readout here would have called the shipping
     * state a failure. Read the gate before drawing any conclusion from the rows. */
    if (c[kWAutoAuthGate] == 0) {
        SummaryText("    AUTO-AUTH IS OFF IN THIS BUILD (kHidAutoAuth 0), which is the");
        SummaryText("    shipping state and NOT a fault. The rows above are 0 because");
        SummaryText("    nothing was asked to run, so this section says nothing about");
        SummaryText("    the keyboard either way. Only a build reporting gate 1 can.");
    } else if (c[kWAutoAuthTried] == 0) {
        if (c[kWConnReqs] == 0) {
            SummaryText("    ⚠ NOTHING PAGED US, so there was no inbound link to");
            SummaryText("    authenticate. NOT a result -- wake the keyboard and rerun.");
        } else if (c[kWArmedFired] != 0) {
            /* ⚠ The Pair arm and this share one `else if`, so the arm firing means
             * auto-auth deliberately did NOT -- the link was authenticated once, by
             * the other path. Without this branch the readout accuses us of a bug for
             * doing exactly the right thing, which is the same defect as reporting the
             * gate-off state as a failure. Read the arm's own rc, not these rows. */
            SummaryText("    THE PAIR ARM AUTHENTICATED THIS LINK INSTEAD, so auto-auth");
            SummaryText("    correctly stood down -- the two share one branch and the");
            SummaryText("    link must not be authenticated twice. Read arm fired on");
            SummaryText("    inbound below; encryption and the channel still apply.");
        } else {
            SummaryText("    ⚠⚠ SOMETHING PAGED US AND WE DID NOT AUTHENTICATE IT. The");
            SummaryText("    address test failed: the connected address matched none of");
            SummaryText("    the addresses that paged us. That is OUR bug, not the");
            SummaryText("    keyboard's -- read PAGED BY against the connected address.");
        }
    } else if ((c[kWAutoAuthRc] & 0xFFUL) != 0) {
        SummaryText("    ⚠ THE COMMAND WAS REFUSED, so the link was never authenticated");
        SummaryText("    and nothing below says anything about the keyboard.");
    } else if (c[kWAuthComps] == 0) {
        SummaryText("    ⚠ SENT, AND NO AUTHENTICATION COMPLETE CAME BACK. Either the");
        SummaryText("    controller never answered or the link died first -- compare the");
        SummaryText("    lifetime below against when this went out.");
    } else if (c[kWEncryptOn] == 0) {
        SummaryText("    ⚠ AUTHENTICATED BUT NOT ENCRYPTED. BTstack sets");
        SummaryText("    BONDING_SEND_ENCRYPTION_REQUEST itself on Auth Complete status");
        SummaryText("    0 (hci.c:5064), so no encryption means the auth status was");
        SummaryText("    NONZERO -- read Auth Complete (LEGACY) in M2c for the reason.");
    } else {
        SummaryText("    ⭐⭐⭐ AUTHENTICATED AND ENCRYPTED. That is exactly what the");
        SummaryText("    keyboard was waiting for, so the control channel should now");
        SummaryText("    OPEN -- and SET_PROTOCOL(Report) goes out the moment it does.");
        SummaryText("    ⇒ Read the REPORT PROTOCOL section below: this run finally");
        SummaryText("    reaches the question it was built to ask.");
    }

    /* ★★★★★ v9.2: EVERY Authentication Complete, with its handle and its time.
     *
     * ⚠⚠ THIS SECTION EXISTS BECAUSE ITS ABSENCE MADE A RUN UNREADABLE. `Auth
     * Completes 2` beside a single last-value-wins status row cannot say whether the
     * FIRST one succeeded, and those are completely different worlds: status 0 first
     * would mean authentication WORKS and encryption is lost downstream, while 0x02
     * on every one means it never ran at all. I proposed this ring, built the
     * outbound ACL capture instead, and then could not read my own log.
     *
     * The handle and time are here for the same reason: 0x02 is Unknown Connection
     * Identifier, so which handle it was asked about, and whether that link was still
     * alive at that moment, IS the question. Compare each ms against link up/down. */
    /* ★★★★★ v9.3: was the verdict UNMASKED this run? Read before the ring below.
     *
     * ⚠⚠ Without this, 0x02 after link-down is ambiguous and always was. The LMP
     * response timeout is 30 s and link supervision is 20 s, so a keyboard that never
     * answers the challenge can NEVER report itself -- the link dies first and the
     * pending authentication closes with 0x02. Stretching supervision to 40 s lets
     * 0x22 surface, which is the difference between blaming the controller and
     * blaming the keyboard. */
    /* ★★★★★★ v9.4: DID THE CONTROLLER PERMIT SNIFF? Read this FIRST of the security
     * sections -- it is the run's whole question.
     *
     * v9.3's verdict was that the peer does not answer LMP (RRSF status 0x08,
     * Connection Timeout, three times). The prior-art read found a mechanism: BTstack
     * never assigns default_link_policy_settings and never sends
     * Write_Default_Link_Policy, so every link ran on the controller's own default,
     * which this project had never measured. A HID keyboard enters SNIFF immediately;
     * if policy forbids it the controller must refuse LMP_sniff_req, and transactions
     * queued behind that refusal time out.
     *
     * ⚠⚠ The READ is published before the WRITE takes effect, on purpose. "We never
     * set it" is not "it was 0" -- that is the Scan_Enable lesson, where page scan was
     * assumed for six versions until a read-back settled it. If the read says sniff
     * was already permitted, this hypothesis is dead and no second boot is needed. */
    /* ★★★★★★ v9.5: BTstack'S OWN HID HOST IS DRIVING. Read this before every other
     * HID section -- when the gate is on, the sections below describe OUR listener,
     * which is not registered at all, so their zeros mean "not used" not "failed".
     *
     * hid_host.c has been linked into every build this project shipped and never
     * called. Eleven versions re-derived its decisions one command at a time. This is
     * the run where the canonical implementation drives instead, and where
     * HID_PROTOCOL_MODE_REPORT -- the Consumer-page unlock -- is a single argument to
     * hid_host_accept_connection rather than a hand-rolled SET_PROTOCOL. */
    /* ★★★★★★ v9.7: DID THE CONTROLLER EVER ACKNOWLEDGE OUR ACL SENDS? Read this
     * before every protocol section below it. Tiger's trace shows one
     * Number_Of_Completed_Packets within ~16 ms of EVERY ACL send on this exact card.
     * If we get none, our packets never leave the controller, the peer never hears our
     * Connection Response, and nothing above the ACL layer can possibly work -- which
     * is the simplest explanation for eight builds of security work moving nothing. */
    SummaryText("  ★★★★★★ DID OUR ACL SENDS GET ACKNOWLEDGED? (v9.7)");
    Row("    ACL packets sent",     c[kWAclSent]);
    Row("    ⭐⭐ 0x13 events",       c[kWNumCompEvts]);
    Row("    packets acknowledged", c[kWNumCompTotal]);
    RowHex("    for handle",        c[kWNumCompHandle], 4);
    Row("    last one at (ms)",     c[kWNumCompMs]);
    RowStatus("    Read_Buffer_Size", (c[kWBufSizeRc] >> 8) & 0xFFUL ?
                  (0x100UL | ((c[kWBufSizeRc] >> 8) & 0xFFUL)) :
                  (c[kWBufSizeRc] ? 0x100UL : 0UL));
    Row("    controller ACL len",   c[kWAclBufLen]);
    Row("    controller ACL bufs",  c[kWAclBufNum]);
    if (c[kWAclSent] == 0) {
        SummaryText("    NO ACL WAS SENT, so there is nothing to acknowledge. Not a");
        SummaryText("    result -- no link got far enough to answer the peer.");
    } else if (c[kWNumCompEvts] == 0) {
        SummaryText("    ⚠⚠⚠ SENT AND NEVER ACKNOWLEDGED. The controller took our ACL");
        SummaryText("    packets over USB and never reported transmitting one. TIGER");
        SummaryText("    GETS ONE PER SEND ON THIS SAME CARD, so the acknowledgement");
        SummaryText("    path works on the hardware and the fault is OURS.");
        SummaryText("    ⇒ THE PEER NEVER HEARS US. Every silence we have diagnosed");
        SummaryText("    above the ACL layer is downstream of this. Read the pipe");
        SummaryText("    descriptors below before anything else.");
    } else if (c[kWNumCompTotal] < c[kWAclSent]) {
        SummaryText("    ⚠ SOME acknowledged, not all. Partial transmission, so the");
        SummaryText("    path works and something is dropping SPECIFIC packets --");
        SummaryText("    compare the count against WHAT WE SENT BACK below.");
    } else {
        SummaryText("    ⭐ EVERY ACL PACKET WAS ACKNOWLEDGED. The transport is doing");
        SummaryText("    its job, so a silent peer is a fact about the KEYBOARD and");
        SummaryText("    the Tiger sequence is the thing to replicate next.");
    }
    SummaryText("    ⚠ THE PIPES WE OPENED, never checked until now. Fields are");
    SummaryText("    classType/subclass/protocol/other as the Expert returned them;");
    SummaryText("    a Bluetooth device is conventionally 0x81 int-IN, 0x02 bulk-OUT,");
    SummaryText("    0x82 bulk-IN, so a wrong pipe should stand out.");
    RowHex("      interrupt-IN", c[kWPipeInt], 8);
    RowHex("      bulk-OUT",     c[kWPipeOut], 8);
    RowHex("      bulk-IN",      c[kWPipeInB], 8);
    SummaryText("    ⚠ THOSE FOUR FIELDS MEASURED NOTHING. They are our own inputs plus");
    SummaryText("    leftovers from the device match: bulk-OUT and bulk-IN came back");
    SummaryText("    BYTE-IDENTICAL in v9.7, which is equally consistent with 'the");
    SummaryText("    fields just echo us' and with 'both finds returned the SAME PIPE'.");
    SummaryText("    The refs below are the disambiguator, and they are what should");
    SummaryText("    have been published first.");

    /* ★★★★★★ M5 STEP 2: DID A KEYSTROKE REACH MAC OS? The whole point of the driver. */
    /* ★★★ v11.2: DID THE DRIVER CLEAR THE SWITCHER'S MARKER? This decides whether the
     * NEXT boot claims the card or falls back, and v11.1 shipped unable to answer it. */
    SummaryText("  ★★★★★ THE SELF-HEALING MARKER, FROM OUR SIDE (v11.2)");
    Row   ("      clear attempts",      c[kWMarkRuns]);
    Row   ("      ⭐ marker cleared",   c[kWMarkCleared]);
    if (c[kWMarkErr] != 0) RowHex("      ⚠ File Manager error", c[kWMarkErr] & 0xFFUL, 4);
    if (c[kWMarkCleared] != 0) {
        SummaryText("    ⭐ CLEARED. We told the switcher we came up, so the NEXT boot will");
        SummaryText("      claim the card again. This is the healthy steady state.");
    } else if (c[kWMarkRuns] == 0) {
        SummaryText("    ⚠⚠ NEVER EVEN TRIED. The task-level hop that clears it never ran,");
        SummaryText("      so the marker is still there and the NEXT BOOT WILL DECLINE --");
        SummaryText("      the card goes back to Mac OS and the keyboard works, but our");
        SummaryText("      stack is out until the boot after. Same trampoline that acquires");
        SummaryText("      KCHR, so if KCHR arrived the hop itself is fine.");
    } else {
        SummaryText("    ⚠ TRIED AND FAILED. Read the error above; the marker survives, so");
        SummaryText("      the next boot declines and then retries the one after.");
    }

    /* ★★★★★★ M6: DID A MEDIA KEY CHANGE THE VOLUME? */
    SummaryText("  ★★★★★★ MEDIA KEYS (M6) - DID A MEDIA KEY DO ANYTHING?");
    Row   ("      rising edges seen",   c[kWMedPresses]);
    Row   ("      task-level services", c[kWMedRuns]);
    Row   ("      volume UP",           c[kWMedVolUp]);
    Row   ("      volume DOWN",         c[kWMedVolDn]);
    Row   ("      MUTE toggles",        c[kWMedMute]);
    Row   ("      EJECT presses",       c[kWMedEject]);
    if (c[kWMedDropped] != 0)
        Row("      ⚠ presses queued behind another", c[kWMedDropped]);
    if (c[kWMedPresses] == 0) {
        SummaryText("    ⚠ NO MEDIA KEY WAS PRESSED, or none produced a RISING EDGE.");
        SummaryText("      This counts 0->1 transitions, not set bits, because a held key");
        SummaryText("      repeats the same byte 8 and would otherwise fire dozens of");
        SummaryText("      times per tap. Press volume up once and run again.");
    } else if (c[kWMedRuns] == 0) {
        SummaryText("    ⚠⚠ EDGES SEEN AND NEVER SERVICED. The press was recorded at");
        SummaryText("      interrupt level and the task-level hop never ran -- the volume");
        SummaryText("      change lives there because the Sound Manager is not documented");
        SummaryText("      interrupt-safe. Check the defer counters; this is the same");
        SummaryText("      trampoline that acquires KCHR, so if KCHR arrived it works.");
    } else {
        Row      ("      ⭐ sdev device found", c[kWMedDevFound]);
        RowStatus("      ⭐⭐ HARDWARE write err", c[kWMedHwSetErr]);
        if ((c[kWMedHwReadBack] & 0x1000000UL) != 0)
            RowHex("      hvol read straight back", c[kWMedHwReadBack] & 0xFFFFFFUL, 6);

        /* ⭐⭐⭐ THE v11.6 DISCRIMINATOR. Read this before anything else in this section.
         *
         * Standing puzzle: the driver reads garbage (0xE16A9C) back from siHardwareVolume
         * while THIS APP reads 01000100 from the same selector on the same machine. Four
         * cheaper explanations were checked and are dead -- same selector constant
         * ('hvol', Sound.h:344), same &(long) argument, same FindNextComponent lookup,
         * and err 0 on both calls. Two possibilities remain and they need DIFFERENT
         * fixes, so the run must not leave it a judgement call. */
        /* ★★★★★★ v11.7: APPLE'S OWN ROUTE. Read this FIRST -- when it is live, every
         * Sound Manager row below is a fallback that did not run. */
        if (c[kWMedTrapOk] != 0) {
            SummaryText("    ★★★★★★ APPLE'S VOLUME TRAP (0xABEC) IS PRESENT AND IN USE.");
            Row      ("      taps issued",       c[kWMedTrapCalls]);
            /* ⚠⚠ PASS RowStatus THE MARKED WORD, NEVER A PRE-MASKED ONE. It does its
             * own & 0xFF and prints "NOT RECEIVED" for a zero, so masking first turns a
             * successful 0x100 into "NOT RECEIVED" -- which is how the v11.7 run read as
             * "the trap never answered" when it had in fact succeeded 20 times. Fourth
             * inverted-verdict readout on this project; the rule is now written down
             * where the mistake was made. [[feedback_test_content_not_return_codes]] */
            RowStatus("      last trap result", c[kWMedTrapRc]);
            if (c[kWMedTrapCalls] == 0) {
                SummaryText("    ⚠ PRESENT BUT NEVER CALLED. The gate opened and no media");
                SummaryText("      key reached it -- look at the rising-edge rows, not here.");
            } else {
                SummaryText("      This is exactly what Apple's own USBKeyboardSupport does");
                SummaryText("      for DoSoundUpButton: GetToolboxTrapAddress(0xABEC) then");
                SummaryText("      CallUniversalProc with {7|6, key, 0}. One trap does BOTH");
                SummaryText("      the level change and the on-screen feedback, so if the");
                SummaryText("      volume still did not move, the block values are wrong --");
                SummaryText("      NOT the context, and NOT the Sound Manager.");
            }
        } else {
            SummaryText("    ⚠⚠ TRAP 0xABEC IS NOT AVAILABLE on this machine, so the driver");
            SummaryText("      fell back to the Sound Manager rows below -- which are");
            SummaryText("      MEASURED INERT here. Expect no volume change.");
        }
        /* ---- v11.9: EJECT, and the one row that matters is the BALANCE ----------
         * Eject is the only held key: press, then release 60 ticks later, because
         * Apple synthesises a one-second hold and the system ignores anything
         * shorter. An unmatched press is a key left DOWN, which for eject means the
         * tray cycling rather than a wandering volume -- so these two numbers must
         * be equal once your finger is off the key. */
        if (c[kWEjectPress] != 0 || c[kWEjectRelease] != 0) {
            Row("      eject holds started", c[kWEjectPress]);
            Row("      eject holds ended",   c[kWEjectRelease]);
            if (c[kWLkArchived] != 0 || c[kWDiscReqs] != 0) {
            SummaryText("  ---- DELETE'S OTHER TWO HALVES (v12.3)");
            Row("      keys archived by a delete", c[kWLkArchived]);
            Row("      links dropped by a delete", c[kWDiscReqs]);
            if (c[kWDiscReqs] != 0) RowStatus("      last disconnect", c[kWDiscRc]);
            Row("      pager records forgotten", c[kWPagedForgot]);
        }
        if (c[kWBlockActive] != 0 || c[kWBlockDrops] != 0) {
            SummaryText("  ---- \"STAY DISCONNECTED\" (v12.8)");
            if (c[kWBlockActive] != 0) {
                RowBDAddr("      refusing", c[kWBlockHi], c[kWBlockLo]);
                SummaryText("    ⚠ THIS DEVICE IS BEING REFUSED ON PURPOSE. It shows as");
                SummaryText("      Disconnected in the panel and will not reconnect until");
                SummaryText("      Connect is clicked or the Mac restarts. If someone is");
                SummaryText("      reporting a dead keyboard, THIS IS THE FIRST THING TO");
                SummaryText("      CHECK -- a blocked device is indistinguishable from a");
                SummaryText("      broken one from the outside.");
            }
            Row("      reconnections refused",  c[kWBlockDrops]);
            SummaryText("      Deleting a key NEVER disconnects anything by itself --");
            SummaryText("      a bond is consulted when a link is MADE. That is why the");
            SummaryText("      keyboard kept typing after every delete and made a");
            SummaryText("      working delete look like a broken one.");
        }
        if ((c[kWEjectWhich] & 0x100UL) != 0)
                RowHex("      eject code sent", c[kWEjectWhich] & 0xFFUL, 2);
            if (c[kWEjectPress] == c[kWEjectRelease]) {
                SummaryText("    ⭐ BALANCED -- every hold ended. No key is left down.");
                SummaryText("      If the disc did NOT eject, the mechanism is right and");
                SummaryText("      the CODE is wrong: 0x44 is the sibling of 0x42 in");
                SummaryText("      Apple's DoEjectButton and is the one flip to try.");
            } else {
                SummaryText("    ⚠⚠ UNBALANCED -- a hold was started and never ended, so");
                SummaryText("      the eject key is logically STUCK DOWN and the system");
                SummaryText("      will repeat it. The deferred release did not run:");
                SummaryText("      suspect the task-level hop, not the trap.");
            }
        } else if (c[kWMedEject] != 0) {
            SummaryText("      eject seen but no hold started -- the trap was absent.");
        }
        RowHex("      driver's 'sdev' Component", c[kWMedDevId], 8);
        RowHex("      this app's  'sdev' Component", gAppSndDevId, 8);
        if (c[kWMedDevId] == 0 || gAppSndDevId == 0) {
            SummaryText("      one side found no component -- the comparison says nothing.");
        } else if (c[kWMedDevId] == gAppSndDevId) {
            SummaryText("    ⭐⭐ SAME COMPONENT, DIFFERENT ANSWER. The driver is holding the");
            SummaryText("      very token this app used successfully, and still reads back");
            SummaryText("      garbage. ⇒ THE LOOKUP IS NOT THE BUG and no amount of work");
            SummaryText("      inside this driver will fix it: the Component Manager will");
            SummaryText("      not dispatch usefully from a driver fragment. That matches");
            SummaryText("      Apple's own handler importing NO sound call of any kind --");
            SummaryText("      44 imports, no SoundLib -- and reaching an APPLICATION");
            SummaryText("      instead. This is a decision about context, not a defect.");
        } else {
            SummaryText("    ⭐⭐ DIFFERENT COMPONENTS. The driver got a token this app did");
            SummaryText("      not, from an identical lookup, so the fault IS in the driver");
            SummaryText("      and IS fixable here -- the garbage read-back is simply a");
            SummaryText("      wrong component answering. Fix the lookup before spending");
            SummaryText("      anything on the context question.");
        }
        if (c[kWMedDevFound] == 0) {
            SummaryText("    ⚠⚠ NO 'sdev' OUTPUT DEVICE FOUND, so the hardware write never");
            SummaryText("      happened. BTCheck measured exactly one on this machine, so");
            SummaryText("      this means FindNextComponent failed from driver context --");
            SummaryText("      a different problem from the volume not moving.");
        } else if ((c[kWMedHwSetErr] & 0xFFUL) != 0) {
            SummaryText("    ⚠ THE HARDWARE WRITE WAS REFUSED. $8002 here is what the Mac");
            SummaryText("      Mini returns for every hardware selector -- if it appears on");
            SummaryText("      THIS machine too, both documented write routes are closed.");
        }
        RowStatus("      last Get error", c[kWMedGetErr]);
        RowStatus("      last Set error", c[kWMedSetErr]);
        if ((c[kWMedLastVol] & 0x1000000UL) != 0)
            RowHex("      ⭐ last volume WRITTEN", c[kWMedLastVol] & 0xFFFFFFUL, 6);
        Row   ("      muted by us now",  c[kWMedMuted]);
        if ((c[kWMedSetErr] & 0xFFUL) == 0 && (c[kWMedGetErr] & 0xFFUL) == 0) {
            SummaryText("    ⭐⭐⭐⭐⭐ A MEDIA KEY ON THE A1016 DROVE THE SYSTEM VOLUME.");
            SummaryText("      The documented API is MEASURED to move real hardware on this");
            SummaryText("      machine (three beeps audibly rising, 2026-09-15), so a clean");
            SummaryText("      write here means the whole path works: radio, L2CAP, report");
            SummaryText("      protocol, byte 8, rising edge, task-level hop, Sound Manager.");
            SummaryText("      ⚠ The EAR is still the final judge -- did the volume audibly");
            SummaryText("      change when you pressed the key?");
        } else {
            SummaryText("    ⚠ The path ran and the Sound Manager refused. Read the two");
            SummaryText("      status codes: $8001/$8002 are what the Mac Mini G4 returns,");
            SummaryText("      and would mean this machine behaves like that one after all.");
        }
    }

    SummaryText("  ★★★★★★ KEY INJECTION (M5 step 2) - DID A KEY REACH MAC OS?");
    Row   ("      KCHR acquired",        c[kWInjKchrOk]);
    if (c[kWInjKchrOk] == 0) {
        SummaryText("    ⚠⚠ NO KCHR, SO NOTHING COULD BE TYPED AT ALL. GetResource runs");
        SummaryText("      once in ValidateHW at TASK level -- the only task-level window");
        SummaryText("      this driver has, since every other entry arrives on a USB");
        SummaryText("      completion. Read the error beside it.");
        RowStatus("      its ResError",  c[kWInjKchrErr]);
        Row   ("      init calls",       c[kWInjInitRuns]);
    } else {
        Row("      decoded reports diffed", c[kWInjCalls]);
        Row("      ⭐ press/release events", c[kWInjEventsSeen]);
        Row("      handed to the injector",  c[kWInjEvents]);
        Row("      ⭐⭐ PostEvent calls",     c[kWInjPosted]);
        if (c[kWInjPosted] != 0) {
            SummaryText("    ⭐⭐⭐⭐⭐ KEYSTROKES WERE POSTED TO MAC OS BY OUR OWN STACK.");
            SummaryText("      Not the card's HID proxy -- our transport, our decode, our");
            SummaryText("      keycode table, our event. If characters also APPEARED on");
            SummaryText("      screen then M5 is done and the card can stay ours full time.");
            SummaryText("      ⚠ If they did NOT appear, the posting is right and the");
            SummaryText("      TRANSLATION is not: check the last usage and virtual key.");
        } else if (c[kWInjEvents] != 0) {
            SummaryText("    ⚠ EVENTS REACHED THE INJECTOR AND NONE WAS POSTED. The rows");
            SummaryText("      below say which gate stopped them; exactly one should be");
            SummaryText("      nonzero and it names the cause.");
        } else if (c[kWInjCalls] != 0) {
            SummaryText("    ⚠ REPORTS WERE DIFFED AND PRODUCED NO EVENTS. That is CORRECT");
            SummaryText("      for a session where no key was pressed -- an idle report");
            SummaryText("      differs from the previous one in nothing. Press a letter.");
        } else {
            SummaryText("    ⚠ THE DIFF NEVER RAN, so no report was decoded. This section");
            SummaryText("      says nothing about injection; read the HID rows above.");
        }
        Row   ("      no virtual keycode",  c[kWInjNoVk]);
        Row   ("      dropped, no KCHR",    c[kWInjNoKchr]);
        Row   ("      KeyTranslate gave nothing", c[kWInjNoChar]);
        /* ⚠⚠ A NONZERO HERE IS NORMAL AND IS NOT A FAULT, and saying so is the whole
         * point. Pressing Shift produces no CHARACTER, so KeyTranslate correctly
         * returns 0 and nothing is posted -- Apple's poster behaves identically. The
         * modifier still took effect, because the KeyMap was updated BEFORE the call
         * and the next letter's translation reads it from there. Expect roughly one
         * count per modifier press AND per modifier release.
         * ⇒ Left unlabelled, a reader would see "KeyTranslate gave nothing 12" and
         * diagnose a broken translator. This project has shipped that mistake three
         * times in other sections; not a fourth. */
        if (c[kWInjNoChar] != 0)
            SummaryText("      ⭐ NORMAL: modifiers produce no character. Expect about one"
                        " per Shift/Ctrl/Option/Command press AND release.");
        /* ⚠⚠ NOT RowStatus, AND THIS IS THE THIRD TIME ITS POLARITY HAS BEEN WRONG
         * FOR A WORD IN THIS SECTION. RowStatus is for HCI status codes, where 0 is
         * success and nonzero names a fault. These two are 0x100 | a USB usage and
         * 0x100 | a Mac virtual keycode -- a NONZERO is the whole point of them. The
         * v10.5 log read:
         *     last usage        10  <== NONZERO, this is the failure
         *     last virtual key   5  <== NONZERO, this is the failure
         * over a PERFECT mapping: usage 0x0A is 'g' and virtual keycode 0x05 is 'g'.
         * ⇒ Reusing a formatter whose convention does not fit the word is how a
         * diagnostic keeps calling the success state a failure. Print the value and
         * say what it means. */
        if ((c[kWInjLastUsage] & 0x100UL) != 0)
            RowHex("      last usage (USB)",     c[kWInjLastUsage] & 0xFFUL, 2);
        else
            SummaryText("      last usage (USB)          none seen");
        if ((c[kWInjLastVk] & 0x100UL) != 0)
            RowHex("      last virtual key (Mac)", c[kWInjLastVk] & 0xFFUL, 2);
        else
            SummaryText("      last virtual key (Mac)    none seen");
        if (c[kWInjPostErr] != 0) {
            RowStatus("      ⚠ last PostEvent error", c[kWInjPostErr]);
            SummaryText("      A nonzero PostEvent result is the event QUEUE refusing --");
            SummaryText("      classically evtNotEnb or a full queue. The post path works;");
            SummaryText("      something downstream is not taking them.");
        }
    }

    /* ---- v11.6: CAPS LOCK, and the four places it can die ---------------------------
     *
     * Reported by the user against v11.5: "Caps Lock does not work; its LED light never
     * comes on, and it has no effect on capitalization." Two symptoms, and they are
     * INDEPENDENT -- the LED is a write back to the keyboard over HIDP, the
     * capitalization is a KeyMap bit read by KeyTranslate. Either can work while the
     * other does not, so they get separate rows and separate verdicts. Diagnosing them
     * as one thing is how a half-fix gets called a fix.
     *
     * ⚠ THE CAPITALIZATION PATH POSTS NO EVENT, AND THAT IS CORRECT, NOT A FAULT.
     * KeyTranslate returns 0 for virtual key 0x39, so BT_InjectKeyEvent takes neither
     * PostEvent branch and increments the no-character counter. Capitalization comes
     * from the KeyMap bit alone: vk 57 lands in bit 1 of gKeyMap[1], and Apple's
     * modifier fold shifts it left 9 to 0x0400 -- exactly alphaLock. So "caps posted 0
     * events" is the EXPECTED reading, and anyone hunting a missing PostEvent here is
     * hunting the wrong bug. */
    SummaryText("  ---- CAPS LOCK (v11.6)");
    if (c[kWHidKeyRpts] == 0) {
        SummaryText("      no key reports at all -- caps says nothing. Fix the link first.");
    } else if (c[kWLedSends] == 0 && c[kWCapsOn] == 0) {
        SummaryText("    ⚠ THE TOGGLE NEVER FIRED. Not one press of usage 0x39 reached");
        SummaryText("      the decoder, so neither symptom is about caps handling --");
        SummaryText("      the key is not arriving. Check the keys array in the byte");
        SummaryText("      dump above for 0x39 while holding Caps Lock down.");
    } else {
        Row("      toggle state now (1 = caps on)", c[kWCapsOn]);
        Row("      LED writes attempted",           c[kWLedSends]);

        /* ⭐⭐⭐ v11.7: THE PERSISTENCE QUESTION, which is all that is left.
         *
         * v11.6 established the toggle RUNS (24 state changes) and the bit arithmetic
         * is right (verified against Apple's own SetBit). So the failure is downstream
         * of both. This counts keystrokes that arrived to find low memory no longer
         * holding what we last wrote there. */
        Row("      ⭐ KeyMap clobbered between keys", c[kWInjMapClobber]);
        if ((c[kWInjMapLastSys] & 0x1000000UL) != 0) {
            RowHex("      system KeyMap byte 7, on entry", c[kWInjMapLastSys] & 0xFFUL, 2);
            RowHex("      our shadow  byte 7, on entry",  c[kWInjMapLastOur] & 0xFFUL, 2);
            SummaryText("      (bit0 shift, bit1 CAPS, bit2 option, bit3 control)");
        }
        if (c[kWInjMapClobber] != 0) {
            SummaryText("    ⭐⭐ SOMETHING ELSE IS WRITING THE KEYMAP. Our caps bit does not");
            SummaryText("      survive to the next keystroke, so KeyTranslate never sees");
            SummaryText("      alphaLock however correctly we set it. ⇒ The fix is to stop");
            SummaryText("      keeping a private shadow and READ low memory back before");
            SummaryText("      each fold -- not to touch the toggle, which is correct.");
        } else if (c[kWInjMapLastSys] != 0) {
            SummaryText("    ⇒ OUR KEYMAP SURVIVES. Nothing is clobbering it, so the caps");
            SummaryText("      bit IS present when KeyTranslate runs and the fault is in");
            SummaryText("      the fold or the KCHR, not in persistence. Rules out the");
            SummaryText("      likeliest suspect and points at the remaining one.");
        }

        /* ⚠ THE SUBMIT CODE IS NOT THE ORACLE. hid_host_send_set_report returns
         * SUCCESS after a lookup and five field stores; the bytes go out later from
         * the can-send-now callback. This row is here to separate "we never asked"
         * from "we asked and it failed", and for nothing else. */
        if ((c[kWLedRc] & 0x100UL) != 0) {
            if ((c[kWLedRc] & 0xFFUL) == 0)
                SummaryText("      submit                     accepted (means queued, NOT sent)");
            else
                RowStatus("    ⚠ LED submit refused",   c[kWLedRc]);
        }

        /* ⭐⭐ THIS one is the keyboard talking -- but ONLY for the control channel.
         *
         * ⚠ v11.8 MOVED THE LED TO THE INTERRUPT CHANNEL, which is fire-and-forget: a
         * DATA output report gets NO handshake by design. So a zero here is now the
         * EXPECTED reading, not a failure, and the old "no handshake came back" verdict
         * would be actively wrong. The only oracle left for the LED is the lamp itself.
         * The 0x03 below is the control channel's refusal, kept because it is what sent
         * us to the interrupt channel in the first place. */
        if (c[kWLedRsp] == 0) {
            SummaryText("      no handshake -- EXPECTED on the interrupt channel, which");
            SummaryText("      does not acknowledge. The lamp is the only oracle now:");
            SummaryText("      if it is still dark, this route is shut too and the next");
            SummaryText("      question is whether the A1016 has a host-settable LED at");
            SummaryText("      all (its caps key may drive the lamp in firmware).");
        } else {
            unsigned long hs = c[kWLedRsp] & 0xFFUL;
            if (hs == 0x00UL) {
                SummaryText("    ⭐⭐ THE KEYBOARD ACCEPTED THE LED REPORT (handshake 0x00).");
                SummaryText("      If the light is still dark the byte is wrong, not the");
                SummaryText("      path: bit 1 is Caps Lock in the HID LED page.");
            } else if (hs == 0x02UL) {
                SummaryText("    ⭐ ERR_INVALID_REPORT_ID -- REPORT ID 1 IS WRONG FOR OUTPUT.");
                SummaryText("      This is the answer we most expected to be wrong: 1 was");
                SummaryText("      chosen because the INPUT reports are ID 1, on symmetry");
                SummaryText("      alone. Retry with 0 (no ID byte) before anything else.");
            } else if (hs == 0x03UL) {
                SummaryText("    ⚠ ERR_UNSUPPORTED_REQUEST -- this keyboard does not take");
                SummaryText("      SET_REPORT at all, and the LED is not reachable this");
                SummaryText("      way. The caps TOGGLE can still work without it.");
            } else if (hs == 0x01UL) {
                SummaryText("      NOT_READY -- the keyboard was busy; it wants a retry.");
            } else {
                RowStatus("    ⚠ LED handshake refused", hs);
            }
        }

        /* The capitalization half, which shares nothing with the rows above. */
        if (c[kWCapsOn] != 0)
            SummaryText("      caps is ON as the driver sees it -- if letters are still");
        else
            SummaryText("      caps is OFF as the driver sees it -- if capitalization is");
        SummaryText("      lowercase, the KeyMap bit is not reaching KeyTranslate and");
        SummaryText("      the LED result above is irrelevant to that half.");
    }

    /* ★★★★★★ M5 STEP 1. The goal this project has chased since M4, read out. */
    SummaryText("  ★★★★★★ THE CONSUMER PAGE - VOLUME, MUTE, EJECT (M5 step 1)");
    Row   ("      reports with byte 8", c[kWHidConsRpts]);
    RowHex("      ⭐ keys ever seen",   c[kWHidConsSeen], 2);
    if (c[kWHidConsRpts] == 0) {
        SummaryText("    ⚠ NO REPORT CARRIED BYTE 8, so the keyboard is in BOOT protocol");
        SummaryText("      and this says nothing about the Consumer page. Check the HIDP");
        SummaryText("      handshake and SET_PROTOCOL rows above -- byte 8 exists only in");
        SummaryText("      report protocol.");
    } else {
        if (c[kWHidConsSeen] & 0x01UL) SummaryText("        ⭐ EJECT seen");
        if (c[kWHidConsSeen] & 0x02UL) SummaryText("        ⭐ MUTE seen");
        if (c[kWHidConsSeen] & 0x04UL) SummaryText("        ⭐ VOLUME UP seen");
        if (c[kWHidConsSeen] & 0x08UL) SummaryText("        ⭐ VOLUME DOWN seen");
        if (c[kWHidConsSeen] == 0) {
            SummaryText("    ⚠ Reports carried byte 8 but every one was ZERO. The channel");
            SummaryText("      and the protocol are right and no media key was pressed --");
            SummaryText("      press volume up, volume down, mute and eject, in that order.");
        }
        if (c[kWHidConsSeen] == 0x0FUL) {
            SummaryText("    ⭐⭐⭐⭐⭐ ALL FOUR CONSUMER KEYS ARRIVED THROUGH OUR OWN STACK.");
            SummaryText("      That is the answer this project has chased since M4, and it");
            SummaryText("      is something the card's HID-proxy path CANNOT deliver: the");
            SummaryText("      proxy truncates byte 8 before Mac OS sees a byte.");
        }
    }
    if (c[kWHidConsPadBits] != 0) {
        Row("    ⚠⚠ reports with PADDING BITS set", c[kWHidConsPadBits]);
        SummaryText("      Bits 4..7 of byte 8 are declared CONSTANT padding. If they are");
        SummaryText("      set, docs/A1016-REPORT-DESCRIPTOR.md is WRONG about the padding");
        SummaryText("      and the decode needs revisiting. Read the raw values below.");
    }
    /* ⚠ THE SEQUENCE IS THE MEASUREMENT, not the individual values. */
    if (c[kWHidConsRingCount] != 0) {
        short ci;
        Str255 rl;
        rl[0] = 0;
        PStrCat(rl, "      byte 8, in order: ");
        for (ci = 0; ci < (short)c[kWHidConsRingCount] &&
                     ci < (short)(kWHidConsRingEnd - kWHidConsRing + 1); ci++) {
            unsigned long v = c[kWHidConsRing + ci];
            if ((v & 0x100UL) == 0) continue;      /* slot never filled */
            PStrCatHexN(rl, v & 0xFFUL, 2);
            PStrCatCh(rl, ' ');
        }
        Summary(rl);
        if (c[kWHidConsRingLost] != 0)
            Row("      ⚠ transitions LOST past the ring", c[kWHidConsRingLost]);
        SummaryText("    ⇒ READ THIS AS A SEQUENCE, and it settles the one thing the");
        SummaryText("      decoder refuses to guess. The descriptor marks Eject and Mute");
        SummaryText("      RELATIVE and the two volume keys ABSOLUTE:");
        SummaryText("        a nonzero FOLLOWED BY 00 = the bit clears on release, so it");
        SummaryText("          behaves as held state and injection tracks edges;");
        SummaryText("        a nonzero with NO following 00 = the bit LATCHED, so that key");
        SummaryText("          must be injected as a one-shot on the set, not on an edge.");
        SummaryText("      ⚠ Getting this wrong makes exactly TWO of the four keys");
        SummaryText("      misbehave, in a way that looks like a radio fault. It is");
        SummaryText("      UNMEASURED until this line has content.");
    }

    SummaryText("  ★★★★★★ DID THE USL ACTUALLY MOVE THE BYTES? (v10.0)");
    Row   ("      bytes we asked for", c[kWAclOutReq]);
    Row   ("      ⭐⭐ bytes it moved", c[kWAclOutAct]);
    if (c[kWAclOutReq] == 0)
        SummaryText("      No ACL send was ever attempted, so this says nothing.");
    else if (c[kWAclOutAct] == c[kWAclOutReq]) {
        SummaryText("    ⭐ EVERY BYTE REACHED THE ENDPOINT. The USL moved the whole");
        SummaryText("      packet out of the host, so the send path is DONE and the");
        SummaryText("      fault is at or beyond the controller. Read the ROLE below:");
        SummaryText("      a SLAVE may only transmit when the master polls it, and");
        SummaryText("      0x13 is emitted on TRANSMISSION, not on acceptance.");
    } else {
        SummaryText("    ⚠⚠⚠ SHORT OR ZERO. The USL completed with status 0 having moved");
        SummaryText("      FEWER bytes than we asked for, so the packet never left the");
        SummaryText("      host and every controller-side theory has been chasing a");
        SummaryText("      phantom. The fault is our own USL usage. Ten builds recorded");
        SummaryText("      a status here and never the count.");
    }

    SummaryText("  ★★★★★★ MASTER OR SLAVE? (v10.0 - never measured before)");
    Row   ("      Role_Change arrivals", c[kWRoleChanges]);
    RowStatus("      its status",        c[kWRoleStatus]);
    RowStatus("      ⭐⭐ NEW ROLE",     c[kWRoleNew]);
    Row   ("      at (ms)",              c[kWRoleMs]);
    if (c[kWRoleChanges] == 0) {
        SummaryText("    ⚠⚠ NO ROLE CHANGE EVER ARRIVED, so we are still the SLAVE and");
        SummaryText("      the keyboard is master. Tiger accepts with role 0x00 and gets");
        SummaryText("      this event 215 ms later on this same pair.");
        SummaryText("      ⚠ ABSENCE IS NOT REFUSAL. A refused switch is reported HERE");
        SummaryText("      with a nonzero status; nothing at all means no switch was");
        SummaryText("      negotiated. Do NOT read this as the keyboard saying no.");
        SummaryText("      ⇒ If the bytes above all reached the endpoint, this is now");
        SummaryText("      the leading explanation for 0x13 events 0: the packets sit");
        SummaryText("      in the controller waiting for a poll that never comes.");
    } else if ((c[kWRoleNew] & 0xFFUL) == 0) {
        SummaryText("    ⭐⭐⭐ WE ARE THE MASTER, which is what Tiger is. The role switch");
        SummaryText("      happened. If 0x13 is STILL 0 above then role was not the");
        SummaryText("      cause either, and the next measured difference from Tiger is");
        SummaryText("      Change_Connection_Packet_Type (0x040F, types 0xcc18), which");
        SummaryText("      BTstack defines and never sends.");
    } else {
        SummaryText("    ⚠ A ROLE CHANGE ARRIVED AND WE ARE STILL THE SLAVE. Read its");
        SummaryText("      status: nonzero means the switch was attempted and REFUSED at");
        SummaryText("      LMP level, which is a real fact about the keyboard and the");
        SummaryText("      first one we have ever had.");
    }

    SummaryText("  ★★★★★★ THE RAW PIPE REFERENCES (v9.9) - THE DECIDING NUMBERS");
    RowHex("      ref interrupt-IN", c[kWRefInt],     8);
    RowHex("      ⭐⭐ ref bulk-OUT", c[kWRefBulkOut], 8);
    RowHex("      ref bulk-IN",      c[kWRefBulkIn],  8);
    Row   ("      discovery order",  c[kWPipeOrder]);
    if (c[kWPipeOrder] == 1)
        SummaryText("      1 = v9.9 order: interrupt-IN, then bulk-IN, then bulk-OUT"
                    " CHAINED off the bulk-IN pipe (Apple's USBEnetSample idiom).");
    else if (c[kWPipeOrder] == 2) {
        SummaryText("      ⚠ 2 = THE CHAINED FIND WAS REFUSED and we fell back to the");
        SummaryText("      old re-seeded search. Do NOT read this run as a test of the");
        SummaryText("      chain -- it is the old behaviour with a new label.");
        RowHex("      chain error",  c[kWPipeChainErr], 8);
    } else {
        SummaryText("      ⚠ 0 = pipe discovery never completed. Nothing below the M0");
        SummaryText("      rows means anything; read the immediate-error code first.");
    }
    if (c[kWRefBulkOut] != 0 && c[kWRefBulkOut] == c[kWRefBulkIn]) {
        SummaryText("  ⚠⚠⚠ BULK-OUT AND BULK-IN ARE THE SAME PIPE. Every ACL 'write' went");
        SummaryText("    to the INBOUND endpoint, which completes with status 0 and moves");
        SummaryText("    nothing outward -- so the controller had nothing to transmit and");
        SummaryText("    could never emit a 0x13. THIS IS THE WHOLE FAULT, and it also");
        SummaryText("    explains why armed reads outnumber read completions: the reads");
        SummaryText("    and the 'writes' were contending for one pipe.");
    } else if (c[kWRefBulkOut] != 0 && c[kWRefBulkIn] != 0) {
        SummaryText("  ⭐ THE THREE PIPES ARE DISTINCT. So the send pipe is a genuine,");
        SummaryText("    separate endpoint and the same-pipe theory is DEAD. If 0x13 is");
        SummaryText("    still 0 above, the fault is NOT pipe identity and the next");
        SummaryText("    measurement is the endpoint DESCRIPTORS (bEndpointAddress via");
        SummaryText("    USBGetConfigurationDescriptor), not another guess.");
    }

    SummaryText("  ★★★★★★ BTstack'S OWN HID HOST (v9.5)");
    Row("    host gate",            c[kWBhGate]);
    RowStatus("    ⭐ GAP level AS SET",  c[kWGapLevel]);
    if ((c[kWGapLevel] & 0x100UL) != 0 && (c[kWGapLevel] & 0xFFUL) == 0)
        SummaryText("      LEVEL_0 -- no authentication, no encryption, which is what"
                    " TIGER does.");
    else if ((c[kWGapLevel] & 0x100UL) != 0)
        SummaryText("      ⚠ NOT LEVEL_0, so this is NOT Tiger's configuration.");
    Row("    hid_host_init ran",    c[kWBhInit]);
    Row("    incoming connections", c[kWBhIncoming]);
    RowHex("    its hid_cid",       c[kWBhCid], 4);
    RowStatus("    accept rc",      c[kWBhAcceptRc]);
    Row("    ⭐ channels OPENED",    c[kWBhOpened]);
    RowStatus("    open status",    c[kWBhOpenedStatus]);
    /* ⭐⭐ DECODE IT. This value cost a grep through bluetooth.h to interpret on
     * 2026-09-18, and it is the single most informative number in a failed run: a
     * non-zero open status is L2CAP's, not HID's. CLAUDE.md's rule -- never log a status
     * register as a bare number. */
    if ((c[kWBhOpenedStatus] & 0xFFUL) != 0 && c[kWBhOpenedStatus] != 0) {
        unsigned long st = c[kWBhOpenedStatus] & 0xFFUL;
        if      (st == 0x69UL) {
            SummaryText("    0x69 = L2CAP RTX TIMEOUT: we sent an L2CAP connection");
            SummaryText("    request and the PEER NEVER ANSWERED within L2CAP's own");
            SummaryText("    10 s window (L2CAP_RTX_TIMEOUT_MS). The keyboard is");
            SummaryText("    reachable at the ACL level -- check 'encryption enabled'");
            SummaryText("    and 'Auth Completes' -- but is not answering HID setup.");
        }
        else if (st == 0x65UL) SummaryText("    0x65 = REFUSED, PSM not supported.");
        else if (st == 0x66UL) SummaryText("    0x66 = REFUSED, insufficient security.");
        else if (st == 0x67UL) SummaryText("    0x67 = REFUSED, no resources.");
        else if (st == 0x6AUL) SummaryText("    0x6A = baseband disconnected under it.");
        else SummaryText("    Non-zero = the L2CAP channel FAILED to come up; see "
                         "L2CAP_CONNECTION_* in vendor/btstack/src/bluetooth.h.");
    }
    Row("    opened at (ms)",       c[kWBhOpenedMs]);
    Row("    closed",               c[kWBhClosed]);
    Row("    closed at (ms)",       c[kWBhClosedMs]);
    RowStatus("    ⭐⭐ HIDP handshake", c[kWBhSetProtoRsp]);
    Row("    descriptor available", c[kWBhDescAvail]);
    Row("    ⭐⭐⭐ HID REPORTS",      c[kWBhReports]);
    Row("    other HID events",     c[kWBhOtherEvts]);

    /* ★★★★★★ v13.8's HALF-OPEN WATCHDOG, and the measurement that sets its timeout.
     * The stall this exists for is: accepted, never opened, ACL link up for 31 minutes,
     * recovered only by power-cycling the keyboard. Printed unconditionally on a driver
     * that has the words, because a ZERO in every row is the healthy answer and a
     * missing section would be indistinguishable from an older driver. */
    if (c[kWBuild] >= kTagFirstWithFailedOpens) {   /* belt and braces over the
                                         * kExpectedDriverTag gate, which already
                                         * refuses a mismatched driver outright.
                                         * ⚠ MUST be >= 13.9, not 13.8: words 702/703
                                         * were REUSED with new meanings, so reading a
                                         * 13.8 block here would print teardown counts
                                         * as failed opens. */
        Row("    accepted at (ms)",      c[kWBhIncomingMs]);
        /* ⭐ THE NUMBER THIS PROJECT DID NOT HAVE. No accept-at timestamp existed before
         * 13.8, so the healthy accept->open gap was unrecoverable from any banked log,
         * and the 10 s stall timeout is a safety factor over the only latency ever
         * measured (271 ms, Auth Complete to open) rather than a value taken from this
         * event. Read it on a HEALTHY boot and tighten the timeout against it. */
        if (c[kWBhOpenedMs] > c[kWBhIncomingMs] && c[kWBhIncomingMs] != 0) {
            Row("    ⭐ accept->open gap (ms)", c[kWBhOpenedMs] - c[kWBhIncomingMs]);
            SummaryText("    THE gap, and it reads TWO ways -- check 'open status'");
            SummaryText("    first. A few hundred ms with status 0 is a healthy setup.");
            SummaryText("    Close to 10000 ms is NOT a slow success: that is L2CAP's");
            SummaryText("    own RTX window expiring (L2CAP_RTX_TIMEOUT_MS = 10000) and");
            SummaryText("    the 'open' is the failure it produced.");
        } else if (c[kWBhIncomingMs] != 0) {
            SummaryText("    ⚠ No accept->open gap: the channel that was accepted last");
            SummaryText("    never opened, or opened BEFORE the last accept. Read the");
            SummaryText("    failed-open rows below.");
        }
        /* ⭐⭐⭐ THE ROW THAT ANSWERS "IS IT WORKING RIGHT NOW", and the reason it
         * exists: this was inferred three different ways and each inference was wrong in
         * a different state (gBhCid, opens-minus-closes, open/close timestamps). The
         * driver maintains it now; everything reads it. */
        /* ★★★★★★ v14.1: THE BATTERY PROBE -- THE ONE QUESTION THIS RUN EXISTS TO ANSWER.
         * Printed unconditionally on a 14.1+ driver: every row reading zero is itself the
         * answer ("we never asked" vs "we asked and got nothing"), and a section that
         * vanishes when the news is bad is how a negative gets mistaken for an absence. */
        if (c[kWBuild] >= kTagFirstWithBattery) {
            /* ⚠ SummaryText, not Summary. Summary takes a Str255 -- a PASCAL string --
             * so a C literal is read with its first byte as the LENGTH. Here that byte
             * is a space (0x20 = 32), and the line printed as exactly 32 bytes:
             * "⭐⭐⭐⭐ BATTERY: DOES THIS", silently losing "KEYBOARD ANSWER A
             * GET_REPORT?". It shipped truncated in every log from v14.1 to v99.80 and
             * nobody noticed, because a heading that reads sensibly up to where it stops
             * does not look cut off. -Wstringop-overflow had been pointing at it the
             * whole time ("accessing 256 bytes in a region of size 64"). */
            SummaryText("  ⭐⭐⭐⭐ BATTERY: DOES THIS KEYBOARD ANSWER A GET_REPORT?");
            Row("    get_report calls sent",    c[kWBatSends]);
            Row("    responses received",       c[kWBatResponses]);
            RowHex("    Feature 71 send rc",    c[kWBatPctRc], 4);
            RowHex("    Feature 71 handshake",  c[kWBatPctHs], 4);
            Row("    Feature 71 payload len",   c[kWBatPctLen]);
            RowHex("    Feature 71 bytes",      c[kWBatPctVal], 8);
            RowHex("    Input 48 send rc",      c[kWBatStRc], 4);
            RowHex("    Input 48 handshake",    c[kWBatStHs], 4);
            Row("    Input 48 payload len",     c[kWBatStLen]);
            RowHex("    Input 48 bytes",        c[kWBatStVal], 8);

            if (c[kWBatSends] == 0) {
                SummaryText("    ⚠ NOTHING WAS ASKED, so this run says nothing about the");
                SummaryText("    battery. The probe arms 2 s after the HID channel opens");
                SummaryText("    and needs 'HID channel LIVE now' 1 -- if that is 0, the");
                SummaryText("    keyboard had not connected when this was read. Type on");
                SummaryText("    it, wait a few seconds, and run BTCheck again.");
            } else if (c[kWBatResponses] == 0) {
                SummaryText("    ⚠⚠ ASKED AND NEVER ANSWERED. The request went out and no");
                SummaryText("    GET_REPORT_RESPONSE came back at all -- not even a");
                SummaryText("    refusal. Read the send rc first: 0x100 means BTstack");
                SummaryText("    accepted it, so silence is the DEVICE's. That is a");
                SummaryText("    different finding from a refusal and worth recording.");
            } else if ((c[kWBatPctHs] & 0xFFUL) == 0x00UL) {
                SummaryText("    ⭐⭐⭐⭐⭐ IT ANSWERED, handshake SUCCESSFUL. The payload");
                SummaryText("    bytes above are a real reading from the keyboard. Tiger");
                SummaryText("    says Feature 71 is ONE byte, 0..100, so the low byte is");
                SummaryText("    the percentage -- but check the length before trusting");
                SummaryText("    that: this project has twice been wrong by believing a");
                SummaryText("    declaration over what the device actually sent.");
                SummaryText("    ⇒ THE CSM BATTERY GAUGE IS REAL. Build it.");
            } else if ((c[kWBatPctHs] & 0xFFUL) == 0x03UL) {
                SummaryText("    ⛔ 0x03 = ERR_UNSUPPORTED_REQUEST. The device declines");
                SummaryText("    GET_REPORT, exactly as it already declines SET_REPORT.");
                SummaryText("    That closes the battery question honestly: the CSM must");
                SummaryText("    then show status only and NOT a field it cannot fill.");
            } else {
                SummaryText("    ⚠ A handshake we have not seen before. HIDP: 0x00");
                SummaryText("    SUCCESSFUL, 0x01 NOT_READY, 0x02 ERR_INVALID_REPORT_ID,");
                SummaryText("    0x03 ERR_UNSUPPORTED_REQUEST, 0x04 ERR_INVALID_PARAM,");
                SummaryText("    0x0E ERR_UNKNOWN, 0x0F ERR_FATAL. Name it before acting.");
            }
            /* ★★★ v14.2: THE PUBLISH. A reading nobody can read is not a feature. */
            Row("    ⭐ readings published",  c[kWBatPublished]);
            Row("    ★ devices ASKED",        c[kWBatTargets]);
            Row("    probes given up on",     c[kWBatGiveUps]);
            if (c[kWBatTargets] < 2) {
                SummaryText("    ⚠ FEWER THAN TWO DEVICES WERE ASKED. Before v15.2 the");
                SummaryText("    chain latched after the first, so the mouse was never");
                SummaryText("    asked -- a different fact from the mouse saying no.");
            }
            Row("    file flushes",          c[kWBatFlushes]);
            Row("    flush errors",          c[kWBatFlushErr]);
            /* ★ THE LOW-BATTERY WARNING, v14.8. Printed unconditionally: zero warnings
             * on a healthy battery is the CORRECT answer, and a section that only
             * appears when it fires cannot be distinguished from one that never ran. */
            Row("    low-battery warnings",  c[kWBatWarnings]);
            Row("    alerts posted",         c[kWAlertsPosted]);
            Row("    ⚠ alerts DROPPED",      c[kWAlertsDropped]);
            RowStatus("    last NMInstall rc",  c[kWAlertErr]);
            if (c[kWAlertsDropped] != 0) {
                SummaryText("    ⚠⚠ A DROP IS A POLICY BUG, NOT A MANAGER FAILURE. Only");
                SummaryText("    one notification may be outstanding, so a drop means the");
                SummaryText("    5%/10% threshold latch fired again before the user");
                SummaryText("    dismissed the last alert -- i.e. it is nagging. Check the");
                SummaryText("    hysteresis in BT_BatteryNote, not the Notification Mgr.");
            } else if (c[kWBatWarnings] != 0 && c[kWAlertsPosted] == 0) {
                SummaryText("    ⚠ DECIDED TO WARN AND POSTED NOTHING. NMInstall failed or");
                SummaryText("    BT_AlertInit never built its UPP -- read the rc above.");
            }
            if (c[kWBatPublished] != 0 && c[kWBatFlushErr] == 0 && c[kWBatFlushes] != 0) {
                /* ⚠ THIS TEXT SAID "format 1" UNTIL DRIVER 14.5 AND WAS THEN WRONG for
                 * one build. Prose about a format goes stale silently -- the log looked
                 * authoritative while naming a version the driver no longer wrote. If
                 * kBatFmtVer moves again, this line moves with it. */
                SummaryText("    ⭐⭐ WRITTEN TO Preferences:Bluetooth Battery -- 'BTBA',");
                SummaryText("    format 2, then one 4-long record per device: addrHi,");
                SummaryText("    addrLo, percent, whenTicks. That file is what the CSM");
                SummaryText("    reads; it does NOT read this block, on purpose, because");
                SummaryText("    kWEnd has moved thirteen times.");
                SummaryText("    ⚠ addrHi is the TOP 3 BYTES and addrLo the low 3, the");
                SummaryText("    same split as everywhere else here. Format 1 wrote 2+4");
                SummaryText("    and the CSM could not join it to Device Kinds, so every");
                SummaryText("    device was listed twice. That is what format 2 fixes.");
            } else if (c[kWBatPublished] != 0 && c[kWBatFlushErr] != 0) {
                SummaryText("    ⚠⚠ READ BUT NOT WRITTEN. The level came back and the");
                SummaryText("    file write failed, so a CSM would show nothing while");
                SummaryText("    this section says the battery is readable. Preferences");
                SummaryText("    unwritable, or the volume full.");
            } else if (c[kWBatPublished] == 0 && (c[kWBatPctHs] & 0xFFUL) == 0x00UL
                                              && c[kWBatPctHs] != 0) {
                SummaryText("    ⚠ ANSWERED BUT NOT PUBLISHED: the payload's last byte");
                SummaryText("    was above 100, so it is not a percentage and was");
                SummaryText("    refused rather than shown. Read 'Feature 71 bytes'.");
            }
            SummaryText("    ⚠⚠ Report IDs 68 and 69 live in the SAME table and are");
            SummaryText("    FactoryDefault / FullFactoryDefault -- DESTRUCTIVE. The");
            SummaryText("    driver sends only 71 and 48, by name. Never sweep this table.");
        }
            /* ★★★★ THE DEVICE'S OWN NAME, AS TEXT *AND* AS BYTES (v99.84).
             *
             * ⚠⚠ THIS SECTION EXISTS BECAUSE ITS ABSENCE COST A ROUND TRIP. The name was
             * in the block all along -- 6 packed words per slot -- and BTCheck never
             * printed it, so when driver 14.8 stored the A1016's UTF-8 name verbatim the
             * only way to SEE "fw800,Aos keyboard" was a screenshot of the Control Strip.
             * A log that carried the name would have shown it on the first run.
             *
             * ⭐ BOTH FORMS, DELIBERATELY. The text says what the user sees; the HEX says
             * what is actually stored, which is the only way to tell a MacRoman 0xD5 from
             * the UTF-8 E2 80 99 that renders as three glyphs. CLAUDE.md's rule is that a
             * hex value in a log is a question and a decoded field is an answer -- here
             * the question and the answer are different facts and both are wanted. */
            if (c[kWNameCount] != 0) {
                unsigned long si;
                SummaryText("");
                SummaryText(" ⭐ DEVICE NAMES (stored as MacRoman, converted from UTF-8)");
                for (si = 0; si < c[kWNameCount] && si < 2; si++) {
                    unsigned long base = (unsigned long)kWNameBase + si * 8;
                    unsigned char nm[25];
                    short k, n = 0, printable = 1;
                    Str255 l;
                    for (k = 0; k < 6; k++) {
                        unsigned long w = c[base + 2 + k];
                        nm[n++] = (unsigned char)((w >> 24) & 0xFF);
                        nm[n++] = (unsigned char)((w >> 16) & 0xFF);
                        nm[n++] = (unsigned char)((w >>  8) & 0xFF);
                        nm[n++] = (unsigned char)( w        & 0xFF);
                    }
                    nm[24] = 0;
                    RowBDAddr("    address", c[base + 0], c[base + 1]);
                    /* the text */
                    l[0] = 0;
                    PStrCat(l, "    name  \"");
                    for (k = 0; k < 24 && nm[k] != 0; k++) {
                        if (l[0] < 200) l[++l[0]] = nm[k];
                        if (nm[k] >= 0x80) printable = 0;
                    }
                    PStrCat(l, "\"");
                    Summary(l);
                    /* and the bytes, which is the half that catches an encoding fault */
                    l[0] = 0;
                    PStrCat(l, "    bytes ");
                    for (k = 0; k < 24 && nm[k] != 0; k++) {
                        PStrCatHexN(l, nm[k], 2);
                        if (l[0] < 240) l[++l[0]] = ' ';
                    }
                    Summary(l);
                    if (!printable) {
                        SummaryText("    ⓘ Contains bytes >= 0x80. That is EXPECTED for an");
                        SummaryText("    accented letter or a curly quote in MacRoman (0xD5");
                        SummaryText("    is a right single quote). It is a FAULT only if you");
                        SummaryText("    see E2 80 xx or C3 xx -- those are raw UTF-8 and");
                        SummaryText("    mean the conversion did not run.");
                    }
                }
            }

            /* ★★★★★ TWO HID DEVICES, v15.0. Read this BEFORE the mouse section: if the
             * mouse never got a slot, every mouse counter below is zero for a reason
             * that has nothing to do with the decoder. */
            SummaryText("");
            SummaryText(" ⭐⭐⭐⭐⭐ HID DEVICES CONNECTED (keyboard + mouse)");
            Row("    slots claimed",       c[kWHidDevOpens]);
            Row("    slots released",      c[kWHidDevCloses]);
            Row("    ⚠ dropped, table full", c[kWHidDevFull]);
            {
                short si;
                for (si = 0; si < 2; si++) {
                    unsigned long cid  = c[kWDev0Cid  + si * 4];
                    unsigned long role = c[kWDev0Role + si * 4];
                    Str255 l;
                    if (cid == 0) continue;
                    l[0] = 0;
                    PStrCat(l, "    slot ");
                    PStrCatNum(l, (long)si);
                    PStrCat(l, ": cid 0x");
                    PStrCatHexN(l, cid, 4);
                    PStrCat(l, role == 1 ? "  KEYBOARD"
                             : role == 2 ? "  MOUSE"
                             : role == 3 ? "  both?!" : "  role not yet known");
                    Summary(l);
                    RowBDAddr("      address", c[kWDev0Hi + si * 4], c[kWDev0Lo + si * 4]);
                    /* ★ v15.3: THIS DEVICE'S OWN LEVEL. The battery rows further down
                     * carry only the LAST probe's bytes, so with two devices answering
                     * they could not be attributed without knowing the scheduler's
                     * order. Here it is beside the address it belongs to. */
                    {
                        unsigned long pct = c[kWDev0Pct   + si * 2];
                        unsigned long bhs = c[kWDev0BatHs + si * 2];
                        Str255 bl;
                        bl[0] = 0;
                        PStrCat(bl, "      battery            ");
                        if (bhs == 0) {
                            PStrCat(bl, "not asked yet");
                        } else if ((bhs & 0xFFUL) != 0) {
                            PStrCat(bl, "DECLINED, handshake 0x");
                            PStrCatHexN(bl, bhs & 0xFFUL, 2);
                        } else if (pct == 0) {
                            PStrCat(bl, "answered, but no level in the payload");
                        } else {
                            PStrCatNum(bl, (long)pct);
                            PStrCat(bl, "%");
                        }
                        Summary(bl);
                    }
                }
            }
            /* ★★ THE v15.0 REGRESSION CHECK, DECODED RATHER THAN LEFT AS TWO HEX
             * NUMBERS TO COMPARE BY EYE. gBhCid is the identity four consumers read as
             * "the connection" -- the Caps LED, the reconnect, the battery probe, the
             * sweep's collision guard. Before v15.0 a mouse connecting stole it and a
             * mouse NAPPING cleared it. If this line ever says the mouse holds it while
             * a keyboard is connected, that is the regression, not a curiosity. */
            {
                unsigned long idc = c[kWBhCid];
                short si2, held = -1;
                for (si2 = 0; si2 < 2; si2++)
                    if (idc != 0 && c[kWDev0Cid + si2 * 4] == idc) held = si2;
                if (idc == 0) {
                    SummaryText("    identity (gBhCid): none held");
                } else if (held < 0) {
                    SummaryText("    ⚠ identity (gBhCid) names a cid that is in NO slot");
                } else {
                    Str255 l; unsigned long rl = c[kWDev0Role + held * 4];
                    l[0] = 0;
                    PStrCat(l, "    identity (gBhCid) is held by slot ");
                    PStrCatNum(l, (long)held);
                    PStrCat(l, rl == 1 ? " -- KEYBOARD, correct"
                             : rl == 2 ? " -- ⚠⚠ THE MOUSE holds it"
                             :           " -- role not yet known");
                    Summary(l);
                }
            }
            SummaryText("    ⓘ A role is learned from WHICH DECODER accepts a report, so");
            SummaryText("    'role not yet known' means the device is connected and has");
            SummaryText("    not sent anything we understood -- which for a mouse means");
            SummaryText("    MOVE IT, and if it stays unknown, read the unclaimed row.");

            SummaryText("");
            SummaryText(" ⭐⭐⭐⭐⭐⭐ AIRPORT COEXISTENCE -- SETTLED, v16.1");
            SummaryText("    The reconnection sweep is DELETED. It paged bonded devices");
            SummaryText("    every 15 s, and paging is continuous 2.4 GHz transmission");
            SummaryText("    inches from the AirPort card -- measured deafening it for");
            SummaryText("    ~15 s of every ~30 s (beacons 10/s -> 0-2/s). The card has no");
            SummaryText("    coexistence hardware, so the only fix was to stop.");
            SummaryText("    Its premise was also false: it existed because 'a mouse does");
            SummaryText("    not page', and the A1015 pages, sleeps, and pages again on");
            SummaryText("    wake -- measured 2026-10-02. Nothing replaced it. Devices");
            SummaryText("    call us, which is exactly what Tiger does.");
            Row("    incoming connections", c[kWBhIncoming]);
            Row("    slots claimed total",  c[kWHidDevOpens]);
            if (c[kWHidDevOpens] != 0 && c[kWBhIncoming] >= c[kWHidDevOpens]) {
                SummaryText("    ⭐ Every connection came from the device paging us.");
            } else if (c[kWHidDevOpens] != 0) {
                SummaryText("    ⚠ Fewer incoming than connections -- something connected");
                SummaryText("    without paging us, which this build has no way to do.");
            }
            SummaryText("    ⚠ Page timeout is Tiger's 0x2000 (5.12 s), not BTstack's");
            SummaryText("    0x6000 (~15.4 s), for the pairing path that still pages.");

            /* ★★★★ THE MOUSE, v14.6. Printed unconditionally, because ZERO IS THE
             * INTERESTING ANSWER until an A1015 has ever reported: it separates "no
             * mouse present" from "a mouse reported and nothing understood it". */
            SummaryText("");
            SummaryText(" ⭐⭐⭐⭐ MOUSE (A1015)");
            Row("    mouse reports handled", c[kWMouseReports]);
            Row("    mouse decodes accepted", c[kWMouseDecodeOk]);
            Row("    cursor moves posted",   c[kWMouseMoves]);
            Row("    button changes posted", c[kWMouseBtnChanges]);
            Row("    dropped, no device yet", c[kWMouseDropped]);
            Row("    cursor device created", c[kWCurCreated]);
            Row("    cursor device errors",  c[kWCurNewErr]);
            Row("    wheel bytes seen",      c[kWMouseWheelSeen]);

            /* ★★★★★ IS THE A1 02 READING RIGHT? v15.0 measured the A1015's form as
             * 0xA1 0x02 + three bytes and v15.1 commits to reading 0x02 as a REPORT ID.
             * That was decided on evidence, not proof, so it stays instrumented: if the
             * deltas were actually one byte to the left, dx would sit at zero while dy
             * carried the button bits, and the mask below would show bits a one-button
             * mouse cannot send. Move the mouse in all four directions before reading. */
            if (c[kWMouseDxMax] == 0 && c[kWMouseDxMin] == 0
             && c[kWMouseDyMax] == 0 && c[kWMouseDyMin] == 0
             && c[kWMouseDecodeOk] == 0) {
                SummaryText("    ⓘ no report decoded yet, so the ranges below are empty");
            } else {
                Str255 l;
                l[0] = 0;
                PStrCat(l, "    dx seen              ");
                PStrCatNum(l, (long)c[kWMouseDxMin]);
                PStrCat(l, " .. ");
                PStrCatNum(l, (long)c[kWMouseDxMax]);
                Summary(l);
                l[0] = 0;
                PStrCat(l, "    dy seen              ");
                PStrCatNum(l, (long)c[kWMouseDyMin]);
                PStrCat(l, " .. ");
                PStrCatNum(l, (long)c[kWMouseDyMax]);
                Summary(l);
                RowHex("    button bits seen",  c[kWMouseBtnMask], 2);
                /* ⚠ (long), AND v15.1 SHIPPED WITHOUT IT. c[] is unsigned long, so
                 * `c[...] < 0` is always false and this verdict read "an axis has not
                 * gone both ways yet" while printing dx -41..33 and dy -54..39 directly
                 * above it. The rows were right and the conclusion under them was wrong,
                 * which is worse than printing nothing. */
                if ((long)c[kWMouseDxMin] < 0 && (long)c[kWMouseDxMax] > 0
                 && (long)c[kWMouseDyMin] < 0 && (long)c[kWMouseDyMax] > 0) {
                    SummaryText("    ⭐ BOTH AXES SPAN BOTH SIGNS -- the deltas are in the");
                    SummaryText("    bytes we think they are, and the sign extension is right.");
                } else {
                    SummaryText("    ⓘ an axis has not gone both ways yet. Move the mouse");
                    SummaryText("    left, right, up and down, then run this again -- an axis");
                    SummaryText("    stuck at 0 would mean the payload is offset by a byte.");
                }
            }
            SummaryText("    ⭐ resolution told to OS 9: 400 units/inch -- APPLE'S value");
            SummaryText("    for every USB mouse (usb-ddk MouseModule.c:658). NOT the");
            SummaryText("    sensor's real 681 cpi: the CDM's curves are ADB-era and");
            SummaryText("    Apple feeds 400 regardless of the actual sensor. 200 was");
            SummaryText("    half of it (imprecise); 681 was 1.7x it (sluggish).");
            {
                Str255 l; unsigned long a = c[kWCurAccelUsed];
                l[0] = 0;
                PStrCat(l, "    acceleration INHERITED  ");
                PStrCatNum(l, (long)(a >> 16));
                PStrCat(l, ".");
                PStrCatNum(l, (long)(((a & 0xFFFFUL) * 100UL) >> 16));
                Summary(l);
            }
            RowStatus("    SetAcceleration said",    c[kWCurAccelErr]);
            RowStatus("    SetButtons said",         c[kWCurButtonsErr]);
            RowStatus("    UnitsPerInch said",       c[kWCurUpiErr]);
            if (c[kWCurUpiErr] != 0) {
                SummaryText("    ⚠⚠ UnitsPerInch was REFUSED, so OS 9 kept its default and");
                SummaryText("    the tracking fix did NOT take effect. That is a different");
                SummaryText("    fault from the resolution being wrong.");
            }
            /* ★★★★★★ WHAT DOES THE MACHINE'S OWN MOUSE USE? v99.95.
             *
             * Setting resolution to the measured 681 made small movements precise and
             * large sweeps SLUGGISH. That is the signature of no acceleration curve:
             * resolution is a physical fact and is now right, but acceleration is a
             * PREFERENCE and we never set one -- it is Apple's sample value of 1.0,
             * which our own comment calls "no scaling", so the mapping is linear.
             *
             * ⚠ Rather than guess a Fixed value, read the known-good device in this
             * same machine. The wired mouse feels right, it is a CursorDevice too, and
             * CursorDevice exposes resolution and acceleration as PUBLIC fields. This
             * walks the list and prints every one. Ours is identifiable by resolution
             * 681; whatever the wired mouse shows is the number to match.
             *
             * ⚠ READ-ONLY. Nothing here changes any device. */
            SummaryText("");
            SummaryText(" ⭐⭐⭐⭐⭐⭐ EVERY CURSOR DEVICE IN THIS MAC (read-only)");
            {
                CursorDevicePtr dev = NULL;
                short guard = 0;
                OSErr e = CursorDeviceNextDevice(&dev);
                if (e != noErr || dev == NULL) {
                    SummaryText("    ⚠ could not walk the cursor device list");
                } else {
                    while (dev != NULL && guard < 8) {
                        Str255 l;
                        long res = (long)dev->resolution;
                        long acc = (long)dev->acceleration;
                        l[0] = 0;
                        PStrCat(l, "    dev ");
                        PStrCatNum(l, (long)guard);
                        PStrCat(l, ": res ");
                        PStrCatNum(l, res >> 16);            /* Fixed -> integer part */
                        PStrCat(l, " u/in   accel ");
                        PStrCatNum(l, acc >> 16);
                        PStrCat(l, ".");
                        PStrCatNum(l, ((acc & 0xFFFFL) * 100L) >> 16);  /* 2 decimals */
                        PStrCat(l, "   buttons ");
                        PStrCatNum(l, (long)dev->cntButtons);
                        /* ⚠ NAME OUR OWN ROW EXACTLY. Two devices read 400 u/in and
                         * cntButtons comes "from ADB reg 1" so it does not reflect our
                         * SetButtons(3) -- there was no way to tell ours apart by
                         * inspection, and guessing which row was ours would have
                         * inverted the acceleration finding. The driver publishes its
                         * CursorDevicePtr; this compares against it. */
                        if (c[kWCurDevPtr] != 0
                         && (unsigned long)dev == c[kWCurDevPtr]) PStrCat(l, "   <== OURS");
                        Summary(l);
                        guard++;
                        if (CursorDeviceNextDevice(&dev) != noErr) break;
                    }
                    SummaryText("    ⓘ Our row is marked. We do not hardcode an");
                    SummaryText("    acceleration: we COPY one from a device already");
                    SummaryText("    here, so the Bluetooth mouse follows the Mouse");
                    SummaryText("    control panel like every other pointer. Alone, we");
                    SummaryText("    start at Apple's 1.0 -- there is no API to read the");
                    SummaryText("    machine's preference, so that is where every new");
                    SummaryText("    pointer starts, Apple's own included.");
                    SummaryText("    ⚠ A device at 1.0 has simply not been adjusted since");
                    SummaryText("    it was created. That is not a fault, and it is NOT");
                    SummaryText("    evidence about which row is ours -- read the marker.");
                }
            }
            RowStatus("    CursorDeviceMove said",    c[kWCurMoveErr]);
            RowStatus("    CursorDeviceButtons said", c[kWCurBtnErr]);
            SummaryText("    ⓘ 'NOT RECEIVED' on those two is the GOOD case: no call ever");
            SummaryText("    failed. A number is the Cursor Device Manager refusing, which");
            SummaryText("    is a different fault from not decoding and from never running.");
            Row("    ⚠ 5-byte reports NOT id 02", c[kWMouseRidOther]);

            /* ★★★★★ v15.5: WHY THE CURSOR IS JERKY. Tiger drives this same mouse on
             * this same card smoothly -- measured 2026-09-25 -- so the hardware is
             * exonerated and the difference is ours. Tiger's captured trace sniffs at
             * 20 slots = 12.5 ms. We have permitted sniff since M4 and never once read
             * the interval back. Slots are 0.625 ms. */
            SummaryText("");
            SummaryText(" ⭐⭐⭐⭐⭐ SNIFF INTERVAL -- the suspected cause of jerky tracking");
            {
                short k;
                const char *who[3];
                unsigned long sl3[3];
                who[0] = "    keyboard link"; sl3[0] = c[kWSniffKbd];
                who[1] = "    MOUSE link";    sl3[1] = c[kWSniffMse];
                who[2] = "    unattributed";  sl3[2] = c[kWSniffOther];
                for (k = 0; k < 3; k++) {
                    Str255 l; unsigned long slots = sl3[k];
                    l[0] = 0;
                    PStrCat(l, who[k]);
                    PStrCat(l, "  ");
                    if (slots == 0) {
                        PStrCat(l, "never entered sniff");
                    } else {
                        PStrCatNum(l, (long)slots);
                        PStrCat(l, " slots = ");
                        PStrCatNum(l, (long)((slots * 625UL) / 1000UL));
                        PStrCat(l, " ms, max ");
                        PStrCatNum(l, (long)(1000UL / ((slots * 625UL) / 1000UL + 1UL)));
                        PStrCat(l, "/sec");
                    }
                    Summary(l);
                }
                SummaryText("    ⓘ Tiger's trace: 20 slots = 12.5 ms. MUCH larger here");
                SummaryText("    means reports are quantised to that interval, which is");
                SummaryText("    what a hand feels as jerk. Equal to Tiger's means sniff");
                SummaryText("    is NOT the cause and the next suspect is the scaling:");
                SummaryText("    CursorDeviceUnitsPerInch is a guessed 200.");
            }
            /* ★★★★★ v15.6: THE CALIBRATION SWIPE. Everything upstream is exonerated
             * -- 84 reports/sec, no sniff, no refusals -- so what is left is SCALING,
             * and CursorDeviceUnitsPerInch is a GUESSED 200 copied from Apple's sample.
             * These totals turn that guess into a measurement. */
            SummaryText("");
            SummaryText(" ⭐⭐⭐⭐⭐ MOUSE CALIBRATION -- total counts since startup");
            Row("    |dx| total", c[kWMouseAbsDx]);
            Row("    |dy| total", c[kWMouseAbsDy]);
            if (c[kWMouseAbsDx] == 0 && c[kWMouseAbsDy] == 0) {
                SummaryText("    (no movement recorded yet)");
            }
            SummaryText("    ⓘ HOW TO USE THIS, and it needs doing ONCE:");
            SummaryText("      1. restart, and do not touch the mouse afterwards");
            SummaryText("      2. drag it in ONE straight line along a ruler, exactly");
            SummaryText("         10 inches, left to right, without lifting it");
            SummaryText("      3. run BTCheck and read |dx| total");
            SummaryText("    |dx| / 10 is the mouse's TRUE counts per inch. We currently");
            SummaryText("    tell OS 9 it is 200. If the real figure is 400, OS 9 thinks");
            SummaryText("    every movement is half as fast as it is, sits at the bottom");
            SummaryText("    of its acceleration curve, and rounds small deltas toward");
            SummaryText("    zero -- which is precisely 'small precise movements and");
            SummaryText("    dragging do not respond'.");
            SummaryText("    ⚠ Lifting or changing direction mid-swipe invalidates it:");
            SummaryText("    these are ABSOLUTE totals and cannot tell motion from");
            SummaryText("    counter-motion. One clean straight pull.");
            Row("    ★ peak reports / 250ms", c[kWMouseRatePeak]);
            {
                unsigned long pk = c[kWMouseRatePeak];
                if (pk == 0) {
                    SummaryText("    ⚠ ZERO -- the mouse was not moved before this ran.");
                    SummaryText("    Move it continuously for a few seconds, then re-run.");
                } else {
                    Str255 l; l[0] = 0;
                    PStrCat(l, "    i.e. about ");
                    PStrCatNum(l, (long)(pk * 4UL));
                    PStrCat(l, " reports/sec at peak");
                    Summary(l);
                }
            }
            if (c[kWMouseRidOther] != 0) {
                SummaryText("    ⚠⚠ NONZERO MEANS THE REPORT-ID READING IS WRONG for this");
                SummaryText("    device: byte 1 varies, so it is data, not an ID.");
            }
            if (c[kWMouseMoves] != 0 || c[kWMouseBtnChanges] != 0) {
                SummaryText("    ⭐⭐⭐⭐⭐ THE MOUSE IS DRIVING THE CURSOR. Moves and/or");
                SummaryText("    button changes went to the Cursor Device Manager.");
            } else if (c[kWMouseReports] != 0 && c[kWCurCreated] == 0) {
                SummaryText("    ⚠⚠ REPORTS ARRIVED BUT NO CURSOR DEVICE EXISTS.");
                SummaryText("    CursorDeviceNewDevice runs at TASK level from the defer");
                SummaryText("    response; if 'cursor device errors' is also 0 then the");
                SummaryText("    defer hop never ran -- the v14.2 battery bug exactly.");
            } else if (c[kWMouseDecodeOk] != 0 && c[kWMouseMoves] == 0) {
                SummaryText("    ⓘ Decoded but never moved: every delta was zero, which");
                SummaryText("    is what a mouse sitting still sends. Move it and re-run.");
            }

            /* ⚠⚠ THE ROW THAT MATTERS ON THE FIRST A1015 RUN. The A1015's wire format
             * has NEVER been measured. The keyboard cost a whole session to this exact
             * shape of unknown: a documented report arrived in an undocumented framing,
             * every decoder branch refused it, and the log said only "reports accepted
             * 0" -- 38 reports thrown away while the keyboard reported itself live. So a
             * report NEITHER decoder claimed has its length and first four bytes printed
             * here, and one run names the real format instead of another guess. */
            if (c[kWUnclaimedReports] != 0) {
                SummaryText("");
                SummaryText("    ⚠⚠ REPORTS NEITHER DECODER ACCEPTED -- READ THIS FIRST");
                Row("    unclaimed reports",   c[kWUnclaimedReports]);
                Row("    last one's length",   c[kWUnclaimedLen]);
                Row("    its first 4 bytes",   c[kWUnclaimedB0]);
                SummaryText("    The keyboard decoder takes 8..11 bytes, the mouse 3..5.");
                SummaryText("    A length outside both, or a framing byte we do not");
                SummaryText("    expect, is the whole answer -- add that form to");
                SummaryText("    bt_bootreport.c rather than guessing again. 0xA1 is the");
                SummaryText("    HIDP header; a second byte of 0x01 or 0x02 is a report");
                SummaryText("    ID. ⛔ 5 bytes starting A1 02 is REFUSED ON PURPOSE:");
                SummaryText("    report-ID-2 and a right-button-held frame cannot be");
                SummaryText("    told apart, and guessing moves the deltas by a byte.");
            }

            /* ★ the scan filter */
            SummaryText("");
            SummaryText(" ⭐ SCANNER: DRIVABLE DEVICES ONLY");
            Row("    responders hidden",     c[kWInqFiltered]);
            Row("    show-all marker set",   c[kWShowAllDevices]);
            if (c[kWShowAllDevices] != 0) {
                SummaryText("    ⚠ FILTER OFF -- Preferences:Bluetooth Show All Devices");
                SummaryText("    exists, so phones, audio and computers are listed again.");
                SummaryText("    That is the BENCH setting: the phone is the only device");
                SummaryText("    here that re-pairs at will, and deleting the A1016 bond");
                SummaryText("    is a one-way door. Delete the marker before shipping.");
            } else if (c[kWInqFiltered] != 0) {
                SummaryText("    Devices answered that we cannot drive and they were");
                SummaryText("    kept out of the list. The tallies above still count");
                SummaryText("    them, so a scan that found only undrivable devices is");
                SummaryText("    still visibly a WORKING scan.");
            }

        Row("    ⭐ HID channel LIVE now", c[kWBhLive]);
        if (c[kWBhLive] == 0 && c[kWBhReports] != 0) {
            SummaryText("    0 with reports already received = the channel came up and");
            SummaryText("    has since gone. Sleep, out of range, or a close we did not");
            SummaryText("    ask for. Not the same as never having connected.");
        }
        Row("    ⭐ failed opens",         c[kWBhFailedOpens]);
        if (c[kWBhFailedOpens] != 0) {
            RowStatus("      last failed status", c[kWBhFailedStatus]);
            SummaryText("    ⭐⭐ A SETUP FAILURE, not a disconnect. hid_host finalizes");
            SummaryText("    the connection on this, so NO close event follows -- which");
            SummaryText("    is why 13.8's retry, armed off the close, never ran once.");
            SummaryText("    Decoded above under 'open status'. 0x69 means the keyboard");
            SummaryText("    did not answer L2CAP within its own 10 s RTX window.");
        }
        Row("    reconnects attempted",  c[kWBhReconnTried]);
        if (c[kWBhReconnTried] != 0) {
            RowHex("      its rc",           c[kWBhReconnRc], 4);
            SummaryText("    0x100 = SUCCESS. 0x10D = COMMAND_DISALLOWED, which here");
            SummaryText("    means the keyboard reconnected on its own first and beat");
            SummaryText("    us to the slot -- also a success, just not ours. Either");
            SummaryText("    way 'HID channel LIVE now' is what says it worked.");
            SummaryText("    ⚠ Capped at 3 per session: bounded so a peer that never");
            SummaryText("    answers cannot turn this into a retry storm.");
        } else if (c[kWBhFailedOpens] != 0) {
            SummaryText("    ⚠⚠ A FAILED OPEN WITH NO RETRY. That is the 13.8 defect");
            SummaryText("    exactly and should not happen on 13.9 -- the retry is");
            SummaryText("    armed from the failed open itself now. If you see this,");
            SummaryText("    the arming did not run: check the build tag first.");
        }
    }
    if (c[kWBhGate] == 0) {
        SummaryText("    THE GATE IS OFF, so our own hand-rolled listener ran and the");
        SummaryText("    sections below are the ones to read.");
    } else if (c[kWBhInit] == 0) {
        SummaryText("    ⚠⚠ hid_host_init NEVER RAN despite the gate. The stack did not");
        SummaryText("    reach BT_StackStart, so nothing below means anything.");
    } else if (c[kWBhIncoming] == 0) {
        SummaryText("    ⚠ NOTHING PAGED THE HID HOST. If Connection Requests above is");
        SummaryText("    non-zero the link came up but L2CAP never offered PSM 0x11 to");
        SummaryText("    hid_host -- which would be a registration problem, OURS. If it");
        SummaryText("    is 0 the keyboard simply did not try; wake it and rerun.");
    } else if (c[kWBhOpened] == 0) {
        SummaryText("    ⚠ ACCEPTED AND NEVER OPENED, the same wall our own listener");
        SummaryText("    hit. Read the open status: it is L2CAP's, so 0x69 is an RTX");
        SummaryText("    timeout and means the peer stopped answering again.");
        SummaryText("    ⇒ If the CANONICAL implementation fails here too, our L2CAP");
        SummaryText("    and security choices were never the cause -- which narrows");
        SummaryText("    this to the peer's LMP behaviour and is worth the run.");
    } else if (c[kWBhReports] == 0) {
        SummaryText("    ⭐⭐ THE CHANNEL OPENED -- FURTHER THAN ANY PREVIOUS RUN. No");
        SummaryText("    reports yet: read the HIDP handshake above. 0x00 is SUCCESS and");
        SummaryText("    would mean REPORT protocol was accepted, so reports should");
        SummaryText("    follow on a keypress.");
    } else {
        SummaryText("    ⭐⭐⭐⭐⭐ HID REPORTS ARRIVED. Read the report bytes below: byte 8");
        SummaryText("    is bit0 Eject, bit1 Mute, bit2 Volume Up, bit3 Volume Down --");
        SummaryText("    the Consumer page, and the answer this project has chased");
        SummaryText("    since M4. See docs/A1016-REPORT-DESCRIPTOR.md.");
    }

    /* ⚠⚠ PLACED AFTER THE hid_host VERDICT CHAIN, NOT BEFORE IT, AND THAT IS A BUG
     * FIX. v13.3 put this block immediately after the hid_host ROWS -- which sits
     * between those rows and the `if (c[kWBhGate] == 0) ... else if` chain that
     * interprets them. The log then printed this section's heading, then this section's
     * verdict, then hid_host's verdict, so hid_host's conclusion read as though it were
     * about the outgoing connect: "ACCEPTED AND NEVER OPENED" directly under "NEVER
     * ARMED".
     *
     * ⇒ That is the same defect I fixed in this file THIS MORNING, where a narrative a
     * few hundred lines from its evidence sent the user to re-install an extension that
     * was fine. A verdict must stay welded to the rows it interprets; a new section goes
     * after the whole chain, never between a chain and its data. */
    /* ★★★★★★ THE OUTGOING CONNECT (v13.3). See kHidOutgoingConnect: the call this stack
     * never made, and the test of whether the power-cycle step after pairing was ever
     * the keyboard's requirement or just the shape of a missing call. */
    SummaryText("  ★★★★★★ DID WE ASK FOR THE HID CHANNEL OURSELVES? (v13.3)");
    Row      ("        gate", c[kWHidOutGate]);
    if (c[kWHidOutGate] == 0) {
        SummaryText("    OFF in this build, so the rows below are 0 by construction and");
        SummaryText("    say nothing. Every run before v13.3 was this build.");
    } else {
        Row      ("        connects attempted",  c[kWHidOutTried]);
        RowStatus("        its rc",              c[kWHidOutRc]);
        RowHex   ("        hid_cid handed back", c[kWHidOutCid], 4);
        Row      ("        fresh bonds seen",   c[kWJustBonded]);
        if (c[kWHidOutTried] == 0 && c[kWJustBonded] == 0) {
            SummaryText("    NEVER ARMED: no new pairing happened this session, so there");
            SummaryText("    was nothing to connect to. Not a failure -- pair a device.");
        } else if (c[kWHidOutTried] == 0) {
            SummaryText("    ⚠⚠ ARMED AND NEVER FIRED. A bond was created and the connect");
            SummaryText("      still did not run, so Authentication Complete did not");
            SummaryText("      arrive with status 0 after it, or a channel was already");
            SummaryText("      open. THIS is the row that caught v13.3 hanging the");
            SummaryText("      trigger on an event that never fires.");
        } else if ((c[kWHidOutRc] & 0xFFUL) == 0 && c[kWBhOpened] > 0) {
            SummaryText("    ⭐⭐⭐⭐⭐ WE OPENED IT OURSELVES. Compare against whether the");
            SummaryText("      user had to switch the device off and on: if they did NOT,");
            SummaryText("      step 4 of the pairing instructions was never the keyboard's");
            SummaryText("      requirement -- it was the shape of this call being missing.");
            SummaryText("      Take it out of the panel and the help text.");
        } else if ((c[kWHidOutRc] & 0xFFUL) == 0) {
            SummaryText("    ⚠ ACCEPTED BUT NO CHANNEL OPENED. hid_host_connect returned");
            SummaryText("      success and HID_SUBEVENT_CONNECTION_OPENED never arrived,");
            SummaryText("      so the peer refused or ignored it. The power cycle is still");
            SummaryText("      needed and this was not the fix.");
        } else {
            SummaryText("    ⚠ THE CALL ITSELF WAS REFUSED -- our side declining, before");
            SummaryText("      anything reached the device. Read the rc.");
        }
    }
    SummaryText("  ★★★★★ DID THE CONTROLLER PERMIT SNIFF? (v9.4)");
    Row("    link-policy gate",    c[kWPolicyGate]);
    Row("    reads sent",          c[kWPolicyReadSends]);
    Row("    answers back",        c[kWPolicyReads]);
    RowStatus("    read send rc",  c[kWPolicyReadRc]);
    if (c[kWPolicyGate] == 0) {
        SummaryText("    THE GATE IS OFF, which is the shipping state. Nothing was read");
        SummaryText("    and nothing was written, so this section says nothing.");
    } else if (c[kWPolicyReads] == 0) {
        SummaryText("    ⚠ NEVER READ BACK, so the policy is STILL an assumption and");
        SummaryText("    this run cannot test the hypothesis. Check the queue below:");
        SummaryText("    a dropped or refused command would explain it.");
    } else if (((c[kWPolicyReadBack] >> 8) & 0xFFUL) != 0) {
        SummaryText("    ⚠ THE READ FAILED -- nonzero HCI status, so the settings byte");
        SummaryText("    beside it means nothing. Reachability of this whole theory is");
        SummaryText("    unresolved; do not read a verdict out of it.");
    } else {
        unsigned long pol = c[kWPolicyReadBack] & 0xFFFFUL;
        RowHex("    ⭐ policy AS FOUND", pol, 4);
        if ((pol & 0x0004UL) != 0) {
            SummaryText("    ⚠⚠ SNIFF WAS ALREADY PERMITTED (bit 2 set) BEFORE we wrote");
            SummaryText("    anything. So a forbidden sniff mode is NOT why the peer");
            SummaryText("    ignores LMP, and the link-policy theory is DEAD. Do not");
            SummaryText("    spend another build on it -- read MODE CHANGE below for");
            SummaryText("    whether the keyboard ever actually asked for sniff.");
        } else {
            SummaryText("    ⭐⭐⭐ SNIFF WAS FORBIDDEN (bit 2 clear). That is a real");
            SummaryText("    finding on its own: every link this project has ever made");
            SummaryText("    ran with sniff disallowed, so a keyboard that needs it was");
            SummaryText("    refused by our controller. The write below permits it.");
        }
        if ((pol & 0x0001UL) != 0)
            SummaryText("      (bit 0 role switch was also permitted)");
    }
    Row("    writes sent",          c[kWPolicyWriteSends]);
    RowStatus("    write send rc",   c[kWPolicyWriteRc]);

    /* ★★★★★ v9.4: MODE CHANGE -- did the keyboard ever ASK for sniff? */
    SummaryText("  ★★★★★ DID THE KEYBOARD ASK FOR SNIFF? MODE CHANGE (v9.4)");
    Row("    mode changes",        c[kWModeChanges]);
    RowStatus("    last mode",     c[kWModeLast]);
    Row("    at (ms)",             c[kWModeLastMs]);
    if (c[kWModeChanges] == 0) {
        SummaryText("    NO MODE CHANGE EVER ARRIVED. Either the keyboard never asked");
        SummaryText("    for a low-power mode, or it asked and the controller refused");
        SummaryText("    without telling us -- a refusal is an LMP matter and does NOT");
        SummaryText("    generate this event. So 0 does NOT clear the theory.");
    } else {
        unsigned long m = c[kWModeLast] & 0xFFUL;
        if (m == 2)      SummaryText("    ⭐⭐⭐ MODE 2 = SNIFF. The link DID enter sniff.");
        else if (m == 0) SummaryText("    mode 0 = ACTIVE, so it came back out of a mode.");
        else if (m == 1) SummaryText("    mode 1 = HOLD.");
        else if (m == 3) SummaryText("    mode 3 = PARK.");
        SummaryText("    ⇒ Compare the ms against link up/down: a mode change BEFORE");
        SummaryText("    the LMP timeout means the low-power path was working.");
    }

    /* ★★★★★ v9.4: the queue, because a command that never went out explains nothing
     * and must not be mistaken for a controller that refused it. */
    SummaryText("  ★★★★★ THE PER-LINK COMMAND QUEUE (v9.4)");
    Row("    enqueued",            c[kWLinkCmdEnq]);
    Row("    sent",                c[kWLinkCmdSent]);
    Row("    refused (retried)",   c[kWLinkCmdRefused]);
    Row("    ⚠ DROPPED",           c[kWLinkCmdDropped]);
    Row("    still queued",        c[kWLinkCmdDepth]);
    if (c[kWLinkCmdDropped] != 0) {
        SummaryText("    ⚠⚠ COMMANDS WERE DROPPED after exhausting their retries, so at");
        SummaryText("    least one per-link command never reached the controller and");
        SummaryText("    whatever it was measuring is UNMEASURED this run.");
    } else if (c[kWLinkCmdEnq] != 0 && c[kWLinkCmdSent] == c[kWLinkCmdEnq]) {
        SummaryText("    ⭐ EVERY QUEUED COMMAND WENT OUT. This is what v9.3 could not");
        SummaryText("    do: four commands per link against one command credit, with");
        SummaryText("    none starved. Refusals above were retried, not lost.");
    }

    SummaryText("  ★★★★★ WAS THE LMP VERDICT UNMASKED? (v9.3)");
    Row("    supervision gate",    c[kWSupGate]);
    Row("    stretches attempted", c[kWSupTimeoutTried]);
    RowStatus("    its send rc",   c[kWSupTimeoutRc]);
    RowHex("    for handle",       c[kWSupTimeoutHandle], 4);
    if (c[kWSupGate] == 0) {
        SummaryText("    THE STRETCH IS OFF IN THIS BUILD, which is the shipping state.");
        SummaryText("    An 0x02 below therefore CANNOT be told from an LMP timeout the");
        SummaryText("    20 s supervision window hid. Not a fault -- just not decisive.");
    } else if (c[kWSupTimeoutTried] == 0) {
        SummaryText("    ⚠ NEVER ATTEMPTED, so the window is still 20 s and an 0x02");
        SummaryText("    below is as ambiguous as before. No link came up, or the");
        SummaryText("    Connection Complete status was non-zero.");
    } else if ((c[kWSupTimeoutRc] & 0xFFUL) != 0) {
        SummaryText("    ⚠ THE COMMAND WAS REFUSED, so the window is still 20 s and the");
        SummaryText("    verdict below is still masked. Check the command credit.");
    } else if (c[kWLinkLifeMs] != 0 && c[kWLinkLifeMs] < 25000UL) {
        SummaryText("    ⚠⚠ SENT AND ACCEPTED, YET THE LINK STILL DIED UNDER 25 s. So the");
        SummaryText("    controller did not honour 40 s, or the KEYBOARD's own");
        SummaryText("    supervision timeout ended it -- which is itself a finding: the");
        SummaryText("    peer is timing us out, not the reverse.");
    } else {
        SummaryText("    ⭐ THE WINDOW IS OPEN past the 30 s LMP timer, so the status");
        SummaryText("    below is a real verdict: 0x22 blames the keyboard, 0x02 still");
        SummaryText("    means the controller never ran the challenge at all.");
    }

    SummaryText("  ★★★★★ EVERY AUTHENTICATION COMPLETE (v9.2)");
    Row("    completes recorded", c[kWAuthRingCount]);
    if (c[kWAuthRingCount] == 0) {
        SummaryText("    ⚠ NONE RECORDED. If Auth Completes above is non-zero this ring");
        SummaryText("    is broken; if it is 0 the controller never answered at all.");
    } else {
        int qi;
        for (qi = 0; qi < (int)c[kWAuthRingCount] && qi < kAuthRingSlots; qi++) {
            unsigned long w = c[kWAuthRing0 + qi];
            Str255 l; l[0] = 0;
            PStrCat(l, "      #");
            PStrCatNum(l, (long)qi);
            if ((w & 0x1000000UL) == 0) {
                PStrCat(l, "  (slot never filled)");
                Summary(l);
                continue;
            }
            PStrCat(l, "  status 0x");
            PStrCatHexN(l, (w >> 16) & 0xFFUL, 2);
            PStrCat(l, "  handle 0x");
            PStrCatHexN(l, w & 0xFFFFUL, 4);
            PStrCat(l, "  at ");
            PStrCatNum(l, (long)c[kWAuthRingMs0 + qi]);
            PStrCat(l, " ms");
            Summary(l);
            {
                unsigned long st = (w >> 16) & 0xFFUL;
                if (st == 0x00)
                    SummaryText("         ⭐ SUCCESS -- this link WAS authenticated.");
                else if (st == 0x02)
                    SummaryText("         0x02 UNKNOWN CONNECTION IDENTIFIER: the link was"
                                " already gone.");
                else if (st == 0x05)
                    SummaryText("         0x05 AUTHENTICATION FAILURE: the keys do not"
                                " match.");
                else if (st == 0x06)
                    SummaryText("         0x06 PIN OR KEY MISSING: no usable key at one"
                                " end.");
                else if (st == 0x22)
                    SummaryText("         0x22 LMP RESPONSE TIMEOUT: the peer never"
                                " answered the challenge.");
            }
        }
        SummaryText("    ⇒ Compare each ms against link up/down below. An 0x02 AFTER the");
        SummaryText("    link went down is the controller closing out a request whose");
        SummaryText("    link expired -- which says the authentication never ran, NOT");
        SummaryText("    that the key was refused. A 0x05 or 0x06 would be a key");
        SummaryText("    problem, and a 0x22 would put it squarely on the keyboard.");
    }

    SummaryText("  ★★★★★ THE LINK'S LIFETIME - reason 8 is a SUPERVISION TIMEOUT");
    Row("    link up at (ms)",     c[kWLinkUpMs]);
    Row("    link down at (ms)",   c[kWLinkDownMs]);
    Row("    ⭐ it lived (ms)",     c[kWLinkLifeMs]);
    if (c[kWLinkUpMs] == 0) {
        SummaryText("    ⚠ NO LINK EVER CAME UP, so there is no lifetime to read and");
        SummaryText("    nothing below is about this question.");
    } else if (c[kWLinkDownMs] == 0) {
        SummaryText("    ⭐ THE LINK WAS STILL UP when this log was written. That is a");
        SummaryText("    different run from every previous one -- read the channel rows.");
    } else if (c[kWLinkLifeMs] >= 15000UL) {
        SummaryText("    ⇒ FIFTEEN SECONDS OR MORE. That is the supervision window, so");
        SummaryText("    the link was NOT killed -- the peer stopped answering at the");
        SummaryText("    baseband and the timeout did its job. Whatever went wrong");
        SummaryText("    happened in the NEGOTIATION, not in the link setup, and it had");
        SummaryText("    the full window to succeed.");
    } else if (c[kWLinkLifeMs] <= 3000UL) {
        SummaryText("    ⚠⚠ THREE SECONDS OR LESS -- far too short for a supervision");
        SummaryText("    timeout. Something ACTIVELY tore this link down, and reason 8");
        SummaryText("    is then the controller's summary rather than the cause. Look");
        SummaryText("    for a command we sent just before it.");
    } else {
        SummaryText("    ⇒ Between 3 and 15 seconds: neither a clean supervision");
        SummaryText("    timeout nor an obvious teardown. Read the ACL bytes below.");
    }
    Row("    0x041B sent at (ms)",   c[kWRrsfSentMs]);
    Row("    its answer at (ms)",    c[kWRrsfDoneMs]);
    if (c[kWRrsfSentMs] != 0 && c[kWLinkDownMs] != 0) {
        if (c[kWRrsfDoneMs] > c[kWLinkDownMs])
            SummaryText("    ⇒ the features answer arrived AFTER the link died, which is");
        else
            SummaryText("    ⇒ the features answer arrived BEFORE the link died, which is");
        SummaryText("      how its status 0x02 should be read.");
    }

    /* ★★★★★ WHAT THE PEER ACTUALLY SAID. Raw, unfiltered, decode by eye:
     *   [0..1] handle+flags LE   [2..3] ACL len LE
     *   [4..5] L2CAP len LE      [6..7] CID LE       [8..] payload
     * CID 0x0001 is L2CAP SIGNALLING -- the traffic that decides whether a channel
     * opens. On CID 1 the payload starts with a code byte: 0x02 Connection Request,
     * 0x03 Connection Response, 0x04 Configuration Request, 0x05 Configuration
     * Response, 0x01 Command Reject. */
    SummaryText("  ★★★★★ WHAT THE PEER SENT US - raw ACL, first packets");
    Row("    ACL packets captured", c[kWAclCapCount]);
    if (c[kWAclCapCount] == 0) {
        SummaryText("    ⚠ NOTHING CAPTURED. If ACL read completions above is also 0");
        SummaryText("    the peer really sent nothing; if it is non-zero this capture");
        SummaryText("    is broken and the run says nothing about the peer.");
    } else {
        int ai, aw;
        for (ai = 0; ai < (int)c[kWAclCapCount] && ai < kAclCapSlots; ai++) {
            Str255 l; l[0] = 0;
            PStrCat(l, "      #");
            PStrCatNum(l, (long)ai);
            PStrCat(l, "  ");
            for (aw = 0; aw < kAclCapBytes / 4; aw++) {
                unsigned long v = c[kWAclCap0 + ai * (kAclCapBytes / 4) + aw];
                PStrCatHexN(l, (v >> 24) & 0xFF, 2); PStrCatCh(l, ' ');
                PStrCatHexN(l, (v >> 16) & 0xFF, 2); PStrCatCh(l, ' ');
                PStrCatHexN(l, (v >>  8) & 0xFF, 2); PStrCatCh(l, ' ');
                PStrCatHexN(l,  v        & 0xFF, 2); PStrCatCh(l, ' ');
            }
            Summary(l);
            /* Decode the fields that matter, so nobody has to count nibbles.
             *
             * ⚠⚠ THE FIRST VERSION OF THIS GOT ALL THREE FIELDS WRONG, caught by the
             * pre-staging falsification pass rather than by a run. It read the CID from
             * bytes 4-5 instead of 6-7, took the signalling code from byte 6 instead of
             * 8, and byte-swapped handle+flags. A misdecoded CID is worse than none: it
             * would have labelled channel data as signalling and sent the next session
             * chasing an L2CAP conversation that never happened.
             *
             * ⇒ ONE helper, derived rather than hand-shifted. Bytes are packed 4 per
             * word with byte 0 in the TOP octet, so
             *     byte(n) = (word[n/4] >> (24 - 8*(n%4))) & 0xFF
             * and the wire layout is
             *     [0..1] handle+flags LE  [2..3] ACL len LE
             *     [4..5] L2CAP len LE     [6..7] CID LE      [8] signalling code */
            {
#define ACLB(n) ((c[kWAclCap0 + ai * (kAclCapBytes / 4) + ((n) / 4)] \
                  >> (24 - 8 * ((n) % 4))) & 0xFFUL)
                unsigned long hf   = ACLB(0) | (ACLB(1) << 8);
                unsigned long cid  = ACLB(6) | (ACLB(7) << 8);
                unsigned long code = ACLB(8);
                Str255 d; d[0] = 0;
                PStrCat(d, "         handle+flags 0x");
                PStrCatHexN(d, hf, 4);
                PStrCat(d, "  CID 0x");
                PStrCatHexN(d, cid, 4);
                if (cid == 1) {
                    PStrCat(d, "  SIGNALLING code 0x");
                    PStrCatHexN(d, code, 2);
                    if (code == 0x01) PStrCat(d, " COMMAND REJECT");
                    else if (code == 0x02) PStrCat(d, " Conn Request");
                    else if (code == 0x03) PStrCat(d, " Conn Response");
                    else if (code == 0x04) PStrCat(d, " Config Request");
                    else if (code == 0x05) PStrCat(d, " Config Response");
                }
                Summary(d);
#undef ACLB
            }
        }
        SummaryText("    ⚠ The CID is the whole point: 0x0001 is signalling, anything");
        SummaryText("    else is channel data. A COMMAND REJECT (code 0x01) would say");
        SummaryText("    the peer refused something we sent and name what.");
    }

    /* ★★★★★ v9.1: AND WHAT WE SENT BACK. Read this against the section above.
     *
     * ⚠⚠ THIS SECTION EXISTS BECAUSE ITS ABSENCE WAS THE BLIND SPOT. The inbound
     * capture arrived in v8.7 and its mirror did not, so five driver versions
     * analysed the peer's silence and none examined our own answer -- while
     * `ACL packets sent 6` with 6 completions and status 0 said all along that our
     * bytes were reaching the wire. If the peer receives a well-formed SUCCESS and
     * stops, that is a fact about the keyboard. If it receives PENDING, or a wrong
     * DCID, it is being patient and the fault is entirely ours. */
    SummaryText("  ★★★★★ WHAT WE SENT BACK - raw ACL, first packets OUT");
    Row("    ACL packets captured", c[kWAclTxCapCount]);
    if (c[kWAclTxCapCount] == 0) {
        SummaryText("    ⚠ NOTHING CAPTURED. If ACL packets sent above is non-zero this");
        SummaryText("    capture is broken; if it is 0 we genuinely never answered.");
    } else {
        int ai, aw;
        for (ai = 0; ai < (int)c[kWAclTxCapCount] && ai < kAclCapSlots; ai++) {
            Str255 l; l[0] = 0;
            PStrCat(l, "      #");
            PStrCatNum(l, (long)ai);
            PStrCat(l, "  ");
            for (aw = 0; aw < kAclTxCapBytes / 4; aw++) {
                unsigned long v = c[kWAclTxCap0 + ai * (kAclTxCapBytes / 4) + aw];
                PStrCatHexN(l, (v >> 24) & 0xFF, 2); PStrCatCh(l, ' ');
                PStrCatHexN(l, (v >> 16) & 0xFF, 2); PStrCatCh(l, ' ');
                PStrCatHexN(l, (v >>  8) & 0xFF, 2); PStrCatCh(l, ' ');
                PStrCatHexN(l,  v        & 0xFF, 2); PStrCatCh(l, ' ');
            }
            Summary(l);
            /* Same derived-byte helper as the inbound decode, for the same reason:
             * the hand-shifted version of that got all three fields wrong. */
            {
#define ACLB(n) ((c[kWAclTxCap0 + ai * (kAclTxCapBytes / 4) + ((n) / 4)] \
                  >> (24 - 8 * ((n) % 4))) & 0xFFUL)
                unsigned long hf   = ACLB(0) | (ACLB(1) << 8);
                unsigned long cid  = ACLB(6) | (ACLB(7) << 8);
                unsigned long code = ACLB(8);
                Str255 d; d[0] = 0;
                PStrCat(d, "         handle+flags 0x");
                PStrCatHexN(d, hf, 4);
                PStrCat(d, "  CID 0x");
                PStrCatHexN(d, cid, 4);
                PStrCat(d, "  full len ");
                PStrCatNum(d, (long)c[kWAclTxLen0 + ai]);
                Summary(d);
                if (cid == 1) {
                    Str255 s; s[0] = 0;
                    PStrCat(s, "         SIGNALLING code 0x");
                    PStrCatHexN(s, code, 2);
                    if (code == 0x01) PStrCat(s, " COMMAND REJECT");
                    else if (code == 0x02) PStrCat(s, " Conn Request");
                    else if (code == 0x03) PStrCat(s, " Conn Response");
                    else if (code == 0x04) PStrCat(s, " Config Request");
                    else if (code == 0x05) PStrCat(s, " Config Response");
                    Summary(s);
                    /* ⭐ A Connection Response is the one we care about. Its payload
                     * after the 4-byte signalling header is DCID, SCID, RESULT,
                     * STATUS -- all little-endian 16-bit:
                     *     [9..10] id+len   [12..13] DCID  [14..15] SCID
                     *     [16..17] RESULT  [18..19] STATUS
                     * ⚠ RESULT is at byte 16, which is PAST a 16-byte capture. So the
                     * result is reported ONLY when the capture reaches it; saying
                     * "cannot see it" is correct and a guess would not be. */
                    if (code == 0x03) {
                        if (kAclTxCapBytes >= 18) {
                            unsigned long res = ACLB(16) | (ACLB(17) << 8);
                            Str255 r; r[0] = 0;
                            PStrCat(r, "         RESULT 0x");
                            PStrCatHexN(r, res, 4);
                            if (res == 0x0000)      PStrCat(r, " SUCCESS");
                            else if (res == 0x0001) PStrCat(r, " ⚠⚠ PENDING");
                            else if (res == 0x0002) PStrCat(r, " PSM NOT SUPPORTED");
                            else if (res == 0x0003) PStrCat(r, " SECURITY BLOCK");
                            else if (res == 0x0004) PStrCat(r, " NO RESOURCES");
                            Summary(r);
                            if (res == 0x0001) {
                                SummaryText("         ⚠⚠ PENDING obliges us to send a FINAL");
                                SummaryText("         response later. If security never resolves");
                                SummaryText("         we never send it, the peer waits, and the");
                                SummaryText("         silence is CORRECT behaviour on its part.");
                            }
                        } else {
                            SummaryText("         RESULT is at byte 16, past this capture --");
                            SummaryText("         raise kAclCapBytes to read it.");
                        }
                    }
                }
#undef ACLB
            }
        }
        SummaryText("    ⇒ Compare the DCID/SCID here against the SCID the peer asked");
        SummaryText("    with above. A response naming the wrong channel would be");
        SummaryText("    ignored, and would look exactly like a silent peer.");
    }

    SummaryText("  ★★★★★ REPORT PROTOCOL - can this keyboard leave boot mode?");
    RowStatus("    interrupt listen rc",  c[kWHidIntrListenRc]);
    Row("    control channels open",      c[kWHidOpened]);
    Row("    SET_PROTOCOL sends",         c[kWHidSetProtoTried]);
    RowStatus("    its l2cap_send rc",    c[kWHidSetProtoRc]);
    if (c[kWHidSetProtoTried] == 0) {
        if (c[kWHidOpened] == 0) {
            SummaryText("    ⇒ NO CONTROL CHANNEL EVER OPENED, so SET_PROTOCOL was");
            SummaryText("      never reachable. This is NOT a result about report");
            SummaryText("      protocol -- it is the OLD blocker. Read the M4 rows and");
            SummaryText("      the disconnect reason above.");
        } else if ((c[kWHidSetProtoRc] & 0xFFUL) == 0xFE) {
            SummaryText("    ⚠ ARMED BUT NEVER SENT: the channel opened and");
            SummaryText("      CAN_SEND_NOW never fired. A BTstack flow problem, not a");
            SummaryText("      statement about the keyboard.");
        }
    } else if ((c[kWHidSetProtoRc] & 0xFFUL) != 0) {
        SummaryText("    ⚠ l2cap_send REFUSED it. Nothing reached the keyboard, so the");
        SummaryText("      handshake row below says nothing either.");
    } else if (c[kWHidHandshake] == 0) {
        SummaryText("    ⚠ SENT, AND NO HANDSHAKE CAME BACK. The keyboard heard a");
        SummaryText("      SET_PROTOCOL and did not answer -- which is what a device");
        SummaryText("      does when it wants an ENCRYPTED link first. That would make");
        SummaryText("      LEVEL_0 the wrong half of the trade, not the right one.");
    } else {
        Row("    HIDP handshake",         c[kWHidHandshake] & 0xFF);
        switch (c[kWHidHandshake] & 0xFFUL) {
        case 0x00:
            SummaryText("    ⭐⭐⭐⭐⭐ SUCCESSFUL. THE KEYBOARD IS IN REPORT PROTOCOL.");
            SummaryText("      That is the Consumer page unlocked -- volume and eject");
            SummaryText("      are in its native reports, and a battery-strength");
            SummaryText("      feature report becomes readable with GET_REPORT.");
            SummaryText("      ⇒ Check the interrupt-channel rows below: reports should");
            SummaryText("      now be ARRIVING, and LONGER than the 8-byte boot form.");
            break;
        case 0x03:
            SummaryText("    ⛔ ERR_UNSUPPORTED. This keyboard cannot do report");
            SummaryText("      protocol at all, and that CLOSES the Consumer-page route");
            SummaryText("      for good -- not a bug, a property of the device.");
            break;
        case 0x01:
            SummaryText("    ⚠ NOT_READY. Transient; the peer was not able to answer");
            SummaryText("      yet. Worth one retry before drawing any conclusion.");
            break;
        default:
            SummaryText("    ⚠ An error handshake. 0x04 invalid parameter, 0x0E");
            SummaryText("      unknown, 0x0F fatal. The keyboard rejected the request");
            SummaryText("      rather than ignoring it, which is still informative.");
            break;
        }
    }
    Row("    interrupt channels open",    c[kWHidIntrOpened]);
    Row("    packets on CONTROL",         c[kWHidCtrlDataPkts]);
    Row("    packets on INTERRUPT",       c[kWHidIntrDataPkts]);
    if (c[kWHidIntrDataPkts] != 0) {
        SummaryText("    ⭐⭐ REPORTS ARE ARRIVING ON THE INTERRUPT CHANNEL. Compare the");
        SummaryText("      first-report bytes below against 8: a LONGER report, or one");
        SummaryText("      with a leading report ID, is the native form rather than the");
        SummaryText("      boot form -- which is the Consumer page in reach.");
    }

    SummaryText("  ⭐⭐ DID THE A1016 ACTUALLY SEND US ANYTHING?");
    Row("L2CAP data packets",    c[kWHidDataPkts]);
    if (c[kWHidDataPkts] == 0) {
        SummaryText("    ZERO. No HID report ever arrived. If a control channel opened");
        SummaryText("    above, the peer connected and then said nothing -- which for a");
        SummaryText("    HID device usually means it is waiting for SET_PROTOCOL, the");
        SummaryText("    step 2 this build does not send yet.");
    } else {
        short k;
        Str255 line;
        Row("  last payload bytes",  c[kWHidLastDataLen]);
        Row("  first payload bytes", c[kWHidFirstFull]);
        line[0] = 0;
        PStrCat(line, "    first payload: ");
        for (k = 0; k < (short)c[kWHidFirstLen] && k < kHidCapBytes; k++) {
            unsigned long w = c[kWHidFirstData + (k / 4)];
            PStrCatHexN(line, (w >> (8 * (3 - (k & 3)))) & 0xFF, 2);
            PStrCatCh(line, ' ');
        }
        Summary(line);
        /* ⚠ The RAW bytes are printed above the decoder's verdict on purpose. The
         * decoder has 32 passing host tests and no hardware mileage; if it rejects a
         * real report, these bytes are the only way to find out why. */
        /* ⚠ NAMED FOR THE FUNCTION THAT ACTUALLY RUNS. This said
         * "BT_DecodeBootKeyboard" for one build after the driver switched to
         * BT_DecodeHidReport -- a label naming the wrong function is the same drift
         * class as a version stamp naming the wrong build.
         *
         * ⚠⚠ AND NOT RowStatus, BECAUSE ITS POLARITY IS BACKWARDS FOR THIS WORD.
         * RowStatus exists for HCI status codes, where 0 is success and nonzero names a
         * failure. This word is 0x100 | (1 = the decoder ACCEPTED the report), so
         * v10.3's log read "BT_DecodeHidReport 1 <== NONZERO, this is the failure" over
         * 45 reports out of 45 decoded perfectly. A diagnostic that calls the success
         * state a failure has cost this project three separate reads already; the fix
         * is to say what the value MEANS rather than reuse a formatter that assumes. */
        if ((c[kWHidDecodeRc] & 0x100UL) == 0) {
            SummaryText("BT_DecodeHidReport         NEVER RAN");
            SummaryText("    ⚠⚠ The decoder was not reached at all. That is a WIRING");
            SummaryText("      fault, not a decode failure -- v10.2 put the call in the");
            SummaryText("      l2cap handler, which is dormant while BTstack's hid_host");
            SummaryText("      owns the channels. Both paths now call BhRecordReport.");
        } else if ((c[kWHidDecodeRc] & 0xFFUL) != 0) {
            SummaryText("BT_DecodeHidReport         ACCEPTED the last report");
        } else {
            SummaryText("BT_DecodeHidReport         ⚠ REJECTED the last report");
            SummaryText("    ⚠ Read the raw bytes above against the accepted forms:");
            SummaryText("      11 = A1 01 + 9 (report protocol), 10 = 01 + 9,");
            SummaryText("      9 = A1 + 8 (boot), 8 = bare boot. Anything else is");
            SummaryText("      refused deliberately rather than half-decoded.");
        }
        Row("  reports accepted",  c[kWHidDecodeOk]);
        Row("  rollovers seen",    c[kWHidRollovers]);
        if (c[kWHidDecodeOk] != 0) {
            RowHex("  last modifiers", c[kWHidLastMods], 2);
            Row("  keys held",         c[kWHidLastNKeys]);
            RowHex("  first usage ID", c[kWHidLastKey0], 2);
            Row   ("  ⭐ reports that HELD a key", c[kWHidKeyRpts]);
            /* ⚠⚠ THIS BRANCH USED TO CLAIM A KEYSTROKE FROM A SUCCESSFUL DECODE ALONE,
             * and report protocol made that wrong. An all-zero RELEASE report decodes
             * perfectly with nKeys 0, so the decode counter goes nonzero as soon as the
             * keyboard says anything -- including nothing. Claim only what was
             * measured. [[feedback_test_content_not_return_codes]] */
            if (c[kWHidKeyRpts] != 0) {
                SummaryText("    ⭐⭐⭐ A REAL KEYSTROKE, DECODED BY OUR OWN STACK. The");
                SummaryText("      keyboard talks to us. That is the first time anything");
                SummaryText("      in this project has read a key from the A1016 without");
                SummaryText("      the card's on-chip stack doing the work.");
            } else {
                SummaryText("    ⚠ REPORTS DECODED, BUT NO KEY WAS HELD IN ANY OF THEM.");
                SummaryText("      That is not a fault: an idle or released report is");
                SummaryText("      all zeros and decodes correctly. It means the decoder");
                SummaryText("      and the channel work and no ordinary key was pressed");
                SummaryText("      while this ran. Press a letter and run again.");
            }
        } else {
            SummaryText("    ⚠ Data arrived and the decoder REJECTED it. Read the raw");
            SummaryText("      bytes above against bt_bootreport.h's framing: 0xA1 then");
            SummaryText("      8 bytes is the framed form, a bare 8 bytes is also");
            SummaryText("      accepted, and anything else is not a boot report.");
        }
    }

    /* ★★★ WHICH BRANCH OF gap_request_security_level RAN.
     *
     * ⚠⚠ THIS SECTION DELIBERATELY DOES NOT PICK AN ANSWER. Authentication_Requested
     * has never been sent, across five runs and four confident theories, every one of
     * them wrong: a missing 0x0B (it arrives, late), LEVEL_0 as the cure (it is the
     * poison), an early injection (the creation path handles that), command credit
     * (restored by session end). Each was refuted by the next measurement.
     *
     * ⇒ What follows is the readings and the decision table that maps them onto the
     * three branches. The reader applies it. A verdict printed here would be a fifth
     * theory dressed as a result. */
    SummaryText("  ⭐⭐ SECURITY BRANCH - why Authentication_Requested never goes out");
    if (c[kWLiveSamples] == 0 && c[kWSecEvtCount] == 0 && c[kWHandlerTotal] == 0) {
        SummaryText("    (not instrumented -- driver older than v7.0)");
    } else {
        Row("live-link samples",       c[kWLiveSamples]);
        /* ⚠⚠ THE HANDLE THE SAMPLER SAW, printed FIRST because v7.0's run turned on
         * exactly this gap: live-link samples 0 while the link was up and the
         * synthetic injection had used the same handle. A count of zero could not
         * distinguish never-set from cleared-early from sampled-before-the-link.
         * These four rows make those different readings. */
        Row("  pump-timer firings",    c[kWTimerAtEnd]);
        RowHex("  handle seen (transport)", c[kWSampHandle], 4);
        RowHex("    max ever seen",         c[kWSampHandleMax], 4);
        RowHex("  handle seen (btstack)",   c[kWSampHciHandle], 4);
        RowHex("    max ever seen",         c[kWSampHciMax], 4);
        if (c[kWSampHandleMax] != c[kWSampHciMax]) {
            SummaryText("      ⚠⚠ THE TWO FILES DISAGREE about the connection handle.");
            SummaryText("        gLastConnHandle lives in hci_transport_os9.c and");
            SummaryText("        gHciConnHandle in bt_btstack.c. A mismatch here IS the");
            SummaryText("        finding and everything below it is suspect.");
        }
        if (c[kWLiveSamples] == 0 && c[kWSampHandleMax] != 0) {
            SummaryText("      ⚠ Zero samples but a handle WAS seen at some point, so");
            SummaryText("        the timer stopped or the link arrived after its last");
            SummaryText("        firing. Compare the firing count above.");
        }
        RowStatus("GAP_EVENT_SECURITY_LEVEL", c[kWSecEvtCount] ? 0x100UL : 0UL);
        Row("  arrivals",              c[kWSecEvtCount]);
        if (c[kWSecEvtCount] != 0) {
            Row("  its level",         c[kWSecEvtLevel]  & 0xFF);
            Row("  its status",        c[kWSecEvtStatus] & 0xFF);
            RowHex("  its handle",     c[kWSecEvtHandle], 4);
        }
        SummaryText("    sampled while a link was UP:      first   last");
        RowPair("gap_security_level >= 2",
                (c[kWSecLvlFirst] & 0xFF) >= 2, (c[kWSecLvlLast] & 0xFF) >= 2);
        RowPair("remote features available",
                (c[kWRemFeatFirst] & 0xFF) != 0, (c[kWRemFeatLast] & 0xFF) != 0);
        Row("  gap_security_level, last", c[kWSecLvlLast] & 0xFF);

        SummaryText("    DECISION TABLE -- apply it, do not take a verdict from here:");
        SummaryText("      event ARRIVED, status 0, level 0");
        SummaryText("        => the `requested <= current` early return fired, so the");
        SummaryText("           level L2CAP asked for was NOT above the current one.");
        SummaryText("           Read the level row: if it is 0 then LEVEL_0 was what");
        SummaryText("           got requested, and the caller is the thing to look at.");
        SummaryText("      event ABSENT, remote features available YES");
        SummaryText("        => the synthetic 0x0B held, so l2cap could reach");
        SummaryText("           gap_request_security_level. Absent event then means the");
        SummaryText("           `authentication_active` branch: requested_security_level");
        SummaryText("           was already above LEVEL_0 before the call.");
        SummaryText("      event ABSENT, remote features available NO");
        SummaryText("        => the flag did NOT survive. hci_connection_init zeroes");
        SummaryText("           bonding_flags on create AND ON RECONNECT, so an");
        SummaryText("           injection before a re-init is wiped. Inject later.");
        SummaryText("      live-link samples 0");
        SummaryText("        => no link was ever up when the timer looked. NOT a");
        SummaryText("           result about security at all.");
    }

    /* ⚠ EVERY code that reached OUR handler, including the ones BTstack invents. The
     * transport's ring cannot show these -- it sees controller packets only, on
     * purpose -- so this is the only view of GAP_EVENT_SECURITY_LEVEL (0xD8) and the
     * L2CAP_EVENT_* family actually arriving. */
    if (c[kWHandlerTotal] != 0) {
        short n;
        Row("events reaching our handler", c[kWHandlerTotal]);
        SummaryText("    last 16, oldest first  (code / len / byte2):");
        for (n = 0; n < kHandlerRingLen; n++) {
            unsigned long e =
                c[kWHandlerRing + ((c[kWHandlerIdx] + n) & (kHandlerRingLen - 1))];
            unsigned long code = (e >> 16) & 0xFF;
            Str255 line;
            if (e == 0) continue;
            line[0] = 0;
            PStrCat(line, "      0x"); PStrCatHexN(line, code, 2);
            PStrCat(line, "  len ");   PStrCatNum(line, (long)((e >> 8) & 0xFF));
            PStrCat(line, "  b2 ");    PStrCatNum(line, (long)(e & 0xFF));
            PStrCat(line, "  ");
            if (code == 0xD8)      PStrCat(line, "*** GAP_EVENT_SECURITY_LEVEL ***");
            /* ⚠ VERIFIED IN btstack_defines.h, NOT REMEMBERED. My first attempt had
             * all three of these wrong -- 0x70 is OPENED, not INCOMING -- which in a
             * table whose only job is to stop hex going misread would have been worse
             * than leaving them unnamed. */
            else if (code == 0x70) PStrCat(line, "L2CAP_EVENT_CHANNEL_OPENED");
            else if (code == 0x71) PStrCat(line, "L2CAP_EVENT_CHANNEL_CLOSED");
            else if (code == 0x72) PStrCat(line, "*** L2CAP_EVENT_INCOMING_CONNECTION ***");
            else if (code == 0x6E) PStrCat(line, "TRANSPORT_PACKET_SENT (ours)");
            else if (code == 0x60) PStrCat(line, "BTSTACK_EVENT_STATE");
            else if (code == 0x66) PStrCat(line, "BTSTACK_EVENT_SCAN_MODE_CHANGED");
            else                   PStrCat(line, EventName(code));
            Summary(line);
        }
    }

    /* ★★★★ THE GOAL, IN FIVE ROWS. Everything else in this log is infrastructure;
     * this is whether a device can be paired FROM OS 9 and the key landed in the
     * card's own store so its proxy stack can use it. */
    SummaryText("  ⭐⭐⭐ M3.7 PAIRING FROM OS 9 - the goal");
    Row("Pair requested",            c[kWPairAsked]);
    if (c[kWPairAsked] == 0) {
        SummaryText("    Never asked. Select a device in the panel and click Pair.");
        SummaryText("    ⚠ NOT a result -- nothing was attempted this session.");
    } else {
        RowBDAddr("  with",              c[kWPairAddrHi], c[kWPairAddrLo]);
        /* ⚠ WHICH PATH RAN. v7.5 proved Create_Connection is rejected 0x12 for an
         * address we already hold a link to, so v7.6 authenticates the existing link
         * instead. Without this row the two paths' failures would look identical. */
        Row("link open at decision", c[kWLinkWasUp] & 0xFF);
        if (c[kWPairPath] == 1) {
            SummaryText("    PATH: authenticated the EXISTING link (0x0411)");
        } else if (c[kWPairPath] == 2) {
            SummaryText("    PATH: no link held, so Create_Connection (0x0405)");
            SummaryText("    ⚠ RETIRED IN v8.3. Every Create_Connection this card was");
            SummaryText("      ever sent was refused (0x12, then 0x0C), and the refusal");
            SummaryText("      then made it refuse Inquiry too. A driver still taking");
            SummaryText("      this path is older than v8.3.");
        } else if (c[kWPairPath] == 3) {
            SummaryText("    ⭐ PATH: ARMED AND WAITING -- nothing was sent.");
            SummaryText("      No link was held, so the driver did NOT try to page the");
            SummaryText("      device. Switch the device on now: the arm fires on its");
            SummaryText("      Connection Complete and sends Authentication_Requested,");
            SummaryText("      which is the path the Pixel actually paired through.");
            SummaryText("      Check 'arm fired on inbound' below -- 0 means the device");
            SummaryText("      has not paged us yet, NOT that anything failed.");
        } else {
            SummaryText("    PATH: not recorded -- driver older than v7.6");
        }
        RowStatus("  send / bonding rc", c[kWPairRc]);
        /* ⚠ HEX, because the explanatory text below talks in hex and this row used to
         * print DECIMAL -- so a status of 0x12 appeared as "18" directly above a list
         * naming 0x06, 0x05 and 0x04. That invites exactly the misreading a status row
         * exists to prevent. */
        if (c[kWCreateConnLen] != 0) {
            short k;
            Str255 line;
            SummaryText("  ⭐ Create_Connection (0x0405) AS SENT:");
            line[0] = 0;
            PStrCat(line, "      ");
            for (k = 0; k < 13; k++) {
                unsigned long w = c[kWCreateConn + (k / 4)];
                PStrCatHexN(line, (w >> (8 * (3 - (k & 3)))) & 0xFF, 2);
                PStrCatCh(line, ' ');
            }
            Summary(line);
            SummaryText("      BD_ADDR(6) PacketType(2) PageScanRep(1) Rsvd(1)");
            SummaryText("      ClockOffset(2) AllowRoleSwitch(1)");
            {   /* Packet_Type is bytes 6..7, little endian on the wire. */
                unsigned long w1 = c[kWCreateConn + 1];
                unsigned long pt = ((w1 >> 8) & 0xFF) | (((w1 >> 0) & 0xFF) << 8);
                RowHex("      packet type mask", pt, 4);
                Row("      packet type PATCHED", c[kWCreateConnPatched]);
                if (pt == 0x0800) {
                    SummaryText("      ⭐ 0x0800 = APPLE'S LITERAL VALUE for this class");
                    SummaryText("        of card, rewritten into the outgoing command by");
                    SummaryText("        the driver. BTstack's own ^ 0x3306 sets EDR");
                    SummaryText("        \"shall not be used\" bits that do not exist on");
                    SummaryText("        a Bluetooth 1.2 controller, and no value passed");
                    SummaryText("        to hci_enable_acl_packet_types can clear them.");
                } else if (pt != 0) {
                    SummaryText("      ⚠ NOT 0x0800. The rewrite did not take -- check");
                    SummaryText("        the patched count above. Any EDR bit set here");
                    SummaryText("        is invalid on an LMP 2 controller.");
                }
                if (pt == 0) {
                    SummaryText("      ⭐⭐ ZERO. A Create_Connection with no permitted");
                    SummaryText("        packet type is exactly what a controller answers");
                    SummaryText("        with 0x12 Invalid HCI Command Parameters -- and");
                    SummaryText("        BTstack derives this mask from the REMOTE's");
                    SummaryText("        supported features, which this build FABRICATES");
                    SummaryText("        as all-zero. Our own synthetic event would then");
                    SummaryText("        be the cause.");
                } else {
                    SummaryText("      non-zero, so an empty packet-type mask is NOT the");
                    SummaryText("        cause. Read the other parameters against the");
                    SummaryText("        spec before blaming the synthetic features.");
                }
            }
        }
        if ((c[kWPairRc] & 0xFFUL) == 0xFE)
            SummaryText("      0xFE is OURS: the stack was not in HCI_STATE_WORKING.");
        /* ★★★★ WHICH COMMAND THE CONTROLLER REJECTED. v7.3 saw two 0x12 Command
         * Status events and could not name the command, because the event ring keeps
         * only (code, len, byte2) and byte2 of a Command Status is the status. */
        if (c[kWBadCmdCount] != 0) {
            SummaryText("  ⭐ THE CONTROLLER REJECTED A COMMAND:");
            Row("    rejections",        c[kWBadCmdCount]);
            /* ⚠ ALL OF THEM, because v7.4's single "first" slot was filled by a
             * harmless 0x0C52 / 0x01 (Write_Extended_Inquiry_Response, Unknown HCI
             * Command -- a bring-up command this card does not implement) and hid the
             * two 0x12s that mattered. The count said 4 and the log could name only
             * the useless one. A single slot cannot represent a sequence. */
            {
                short k;
                for (k = 0; k < 4; k++) {
                    unsigned long e = c[kWBadCmdRing + k];
                    unsigned long op = (e >> 16) & 0xFFFF;
                    Str255 line;
                    if (e == 0) continue;
                    line[0] = 0;
                    PStrCat(line, "      opcode 0x"); PStrCatHexN(line, op, 4);
                    PStrCat(line, "  status 0x");     PStrCatHexN(line, e & 0xFF, 2);
                    PStrCat(line, "  ");
                    if      (op == 0x0405) PStrCat(line, "Create_Connection");
                    else if (op == 0x0419) PStrCat(line, "Remote_Name_Request");
                    else if (op == 0x0401) PStrCat(line, "Inquiry");
                    else if (op == 0x0411) PStrCat(line, "Authentication_Requested");
                    else if (op == 0x0C52) PStrCat(line, "Write_Ext_Inquiry_Resp (benign)");
                    Summary(line);
                }
            }
            RowHex("    first opcode",  (c[kWFirstBadCmd] >> 16) & 0xFFFF, 4);
            RowHex("    its status",     c[kWFirstBadCmd]        & 0xFF, 2);
            /* ⚠ BRACES. The first version put two SummaryText calls after a
             * braceless if and then an else -- the second call would have run
             * unconditionally and the else would not have compiled. */
            {
                unsigned long op = (c[kWFirstBadCmd] >> 16) & 0xFFFFUL;
                if (op == 0x0405) {
                    SummaryText("      ^ Create_Connection -- rejected AT COMMAND");
                    SummaryText("        TIME, so the parameter bytes above are what");
                    SummaryText("        the controller objected to.");
                } else if (op == 0x0419) {
                    SummaryText("      ^ Remote_Name_Request");
                } else if (op == 0x0401) {
                    SummaryText("      ^ Inquiry");
                } else if (op == 0x0411) {
                    SummaryText("      ^ Authentication_Requested");
                }
            }
        }
        /* ★★★★ DID THE ARM FIRE? Create_Connection is rejected, so the only way to
         * a link is the device connecting to US -- which it does constantly, just
         * never at the instant the user clicks Pair. */
        Row("arm fired on inbound",      c[kWArmedFired]);
        RowStatus("  Authentication_Requested rc", c[kWArmedRc]);
        if (c[kWArmedFired] == 0)
            SummaryText("      0 = the device never connected after Pair was clicked.");
        Row("bonding completions",       c[kWBondDone]);
        RowHex("  its status (HEX)",     c[kWBondStatus] & 0xFF, 2);
        if (c[kWBondDone] == 0) {
            SummaryText("    ⚠ NO VERDICT YET. gap_dedicated_bonding was accepted but");
            SummaryText("      GAP_EVENT_DEDICATED_BONDING_COMPLETED never arrived, so");
            SummaryText("      the attempt is still open or died silently. Read the");
            SummaryText("      opcode ring for 0x0411 Authentication_Requested and the");
            SummaryText("      PIN rows in M2c -- a legacy device asks within seconds.");
        } else if ((c[kWBondStatus] & 0xFFUL) == 0) {
            SummaryText("    ⭐⭐⭐ BONDED. A pairing was negotiated from OS 9.");
        } else {
            SummaryText("    ⚠ Bonding FAILED with the status above. 0x06 PIN or key");
            SummaryText("      missing, 0x05 authentication failure, 0x04 page timeout");
            SummaryText("      (not listening), 0x12 INVALID HCI COMMAND PARAMETERS --");
            SummaryText("      the controller rejected a parameter of the command we");
            SummaryText("      sent, so read the Create_Connection bytes above.");
        }
    }   /* <- the initiator-gated block now ENDS HERE */

    /* ★★★★★★ LIFTED OUT OF THE INITIATOR-ONLY BLOCK, v99.49.
     *
     * ⚠⚠ THIS SECTION USED TO SIT INSIDE the block gated on the panel's Pair button
     * having fired. db_put_link_key calls BT_WriteKeyToController for EVERY pairing --
     * ours or the device's -- so when the first successful pairing in this project's
     * history arrived IN from a phone, the one readout that says whether the key
     * reached the card was skipped. The card's store fell from 1 key to 0 across that
     * pairing and the number explaining it was collected and never printed.
     *
     * ⇒ THAT IS THE THIRD TIME THIS SESSION an answer sat in the block and not in the
     * log: kWCmdResult hid a missing mailbox case for two runs, the safety-net verdict
     * tested a count where it should have tested an address, and this. A counter that
     * is published but unprinted is not instrumentation.
     *
     * It now prints whenever a write was attempted, whoever started the pairing. */
    if (c[kWWroteKeyTried] != 0 || c[kWWroteKeyDone] != 0) {
        SummaryText("  ⭐⭐ AND DID THE KEY REACH THE CARD?");
        Row("Write_Stored_Link_Key sent", c[kWWroteKeyTried]);
        RowStatus("  send rc",            c[kWWroteKeyRc]);
        if (c[kWWroteKeyDone] == 0) {
            SummaryText("    no completion. Either it was never sent, or the controller");
            SummaryText("    has not answered yet.");
        } else {
            Row("  its status",           c[kWWroteKeyDone]        & 0xFF);
            Row("  keys written",        (c[kWWroteKeyDone] >> 8)  & 0xFF);
            if ((c[kWWroteKeyDone] & 0xFFUL) == 0
                && ((c[kWWroteKeyDone] >> 8) & 0xFFUL) != 0) {
                SummaryText("    ⭐⭐⭐⭐ THE KEY IS IN THE CONTROLLER'S OWN STORE. That is");
                SummaryText("      the store its on-chip proxy stack reconnects from, so");
                SummaryText("      switch the card back to HID-proxy and the device");
                SummaryText("      should work with nothing of ours running.");
            } else {
                SummaryText("    ⚠ The controller REFUSED the write. Compare Max_Num_Keys");
                SummaryText("      above: a full store, or an unsupported command.");
            }
        }
    }

    /* ★★★★ WHAT THE CONTROLLER SAYS IT SUPPORTS. Create_Connection has been
     * rejected 0x12 with BTstack's parameters and with Apple's -- and this card
     * already answers 0x0C52 with "Unknown HCI Command", so its command set is
     * demonstrably incomplete. It declared the whole set at bring-up and we never
     * looked.
     *
     * ⚠ THE RAW OCTETS ARE PRINTED BESIDE THE DECODE, on purpose. BTstack's own
     * SUPPORTED_HCI_COMMANDS table does not list Create_Connection, so its bit
     * position comes from the Core spec rather than from the vendored source -- and a
     * bit offset recalled rather than read is exactly the kind of thing this project
     * has been wrong about. If the decode disagrees with the hex, trust the hex. */
    /* ★★★★ THE CARD'S OWN VERSION -- which devices can EVER pair with it. */
    /* ★★★★ IS THE CARD'S STORE WRITABLE? Read_Stored_Link_Key proved it can be READ.
     * Nothing has proved a host can CHANGE it -- and Write_Stored_Link_Key, which the
     * whole goal depends on, needs exactly that. A delete is the cheapest proof. */
    if (c[kWDelStoredTried] != 0) {
        SummaryText("  ⭐⭐⭐ DELETE FROM THE CARD'S OWN STORE");
        Row("attempts",              c[kWDelStoredTried]);
        RowBDAddr("  address",       c[kWDelStoredHi], c[kWDelStoredLo]);
        RowStatus("  send rc",       c[kWDelStoredRc]);
        if (c[kWDelStoredDone] == 0) {
            SummaryText("    no completion yet. The command went out and the controller");
            SummaryText("    has not answered, or it never went out -- read the rc.");
        } else {
            Row("  its status",      c[kWDelStoredDone]        & 0xFF);
            Row("  keys deleted",   (c[kWDelStoredDone] >> 8)  & 0xFF);
            if ((c[kWDelStoredDone] & 0xFFUL) == 0
                && ((c[kWDelStoredDone] >> 8) & 0xFFUL) != 0) {
                SummaryText("    ⭐⭐⭐⭐ THE CARD'S STORE IS WRITABLE. A host CAN change");
                SummaryText("      it, which is the precondition Write_Stored_Link_Key");
                SummaryText("      needs and which nothing had established. Check that");
                SummaryText("      Num_Keys_Read above has dropped by one.");
            } else {
                SummaryText("    ⚠ The controller REFUSED the delete. If the store cannot");
                SummaryText("      be changed by a host, Write_Stored_Link_Key cannot");
                SummaryText("      work either and the whole approach needs rethinking.");
            }
        }
    }

    SummaryText("  ⭐⭐ WHAT THE CARD IS");
    if ((c[kWLocalVer] & 0x1000000UL) == 0) {
        SummaryText("    version NOT captured -- driver older than v7.9.");
    } else {
        Row("HCI version",   (c[kWLocalVer] >> 16) & 0xFF);
        Row("LMP version",   (c[kWLocalVer] >>  8) & 0xFF);
        RowHex("manufacturer", c[kWLocalMfr], 4);
        {
            unsigned long lmp = (c[kWLocalVer] >> 8) & 0xFF;
            if (lmp < 4) {
                SummaryText("    LMP < 4, so NO SECURE SIMPLE PAIRING. Only LEGACY PIN");
                SummaryText("    pairing is possible, and any device that REQUIRES SSP");
                SummaryText("    -- which is most hardware made after about 2008 --");
                SummaryText("    cannot pair with this card at all. A modern keyboard or");
                SummaryText("    headset failing here would prove nothing about our code.");
            } else {
                SummaryText("    LMP >= 4, so SSP IS available and modern devices are");
                SummaryText("    fair test subjects.");
            }
            if (lmp < 6)
                SummaryText("    LMP < 6: NO Bluetooth Low Energy. BLE-only devices are");
            else
                SummaryText("    LMP >= 6: BLE present.");
        }
    }

    SummaryText("  ⭐⭐ WHAT THE CONTROLLER SAYS IT SUPPORTS");
    if (c[kWSuppCmdsGot] == 0) {
        SummaryText("    NOT CAPTURED. Driver older than v7.8, or the controller never");
        SummaryText("    answered Read_Local_Supported_Commands (0x1002).");
    } else {
        Str255 line;
        short k;
        line[0] = 0;
        PStrCat(line, "    octets 0-7: ");
        for (k = 0; k < 8; k++) {
            unsigned long w = c[kWSuppCmds0 + (k / 4)];
            PStrCatHexN(line, (w >> (8 * (3 - (k & 3)))) & 0xFF, 2);
            PStrCatCh(line, ' ');
        }
        Summary(line);
        {   /* Octet 0 per the Core spec: bit 0 Inquiry, 1 Inquiry_Cancel,
             * 2 Periodic_Inquiry_Mode, 3 Exit_Periodic_Inquiry_Mode,
             * 4 CREATE_CONNECTION, 5 Disconnect, 7 Create_Connection_Cancel. */
            unsigned long o0 = (c[kWSuppCmds0] >> 24) & 0xFF;
            Row("    octet 0 (decimal)", o0);
            SummaryText("    octet 0, per the Core spec's table:");
            RowPair("      Inquiry",            (o0 & 0x01) != 0, (o0 & 0x01) != 0);
            RowPair("      Create_Connection",  (o0 & 0x10) != 0, (o0 & 0x10) != 0);
            RowPair("      Disconnect",         (o0 & 0x20) != 0, (o0 & 0x20) != 0);
            if ((o0 & 0x10) == 0) {
                SummaryText("      ⭐⭐⭐ CREATE_CONNECTION IS NOT SUPPORTED. The card");
                SummaryText("        declared that at bring-up, and 0x12 is it saying so");
                SummaryText("        again. Outgoing connections are not available in");
                SummaryText("        this personality -- pairing must ride on the link");
                SummaryText("        the DEVICE makes, which is what the arm above does.");
            } else {
                SummaryText("      ⚠ It claims Create_Connection IS supported, so 0x12 is");
                SummaryText("        about a parameter after all -- and both BTstack's");
                SummaryText("        and Apple's values have now been rejected. Check the");
                SummaryText("        raw hex against the spec before going further.");
            }
        }
    }

    SummaryText("  the Expert-log channel, for the record");
    Row("BT_Log calls",         c[kWSayCalls]);
    SummaryText("    (every one of those produced NOTHING in the USB Expert log)");
}

/* ⚠ PICK THE MOST ADVANCED BLOCK, NEVER SIMPLY THE FIRST.
 *
 * Run 17 found TWO live blocks. The USB Expert offers this generically-matched
 * driver more than one device, so a second instance had bound to a bus-powered
 * HUB (interface 0 class 0x09), correctly failed to find a Bluetooth interface,
 * and stopped -- harmless, but it sorted FIRST in the heap scan. Verdict() read
 * gC[0] and therefore announced "M0.1 DID NOT COMPLETE" on a run where the real
 * instance had opened all three pipes, connected ACL and moved 1495 bytes.
 *
 * That is the same class of mistake as BTCheck v9 reporting a passed gate as a
 * failure, and it is the second time a wrong summary has had to be untangled by
 * hand. Rank by how far the instance actually got. */
static short BestBlock(void)
{
    short b, best = 0;
    unsigned long bestScore = 0;
    for (b = 0; b < gBlocks; b++) {
        unsigned long *c = gC[b];
        unsigned long score = (c[kWInit] > 0 ? 1UL : 0UL)
                            + (c[kWCfgDone] > 0 ? 1000UL : 0UL)
                            + c[kWStageMax] * 10UL
                            + (c[kWStkState] == 2 ? 5000UL : 0UL)
                            + (c[kWSdpAttrBytes] > 0 ? 50000UL : 0UL);
        if (score > bestScore) { bestScore = score; best = b; }
    }
    return best;
}

/* ---- USB BUS DUMP -------------------------------------------------------- *
 * ⚠ PHASE 1 NEEDS THIS, AND IT MUST GO IN A LOG FILE.
 *
 * The question is how the A1044 internal module PRESENTS itself: standard HCI
 * (class 0xE0/0x01/0x01), vendor-specific (0xFF), or HID-proxy (0x03). Any of the
 * last two dodges the driver's generic 0xE0 matcher, and the answer decides the
 * work. Until now the plan was to read USB Prober's Bus Devices window -- which
 * means photographing a screen, and this project's standing rule is that a
 * diagnostic MUST write a log rather than make the user photograph a window.
 *
 * USBGetNextDeviceByClass + USBGetDeviceDescriptor are the APP-LEVEL half of the
 * USB Manager: enumerate and read descriptors, no transfers. Exactly what an
 * app is allowed to do, and it works whether or not any driver bound the device.
 * That last part is the point -- if our driver never matches the A1044, this is
 * the only way to see it at all. */
/* ⚠ SWAP LOCALLY RATHER THAN CALLING USBToHostWord. That helper lives in
 * USBServicesLib, which is the DRIVERS' library; an application links
 * USBManagerLib and gets an undefined symbol for it at link time (which is how
 * this was found). USB descriptors are little-endian and PowerPC is big-endian, so
 * the swap is the whole job and stating it explicitly is clearer than the call. */
static UInt16 leWord(UInt16 v)
{
    return (UInt16)(((v & 0x00FFu) << 8) | ((v >> 8) & 0x00FFu));
}

/* ★ The card's own device reference, captured as the bus dump walks past it, so the
 * descriptor section does not have to enumerate a second time. Either personality is
 * a valid target: 1000 is the proxy (the interesting one for the media keys) and 8204
 * is HCI mode. 0 means the dump never saw it. */
static USBDeviceRef  gCardRef   = 0;
static unsigned long gCardVidPid = 0;

/* ⚠⚠ ITS OWN FUNCTION, AND CALLED UNCONDITIONALLY, BECAUSE v99.28 PUT THIS INSIDE
 * ReportBlock() AND IT NEVER RAN. ReportBlock is invoked once per 'BTP1' block
 * found, so with the driver not loaded -- which is the normal state on a machine
 * that has not been put into pairing mode -- there are no blocks and the whole
 * section is skipped. A WHOLE RUN PRODUCED NOTHING.
 *
 * ⇒ THE VOLUME QUESTION IS ABOUT THE MACHINE, NOT ABOUT OUR DRIVER. It has no
 * business being gated on our driver being resident. Same defect class as wiring
 * the report decode into the dormant l2cap path: code placed where it does not
 * run, reported as an absence rather than as a mistake.
 * [[feedback_control_must_exercise_the_change]] */
static void ProbeSystemVolume(void)
{
    /* ★★★★★★ M6 STEP 0: CAN THIS MACHINE CHANGE ITS VOLUME IN SOFTWARE AT ALL?
     *
     * ⚠⚠ THIS IS A MEASUREMENT, NOT A FEATURE, AND IT COMES FIRST ON PURPOSE. The media
     * keys already ARRIVE and DECODE -- byte 8 is read correctly and all four keys were
     * seen. What is missing is the other end: something that turns "volume up was
     * pressed" into the volume actually changing. Building that plumbing before knowing
     * whether the destination exists would be building a pipe to nowhere.
     *
     * ⚠ AND THERE IS REAL EVIDENCE THAT IT MIGHT NOT EXIST. The user's own
     * Mini-G4-Audio TIER3_FINDINGS.md records, MEASURED: "No hardware/Sound-Manager
     * volume works on this machine. GetDefaultOutputVolume errors $8001;
     * SetDefaultOutputVolume does nothing; per-channel volumeCmd does nothing; the lone
     * output component 'awgc' returns $8002 for every siHardwareVolume and
     * siHardwareMute selector."
     * ⚠ (that sentence originally quoted the selector names with a wildcard, whose
     * trailing slash-star closed this comment early and broke the build -- a reminder
     * that a block comment is code)
     *
     * ⇒ BUT THAT WAS THE MAC MINI G4, NOT THIS MDD. Different machine, different audio
     * hardware, and this project has been bitten before by carrying a measurement from
     * one machine to another. It is evidence that the question is REAL, not an answer
     * to it.
     *
     * ⚠⚠ AND APPLE'S OWN OS 9 MEDIA-KEY HANDLER DOES NOT USE THIS API. The shipping
     * `USB Device Extension` contains USBKeyboardSupport / KeyboardSupportShim with
     * DoSoundUpButton, DoSoundDownButton, DoSoundMuteButton and DoEjectButton -- and it
     * imports NO SoundLib. Its volume path goes through PrivateInterfaceLib, i.e.
     * undocumented calls. So a failure here does NOT mean the machine cannot do it; it
     * means the DOCUMENTED route cannot, and the undocumented one would need
     * disassembly. Report exactly that distinction rather than a verdict.
     *
     * ⚠ SAFE: this reads the volume, sets it to what it already was, and reads back.
     * Nothing is left changed even if every call succeeds. */
    /* ★★★★★★ M6 step 1: THE HARDWARE-VOLUME ROUTE, which is the one still untested.
     *
     * ⚠⚠ SetDefaultOutputVolume IS MEASURED WRONG. v10.8 drove it 36 times from real
     * media-key presses, every call returned noErr -- and no volume bar appeared and
     * nothing audibly changed, while the user's WIRED USB keyboard did both on the same
     * machine at the same moment. My earlier beep test proved only that SysBeep respects
     * that value, not that it is the system volume.
     *
     * ⇒ THE OTHER DOCUMENTED ROUTE is siHardwareVolume through Get/SetSoundOutputInfo on
     * a sound output device component ('sdev'). That is a DIFFERENT selector on a
     * DIFFERENT object, and the Mac Mini findings tested the two separately -- there,
     * 'awgc' returned $8002 for every hardware selector. Untested here.
     *
     * ⚠ Read-only: this ENUMERATES devices and READS their hardware volume. It writes
     * nothing. After the last run left the machine muted, a probe that changes audio
     * state has to earn it, and this one does not need to. */
    SummaryText("  ★★★★★★ THE HARDWARE VOLUME ROUTE (M6 step 1) - read only");
    {
        ComponentDescription cd;
        Component c = 0;
        short n = 0;
        cd.componentType         = FOUR_CHAR_CODE('sdev');
        cd.componentSubType      = 0;
        cd.componentManufacturer = 0;
        cd.componentFlags        = 0;
        cd.componentFlagsMask    = 0;
        while ((c = FindNextComponent(c, &cd)) != 0 && n < 6) {
            long hv = 0, hm = 0;
            OSErr eV = GetSoundOutputInfo(c, FOUR_CHAR_CODE('hvol'), &hv);
            OSErr eM = GetSoundOutputInfo(c, FOUR_CHAR_CODE('hmut'), &hm);
            Str255 ln; ln[0] = 0;
            n++;
            PStrCat(ln, "      sdev #");
            PStrCatHexN(ln, (unsigned long)n, 1);
            PStrCat(ln, "  hvol err ");
            PStrCatHexN(ln, (unsigned long)(unsigned short)eV, 4);
            if (eV == noErr) { PStrCat(ln, " = "); PStrCatHexN(ln, (unsigned long)hv, 8); }
            PStrCat(ln, "  hmut err ");
            PStrCatHexN(ln, (unsigned long)(unsigned short)eM, 4);
            if (eM == noErr) { PStrCat(ln, " = "); PStrCatHexN(ln, (unsigned long)hm, 2); }
            /* ⭐ THE TOKEN ITSELF, which is half of the v11.6 discriminator. The driver
             * publishes the one IT got from the identical lookup at kWMedDevId; these two
             * numbers side by side are the only thing that separates "the driver found a
             * different component" from "the driver found the right one and the Component
             * Manager will not dispatch it from a driver fragment." Printed for EVERY
             * device found, not just the first, so a second 'sdev' cannot hide here. */
            PStrCat(ln, "  id ");
            PStrCatHexN(ln, (unsigned long)c, 8);
            Summary(ln);
            if (n == 1) gAppSndDevId = (unsigned long)c;
        }
        Row("      sound output devices found", (unsigned long)n);
        if (n == 0) {
            SummaryText("    ⚠ NO 'sdev' COMPONENT AT ALL. Then this route does not exist");
            SummaryText("      here and the answer is elsewhere entirely.");
        } else {
            SummaryText("    ⇒ err 0000 on hvol means THE HARDWARE VOLUME IS READABLE, and");
            SummaryText("      is the strongest candidate for what the wired keyboard moves.");
            SummaryText("      ⚠ $8002 is what the Mac Mini returns for every hardware");
            SummaryText("      selector -- if that appears here too, BOTH documented routes");
            SummaryText("      are closed and Apple's internal DoSoundUpButton is doing");
            SummaryText("      something neither of them exposes.");
        }
    }

    /* ★★★★★★ M6 step 0: WHAT THE SOUND MANAGER SAYS ABOUT ITS OWN VOLUME.
     *
     * ⭐⭐ THE QUESTION THIS SECTION EXISTED TO ANSWER IS SETTLED, AND THE ANSWER WAS
     * SOMEWHERE ELSE ENTIRELY. This probe was built to find out whether media-key
     * volume could be driven through the documented Sound Manager API, because a silent
     * no-op looks exactly like success in a log. It ended with three beeps at rising
     * volume and a human ear for a discriminator.
     *
     * The working mechanism turned out to be TRAP 0xABEC -- OS 9's own volume and eject
     * path, callable from driver context -- not the Sound Manager at all. Volume up,
     * down, mute and eject all run through it now and the user has confirmed every one
     * of them on the hardware. [[reference_os9_volume_trap_abec]]
     *
     * ⇒ THE BEEPS ARE GONE, at the user's request, and with them every write. What is
     * left is a plain read, which still says something worth a line (whether the API
     * answers at all on this machine, which differs between the MDD and the Mini) and
     * costs nothing.
     *
     * ⚠⚠ AND THIS MAKES THE BANNER AT THE TOP OF THE LOG TRUE AGAIN. BTCheck announces
     * itself as "Read-only: a System heap scan and this log file. No hardware touched."
     * The audible test set the output volume three times and beeped -- it restored the
     * original afterwards, but the claim was false while it ran. A tool that describes
     * itself as read-only must be read-only; removing the last writer is what makes the
     * sentence honest rather than nearly true. */
    SummaryText("  ★★★★★★ THE SOUND MANAGER'S VOLUME, READ ONLY (M6 step 0)");
    {
        long  vol = 0;
        OSErr eGet = GetDefaultOutputVolume(&vol);
        RowStatus("      GetDefaultOutputVolume", 0x100UL | (unsigned long)(unsigned short)eGet);
        if (eGet == noErr) {
            RowHex("      current volume (L<<16|R)", (unsigned long)vol, 8);
            SummaryText("    The documented API answers on this machine. Recorded because");
            SummaryText("    it does NOT on the Mac Mini G4 ($8001), and because media-key");
            SummaryText("    volume does not use it either way -- trap 0xABEC does.");
        } else {
            SummaryText("    ⚠ The documented API does not answer here. Not a fault and");
            SummaryText("      not in the way: the media keys run through trap 0xABEC,");
            SummaryText("      which is a different mechanism and is already working.");
        }
    }
}

/* ======================================================================= */
/*  WHICH OF OUR FILES ARE ACTUALLY INSTALLED, read from the FILES         */
/* ======================================================================= */
/* ★★★ A FRESHNESS WITNESS THAT SURVIVES THE DRIVER NEVER BINDING.
 *
 * Every other version signal in this log comes from the driver's counter block: the
 * build tag, the bound device, the counters. If the driver never MATCHES, there is no
 * block -- and "the new driver did not bind" then reads EXACTLY like "the old driver is
 * still in Extensions". Those two need completely different next steps, and nothing in
 * the log could tell them apart.
 *
 * That is not hypothetical. The 2026-10-03 FW400 dongle run came back with
 * "block found: False" and no way to say which driver had been installed, on a build
 * whose whole purpose was to change the matching rules. Reading the 'vers' resource off
 * the FILES answers it without the driver's cooperation.
 *
 * ⚠ It also catches the run-27 failure: TWO Bluetooth drivers left in Extensions, where
 * the Expert may load either one and the version you think you are testing is not the
 * version that ran. A rename ("USBBluetoothSupport" -> "USB Bluetooth Support") makes
 * that easy to do by accident, because the old file does not get replaced.
 *
 * Task level, from an application: the File and Resource Managers are both fine here.
 * See reference_os9_no_filemgr_at_interrupt for why this may never move into the driver. */
static Boolean PStrContains(const unsigned char *s, const char *needle)
{
    short sl = s[0], nl = 0, i, j;
    while (needle[nl]) nl++;
    if (nl == 0 || nl > sl) return false;
    for (i = 0; i <= sl - nl; i++) {
        for (j = 0; j < nl && s[1 + i + j] == (unsigned char)needle[j]; j++) ;
        if (j == nl) return true;
    }
    return false;
}

/* Prints "<name>  <long vers string>" for one file, or the name alone if it carries no
 * 'vers' (1). ⚠ Get Info shows the LONG string, so that is the one to print -- a short
 * string that agrees with a stale long string is how version drift hides. */
static void ReportFileVersion(const FSSpec *spec)
{
    Str255 line;
    short  saveRes = CurResFile();
    short  rf;

    line[0] = 0;
    PStrCat(line, "    ");
    PStrCatPStr(line, spec->name);

    rf = FSpOpenResFile((FSSpec *)spec, fsRdPerm);
    if (rf != -1) {
        Handle h = Get1Resource('vers', 1);
        if (h != NULL && *h != NULL) {
            /* VersRec: 4 bytes numeric, 2 bytes country, then shortVersion and
             * longVersion as consecutive Pascal strings. */
            unsigned char *p     = (unsigned char *)*h;
            unsigned char *shrt  = p + 6;
            unsigned char *lng   = shrt + 1 + shrt[0];
            unsigned char *use   = (lng[0] > 0) ? lng : shrt;
            if (use[0] > 0) {
                PStrCat(line, "  -  ");
                PStrCatPStr(line, use);
            }
        } else {
            PStrCat(line, "  -  (no vers resource)");
        }
        CloseResFile(rf);
    } else {
        PStrCat(line, "  -  (resource fork unreadable)");
    }
    UseResFile(saveRes);
    Summary(line);
}

static void ScanFolderForBluetooth(OSType folderType, const char *label)
{
    short  vRefNum;
    long   dirID;
    short  i;
    short  found = 0;
    short  drivers = 0;      /* files whose name says "driver", not just "Bluetooth" */
    Str255 line;

    line[0] = 0;
    PStrCat(line, "  ");
    PStrCat(line, label);
    PStrCat(line, ":");
    Summary(line);

    if (FindFolder(kOnSystemDisk, folderType, kDontCreateFolder,
                   &vRefNum, &dirID) != noErr) {
        SummaryText("    (folder not found)");
        return;
    }

    /* ⚠ Bounded. A corrupt catalog must not spin this app forever; 2000 is far past any
     * real Extensions folder (the FW400's has 202 files). */
    for (i = 1; i <= 2000; i++) {
        CInfoPBRec pb;
        Str255     name;
        FSSpec     spec;

        memset(&pb, 0, sizeof(pb));
        name[0]          = 0;
        pb.hFileInfo.ioNamePtr   = name;
        pb.hFileInfo.ioVRefNum   = vRefNum;
        pb.hFileInfo.ioDirID     = dirID;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;       /* past the last entry */
        if (pb.hFileInfo.ioFlAttrib & ioDirMask) continue;   /* a folder */

        if (!PStrContains(name, "Bluetooth") && !PStrContains(name, "BT")) continue;

        if (FSMakeFSSpec(vRefNum, dirID, name, &spec) != noErr) continue;
        ReportFileVersion(&spec);
        found++;
        /* ⚠ COUNT DRIVERS, NOT MATCHES. The first cut warned on any two matches, and the
         * very first run tripped it on the legitimate pair "USB Bluetooth Support" +
         * "USBBluetoothSwitch" -- the switcher is SUPPOSED to sit beside the driver. A
         * warning that fires on the normal install is worse than none, because it
         * teaches you to skip the line that will one day be real. The run-27 hazard is
         * specifically TWO DRIVERS, so count the driver name only. */
        if (PStrContains(name, "Bluetooth Support") || PStrContains(name, "BluetoothSupport"))
            drivers++;
    }

    if (found == 0) SummaryText("    (nothing Bluetooth-related here)");
    else if (drivers > 1) {
        SummaryText("    ⚠⚠ TWO BLUETOOTH DRIVERS ARE INSTALLED. The Expert may load");
        SummaryText("      either one, so the version you think you are testing may not");
        SummaryText("      be the version that ran. Renaming an extension does NOT");
        SummaryText("      replace the file it was renamed from -- delete the old one.");
    }
}

static void DumpInstalledVersions(void)
{
    SummaryText("");
    SummaryText("=========================================================");
    SummaryText("  INSTALLED FILES - read from the files, not from the driver");
    SummaryText("=========================================================");
    SummaryText("  This section works even when nothing bound, which is exactly");
    SummaryText("  when you need it. Get Info shows the long string printed here.");
    SummaryText("");
    ScanFolderForBluetooth(kExtensionFolderType,        "Extensions");
    ScanFolderForBluetooth(kControlPanelFolderType,     "Control Panels");
    ScanFolderForBluetooth(kControlStripModulesFolderType, "Control Strip Modules");
}

static void DumpUSBBus(void)
{
    USBDeviceRef       ref   = 0;
    CFragConnectionID  connID;
    OSStatus           err;
    short              n = 0;
    short              stumbles = 0;   /* consecutive enumeration errors tolerated */

    SummaryText("");
    SummaryText("=========================================================");
    SummaryText("  USB BUS DUMP - every device, whether or not a driver bound");
    SummaryText("=========================================================");

    for (;;) {
        USBDeviceDescriptor d;
        Str255 line;

        err = USBGetNextDeviceByClass(&ref, &connID, kUSBAnyClass,
                                      kUSBAnySubClass, kUSBAnyProtocol);
        /* ⚠⚠⚠ THIS USED TO `break` ON ANY NON-noErr, AND THAT SILENTLY TRUNCATED THE
         * WHOLE DUMP -- the single most consequential bug in this diagnostic, because
         * most of this project's conclusions were drawn from its output.
         *
         * PROVED by the user's own Apple System Profiler report. ASP lists TWO
         * controllers, USB 0 and USB 1, with the keyboard and mouse on USB 0 and the
         * dongle and the internal modem on USB 1. Our dump reported 12 devices in run
         * 46 and showed NEITHER the dongle nor the modem -- while ASP showed both,
         * plugged into the same machine at the same time.
         *
         * ⇒ The walk stops at the first hiccup, so how much of the bus you see depends
         * on where that hiccup falls. It also explains why 05AC:8202 appeared in some
         * runs and not others with nothing changing: not an intermittent device, an
         * intermittently truncated dump.
         *
         * Keep going instead, bounded by the iteration count. Report the stop reason
         * rather than swallowing it, because a dump that quietly ends early is how a
         * whole session's worth of "the card is ABSENT" got recorded. */
        if (err != noErr) {
            if (n == 0) {
                Str255 l; l[0] = 0;
                PStrCat(l, "  enumeration returned ");
                PStrCatNum(l, (long)err);
                PStrCat(l, " before any device was seen.");
                Summary(l);
            }
            if (++stumbles > 8) {
                Str255 l; l[0] = 0;
                PStrCat(l, "  (walk ended after ");
                PStrCatNum(l, (long)stumbles);
                PStrCat(l, " consecutive errors, last ");
                PStrCatNum(l, (long)err);
                PStrCat(l, ")");
                Summary(l);
                break;
            }
            continue;
        }
        stumbles = 0;
        if (++n > 24) { SummaryText("  (more than 24 devices; list truncated)"); break; }

        memset(&d, 0, sizeof(d));
        if (USBGetDeviceDescriptor(&ref, &d, (UInt32)sizeof(d)) != noErr) {
            SummaryText("    (device found, but its descriptor could not be read)");
            continue;
        }

        line[0] = 0;
        PStrCat(line, "    VID ");
        PStrCatHexN(line, leWord(d.vendor), 4);
        PStrCat(line, "  PID ");
        PStrCatHexN(line, leWord(d.product), 4);
        PStrCat(line, "  class ");
        PStrCatHexN(line, d.deviceClass, 2);
        PStrCat(line, "/");
        PStrCatHexN(line, d.deviceSubClass, 2);
        PStrCat(line, "/");
        PStrCatHexN(line, d.protocol, 2);
        /* ★ numConf answers run 32's open question with no driver change and no
         * hardware touched. A HID-proxy adapter that carries its HCI personality as
         * a SECOND USB CONFIGURATION can be switched with a plain
         * USBSetConfiguration -- standard USB, no vendor control request, no risk.
         * If this reads 1 for the A1044 then that safe door is closed and the switch
         * has to be a vendor request, which must be researched and not guessed. */
        PStrCat(line, "  conf ");
        PStrCatNum(line, (long)d.numConf);
        Summary(line);

        /* ★ Remember the card whichever personality it is wearing. Proxy (1000) is the
         * one the media-key question is about; HCI (8204) is worth dumping too, since
         * comparing the two modes' published properties is itself informative. */
        if (leWord(d.vendor) == 0x05AC
            && (leWord(d.product) == 0x1000 || leWord(d.product) == 0x8204)) {
            gCardRef    = ref;
            gCardVidPid = ((unsigned long)leWord(d.vendor) << 16)
                        |  (unsigned long)leWord(d.product);
        }

        /* Name the ones that matter, so the answer does not need a lookup table. */
        if (leWord(d.vendor) == 0x05AC && leWord(d.product) == 0x1000) {
            SummaryText("      ^^ 05AC:1000 = APPLE INTERNAL BLUETOOTH (the A1044)");
            if (d.deviceClass == 0xE0)
                SummaryText("         class 0xE0 = STANDARD HCI. Our 0xE0 matcher binds it AS IS.");
            else if (d.deviceClass == 0xFF)
                SummaryText("         class 0xFF = VENDOR-SPECIFIC. Needs a VID/PID match rule.");
            else if (d.deviceClass == 0x03)
                SummaryText("         class 0x03 = HID-PROXY. Needs a hid2hci mode switch first.");
            else if (d.deviceClass == 0x00) {
                /* ★ RUN 26's ACTUAL ANSWER. Device class 0 is the standard USB way of
                 * saying "the class is declared on the interfaces, look there", so a
                 * device-level 0xE0 match can never see this card. v2.0 adds the
                 * interface path for exactly this. */
                SummaryText("         class 0x00 = HID-PROXY. The class is on the");
                SummaryText("         INTERFACES, so no device-level 0xE0 rule can see");
                SummaryText("         it, and OS 9's own USBHIDKeyboardModule claims it.");
                SummaryText("         ⇒ A PAIRED KEYBOARD TYPES IN THIS STATE with");
                SummaryText("         nothing of ours bound. CONFIRMED 2026-09-07 on a");
                SummaryText("         key this project negotiated in OS 9, so the proxy");
                SummaryText("         firmware DOES read the HCI stored-key store.");
                /* ⚠⚠ WHAT THIS BRANCH USED TO CLAIM, AND MUST NOT.
                 *
                 * It said "(iface 0 boot keyboard, iface 1 boot mouse)" as though that
                 * had been read off the device. It had not: this whole block is keyed
                 * on deviceClass == 0x00 and nothing here looks at an interface at all.
                 * The claim is plausible -- it is the ordinary HID-proxy shape -- but a
                 * hardcoded sentence sitting inside a measurement report reads as
                 * measured, and that is how a guess becomes a fact nobody re-checks.
                 * [[reference_static_audit_blind_spots]] is the same lesson.
                 *
                 * ⭐ AND IT IS NOW THE LOAD-BEARING QUESTION. The A1016's volume keys do
                 * nothing in this mode and its eject key arrives as a function key. If
                 * the proxy really does expose only BOOT-PROTOCOL interfaces then that
                 * is fully explained and unfixable here, because the boot report is a
                 * fixed 8 bytes drawing usages only from the Keyboard/Keypad page
                 * (0x07) -- and volume and eject live on the Consumer page (0x0C),
                 * which no boot collection can express. But that is a deduction from an
                 * assumption, not a result.
                 *
                 * ⇒ TO SETTLE IT: dump this device's CONFIGURATION descriptor and its
                 * HID REPORT descriptors. Both are app-level reads of the same kind the
                 * bus dump already does, they work in proxy mode so the keyboard keeps
                 * working while we measure, and they answer it definitively. Until then
                 * the media-key explanation stays a hypothesis. */
            } else {
                SummaryText("         unexpected class -- record it before any rule.");
            }
            /* ⭐ RUN 33's QUESTION, and the whole reason for this build. */
            if (d.numConf > 1) {
                SummaryText("         *** MORE THAN ONE CONFIGURATION. The HCI");
                SummaryText("         personality may live in config 2, reachable by a");
                SummaryText("         plain USBSetConfiguration -- standard USB, no");
                SummaryText("         vendor request, no risk. TRY THAT NEXT.");
            } else {
                /* ⚠ This branch used to end "must be researched from Apple's OS X
                 * driver, never guessed" -- advice that was correct when written and
                 * has since been CARRIED OUT. The request is known to the byte and
                 * demonstrably works; leaving the old text in place made a solved
                 * problem read as open every time the card appeared in proxy mode. */
                SummaryText("         only ONE configuration, so there is no plain");
                SummaryText("         USBSetConfiguration route. The vendor request is");
                SummaryText("         KNOWN and works: see docs/MODE-SWITCH-RUNBOOK.md.");
                SummaryText("         Boot the switch-ON build to apply it.");
            }
        } else if (leWord(d.vendor) == 0x0A12) {
            SummaryText("      ^^ 0A12 = Cambridge Silicon Radio (the CSR dongle)");
        } else if (d.deviceClass == 0xE0) {
            SummaryText("      ^^ class 0xE0 = a Bluetooth HCI controller of some kind");
        }
    }

    if (n == 0)
        SummaryText("  NO USB DEVICES ENUMERATED (USBGetNextDeviceByClass gave none)");
    else {
        Str255 l; l[0] = 0;
        PStrCat(l, "  ");
        PStrCatNum(l, (long)n);
        PStrCat(l, " USB device(s) enumerated.");
        Summary(l);
    }

    /* ★★★★★ A DIRECT QUERY FOR HCI CONTROLLERS, because the walk above CANNOT PROVE
     * ABSENCE and a run on 2026-09-21 turned on exactly that.
     *
     * ⚠⚠ THE ANY-CLASS WALK IS KNOWN TO TRUNCATE. Its own comment records the proof:
     * Apple System Profiler showed a dongle and an internal modem on USB 1 while this
     * dump listed neither, same machine, same moment. It was fixed to keep going past a
     * stumble, but it still stops after 9 consecutive errors -- and on the FW400 dongle
     * run it did exactly that, after listing 8 devices with DUPLICATES among them
     * (05AC:0220 three times), which a clean iteration cannot produce. A dump that both
     * truncates and repeats can support "X is present" and can NEVER support "X is
     * absent".
     *
     * ⇒ So ask the USB Manager the actual question instead of inferring it from a list:
     * enumerate class 0xE0 / 0x01 / 0x01 SPECIFICALLY. This is a different query, not a
     * filter over the same walk, so a truncation there does not hide anything here. If a
     * Bluetooth controller is on this bus, this finds it and prints its VID/PID; if this
     * says none, that is real evidence rather than the absence of evidence.
     *
     * ⚠ Device-level only, like the walk -- a controller declaring 0x00 at device level
     * with 0xE0 on interface 0 needs the CONFIGURATION descriptor, which this app does
     * not fetch. Said out loud below so a silent "none" is never read as "no controller". */
    {
        /* ⚠ Spelled out here: this is a separately built binary and nothing links it to
         * src/bt_probe.c's enum. The USB spec assigns these -- 0xE0 Wireless Controller,
         * 0x01 RF Controller, 0x01 Bluetooth Programming Interface -- so they are not
         * ours to change, but they must MATCH the driver's rules 2a/2b or this query
         * answers a different question than the one the driver asks. */
        const UInt8       kBTClass = 0xE0, kBTSubClass = 0x01, kBTProto = 0x01;
        USBDeviceRef      href = 0;
        CFragConnectionID hconn;
        short             hn = 0, hstumble = 0;
        OSStatus          herr;

        SummaryText("");
        SummaryText("  DIRECT QUERY: any USB device declaring Bluetooth HCI (E0/01/01)");
        for (;;) {
            USBDeviceDescriptor hd;
            Str255 l;
            herr = USBGetNextDeviceByClass(&href, &hconn, kBTClass, kBTSubClass, kBTProto);
            if (herr != noErr) {
                if (++hstumble > 8) break;
                continue;
            }
            hstumble = 0;
            if (++hn > 8) { SummaryText("    (more than 8; list truncated)"); break; }
            memset(&hd, 0, sizeof(hd));
            if (USBGetDeviceDescriptor(&href, &hd, (UInt32)sizeof(hd)) != noErr) {
                SummaryText("    (a controller answered, descriptor unreadable)");
                continue;
            }
            l[0] = 0;
            PStrCat(l, "    ⭐ HCI CONTROLLER  VID ");
            PStrCatHexN(l, leWord(hd.vendor), 4);
            PStrCat(l, "  PID ");
            PStrCatHexN(l, leWord(hd.product), 4);
            PStrCat(l, "  conf ");
            PStrCatNum(l, (long)hd.numConf);
            Summary(l);
        }
        if (hn == 0) {
            SummaryText("    NONE declared E0/01/01 at DEVICE level.");
            SummaryText("    ⚠ That is not the same as 'no controller present'. A device");
            SummaryText("    may declare class 0x00 and put E0/01/01 on INTERFACE 0 --");
            SummaryText("    driver rule 2b matches that shape, but this app reads only");
            SummaryText("    the DEVICE descriptor and cannot see it. If Software Update");
            SummaryText("    offered to find a driver, the OS DID enumerate something we");
            SummaryText("    did not claim; compare with Apple System Profiler, which");
            SummaryText("    walks the bus properly and has caught this dump missing a");
            SummaryText("    dongle before.");
        } else {
            SummaryText("    ⇒ A controller IS on the bus and declares HCI at device");
            SummaryText("    level, so driver rule 2a should bind it. If there is no");
            SummaryText("    'BTP1' block above, the driver did not load or was not");
            SummaryText("    offered the device -- check the extension (a name");
            SummaryText("    beginning \"USB\") is in");
            SummaryText("    Extensions, enabled, and EXPANDED (a raw MacBinary is not");
            SummaryText("    type 'ndrv' and the Expert skips it in silence).");
        }
    }
}

/* ===========================================================================
 *  WHAT THE PROXY ACTUALLY EXPOSES -- the media-key question, measured
 * ===========================================================================
 *
 * ⭐ THE QUESTION. The A1016 now pairs in OS 9 and TYPES through the card's on-chip
 * proxy. But its volume keys do nothing and its eject key arrives as a function key.
 *
 * The standing explanation is that the proxy presents only BOOT-PROTOCOL HID
 * interfaces. The boot keyboard report is a fixed 8 bytes -- modifiers, a reserved
 * byte, six keycodes -- drawing usages only from the Keyboard/Keypad page (0x07).
 * Volume and eject are Consumer page (0x0C) usages, which no boot collection can
 * express, so they could not arrive at all.
 *
 * ⚠⚠ THAT EXPLANATION HAS NEVER BEEN MEASURED. Worse, this file used to PRINT the
 * premise -- "(iface 0 boot keyboard, iface 1 boot mouse)" -- as hardcoded text inside
 * a measurement report, from a branch that looks at no interface whatsoever. This
 * section replaces the assumption with a reading.
 *
 * ⚠ WHY THE REGISTRY AND NOT A CONTROL TRANSFER. Fetching a HID report descriptor
 * needs a class-specific GET_DESCRIPTOR, i.e. a transfer, and an application may not
 * do transfers -- USBGetConfigurationDescriptor and friends live in USBServicesLib,
 * the DRIVER-level library. Only USBGetNextDeviceByClass, USBGetDeviceDescriptor,
 * USBGetInterfaceDescriptor and USBReferenceToRegEntry are in USBManagerLib. So the
 * route is USBReferenceToRegEntry, then read what the USB Expert has already
 * published into the Name Registry for that node and its children.
 *
 * ⚠⚠ AND THE PROPERTY NAMES ARE NOT GUESSED. This dumps EVERY property on the node
 * and on each child, by name and size, because guessing which one holds a descriptor
 * is how a diagnostic ends up reporting the absence of its own assumption. If the
 * Expert publishes nothing useful, the honest output is "not published here", NOT
 * "the keyboard has no consumer page". [[reference_os9_nameregistry_iterate]]'s design
 * rule: never let an API failure surface as a hardware conclusion. */

#define kBlobBytes   192      /* hex per property; a config descriptor is ~60-100 */
#define kMaxKids     12

/* Self-test, so a silent traversal failure cannot be read as a hardware fact. */
static OSStatus gRegLookupErr = 1;    /* 1 = never attempted */
static short    gRegNodesSeen = 0;

/* ⚠ v96 PRINTED EVERYTHING AS HEX, and several of these properties are ASCII. The
 * first run's most useful single line arrived as "name = [6] 6D 6F 75 73 65 00" and
 * had to be decoded by hand to read "mouse" -- in a log whose whole job is to be read.
 * fw-regdump solved this in 2026-08 and I did not reuse it. Same shape: text if every
 * byte is printable, hex otherwise, NULs shown as | so a NUL-separated list stays
 * visible rather than looking like one string. */
static void PStrCatBlob(Str255 dst, const unsigned char *b, long len, long limit)
{
    long    i;
    Boolean printable = (len > 0);
    long    realChars = 0;

    if (len > limit) len = limit;
    for (i = 0; i < len; i++) {
        unsigned char c = b[i];
        if (c == 0) continue;                        /* NUL separators are fine */
        if (c < 0x20 || c > 0x7E) { printable = false; break; }
        realChars++;
    }
    /* ⚠ AN ALL-ZERO PROPERTY IS A NUMBER, NOT A STRING. The NUL skip above made
     * 00 00 00 00 pass the printable test, so v97 rendered every zero-valued 4-byte
     * field as "||||" -- InterfaceNumber, AlternateSetting, DeviceClass and the rest,
     * all of which are integers and several of which are 0 precisely because that is
     * the interesting value. "InterfaceNumber = [4] ||||" is the keyboard being
     * interface 0, rendered unreadably. Require at least one real character. */
    if (realChars == 0) printable = false;
    if (printable) {
        PStrCatCh(dst, '"');
        for (i = 0; i < len && dst[0] < 240; i++)
            PStrCatCh(dst, b[i] ? (char)b[i] : '|');
        PStrCatCh(dst, '"');
        return;
    }
    for (i = 0; i < len; i++) {
        if (dst[0] > 240) { PStrCat(dst, ".."); return; }
        PStrCatHexN(dst, b[i], 2);
        PStrCatCh(dst, ' ');
    }
}

/* ★ Read a 4-byte big-endian integer property, the shape the USB Expert uses for every
 * numeric descriptor field (InterfaceClass, VendorID, parent-deviceRef and the rest).
 * Returns 0 on any failure and sets *okOut, because a property that is absent and a
 * property that is zero are different facts. */
static unsigned long GetU32Prop(const RegEntryID *id, const char *name, short *okOut)
{
    RegPropertyValueSize sz = 4;
    unsigned char        b[4];
    RegPropertyNameBuf   pn;

    if (okOut != NULL) *okOut = 0;
    memset(pn, 0, sizeof(pn));
    strncpy((char *)pn, name, sizeof(pn) - 1);
    memset(b, 0, sizeof(b));
    if (RegistryPropertyGet((RegEntryID *)id, pn, b, &sz) != noErr) return 0;
    if (sz != 4) return 0;
    if (okOut != NULL) *okOut = 1;
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16)
         | ((unsigned long)b[2] << 8)  |  (unsigned long)b[3];
}

/* ★★ Decode one USB interface node's descriptor fields into a verdict-relevant line.
 *
 * ⭐ THIS IS THE EVIDENCE THE MEDIA-KEY QUESTION ACTUALLY TURNS ON, and the v96 run
 * proved the Expert publishes it: InterfaceClass / InterfaceSubClass /
 * InterfaceProtocol / NumEndpoints, as 4-byte big-endian properties. */
static void DescribeInterfaceNode(const RegEntryID *id)
{
    short okC = 0, okS = 0, okP = 0, okN = 0, okI = 0;
    unsigned long cls = GetU32Prop(id, "InterfaceClass", &okC);
    unsigned long sub = GetU32Prop(id, "InterfaceSubClass", &okS);
    unsigned long pro = GetU32Prop(id, "InterfaceProtocol", &okP);
    unsigned long eps = GetU32Prop(id, "NumEndpoints", &okN);
    unsigned long num = GetU32Prop(id, "InterfaceNumber", &okI);
    Str255 l;

    if (!okC && !okS && !okP) return;    /* not an interface node */

    l[0] = 0;
    PStrCat(l, "      ⇒ INTERFACE ");
    if (okI) PStrCatNum(l, (long)num); else PStrCat(l, "?");
    PStrCat(l, ": class ");
    PStrCatHexN(l, cls, 2);
    PStrCat(l, " sub ");
    PStrCatHexN(l, sub, 2);
    PStrCat(l, " proto ");
    PStrCatHexN(l, pro, 2);
    PStrCat(l, "  endpoints ");
    if (okN) PStrCatNum(l, (long)eps); else PStrCat(l, "?");
    Summary(l);

    if (cls == 0x03) {
        if (sub == 0x01) {
            if (pro == 0x01)      SummaryText("        HID, BOOT subclass, KEYBOARD");
            else if (pro == 0x02) SummaryText("        HID, BOOT subclass, MOUSE");
            else                  SummaryText("        HID, BOOT subclass, other protocol");
            SummaryText("        ⚠ Boot SUBCLASS means boot protocol is SUPPORTED. It");
            SummaryText("        does NOT prove the report descriptor is boot-only.");
        } else if (sub == 0x00) {
            SummaryText("        ⭐⭐ HID, NON-BOOT subclass. This kind of interface");
            SummaryText("        carries an arbitrary report descriptor, so a Consumer");
            SummaryText("        page -- volume, eject -- CAN live here.");
        }
    }
}

/* ⭐ THE ONE BYTE PAIR THAT ANSWERS THE QUESTION.
 *
 * A HID report descriptor writes "Usage Page (Consumer Devices)" as the item 0x05 0x0C
 * -- a 1-byte Global item, tag Usage Page, value 0x0C. If that pair appears in a
 * report descriptor the device can express volume and eject; if it appears nowhere,
 * it cannot.
 *
 * ⚠ A SUBSTRING SEARCH IS EVIDENCE, NOT PROOF, and the difference matters. 05 0C can
 * occur by coincidence inside unrelated bytes, and a report descriptor may not be in
 * the registry at all. Reported as "a candidate at offset N" for that reason -- the
 * offset is printed so the surrounding bytes can be judged by eye rather than trusted.
 */
static short BlobHasConsumerPage(const unsigned char *b, long len, long *whereOut)
{
    long i;
    for (i = 0; i + 1 < len; i++) {
        if (b[i] == 0x05 && b[i + 1] == 0x0C) {
            if (whereOut != NULL) *whereOut = i;
            return 1;
        }
    }
    return 0;
}

/* Walk a standard USB configuration descriptor chain and name what is in it. This is
 * the part that decides the question even WITHOUT a report descriptor: an interface
 * declaring subclass 0x01 (Boot Interface Subclass) with protocol 1 or 2 is a boot
 * keyboard or mouse, and a HID descriptor (type 0x21) states its report descriptor's
 * length even when the descriptor itself is out of reach. */
static void ParseConfigBlob(const unsigned char *b, long len)
{
    long  i = 0;
    short ifaces = 0, bootIfaces = 0, hidDescs = 0;

    if (len < 2 || b[1] != 0x02) return;      /* not a configuration descriptor */
    SummaryText("      -- parsed as a CONFIGURATION descriptor --");
    while (i + 1 < len) {
        unsigned char bLen = b[i], bType = b[i + 1];
        if (bLen < 2) break;                  /* malformed; stop rather than loop */
        if (bType == 0x04 && i + 8 < len) {   /* INTERFACE */
            Str255 l; l[0] = 0;
            ifaces++;
            PStrCat(l, "      iface ");
            PStrCatNum(l, (long)b[i + 2]);
            PStrCat(l, "  class ");
            PStrCatHexN(l, b[i + 5], 2);
            PStrCat(l, " sub ");
            PStrCatHexN(l, b[i + 6], 2);
            PStrCat(l, " proto ");
            PStrCatHexN(l, b[i + 7], 2);
            if (b[i + 5] == 0x03) {
                PStrCat(l, "  HID");
                if (b[i + 6] == 0x01) {
                    bootIfaces++;
                    PStrCat(l, " BOOT");
                    if (b[i + 7] == 0x01)      PStrCat(l, " keyboard");
                    else if (b[i + 7] == 0x02) PStrCat(l, " mouse");
                } else if (b[i + 6] == 0x00) {
                    PStrCat(l, " non-boot");
                }
            }
            Summary(l);
        } else if (bType == 0x21 && i + 8 < len) {   /* HID class descriptor */
            Str255 l; l[0] = 0;
            hidDescs++;
            PStrCat(l, "        HID desc: report descriptor is ");
            PStrCatNum(l, (long)(b[i + 7] | ((long)b[i + 8] << 8)));
            PStrCat(l, " bytes (type ");
            PStrCatHexN(l, b[i + 6], 2);
            PStrCat(l, ")");
            Summary(l);
        }
        i += bLen;
    }
    Row("      interfaces total", (unsigned long)ifaces);
    Row("      boot-subclass HID", (unsigned long)bootIfaces);
    Row("      HID descriptors", (unsigned long)hidDescs);
    /* ⚠ GATED. A report descriptor length is not its contents: boot subclass says the
     * device SUPPORTS boot protocol, not that boot protocol is all it has. The verdict
     * belongs to whoever reads the report descriptor, not to this count. */
    if (ifaces > 0 && bootIfaces == ifaces) {
        SummaryText("      ⇒ EVERY interface here is boot-subclass HID. Consistent with");
        SummaryText("        the media-key explanation -- but boot SUBCLASS means boot");
        SummaryText("        protocol is SUPPORTED, not that it is all there is. A");
        SummaryText("        report descriptor with a Consumer collection can sit");
        SummaryText("        behind a boot-capable interface. Read the descriptor.");
    } else if (ifaces > bootIfaces) {
        SummaryText("      ⭐ NOT every interface is boot-subclass. A non-boot HID");
        SummaryText("        interface can carry a Consumer page, so the media keys may");
        SummaryText("        be reachable after all. Read its report descriptor next.");
    }
}

/* Dump every property on one registry node, hex-dumping small ones, and flag anything
 * that looks like a USB descriptor or contains a Consumer-page item. */
static void DumpNodeProps(const RegEntryID *id, const char *what)
{
    RegPropertyIter    cookie;
    RegPropertyNameBuf pname;
    Boolean            done = false;
    unsigned char      buf[kBlobBytes];
    Str255             l;

    l[0] = 0;
    PStrCat(l, "    ");
    PStrCat(l, what);
    PStrCat(l, ":");
    Summary(l);

    if (RegistryPropertyIterateCreate((RegEntryID *)id, &cookie) != noErr) {
        SummaryText("      <property iteration unavailable on this node>");
        return;
    }
    for (;;) {
        RegPropertyValueSize sz = 0, got;
        long                 where = 0;

        memset(pname, 0, sizeof(pname));
        if (RegistryPropertyIterate(&cookie, pname, &done) != noErr || done) break;

        l[0] = 0;
        PStrCat(l, "      ");
        PStrCat(l, pname);
        PStrCat(l, " = ");
        if (RegistryPropertyGetSize((RegEntryID *)id, pname, &sz) != noErr) {
            PStrCat(l, "<size failed>");
            Summary(l);
            continue;
        }
        PStrCat(l, "[");
        PStrCatNum(l, (long)sz);
        PStrCat(l, "] ");
        if (sz == 0) { PStrCat(l, "<empty>"); Summary(l); continue; }

        got = (sz <= (RegPropertyValueSize)sizeof(buf))
              ? sz : (RegPropertyValueSize)sizeof(buf);
        memset(buf, 0, sizeof(buf));
        if (RegistryPropertyGet((RegEntryID *)id, pname, buf, &got) != noErr) {
            PStrCat(l, "<read failed>");
            Summary(l);
            continue;
        }
        PStrCatBlob(l, buf, (long)got, kBlobBytes);
        Summary(l);

        /* ⭐ Now say what it might BE, rather than leaving the reader to spot it. */
        if (got >= 2 && buf[0] >= 2 && buf[0] <= got) {
            if (buf[1] == 0x02) ParseConfigBlob(buf, (long)got);
            else if (buf[1] == 0x22)
                SummaryText("      ⭐⭐ descriptor type 0x22 = a HID REPORT DESCRIPTOR.");
        }
        if (BlobHasConsumerPage(buf, (long)got, &where)) {
            l[0] = 0;
            PStrCat(l, "      ⭐⭐ CANDIDATE Usage Page (Consumer) item 05 0C at offset ");
            PStrCatNum(l, where);
            Summary(l);
            SummaryText("        ⚠ A byte-pair match, not proof. Judge the surrounding");
            SummaryText("        bytes above by eye before concluding anything.");
        }
    }
    RegistryPropertyIterateDispose(&cookie);
    /* ★ And say what the node IS, after listing what it holds. */
    DescribeInterfaceNode(id);
}

/* The section itself. Takes the device reference the bus dump captured for the card. */
static void ReportProxyDescriptors(USBDeviceRef target, unsigned long vidpid)
{
    RegEntryID node, kid;
    RegEntryIter kids;
    Boolean done = false;
    short kidsSeen = 0;

    SummaryText("=========================================================");
    SummaryText("  WHAT THE PROXY EXPOSES - the media-key question");
    SummaryText("=========================================================");

    if (target == 0) {
        SummaryText("  NO TARGET. The bus dump did not identify an Apple Bluetooth");
        SummaryText("  device this run, so there is nothing to look up. This is a");
        SummaryText("  statement about the dump, NOT about the card.");
        return;
    }
    RowHex("target VID:PID", vidpid, 8);

    RegistryEntryIDInit(&node);
    /* ⚠ ARGUMENT ORDER: (RegEntryID *out, USBDeviceRef in) -- OUT FIRST, which is the
     * opposite of the USBGetDeviceDescriptor(&ref, &desc, size) shape right above it
     * in this file. I wrote it ref-first and GCC caught both conversions; on hardware
     * it would have handed a device reference to a pointer parameter. Also note the
     * header calls both parameters "parentEntry"/"parentDeviceRef", which is why it is
     * worth reading rather than pattern-matching from the neighbouring calls. */
    gRegLookupErr = USBReferenceToRegEntry(&node, target);
    Row("USBReferenceToRegEntry err", (unsigned long)(long)gRegLookupErr);
    if (gRegLookupErr != noErr) {
        SummaryText("  ⚠ THE LOOKUP FAILED, so nothing below is a fact about the card.");
        SummaryText("  USBReferenceToRegEntry is USBManagerLib and app-legal, so a");
        SummaryText("  failure here is about the reference or the Expert's tree -- it");
        SummaryText("  is NOT evidence that the device exposes no descriptors.");
        RegistryEntryIDDispose(&node);
        return;
    }
    gRegNodesSeen = 1;
    /* ⚠⚠ NOT THE DEVICE NODE, WHICH IS WHAT v96 CALLED IT AND WHAT I EXPECTED.
     *
     * The v96 run resolved this reference to an INTERFACE node -- AAPL,USBNodeType
     * "usbi", name "mouse", InterfaceNumber 1, class 03 sub 01 proto 02. So the ref the
     * bus dump hands out maps to one interface, not to the device, and iterating its
     * children found nothing because interfaces are leaves. `children seen 0` was
     * correct and completely uninformative.
     *
     * ⇒ The heading now says what the node actually is instead of asserting a shape.
     * The device node is reached below, through this node's own parent-deviceRef. */
    DumpNodeProps(&node, "the node this reference resolves to");

    /* ⚠⚠ THE INTERFACES ARE CHILDREN, AND REACHING THEM NEEDS TWO CALLS, NOT ONE.
     *
     * RegistryEntryIterateCreate "defaults to ROOT with relationship =
     * kRegIterDescendants" -- the header says so in as many words. So iterating with
     * kRegIterChildren straight after Create walks THE REGISTRY ROOT'S children and
     * hands them back with no error at all. My first version did exactly that, and it
     * would have printed the root's children under the heading "the card's interfaces":
     * plausible, wrong, and silent. RegistryEntryIterateSet is what roots the iterator
     * on the node we actually care about.
     *
     * This is the same trap as [[reference_os9_nameregistry_iterate]] wearing different
     * clothes -- there it was kRegIterContinue on a whole-tree walk returning zero
     * nodes; here it is a missing Set returning the WRONG nodes, which is worse because
     * zero at least looks like a failure. Both cost nothing to prevent and a hardware
     * cycle to discover. */
    if (RegistryEntryIterateCreate(&kids) == noErr) {
        OSStatus setErr = RegistryEntryIterateSet(&kids, &node);
        Row("IterateSet err", (unsigned long)(long)setErr);
        if (setErr != noErr) {
            SummaryText("    ⚠ THE ITERATOR IS NOT ROOTED ON THIS DEVICE, so its");
            SummaryText("    children are NOT read. Nothing is printed rather than");
            SummaryText("    printing whatever the unrooted iterator would return.");
        } else {
            for (;;) {
                RegistryEntryIDInit(&kid);
                /* ⚠ Relationship first, then kRegIterContinue -- see the long note at
                 * the device-node loop below for what passing it every time did. */
                if (RegistryEntryIterate(&kids,
                        (kidsSeen == 0) ? kRegIterChildren : kRegIterContinue,
                        &kid, &done) != noErr || done) {
                    RegistryEntryIDDispose(&kid);
                    break;
                }
                if (++kidsSeen > kMaxKids) {
                    RegistryEntryIDDispose(&kid);
                    SummaryText("    (child list truncated)");
                    break;
                }
                {
                    /* ⚠ NUMBERED. The first version passed the literal "child node"
                     * for every one, so a dump whose entire purpose is telling the
                     * interfaces apart labelled them identically. */
                    char w[24];
                    w[0] = 'c'; w[1] = 'h'; w[2] = 'i'; w[3] = 'l'; w[4] = 'd';
                    w[5] = ' ';
                    w[6] = (char)('0' + (kidsSeen / 10) % 10);
                    w[7] = (char)('0' + kidsSeen % 10);
                    w[8] = 0;
                    DumpNodeProps(&kid, w);
                    gRegNodesSeen++;
                }
                RegistryEntryIDDispose(&kid);
            }
        }
        RegistryEntryIterateDispose(&kids);
    } else {
        SummaryText("    <child iteration unavailable>");
    }
    Row("children seen", (unsigned long)kidsSeen);
    /* ★★★ UP TO THE DEVICE, THEN BACK DOWN TO EVERY INTERFACE.
     *
     * The v96 run showed the reference resolves to ONE interface, so its siblings --
     * including interface 0, the keyboard, which is the one the media-key question is
     * about -- were never seen. But that same dump published the way out:
     * `parent-deviceRef = 04 81 35 FC`. Resolve THAT and the device node's children
     * are the complete interface list. The answer was in the first run's own output. */
    {
        short okP = 0;
        unsigned long pref = GetU32Prop(&node, "parent-deviceRef", &okP);
        SummaryText("");
        SummaryText("  ⭐ UP TO THE DEVICE NODE, VIA parent-deviceRef");
        if (!okP || pref == 0) {
            SummaryText("    no parent-deviceRef on this node, so the sibling");
            SummaryText("    interfaces cannot be reached this way. The whole-registry");
            SummaryText("    sweep below is the fallback.");
        } else {
            RegEntryID dev, ifn;
            RegEntryIter it;
            OSStatus e;
            RowHex("    parent-deviceRef", pref, 8);
            RegistryEntryIDInit(&dev);
            e = USBReferenceToRegEntry(&dev, (USBDeviceRef)pref);
            Row("    lookup err", (unsigned long)(long)e);
            if (e == noErr) {
                gRegNodesSeen++;
                DumpNodeProps(&dev, "device node");
                if (RegistryEntryIterateCreate(&it) == noErr) {
                    OSStatus se = RegistryEntryIterateSet(&it, &dev);
                    Row("    IterateSet err", (unsigned long)(long)se);
                    if (se == noErr) {
                        short k = 0;
                        Boolean d2 = false;
                        for (;;) {
                            RegistryEntryIDInit(&ifn);
                            /* ⚠⚠⚠ THE RELATIONSHIP GOES ON THE FIRST CALL ONLY, AND
                             * kRegIterContinue ON EVERY CALL AFTER IT.
                             *
                             * v97 passed kRegIterChildren every time and reported
                             * "interfaces found 1" over a device whose name is
                             * "composite" and which we already knew had at least two.
                             * NameRegistry.h labels kRegIterContinue "Keep doing the
                             * same thing"; a relationship is always relative to the
                             * CURRENT entry, so the second call asked for the children
                             * of interface 0 -- a leaf -- and set done. One child, no
                             * error, and my own text underneath then declared it "the
                             * complete interface list for the proxy".
                             *
                             * ⇒ A conclusion manufactured by the defect it was meant to
                             * measure around. This is [[reference_os9_nameregistry_iterate]]
                             * exactly, in the variant that note does not spell out:
                             * there it returned ZERO nodes on a whole-tree walk, here it
                             * returned the FIRST ONE ONLY on a child walk. Zero looks
                             * broken; one looks like an answer. */
                            if (RegistryEntryIterate(&it,
                                    (k == 0) ? kRegIterChildren : kRegIterContinue,
                                    &ifn, &d2) != noErr || d2) {
                                RegistryEntryIDDispose(&ifn);
                                break;
                            }
                            if (++k > kMaxKids) {
                                RegistryEntryIDDispose(&ifn);
                                SummaryText("    (interface list truncated)");
                                break;
                            }
                            {
                                char w[24];
                                strncpy(w, "interface node 0", sizeof(w) - 1);
                                w[15] = (char)('0' + (k - 1) % 10);
                                w[16] = 0;
                                DumpNodeProps(&ifn, w);
                                gRegNodesSeen++;
                            }
                            RegistryEntryIDDispose(&ifn);
                        }
                        Row("    interfaces found", (unsigned long)k);
                        /* ⚠⚠ NO COMPLETENESS CLAIM HERE ANY MORE. v97's text said "That
                         * is the complete interface list for the proxy" underneath a
                         * count its own iteration bug had capped at 1. A count is a
                         * count; whether it is the whole list is settled by the
                         * INDEPENDENT sweep below agreeing with it, not by this loop
                         * vouching for itself. */
                        if (k == 1)
                            SummaryText("    ⚠ ONE interface only. Cross-check the sweep");
                        else if (k > 1)
                            SummaryText("    ⇒ Cross-check the count against the sweep");
                    }
                    RegistryEntryIterateDispose(&it);
                }
                RegistryEntryIDDispose(&dev);
            } else {
                SummaryText("    ⚠ the parent lookup FAILED, so this says nothing about");
                SummaryText("    how many interfaces the proxy has.");
            }
        }
    }
    /* ★★★ AN INDEPENDENT SWEEP, because the child walk has now been wrong twice.
     *
     * v96 read one interface and called it the device. v97 read one interface of a
     * "composite" device and called it the complete list. Both times the loop was the
     * only witness to its own correctness, and both times it was wrong in a way that
     * produced a plausible number rather than an obvious failure.
     *
     * ⇒ This walks the WHOLE registry -- Create defaults to root with descendants, so
     * kRegIterContinue on every call is the correct form here, which is the case
     * [[reference_os9_nameregistry_iterate]] does document -- and counts every node
     * whose VendorID and ProductID match the card. Two routes that agree are worth far
     * more than one route that asserts. If they disagree, BELIEVE NEITHER and say so. */
    {
        RegEntryIter  sweep;
        RegEntryID    e;
        Boolean       sdone = false;
        short         seen = 0, ifaces = 0, boots = 0, nonBoot = 0, guard = 0;

        SummaryText("");
        SummaryText("  ⭐ INDEPENDENT SWEEP - the whole registry, matched by VID:PID");
        if (RegistryEntryIterateCreate(&sweep) != noErr) {
            SummaryText("    <whole-registry iteration unavailable>");
        } else {
            for (;;) {
                short okV = 0, okP2 = 0;
                RegistryEntryIDInit(&e);
                if (RegistryEntryIterate(&sweep, kRegIterContinue, &e, &sdone) != noErr
                    || sdone) {
                    RegistryEntryIDDispose(&e);
                    break;
                }
                if (++guard > 4000) {          /* the tree is finite; do not trust it */
                    RegistryEntryIDDispose(&e);
                    SummaryText("    (sweep bailed at 4000 nodes)");
                    break;
                }
                if (GetU32Prop(&e, "VendorID", &okV) == 0x05AC && okV
                    && GetU32Prop(&e, "ProductID", &okP2) == (vidpid & 0xFFFF) && okP2) {
                    short okC = 0;
                    unsigned long cls = GetU32Prop(&e, "InterfaceClass", &okC);
                    seen++;
                    if (okC) {
                        ifaces++;
                        if (cls == 0x03) {
                            short okS = 0;
                            unsigned long sub = GetU32Prop(&e, "InterfaceSubClass", &okS);
                            if (okS && sub == 0x01) boots++;
                            else if (okS && sub == 0x00) nonBoot++;
                        }
                        DescribeInterfaceNode(&e);
                    }
                }
                RegistryEntryIDDispose(&e);
            }
            RegistryEntryIterateDispose(&sweep);
            Row("    nodes walked", (unsigned long)guard);
            Row("    matching this card", (unsigned long)seen);
            Row("    of those, interfaces", (unsigned long)ifaces);
            Row("    HID boot subclass", (unsigned long)boots);
            Row("    HID NON-boot subclass", (unsigned long)nonBoot);

            /* ⚠ THE VERDICT, AND IT STOPS SHORT ON PURPOSE. */
            if (guard <= 1) {
                SummaryText("    ⚠ THE SWEEP WALKED NOTHING, so it cannot corroborate");
                SummaryText("    anything. Believe neither count. VOID, not negative.");
            } else if (nonBoot > 0) {
                SummaryText("    ⭐⭐⭐ A NON-BOOT HID INTERFACE EXISTS. It can carry an");
                SummaryText("    arbitrary report descriptor, so the volume and eject");
                SummaryText("    keys may be reachable. Read ITS report descriptor.");
            } else if (ifaces > 0 && boots == ifaces) {
                SummaryText("    ⇒ EVERY HID interface on this card is boot subclass,");
                SummaryText("    found by two independent routes. The boot report is a");
                SummaryText("    fixed 8 bytes carrying only Keyboard/Keypad page (0x07)");
                SummaryText("    usages, and volume and eject are Consumer page (0x0C).");
                SummaryText("    ⚠ STILL NOT PROOF. Boot SUBCLASS means boot protocol is");
                SummaryText("    SUPPORTED; a report descriptor can hold a Consumer");
                SummaryText("    collection behind a boot-capable interface, and no");
                SummaryText("    report descriptor is published anywhere in this tree.");
                SummaryText("    This is a STRONG EXPLANATION, not a settled fact, and");
                SummaryText("    the difference is the whole reason for this section.");
            }
        }
    }
    Row("registry nodes read", (unsigned long)gRegNodesSeen);
    SummaryText("  ⚠ IF NO DESCRIPTOR BLOB APPEARED ABOVE, the finding is that the USB");
    SummaryText("  Expert does not publish one on this node -- NOT that the keyboard");
    SummaryText("  has no Consumer page. Those are different claims and only the first");
    SummaryText("  one is supported by this run.");
    RegistryEntryIDDispose(&node);
}

/* ★★ USBBluetoothSwitch's report.
 *
 * ⚠ PRINTED WHETHER OR NOT THE DRIVER'S BLOCK EXISTS, and that is the whole point. A
 * run where the driver bound nothing is exactly the run where the only question worth
 * answering is "did the switcher fire, and what did the card say?" -- and until now
 * that run produced no evidence at all beyond a bus dump. Three of five runs were lost
 * that way. */
static void SwitchReport(void)
{
    Str255 line;

    SummaryText("");
    SummaryText("=========================================================");
    SummaryText("  USBBluetoothSwitch - did the card get moved out of proxy?");
    SummaryText("=========================================================");

    if (!gSwFound) {
        /* ★ v99.102: say which of the three it is when we can. A dongle-only machine
         * has no 'BTSW' block FOREVER and nothing is wrong -- the switcher's only job
         * is taking the INTERNAL card out of proxy mode. Reporting that as "not
         * installed" sent the user looking for a missing extension that the INSTALLED
         * FILES section above prints by name. The panel's Card switch line now draws
         * the same distinction. */
        if (gC[BestBlock()][kWIfaceVIDPID] != 0
            && ((gC[BestBlock()][kWIfaceVIDPID] >> 16) & 0xFFFFUL) != 0x05ACUL) {
            SummaryText("  NO 'BTSW' BLOCK, AND THAT IS CORRECT HERE. The driver bound a");
            SummaryText("  NON-APPLE controller, so there is no A1044 in HID proxy for");
            SummaryText("  the switcher to claim -- if there were, it would have claimed");
            SummaryText("  it and left a block. Nothing is missing and nothing is wrong.");
            SummaryText("  ⚠ Check INSTALLED FILES above for whether the extension is");
            SummaryText("    actually present; this section cannot tell you that.");
            return;
        }
        SummaryText("  NO 'BTSW' BLOCK. The switcher extension is not installed, is");
        SummaryText("  disabled, or never got as far as Initialize.");
        SummaryText("  ⚠ If the card shows as 05AC:1000 in the dump above and there is");
        SummaryText("    no block here, that is the ONE combination that really does");
        SummaryText("    point at the install: check USBBluetoothSwitch is in");
        SummaryText("    Extensions, enabled, and EXPANDED (a raw MacBinary is not");
        SummaryText("    type 'ndrv' and the Expert skips it in silence). Its name must");
        SummaryText("    begin \"USB\" -- the Expert scans nothing else.");
        return;
    }

    line[0] = 0;
    PStrCat(line, "  BTSW block @0x"); PStrCatHexN(line, gSwAt, 8);
    PStrCat(line, "   build '");       PStrCatOSType(line, gSw[kSwBuild]);
    PStrCat(line, "'");
    Summary(line);

    Row("Initialize calls",      gSw[kSwInit]);
    Row("Finalize calls",        gSw[kSwFinal]);
    Row("matched the proxy card",gSw[kSwSeenProxy]);
    /* ★★★★★ THE HANDBACK STORY, and it needs the two rows read together.
     *
     * v8.5's reverse switch worked -- the card really does return to 05AC:1000 on
     * command -- but the keyboard did not come back, and this block is what said why:
     * "Initialize calls 2 / matched the proxy card 2 / switch attempts 7". The
     * switcher re-claimed the handed-back card and then did nothing with it, because
     * its attempt budget lives in this shared block so it survives re-enumeration.
     * A device held by a driver that will not drive it is a device its real driver
     * never gets.
     *
     * Switcher 1.1 stands down instead: once it has switched successfully, any later
     * appearance at 1000 is a deliberate handback and belongs to OS 9's own
     * USBHIDKeyboardModule. */
    /* ★★★★★★ IDLE AT BOOT (switcher 1.2). The shipping shape: the card is claimed
     * only for the one boot after the panel asks, so a paired keyboard types from
     * startup every other time. */
    /* ★★★ SWITCHER 1.3 INVERTED THE DEFAULT. It now switches on EVERY boot, and
     * declines only when the self-healing marker says the PREVIOUS boot switched and
     * the driver never confirmed. So a decline is no longer "nobody asked" -- it is
     * "last boot did not come up", which is a FAULT REPORT, not the resting state. */
    if (gSw[kSwMarkWrote] != 0) {
        Row("⭐ marker written, switching", gSw[kSwMarkWrote]);
        SummaryText("    The healthy path: no stale marker, so this boot claimed the card.");
        SummaryText("    The driver deletes the marker once it binds -- see the BTP1 rows.");
    }
    if (gSw[kSwMarkStale] != 0) {
        Row("⚠⚠ STALE MARKER - declined", gSw[kSwMarkStale]);
        SummaryText("    ⚠⚠ THE PREVIOUS BOOT SWITCHED THE CARD AND THE DRIVER NEVER CAME");
        SummaryText("    UP. This boot therefore left the card with Mac OS, which is why");
        SummaryText("    the keyboard works -- the fallback did its job.");
        SummaryText("    ⇒ THIS IS A FAULT REPORT ABOUT THE LAST BOOT, not a resting");
        SummaryText("    state. Something stopped the driver binding: check it is");
        SummaryText("    installed and EXPANDED, and read the BTP1 section -- if there is");
        SummaryText("    no block at all, the fragment did not load.");
        /* ⚠⚠ THIS PARAGRAPH USED TO READ "It self-heals: this boot cleared nothing, so
         * the NEXT boot tries again", which is a non-sequitur -- clearing nothing is
         * exactly WHY the next boot did not try again. The diagnostic asserted a
         * self-heal that the code never performed, for three versions. Switcher 1.5
         * makes it true; read the row below to know which behaviour is installed. */
        if (gSw[kSwMarkCleared] != 0) {
            SummaryText("    ⇒ AND THIS BOOT CLEARED IT (1.5+), so the fallback lasted");
            SummaryText("    exactly one boot and the NEXT boot switches again. Nothing");
            SummaryText("    to do -- just restart.");
        } else if ((gSw[kSwBuild] & 0xFFFFUL) >= 0x3135UL) {
            SummaryText("    ⚠⚠ AND THIS BOOT FAILED TO CLEAR IT -- read the marker");
            SummaryText("    error below. It IS still latched, and only File > Turn On");
            SummaryText("    Pairing at Restart will get past it.");
        } else {
            SummaryText("    ⚠⚠ SWITCHER OLDER THAN 1.5: this boot cleared NOTHING, so");
            SummaryText("    every later boot declines too -- Bluetooth is silently off");
            SummaryText("    until you use File > Turn On Pairing at Restart. 1.5 fixes");
            SummaryText("    this; the row above says which build is installed.");
        }
    }
    if (gSw[kSwMarkErr] != 0) {
        RowHex("⚠ marker read/write error", gSw[kSwMarkErr], 8);
        SummaryText("    Preferences unreachable, or the marker could not be created.");
        SummaryText("    Every such failure DECLINES, which is where the keyboard works.");
    }
    if (gSw[kSwIdleDecline] != 0) {
        Row("⭐ IDLE - declined at boot", gSw[kSwIdleDecline]);
        SummaryText("    It matched the card and LEFT IT ALONE. Read the marker rows");
        SummaryText("    above for WHY -- a stale marker means last boot failed, and a");
        SummaryText("    marker error means we could not find out.");
        /* ★★★ 1.6: A COUNT ABOVE 1 IS THE STICKY STAND-DOWN, NOT A REPEATED FAULT.
         * 1.5 declined match 1 and then SWITCHED on match 2, because clearing the marker
         * on the way out left the second match a clean slate -- measured 2026-09-19 as
         * `STALE MARKER - declined 1` beside `marker written, switching 1` in ONE boot.
         * 1.6 makes the decision stick for the session, so the expected healthy shape of
         * a stood-down boot on an EHCI machine is now 2, matching `matched the proxy
         * card`. Seeing 1 there with `marker written` beside it is the 1.5 defect. */
        if (gSw[kSwIdleDecline] > 1) {
            SummaryText("    ⭐ MORE THAN ONE = the 1.6 sticky stand-down working. The");
            SummaryText("    card is offered twice per boot when the USB 2.0 extension");
            SummaryText("    is present, and ALL of those matches must stand down or");
            SummaryText("    the fallback only lasts one match. Expect this to equal");
            SummaryText("    'matched the proxy card' on a boot that stood down.");
        } else if (gSw[kSwMarkWrote] != 0) {
            SummaryText("    ⚠⚠ DECLINED ONCE **AND** WROTE A MARKER IN THE SAME BOOT.");
            SummaryText("    That is the 1.5 defect: one match stood down, a later one");
            SummaryText("    switched anyway, so the fallback did not last the boot.");
            SummaryText("    Switcher 1.6 fixes it -- check the build tag above.");
        }
    }
    /* ★★★ SWITCHER 1.4's ROW, AND THE RUN IS BLIND WITHOUT IT. The whole question a
     * 1.4 boot asks is "did the second match get its attempt", and the answer lives in
     * exactly one word. Printed unconditionally on a 'sw14'+ block -- a ZERO here is a
     * real answer (the override was never needed, or was not granted), not a missing
     * one, which is why this is not wrapped in a != 0 test like its neighbours. */
    if ((gSw[kSwBuild] & 0xFFFFUL) >= 0x3134UL) {
        Row("⭐ stale-ref retry granted (1.4+)", gSw[kSwRefRetry]);
        SummaryText("    1 = an earlier match THIS BOOT died with -6998 on a stale");
        SummaryText("    device ref, and 1.4 overrode the marker to give the next");
        SummaryText("    match -- the one holding a LIVE ref -- its attempt. That is");
        SummaryText("    the EHCI-coexistence path working as designed.");
        SummaryText("    0 = never needed it, or the budget was already spent. Read");
        SummaryText("    'switch attempts' and 'last immediate err' beside it.");
        SummaryText("    ⚠ Capped at one per boot ON PURPOSE: past the decline the");
        SummaryText("    switcher OWNS the card, so a failed switch leaves the A1016");
        SummaryText("    with no proxy to talk through. One retry, then fail-safe.");
    }
    if (gSw[kSwFlagSeen] != 0) {
        Row("pair-mode flag consumed", gSw[kSwFlagSeen]);
        SummaryText("    The panel had asked for pairing mode and this boot took it.");
        SummaryText("    ⚠ CONSUMED BEFORE THE SWITCH, never after: the switch makes");
        SummaryText("    the card vanish mid-request, so a delete afterwards might");
        SummaryText("    never run and the flag would fire on EVERY later boot -- a");
        SummaryText("    permanently dead keyboard with no visible cause.");
    }
    if (gSw[kSwFlagErr] != 0) {
        RowHex("⚠ pair-mode flag error", gSw[kSwFlagErr], 8);
        SummaryText("    ⚠ Reading or deleting the flag failed, so the card was NOT");
        SummaryText("    claimed. That is the safe direction -- the keyboard still");
        SummaryText("    works -- but pairing will not start until this is fixed.");
        SummaryText("    -43 fnfErr here is NOT an error: it means nobody asked.");
    }
    if (gSw[kSwStoodDown] != 0) {
        Row("⭐ STOOD DOWN (handback)", gSw[kSwStoodDown]);
        SummaryText("    ⭐ It matched the card again and REFUSED it, on purpose. The");
        SummaryText("    switch had already happened this boot, so this was the card");
        SummaryText("    coming BACK from a Hand Bluetooth Back to Mac OS -- and it is");
        SummaryText("    OS 9's own USBHIDKeyboardModule that should have it now.");
        SummaryText("    ⇒ Check the bus dump for 05AC:1000, then try the keyboard.");
        SummaryText("    If it still does not type, declining is NOT enough: the");
        SummaryText("    Expert does not pass a refused device on, and the switcher");
        SummaryText("    must not be resident at handback time at all.");
    } else if (gSw[kSwSeenProxy] > 1 && gSw[kSwVerdict] != kVerdictSwitched) {
        /* ⚠⚠ NARROWED 2026-09-17, because this fired on the run that PROVED Gate 2.
         * The old guard was `kSwSeenProxy > 1` alone, and as of switcher 1.4 matching
         * twice is the NORMAL signature when the USB 2.0 extension is present: the
         * card's root port changes hands mid-boot, so the Expert offers it to us again.
         * A healthy 1.4 run reads `matched 2` + `stale-ref retry granted 1` + VERDICT
         * SWITCHED, and this paragraph told its reader to go chase a refuted v8.5
         * diagnosis instead. Gating on the verdict keeps the real fault visible -- a
         * double match that never switched IS still the handback failure -- and stops
         * the false alarm on a success. */
        SummaryText("    ⚠⚠ MATCHED MORE THAN ONCE, NEVER STOOD DOWN, AND NEVER");
        SummaryText("    SWITCHED. Two shapes fit, so read 'stale-ref retry granted':");
        SummaryText("    0 = the v8.5 handback failure -- the card was handed back,");
        SummaryText("    this driver re-claimed it with its budget spent, and OS 9's");
        SummaryText("    HID driver never got it. Switcher 1.1 fixed that one.");
        SummaryText("    1 = both refs were stale, so the re-enumeration is not what");
        SummaryText("    invalidates them and the 1.4 model is wrong. Read");
        SummaryText("    'last immediate err' and docs/RELEASE-GATES.md.");
    }
    if (gSw[kSwSeenOther] != 0) {
        Row("⚠ matched SOMETHING ELSE", gSw[kSwSeenOther]);
        RowHex("  its VID:PID",         gSw[kSwOtherVid], 8);
        SummaryText("    ⚠⚠ It refused that device, but a rule pinned to 05AC:1000");
        SummaryText("      should never have been offered it at all. Investigate");
        SummaryText("      before trusting anything else in this run.");
    }
    Row("switch attempts",       gSw[kSwTried]);
    Row("trampoline runs",       gSw[kSwDeferRuns]);
    Row("pipe stalls seen",      gSw[kSwStalls]);
    RowHex("last immediate err", gSw[kSwImmErr], 8);
    RowHex("last completion status", gSw[kSwStatus], 8);

    /* ⚠⚠ A TIMEOUT IS SUCCESS AND A CLEAN COMPLETION IS FAILURE. The card changes USB
     * personality mid-request, so it never completes the handshake. Anyone reading the
     * status word without this in front of them draws exactly the wrong conclusion,
     * which is why it is spelled out here rather than left to be remembered. */
    switch (gSw[kSwVerdict]) {
    case kVerdictSwitched:
        SummaryText("  VERDICT: *** SWITCHED ***  (timed out = SUCCESS here)");
        SummaryText("    The card stopped answering mid-request, which is what happens");
        SummaryText("    when it changes USB personality. Expect 05AC:8204 in the dump");
        SummaryText("    and the driver's 'BTP1' block below.");
        break;
    case kVerdictIgnored:
        SummaryText("  VERDICT: IGNORED - the request completed CLEANLY, which means");
        SummaryText("    the card did NOT switch. BlueZ calls this EALREADY: either it");
        SummaryText("    was already in HCI mode, or it is not a switchable part.");
        break;
    case kVerdictStalled:
        SummaryText("  VERDICT: STALLED - the control pipe stalled. The stall was");
        SummaryText("    cleared and a retry deferred. ⚠ This path has never fired on");
        SummaryText("    the A1044 before, so read the clear rc carefully.");
        RowHex("    clear rc", gSw[kSwClearRc], 8);
        break;
    case kVerdictOther:
        SummaryText("  VERDICT: OTHER ERROR - read the status word above. The request");
        SummaryText("    was issued and failed in a way we have not seen.");
        break;
    default:
        /* ★★★ THE IDLE CASE IS NOT A MISSING COMPLETION, and printing the old
         * diagnosis over it was wrong the very first time switcher 1.2 ran.
         *
         * That run declined the card on purpose: switch attempts 0, trampoline runs 0,
         * no completion -- and this branch announced "Initialize ran but no completion
         * ever reported [...] the switch was never sent", as though something had gone
         * wrong. Nothing had. The switch was never sent BECAUSE NOBODY ASKED FOR IT,
         * which is the shipping state and the reason the keyboard works.
         *
         * ⚠ Same mistake as the panel's "Components missing" and "Driver active: No"
         * in the same release: three places describing the normal state in the
         * vocabulary of a failure, because all three were written when the only
         * possible state was "we tried to switch". A diagnostic that cries wolf over
         * its own success teaches the reader to discount it. */
        if (gSw[kSwIdleDecline] != 0) {
            SummaryText("  VERDICT: IDLE, AND CORRECTLY SO. No switch was attempted");
            SummaryText("    because no pairing was requested -- see the IDLE row");
            SummaryText("    above. Mac OS has the card and a paired keyboard works.");
            SummaryText("    ⇒ NOT a missing completion. Nothing to diagnose.");
        } else {
            SummaryText("  VERDICT: NONE RECORDED. Initialize ran but no completion");
            SummaryText("    ever reported. If trampoline runs is 0 the notification");
            SummaryText("    never got serviced at task level, and the switch was");
            SummaryText("    never sent.");
        }
        break;
    }
    if (gSw[kSwDeferErr] != 0)
        RowHex("  ⚠ last NMInstall error", gSw[kSwDeferErr], 8);
}

static void Verdict(void)
{
    unsigned long *c;
    SwitchReport();
    SummaryText("");
    SummaryText("=========================================================");
    if (gBlocks == 0) {
        /* ⚠⚠ THIS MESSAGE USED TO BLAME THE INSTALL, AND AS OF DRIVER v3.3 THAT IS
         * USUALLY WRONG.
         *
         * v3.3 ships with the mode switch off and rule 1 COMPILED OUT, so the driver
         * deliberately does not bind the internal A1044 at all. With no dongle
         * attached there is nothing for it to bind, so no instance is created and NO
         * BLOCK IS THE CORRECT AND EXPECTED STATE -- with the extension present and
         * enabled the whole time.
         *
         * The panel had the identical fault one revision earlier and told the user to
         * go and check an install that was fine. Same class of bug, so the same fix:
         * say what is true, and only point at the install when the install really is
         * the remaining candidate. */
        /* ⚠ THIS TEXT IS READ AT THE WORST POSSIBLE MOMENT -- when someone already
         * cannot tell an install fault from an expected no-op -- so it being three
         * driver generations stale actively cost a run's readability. It still named
         * v3.3, still said the terminator was at word 191, and still advised that the
         * mode switch "must be researched from Apple's OS X driver, never guessed"
         * long after that research was done, byte-identical, and shipped. */
        SummaryText("  NO BLOCK FOUND. Which of the three cases below applies is");
        SummaryText("  decided by the USB BUS DUMP above -- read it first.");
        SummaryText("");
        SummaryText("   * 05AC:1000 in the dump -> THE CARD IS IN HID-PROXY MODE and no");
        SummaryText("     build binds it. EXPECTED, not a fault. The card runs its own");
        SummaryText("     stack on-chip and OS 9's stock USBHIDKeyboardModule drives a");
        SummaryText("     paired keyboard through it -- which is why the A1016 types");
        SummaryText("     with nothing of ours involved. To measure anything, the card");
        SummaryText("     must be switched to HCI mode first: boot the switch-ON build");
        SummaryText("     (kSendModeSwitch 1), then swap in the switch-OFF build whose");
        SummaryText("     rule 1b binds 05AC:8204. ⚠ THE RUN IS VOID, NOT NEGATIVE.");
        SummaryText("");
        SummaryText("   * 05AC:8204 in the dump -> the card IS switched and we still");
        SummaryText("     did not bind it. Check the install: in Extensions, enabled,");
        SummaryText("     named beginning \"USB\", and EXPANDED (a raw MacBinary is");
        SummaryText("     not type 'ndrv' and the Expert skips it in silence). Check");
        SummaryText("     Get Info shows the version you installed.");
        SummaryText("     ⚠⚠ BUT DO NOT STOP AT THE INSTALL. v10.4 produced EXACTLY this");
        SummaryText("     state -- switched to 8204, VERDICT SWITCHED, no block -- and");
        SummaryText("     the install was FINE. The fragment failed to LOAD, because a");
        SummaryText("     GetResource call had been added to ValidateHW and the Resource");
        SummaryText("     Manager is not usable on the driver load path. Apple's own OS 9");
        SummaryText("     keyboard driver never calls it there either.");
        SummaryText("     ⇒ EnsureBlock is the FIRST statement in ValidateHW, so no block");
        SummaryText("     at all means ValidateHW did not COMPLETE. If the install checks");
        SummaryText("     out, suspect the most recent code added to the load path -- not");
        SummaryText("     the Extensions folder.");
        SummaryText("");
        SummaryText("   * neither present -> no Bluetooth hardware on the bus at all.");
        SummaryText("");
        SummaryText("  Or THIS APP IS TOO OLD for the resident driver: it matches 'BTP1',");
        SummaryText("  a tag beginning 'v', and 'ENDS' at the word named by kWEnd, which");
        SummaryText("  MOVES whenever the block grows. A driver that moved it needs the");
        SummaryText("  BTCheck built in the same commit -- that has bitten nine times.");
        return;
    }
    c = gC[BestBlock()];
    if (gBlocks > 1)
        SummaryText("  (verdict is for the MOST ADVANCED of the blocks found; a second\n   instance bound to a hub and failing is expected and harmless)");
    if (c[kWInit] == 0) {
        SummaryText("  BLOCK FOUND BUT NEVER RAN. See the all-zero note above.");
    } else if (c[kWCfgDone] > 0) {
        /* ⚠ ORDER MATTERS: report the HIGHEST milestone reached, most advanced
         * first. BTCheck v9 tested M1's identity words before anything about M2
         * and so announced a failure on run 15, which had passed the M2 gate.
         * Never reintroduce a verdict that depends on the retired M1 counters. */
        SummaryText("  M0 COMPLETE: interrupt-IN and both bulk pipes are open.");
        if (c[kWSdpAttrBytes] > 0) {
            SummaryText("  *** M2 L2CAP PROVEN: an SDP query went out over an L2CAP");
            SummaryText("  channel and real attribute data came back. That means ACL,");
            SummaryText("  L2CAP signalling, L2CAP config and the bulk pipes all work.");
            SummaryText("  Next is M3 pairing, then the HID host role at M4.");
        } else if (c[kWStkState] == 2) {
            SummaryText("  M2 GATE PASSED: BTstack reached HCI_STATE_WORKING, so the");
            SummaryText("  controller is configured. L2CAP did not complete a round");
            SummaryText("  trip -- read the M2b rows, they say which step stopped it.");
        } else if (c[kWStkPump] > 0) {
            SummaryText("  BTstack is running but has not reached WORKING. Read");
            SummaryText("  'HCI state' plus packets sent vs delivered.");
        } else if (c[kWIntComp] == 0) {
            SummaryText("  No events yet. The HCI sequence is where to look.");
        } else {
            SummaryText("  Events arriving, but BTstack never pumped. Check that");
            SummaryText("  ConfigDone called BT_StackStart.");
        }
    } else {
        SummaryText("  M0.1 DID NOT COMPLETE. 'highest stage reached' names the");
        SummaryText("  step it died on, and 'last usbStatus' says why.");
    }
    SummaryText("=========================================================");
}

static void ShowResultWindow(void)
{
    WindowPtr win;
    Rect      bounds;
    EventRecord evt;
    Str255    s;
    short     screenW, screenH, page, pages, i, y;
    long      deadline;

    screenW = qd.screenBits.bounds.right  - qd.screenBits.bounds.left;
    screenH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    pages   = (short)((gSummaryCount + kLinesPerPage - 1) / kLinesPerPage);
    if (pages < 1) pages = 1;

    bounds.left   = (screenW - 660) / 2; if (bounds.left < 4)  bounds.left = 4;
    bounds.top    = (screenH - 460) / 2; if (bounds.top  < 40) bounds.top  = 40;
    bounds.right  = bounds.left + 660;
    bounds.bottom = bounds.top  + 460;

    { Str255 title;
      BTCheckName(title, "BTCheck v", NULL);
      win = NewWindow(NULL, &bounds, title, true, documentProc,
                      (WindowPtr)-1L, false, 0); }
    if (win == NULL) return;
    SetPort((GrafPtr)win);
    TextFont(kFontIDGeneva);
    TextSize(9);

    for (page = 0; page < pages; page++) {
        EraseRect(&win->portRect);
        TextFace(bold);
        MoveTo(16, 22);
        DrawString("\pWhat did the Bluetooth probe driver actually do?");
        TextFace(normal);

        y = 42;
        for (i = page * kLinesPerPage;
             i < gSummaryCount && i < (page + 1) * kLinesPerPage; i++) {
            MoveTo(16, y);
            DrawString(gSummary[i]);
            y += 15;
        }

        y = bounds.bottom - bounds.top - 34;
        s[0] = 0;
        PStrCat(s, "Page ");
        PStrCatNum(s, (long)(page + 1));
        PStrCat(s, " of ");
        PStrCatNum(s, (long)pages);
        PStrCat(s, (page + 1 < pages) ? " - click or key for the next page."
                                      : " - click or key to quit.");
        TextFace(bold); MoveTo(16, y); DrawString(s); TextFace(normal);

        s[0] = 0;
        PStrCat(s, "Log: ");
        PStrCatPStr(s, gLog.where);
        MoveTo(16, y + 16);
        DrawString(s);

        deadline = TickCount() + kLingerTicks;
        while (TickCount() < deadline) {
            if (WaitNextEvent(mDownMask | keyDownMask, &evt, 6, NULL))
                if (evt.what == mouseDown || evt.what == keyDown) break;
        }
    }
    DisposeWindow(win);
}

static void BeepN(short n)
{
    short i;
    for (i = 0; i < n; i++) { SysBeep(12); Delay(18, NULL); }
}

int main(void)
{
    short b;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    LogOpen();
    { Str255 banner;
      BTCheckName(banner, "BTCheck v", " - reading the Bluetooth probe driver's block");
      Summary(banner); }
    SummaryText("Read-only: a System heap scan and this log file. No hardware touched.");
    SummaryText("");

    /* ⚠ FIRST, and deliberately before the heap scan: this is the one section that
     * still says something useful when the driver never bound. */
    DumpInstalledVersions();

    ScanSystemHeap();

    if (gBlocks > 1) {
        Str255 line;
        line[0] = 0;
        PStrCatNum(line, (long)gBlocks);
        PStrCat(line, " blocks found. One will be the file's copy with zero counters;");
        Summary(line);
        SummaryText("the one with Initialize calls > 0 is the live driver.");
    }
    /* ⚠ BEFORE the block loop and OUTSIDE it: this measures the MACHINE, and must
     * print whether or not our driver is loaded. */
    ProbeSystemVolume();
    for (b = 0; b < gBlocks; b++) ReportBlock(b);
    DumpUSBBus();
    /* ⚠ AFTER the bus dump, which is what captures gCardRef. */
    ReportProxyDescriptors(gCardRef, gCardVidPid);
    Verdict();

    LogClose();
    /* one beep = no block, two = found and it ran, three = found but never ran */
    BeepN((short)(gBlocks == 0 ? 1 : (gC[BestBlock()][kWInit] > 0 ? 2 : 3)));
    ShowResultWindow();
    return 0;
}
