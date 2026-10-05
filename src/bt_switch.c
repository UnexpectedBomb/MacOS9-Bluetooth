/*
 *  bt_switch.c -- USBBluetoothSwitch, a one-job 'ndrv'.
 *
 *  It claims the A1044 in HID-proxy mode (05AC:1000), sends the CSR mode-switch
 *  vendor request from TASK level, and gets out of the way. It never opens a pipe,
 *  never runs a stack, and has no idea what happens next.
 *
 *  ======================================================================
 *  ⭐⭐ WHY THIS IS A SEPARATE FILE AND A SEPARATE EXTENSION
 *  ======================================================================
 *
 *  Not because two things must happen at different points in the startup sequence --
 *  they do not. Both extensions load in the same INIT pass and register with the USB
 *  Expert at the same time. The sequencing is driven by USB ENUMERATION EVENTS:
 *
 *      both drivers register with the Expert
 *      card enumerates at 05AC:1000  -> Expert matches THIS driver
 *      this driver sends the switch and releases
 *      card resets, re-enumerates at 05AC:8204
 *                                    -> Expert matches USBBluetoothSupport
 *      that driver binds it and brings the stack up
 *
 *  The last step is ordinary USB hot-plug, which the Expert already handles, so it
 *  works whenever it lands rather than needing a particular moment.
 *
 *  The real reason is descriptor matching. One 'ndrv' exports ONE
 *  TheUSBDriverDescription, and on this stack nothing beyond its FIRST record has
 *  ever demonstrably matched anything (see the array comment in bt_probe.c: rule 1 at
 *  position 1 matched, rule 2a at position 1 matched, rule 1b at position 2 never did
 *  across two runs). So one file can express exactly ONE match rule -- and we need
 *  two live at once, because the switch makes the device vanish at one PID and
 *  reappear at another.
 *
 *  ⇒ Two rules, two files. This also SIDESTEPS the only-first-record question instead
 *  of fighting it: each file gets its own record 1, so the design is correct whether
 *  the inference is right or wrong.
 *
 *  ======================================================================
 *  ⚠⚠ WHAT THIS REPLACES, AND WHY IT HAD TO GO
 *  ======================================================================
 *
 *  Until now the switch lived in bt_probe.c behind kSendModeSwitch, which forced the
 *  two rules into ONE file where they were mutually exclusive: switch ON put rule 1
 *  (1000) first and could not bind the result, switch OFF put rule 1b (8204) first
 *  and could not switch anything. That made every measurement a TWO-BOOT dance whose
 *  middle step depended on the switched state surviving a restart.
 *
 *  It does not reliably survive. Measured, both ways, two for two:
 *
 *      v6.4 switch -> v6.3 found 8204   SURVIVED
 *                  -> (other work) found 1000   REVERTED
 *      v6.4 switch -> v6.5 found 8204   SURVIVED  (the Max_Num_Keys run)
 *                  -> v6.6 found 1000   REVERTED
 *
 *  THREE OF FIVE RUNS WERE LOST to that coin flip -- void, not negative, which is the
 *  worst kind of lost run because it teaches nothing. This file removes the dance.
 *
 *  ⚠ USBExpertInstallDeviceDriver is NOT the fix, and PROXY-FIRST-PLAN §5 was too
 *  optimistic about it. Its signature is (ref, desc, hubRef, port, busPowerAvailable):
 *  it needs the identity of the device to claim, and after a switch the old device is
 *  gone and the new one's ref was never ours. Read in USB.h, not assumed.
 *
 *  ======================================================================
 *  ⭐⭐⭐ SWITCH BY DEFAULT AS OF 1.3 -- AND THIS SECTION DESCRIBED 1.2
 *  ======================================================================
 *
 *  ⚠⚠⚠ READ THIS BEFORE THE BLOCK BELOW, WHICH IS KEPT AS HISTORY AND IS NO LONGER
 *  WHAT THIS FILE DOES. v1.3 SWITCHES BY DEFAULT. The decline is now the EXCEPTION,
 *  taken only when there is no pair-mode flag AND a stale marker says the last boot
 *  switched but the driver never confirmed -- an anti-loop safety, not a mode.
 *  See SwInitialize: `if (!SwWantsPairMode() && SwMarkerPresentOrUncreatable())`.
 *
 *  ⚠ THIS STALE HEADER COST A DAY. On 2026-09-16 I read it, told the user the shipping
 *  default was "idle at boot, claim on demand", and built an entire design discussion on
 *  top of that -- including whether to ADD a persistent-claim mode the file had already
 *  had for a version. The user's own instinct ("I thought we were automatically turning
 *  on pairing at every boot now") was correct and I talked them out of it.
 *
 *  ⭐ The irony is that the block below opens by congratulating itself for replacing a
 *  header that had gone stale, with the words "a header that describes behaviour the
 *  file no longer has is worse than no header". It then went stale in exactly the same
 *  way, one version later. Behaviour changed in SwInitialize and nothing dragged the
 *  header along. ⇒ If you change the claim/decline decision, change THIS paragraph in
 *  the same commit.
 *
 *  ⚠ The build tag did not move either: the log still reports 'sw12' for code that
 *  implements 1.3, so the tag cannot be used to tell which logic is installed.
 *
 *  ★★★ v1.4, 2026-09-17 -- AND BOTH OF THOSE ARE NOW FIXED, because this version does
 *  change the claim/decline decision and the paragraph above says to say so here.
 *
 *  WHAT 1.4 DOES: the decline is still the exception, and there is now a THIRD way past
 *  it. `!SwWantsPairMode() && !SwRetryAfterStaleRef() && SwMarkerPresentOrUncreatable()`
 *  -- the new middle term grants ONE extra attempt per boot when an earlier match in the
 *  SAME boot died with kUSBUnknownDeviceErr (-6998), i.e. on a device ref that went stale
 *  under us. That happens when the card's root port changes hands mid-boot, which is what
 *  the USB 2.0 EHCI extension does on the FW800 MDD: the card is offered to us twice,
 *  match 1 burns the pair-mode flag on a ref that is dead by the time the deferred request
 *  goes out, and match 2 -- holding a live ref -- used to stand down on the marker. See
 *  SwRetryAfterStaleRef for the measured numbers and docs/RELEASE-GATES.md for the run.
 *
 *  ⚠ The cap of one is the safety: past the decline site we return noErr and OWN the
 *  card, so a switch that then fails leaves the A1016 with no proxy to talk through.
 *  One retry, then the fail-safe returns.
 *
 *  ⚠ THE BUILD TAG NOW MOVES WITH THE VERSION: 'sw14'. It skips 'sw13' deliberately,
 *  because 1.3 shipped as 'sw12' and there has never been an 'sw13' -- aligning the tag
 *  to the version from here on is worth more than a contiguous sequence. BTCheck matches
 *  'sw..' so it reads any of them; this file's own block discovery gates on the exact
 *  tag, which is why both sites moved together.
 *
 *  ★★★ v1.5, 2026-09-17 ('sw15') -- THE FALLBACK NOW REALLY DOES LAST ONE BOOT, and the
 *  decline decision changed again, so per the rule above: here it is.
 *
 *  The condition is UNCHANGED from 1.4. What changed is inside
 *  SwMarkerPresentOrUncreatable: when it finds a stale marker it now DELETES it on the
 *  way out. Before, nothing removed that file except BT_ClearSwitchMarker in the driver,
 *  which runs only when the driver comes up -- precisely the event that had not happened.
 *  So one bad boot disabled Bluetooth on every boot after it, silently, while the 'vers'
 *  string has promised "falls back for ONE boot" since 1.3. Measured 2026-09-17: with
 *  Gate 2 otherwise passing, the first boot STILL needed File > Turn On Pairing At
 *  Restart to break out of the latch, and 1.4's retry cannot help because it needs
 *  kSwTried > 0 while this decline happens before any attempt is made.
 *
 *  ⇒ boot N switches and fails, boot N+1 declines AND clears, boot N+2 tries again.
 *  A broken setup alternates instead of sticking: never permanently disabled, never a
 *  permanently dead keyboard. See the long note at the delete itself for why a
 *  "driver worked here once" token was considered and rejected.
 *
 *  ★★★ v1.6, 2026-09-19 ('sw16') -- 1.5's STAND-DOWN ONLY LASTED ONE MATCH, and on a
 *  machine with the USB 2.0 extension there are two matches per boot. Measured: match 1
 *  declined and consumed the marker, match 2 found none and switched, all in one boot
 *  (`STALE MARKER - declined 1` beside `marker written, switching 1`). The fallback
 *  therefore did not protect the case it exists for. 1.6 makes the decision sticky for
 *  the session via SwDeclinedThisBoot -- see the long note there. The condition is now
 *
 *      !SwWantsPairMode()
 *      && (SwDeclinedThisBoot()
 *          || (!SwRetryAfterStaleRef() && SwMarkerPresentOrUncreatable()))
 *
 *  and the pair-mode flag still outranks all of it, so the menu item is untouched.
 *
 *  ---- what follows described 1.2 and is retained only as history ----
 *
 *  It read: "While this extension is installed, the card is switched out of HID-proxy
 *  at every boot -- so a keyboard paired through the card's own on-chip stack STOPS
 *  WORKING [...] THIS EXTENSION IS A DIAGNOSTIC TOOL, NOT PART OF THE SHIPPING
 *  PRODUCT." That was true and it is not any more, so it is replaced rather than
 *  annotated: a header that describes behaviour the file no longer has is worse than
 *  no header.
 *
 *  What it does now:
 *
 *      boot, no flag        -> DECLINE the card -> OS 9's HID driver has it,
 *                              a paired keyboard types from startup
 *      panel asks to pair   -> writes Preferences:Bluetooth Pair At Restart
 *      next boot only       -> consume the flag, claim the card, switch to HCI
 *      pair, then Hand Back -> driver 8.5 sends wValue 1, card returns to 1000
 *      we match again       -> DECLINE (already switched) -> keyboard types again
 *      every later boot     -> no flag -> decline -> keyboard works
 *
 *  ⇒ One deliberate restart to enter pairing, and a working keyboard the rest of the
 *  time. The claim is on demand, which is what PROXY-FIRST-PLAN §5 asked for -- though
 *  not by the route it sketched: USBExpertInstallDeviceDriver was never the answer
 *  (see below), and declining turned out to be enough because the Expert hands a
 *  refused device to the next matching driver. Measured, 2026-09-08.
 *
 *  ⚠ EVERY FAILURE PATH DECLINES, which is the state where the keyboard works:
 *  Preferences unreachable, flag absent, or flag undeletable all mean "do not switch".
 *  The flag is consumed BEFORE the switch, never after -- see SwWantsPairMode for why
 *  that ordering is the whole safety argument.
 *
 *  ⚠ A WIRED keyboard and mouse are still worth having for a pairing boot: during that
 *  one boot the card is in HCI mode and proxied devices do not work.
 */

#include <stdbool.h>
#include <USB.h>
#include <Notification.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <DriverServices.h>
/* ⚠ NEW IN v1.2, and it changes what this fragment links (InterfaceLib). The pair-mode
 * flag is a file, because the decision has to be made inside SwInitialize -- before we
 * either claim the card or hand it to OS 9's HID driver -- and nothing else available
 * at that point survives a restart. See SwWantsPairMode for the safety argument. */
#include <Files.h>
#include <Folders.h>
/* ⚠ v1.3: smSystemScript for FSpCreate. bt_keyfile.c gets it the same way -- it is
 * a Script Manager constant, not a File Manager one, which is why <Files.h> alone
 * leaves it undeclared and the build fails inside our own function. */
#include <Script.h>

/* ---- the descriptor layout, copied verbatim from bt_probe.c -------------------
 *
 * ⚠⚠ THE PAD BYTE AT OFFSET 21 IS THE WHOLE POINT and it is not cosmetic. GCC packs
 * USBInterfaceInfo to 5 bytes; the Expert reads 6. Without the pad every field after
 * it shifts by one and the Expert misparses the record SILENTLY -- by binding the
 * wrong device, not by failing. See [[reference_os9_usb_ddk_struct_alignment]].
 *
 * Duplicated rather than shared through a header on purpose: this file is meant to be
 * a standalone fragment with no coupling to the main driver's globals, and the
 * compile-time assertions below prove the copy is right rather than trusting it. */
typedef struct {
    OSType  sig;                    /*  0  'usbd'                              */
    UInt32  descVersion;            /*  4  kInitialUSBDriverDescriptor         */
    UInt16  vendor;                 /*  8                                      */
    UInt16  product;                /* 10                                      */
    UInt16  release;                /* 12                                      */
    UInt16  devProtocol;            /* 14                                      */
    UInt8   configValue;            /* 16                                      */
    UInt8   interfaceNum;           /* 17                                      */
    UInt8   ifClass;                /* 18                                      */
    UInt8   ifSubClass;             /* 19                                      */
    UInt8   ifProtocol;             /* 20                                      */
    UInt8   pad;                    /* 21  <-- the byte GCC omits              */
    UInt8   name[32];               /* 22  Str31, Pascal                       */
    UInt8   driverClass;            /* 54                                      */
    UInt8   driverSubClass;         /* 55                                      */
    UInt8   verMajor;               /* 56                                      */
    UInt8   verMinorBug;            /* 57                                      */
    UInt8   verStage;               /* 58                                      */
    UInt8   verNonRel;              /* 59                                      */
    UInt32  loadingOptions;         /* 60                                      */
} SwDriverDesc;                     /* 64 bytes total                          */

#define SW_ASSERT(name, cond) typedef char name[(cond) ? 1 : -1]
SW_ASSERT(sw_desc_is_64,     sizeof(SwDriverDesc) == 64);
SW_ASSERT(sw_name_at_22,     __builtin_offsetof(SwDriverDesc, name) == 22);
SW_ASSERT(sw_class_at_54,    __builtin_offsetof(SwDriverDesc, driverClass) == 54);
SW_ASSERT(sw_version_at_56,  __builtin_offsetof(SwDriverDesc, verMajor) == 56);
SW_ASSERT(sw_options_at_60,  __builtin_offsetof(SwDriverDesc, loadingOptions) == 60);

#define kA1044Vendor   0x05AC
#define kA1044Proxy    0x1000       /* HID-proxy: what we claim                   */
#define kA1044HCI      0x8204       /* what it should become -- NOT ours to bind   */

static OSStatus SwValidateHW (USBDeviceRef device, USBDeviceDescriptor *desc);
static OSStatus SwInitialize (USBDeviceRef device, USBDeviceDescriptorPtr pDesc, UInt32 busPower);
static OSStatus SwFinalize   (USBDeviceRef device, USBDeviceDescriptorPtr pDesc);
/* ⚠ (notification, pointer, refCon) -- NOT (device, ...). The first attempt at this
 * file guessed a device-first signature and GCC caught it as an incompatible pointer
 * type; had the arguments happened to be assignable it would have compiled silently
 * and every notification would have been misread. Matches ProbeNotify in bt_probe.c. */
static OSStatus SwNotify     (UInt32 notification, void *pointer, UInt32 refCon);

/* ---- Exported symbol #1: the one match rule ---------------------------------
 *
 * ⚠ ONE RECORD, DELIBERATELY. A second record here would be dead weight at best (see
 * the only-first-record note above) and a way to accidentally claim something at
 * worst. If this driver ever needs a second rule, it needs a second FILE.
 *
 * ⚠⚠ VENDOR AND PRODUCT ARE BOTH PINNED, AND THAT IS A SAFETY REQUIREMENT. Three
 * other devices on this bus present device class 00/00/00 -- 05AC:0204, an Apple
 * internal keyboard, and 413C:301A, the user's Dell mouse. A rule matching device
 * class 0 alone would claim the KEYBOARD and try to transition it.
 * kUSBDoNotMatchGenericDevice additionally forces the vendor comparison. */
SwDriverDesc TheUSBDriverDescription[1] =
{
  {
    kTheUSBDriverDescriptionSignature,
    kInitialUSBDriverDescriptor,
    kA1044Vendor, kA1044Proxy, 0, 0,            /* the HID-proxy A1044 ONLY      */
    0, 0, 0x00, 0x00, 0x00, 0,                  /* device class 0 + the pad      */
    { 11, 'O','S','9','B','T','S','w','i','t','c','h' },
    0x00,                                       /* matches device class 0        */
    0x00,
    1, 0x00, finalStage, 0,                     /* 1.0                           */
    kUSBDoNotMatchGenericDevice | kUSBDoNotMatchInterface
  }
};
SW_ASSERT(sw_one_rule, sizeof(TheUSBDriverDescription) == 64);

/* ---- Exported symbol #2: the dispatch table --------------------------------
 *
 * ⚠ InitializeInterface is nil ON PURPOSE. We match at DEVICE level with
 * kUSBDoNotMatchInterface, so the Expert never has an interface to offer us, and this
 * driver has no business being handed one. bt_probe.c had to grow a real
 * InitializeInterface for its class-0xE0 rules; this file must not. */
USBClassDriverPluginDispatchTable TheClassDriverPluginDispatchTable =
{
    kClassDriverPluginVersion,
    SwValidateHW,
    SwInitialize,
    nil,
    SwFinalize,
    SwNotify
};

/* ======================================================================= */
/*  The counter block -- 'BTSW', entirely separate from the driver's 'BTP1'  */
/* ======================================================================= */
/* ⚠ A DIFFERENT SIGNATURE, NOT A DIFFERENT REGION OF THE SAME BLOCK. Sharing 'BTP1'
 * would mean two independently-built extensions agreeing on a layout, which is the
 * single most repeated mistake on this project -- ten moves of one terminator, and a
 * hand-typed word count that silently drifted into an out-of-bounds read. Two blocks
 * with two magics cannot drift into each other.
 *
 * ⚠ Same lifetime rules as 'BTP1': NewPtrSys, System heap, DELIBERATELY NEVER FREED,
 * and rebuilt from zero on every boot. A leak of this size once per boot is the price
 * of counters that outlive the instance being measured. */
enum {
    kSwMagic = 0,       /* 'BTSW'                                              */
    kSwBuild,           /* 'sw16'                                              */
    kSwValidate,        /* ValidateHW calls                                    */
    kSwInit,            /* Initialize calls                                    */
    kSwFinal,           /* Finalize calls  -- ⭐ 1 = the card left the bus       */
    kSwNotifyC,         /* Notify calls                                        */
    kSwNotifyCode,      /* the last notification code                          */
    kSwSeenProxy,       /* matched 05AC:1000                                   */
    kSwSeenOther,       /* matched something else -- should be 0               */
    kSwOtherVid,        /* (vid << 16) | pid of that something else            */
    kSwTried,           /* switch attempts actually issued                     */
    kSwImmErr,          /* last immediate error from USBDeviceRequest          */
    kSwStatus,          /* last completion usbStatus                           */
    kSwStalls,          /* control-pipe stalls seen                            */
    kSwClearRc,         /* USBClearPipeStallByReference result                 */
    kSwDeferRuns,       /* trampoline task-level halves that ran               */
    kSwDeferErr,        /* last NMInstall error                                */
    kSwVerdict,         /* one of kVerdict* below                             */
    /* ★ v1.1: times we matched the card and DELIBERATELY refused it, because the
     * switch had already happened this boot and the card was handed back. A nonzero
     * value here beside 05AC:1000 in the bus dump is the whole handback story. */
    kSwStoodDown,
    /* ★★★ v1.2: IDLE AT BOOT. The switcher no longer claims the card just because it
     * is installed -- it claims it only when the panel has asked for pairing mode. */
    kSwIdleDecline,     /* declined because no pair-mode flag was set          */
    kSwFlagSeen,        /* the flag file was found (and consumed)              */
    kSwFlagErr,         /* FindFolder / FSpDelete error, full width            */
    kSwMarkStale,       /* v1.3: marker found -> previous boot never confirmed */
    kSwMarkWrote,       /* v1.3: marker written, switching                     */
    kSwMarkErr,         /* v1.3: marker could not be read or created           */
    /* ★★★ v1.4: times the marker was OVERRIDDEN because an earlier match THIS BOOT
     * failed with a stale device ref (kUSBUnknownDeviceErr). Capped at 1 -- see
     * SwRetryAfterStaleRef. Nonzero here means the EHCI-present double-offer happened
     * and we took our second shot at it. */
    kSwRefRetry,
    /* ★★★ v1.5: the stale marker was found AND CLEARED, so the fallback really does last
     * one boot and the next one tries again. 1 here beside kSwMarkStale 1 = self-healed.
     * kSwMarkErr set instead = the delete failed and it IS still latched. */
    kSwMarkCleared,
    kSwEnd,             /* 'ENDS'                                              */
    kSwCount
};

/* ⚠ THE VERDICT VOCABULARY, AND ITS ONE COUNTER-INTUITIVE ENTRY.
 *
 * A TIMEOUT IS SUCCESS AND A CLEAN COMPLETION IS FAILURE. The card changes USB
 * personality mid-request, so it never completes the handshake -- BlueZ's success
 * signature. A clean noErr means the card ACCEPTED and IGNORED the request, which
 * BlueZ calls EALREADY. Anyone reading kSwStatus without this note in front of them
 * will draw exactly the wrong conclusion, which is why it is written here and printed
 * by BTCheck rather than left to be remembered. */
enum {
    kVerdictNone     = 0,
    kVerdictSwitched = 1,   /* timed out / device vanished  => IT WORKED       */
    kVerdictIgnored  = 2,   /* completed cleanly            => it did NOT     */
    kVerdictStalled  = 3,   /* control pipe stalled, cleared, retrying        */
    kVerdictOther    = 4    /* something else; read kSwStatus                 */
};

static unsigned long *gSw;

/* ⚠ ALLOCATION IS TASK LEVEL ONLY -- NewPtrSys is on the static audit's forbidden
 * list. Called from SwInitialize, which the Expert calls at task level, and from
 * nowhere else. */
static void SwEnsureBlock(void)
{
    short i;
    if (gSw != NULL) return;

    /* Adopt an existing block before making one. The Expert can offer this driver the
     * card more than once in a boot -- the switch causes a re-enumeration, and if the
     * switch does NOT take, the card comes back still at 1000 and we are matched
     * again. Each fragment copy has its own globals
     * ([[reference_os9_two_fragment_copies_own_globals]]), so a static guard in one
     * instance is invisible to the next; the shared block is the only place they can
     * see each other's attempt count. That is what bounds the retry loop. */
    {
        THz zone = SystemZone();
        unsigned long lo = (unsigned long)zone->heapData;
        unsigned long hi = (unsigned long)zone->bkLim;
        unsigned long *p, *lim = (unsigned long *)(hi - (kSwCount * 4 + 16));

        for (p = (unsigned long *)((lo + 3) & ~3UL); p < lim; p++) {
            if (p[kSwMagic] == 0x42545357UL &&      /* 'BTSW' */
                p[kSwBuild] == 0x73773136UL &&      /* 'sw16' -- THIS build only */
                p[kSwEnd]   == 0x454E4453UL) {      /* 'ENDS' */
                gSw = p;
                return;
            }
        }
    }

    gSw = (unsigned long *)NewPtrSys((Size)kSwCount * sizeof(unsigned long));
    if (gSw == NULL) return;        /* counters silently absent; the switch still works */

    for (i = 0; i < kSwCount; i++) gSw[i] = 0;
    gSw[kSwMagic] = 0x42545357UL;   /* 'BTSW' */
    gSw[kSwBuild] = 0x73773136UL;   /* 'sw16' */
    gSw[kSwEnd]   = 0x454E4453UL;   /* 'ENDS' */
}

/* ⚠ Both guard gSw. It is NULL if NewPtrSys failed, and a diagnostic that crashes the
 * machine when it cannot allocate is worse than no diagnostic. */
static void SwBump(short w) { if (gSw != NULL) gSw[w]++; }
static void SwNote(short w, unsigned long v) { if (gSw != NULL) gSw[w] = v; }

/* ======================================================================= */
/*  The interrupt -> task trampoline                                        */
/* ======================================================================= */
/* ⚠⚠ THE SWITCH MUST GO OUT AT TASK LEVEL. This is not a style preference, it is the
 * difference between a working card and one that leaves the bus until a Tiger boot
 * clears it.
 *
 * v2.4 of the main driver sent the request straight from Initialize (task level) and
 * the card came back healthy. v2.5 walked the device first and sent from the walk's
 * COMPLETION -- interrupt level -- and the card vanished from the bus for three runs.
 * Which half mattered was never isolated, so neither half is reintroduced: this file
 * does not walk, and it does not send from interrupt level.
 *
 * ⚠ Apple's CSRHIDTransitionDriver simply does IOSleep(1000) inside start(). We
 * cannot: Initialize is what the Expert calls to load us, and blocking it for a second
 * stalls driver loading for every other device on the bus. Deferring through the
 * Notification Manager gives the card AT LEAST as much settle time -- the notification
 * is not serviced until something runs an event loop -- and costs the Expert nothing.
 *
 * ⚠ Its own copy rather than bt_defer.c's, because that one also calls
 * BT_KeyFileFlushIfDirty and pulls in bt_pump.h's globals. This file stays standalone. */
static NMRec        gNM;
static NMUPP        gNMUpp;
static volatile int gQueued;
static volatile int gSwitchPending;

static void SwCsrModeSwitch(USBDeviceRef device);
static USBDeviceRef gDevice;

static pascal void sw_defer_resp(NMRecPtr nmReqPtr)
{
    SwBump(kSwDeferRuns);

    /* ⚠ NMRemove FIRST and unconditionally. The record must leave the queue before
     * anything else can go wrong, or a failure below strands it and no further
     * notification can ever be installed. [[reference_os9_recovery_paths_fail_open]]. */
    (void)NMRemove(nmReqPtr);
    gQueued = 0;

    if (gSwitchPending) {
        gSwitchPending = 0;
        SwCsrModeSwitch(gDevice);
    }
}

/* ⚠ TASK LEVEL ONLY. NewNMUPP -> NewRoutineDescriptor is a Mixed Mode allocator and
 * must not be called below task level. Called from SwInitialize and nowhere else. */
static void SwDeferInit(void)
{
    if (gNMUpp != NULL) return;
    gNMUpp = NewNMUPP(sw_defer_resp);
}

static void SwDeferRequest(void)
{
    OSErr err;
    if (gNMUpp == NULL) return;
    if (gQueued) return;            /* coalesce; one pending notification is enough */

    gNM.qType    = nmType;
    gNM.nmMark   = 0;
    gNM.nmIcon   = nil;
    gNM.nmSound  = nil;
    gNM.nmStr    = nil;
    gNM.nmResp   = gNMUpp;
    gNM.nmRefCon = 0;

    gQueued = 1;
    err = NMInstall(&gNM);
    if (err != noErr) {
        /* ⚠ FAIL OPEN. An install that failed leaves nothing to clear the flag, so
         * clearing it here is what lets a later attempt through. */
        gQueued = 0;
        SwNote(kSwDeferErr, (unsigned long)err);
    }
}

/* ======================================================================= */
/*  The switch itself                                                       */
/* ======================================================================= */
/* Apple's CSRHIDTransitionDriver sends this request SEVEN times, unrolled and
 * unconditional (docs/M0-MODE-SWITCH.md §9c), and the v6.4 run measured exactly seven
 * attempts producing a switched card. Same number here, same reason. */
#define kSwMaxTries 7

static USBPB   gSwitchPB;
static Boolean gSwitchBusy;

/* ⚠⚠ DO NOT TREAT A NONZERO usbStatus AS FATAL HERE. The FAILING status IS the success
 * case -- see the verdict enum. */
static void SwSwitchCompletion(USBPB *pb)
{
    OSStatus st = pb->usbStatus;

    gSwitchBusy = false;            /* released FIRST -- the fail-open rule */
    SwNote(kSwStatus, (unsigned long)st);

    /* ★ THE STALL PATH. Apple's driver tests the result against kIOUSBPipeStalled and,
     * on getting it, clears the stall on pipe zero and re-sends. A device whose DEFAULT
     * CONTROL PIPE is stalled cannot be enumerated at all, which is exactly the card
     * vanishing from the bus in the main driver's runs 35 and 40.
     * [[reference_os9_usb_pipe_stall_must_be_cleared]]: a stalled pipe that is never
     * cleared is PERMANENT DEATH.
     *
     * ⚠ The device reference IS the default control pipe's reference in this API,
     * which is why gDevice is what gets cleared.
     *
     * ⚠ Retried through the TRAMPOLINE rather than re-entered from here, so the resend
     * happens at task level like the first one. This path has never fired on the A1044
     * -- the v6.4 run measured `pipe stalls seen 0` -- so it is written to Apple's
     * shape and is untested on this card. */
    if (st == kUSBPipeStalledError) {
        SwBump(kSwStalls);
        SwNote(kSwVerdict, kVerdictStalled);
        SwNote(kSwClearRc, (unsigned long)USBClearPipeStallByReference(gDevice));
        if (gSw != NULL && gSw[kSwTried] < kSwMaxTries) {
            gSwitchPending = 1;
            SwDeferRequest();
        }
        return;
    }

    if (st == noErr) {
        /* Completed cleanly => the card did NOT switch. BlueZ calls this EALREADY:
         * either it was already in HCI mode, or it is not a switchable part.
         * ⚠ This is the ONE case that stops early -- resending an ignored request is
         * pointless, and it is not a latching guard because it cannot suppress a retry
         * that could have succeeded. */
        SwNote(kSwVerdict, kVerdictIgnored);
        return;
    }

    if (st == kUSBNotRespondingErr || st == kUSBTimedOut) {
        /* -6911 "Pipe stall, No device, device hung" and -6971 "Transaction timed out"
         * are the two ways this SDK reports the card going away mid-request. That is
         * BlueZ's success signature and what the v6.4 run measured. Apple's driver
         * instead expects kIOUSBPipeStalled: the two USB stacks report the same
         * physical event differently, which is why the stall path above is correct
         * against Apple's code and inert against ours. */
        SwNote(kSwVerdict, kVerdictSwitched);
    } else {
        SwNote(kSwVerdict, kVerdictOther);
    }

    /* ★ KEEP SENDING, up to seven, matching Apple. Chained from the completion so the
     * sends are sequential and back to back the way Apple's unrolled ones are -- and
     * this is the path the v6.4 run drove seven times without incident. Note the
     * asymmetry with the stall path above, which defers instead: that one is untested
     * on this card, so it takes the more conservative route. */
    if (gSw != NULL && gSw[kSwTried] < kSwMaxTries) SwCsrModeSwitch(gDevice);
}

static void SwCsrModeSwitch(USBDeviceRef device)
{
    OSStatus err;

    /* ⚠⚠ BOUND THE ATTEMPTS AND GUARD THE SHARED PB.
     *
     * If the switch does not take, the card re-enumerates still at 1000, this rule
     * matches again, and we switch again -- an unbounded switch/re-enumerate loop with
     * the USB bus in the middle of it. Worse, gSwitchPB is ONE static parameter block:
     * a second call while the first is in flight would overwrite a PB the USL still
     * owns. The bound lives in the SHARED block precisely so it survives the
     * re-enumeration that creates a fresh instance with fresh globals.
     *
     * ⚠ Not the latching guard [[feedback_guards_must_not_latch]] warns about: it does
     * not suppress a retry that could succeed, it stops a loop that by construction
     * cannot, and kSwTried shows exactly how many attempts happened. */
    if (gSw == NULL || gSw[kSwTried] >= kSwMaxTries || gSwitchBusy) return;
    gSwitchBusy = true;
    SwBump(kSwTried);

    gSwitchPB.pbVersion     = kUSBCurrentPBVersion;
    gSwitchPB.pbLength      = sizeof(gSwitchPB);
    gSwitchPB.usbReference  = device;
    gSwitchPB.usbCompletion = SwSwitchCompletion;
    gSwitchPB.usbStatus     = noErr;
    /* The request, byte for byte from Apple's CSRHIDTransitionDriver via
     * docs/M0-MODE-SWITCH.md §9a-9d. bmRequestType 0x40 = host-to-device, vendor,
     * device. wValue is the mode: enum { HCI = 0, HID = 1 }, so 0 asks for HCI and 1
     * is the documented way back. No data stage. */
    gSwitchPB.usb.cntl.BMRequestType =
        USBMakeBMRequestType(kUSBOut, kUSBVendor, kUSBDevice);   /* 0x40 */
    gSwitchPB.usb.cntl.BRequest = 0;
    gSwitchPB.usb.cntl.WValue   = 0;
    gSwitchPB.usb.cntl.WIndex   = 0;
    gSwitchPB.usbBuffer         = nil;
    gSwitchPB.usbReqCount       = 0;

    err = USBDeviceRequest(&gSwitchPB);
    SwNote(kSwImmErr, (unsigned long)err);

    /* ⚠ An immediate error is a REAL failure -- the request was never issued, which is
     * different from being issued and timing out. Only the completion can report the
     * success case. And the completion will never run, so release the flag here or the
     * switch is stranded for the rest of the boot:
     * [[reference_os9_recovery_paths_fail_open]]. */
    if (err != noErr && err != kUSBPending) {
        gSwitchBusy = false;
        SwNote(kSwVerdict, kVerdictOther);
    }
}

/* ======================================================================= */
/*  The USL entry points                                                    */
/* ======================================================================= */

static OSStatus SwValidateHW(USBDeviceRef device, USBDeviceDescriptor *desc)
{
    (void)device; (void)desc;
    SwEnsureBlock();
    SwBump(kSwValidate);
    return noErr;
}

/* ★★★★★★ IS PAIRING MODE WANTED FOR THIS BOOT? A ONE-SHOT FLAG FILE.
 *
 * The panel writes `Preferences:Bluetooth Pair At Restart`; this consumes it. True
 * means "claim the card and switch it"; false means "leave it to Mac OS".
 *
 * ⚠ WHY A FILE, when this driver otherwise touches no file at all. The decision has to
 * be made INSIDE SwInitialize -- that is the only moment at which we can either claim
 * the card or decline and let OS 9's HID driver have it -- and it has to survive a
 * restart, because entering pairing mode means the card must be switched before
 * anything else binds it. Nothing else available at that point does both. The Name
 * Registry does not survive a boot; PRAM does, but scribbling on a scarce shared
 * resource to store one bit is not a trade worth making.
 *
 * ⚠⚠⚠ TASK LEVEL, AND THE ENTIRE SAFETY ARGUMENT IS THAT ONE FACT. The Expert calls
 * Initialize at task level, so the File Manager is legal here --
 * [[reference_os9_no_filemgr_at_interrupt]] is about ISR and SIH context, and
 * scripts/level-audit.py's own header states the same thing as its reason for not
 * making Initialize a root.
 *
 * ⇒ THE EXPOSURE IS AUDITABLE RATHER THAN ASSUMED, and it is worth looking at:
 *
 *     python3 scripts/level-audit.py --roots SwInitialize
 *     RESULT: FAIL -- 5 forbidden primitive(s) reachable.
 *              FindFolder / FSMakeFSSpec / FSpCreate / FSpDelete / NewPtrSys
 *
 * ⚠ THAT COUNT WAS RECORDED AS 4, NAMING 3, AND WAS WRONG BEFORE 1.5 TOUCHED ANYTHING --
 * FSpCreate (the marker write) and NewPtrSys (SwEnsureBlock) were always reachable and
 * were simply left out of the snapshot. Corrected 2026-09-17 by re-running it rather than
 * trusting it, which is the whole point of the paragraph below. 1.5's new FSpDelete on
 * the stale-marker path adds NO new primitive to this set: FSpDelete was already here via
 * SwWantsPairMode, so the safety argument is unchanged in scope.
 *
 * That FAIL is the honest picture of what this function reaches. It is safe ONLY
 * because nothing below task level can call it -- verified: SwWantsPairMode has exactly
 * one caller and the default audit (rooted at the completion routines) stays clean. If
 * anything ever calls this from a completion or an SIH, that clean result flips and the
 * machine hangs with no NMI. Do not add a second caller.
 *
 * ⚠ Adding these calls also revealed that the audit's table did not know FindFolder at
 * all: two of the three File Manager calls in this one function were flagged and the
 * third was invisible. Fixed there, not worked around here. It is still a synchronous call inside a function that
 * holds up driver loading for the whole bus, which is why it is one lookup and one
 * delete of a zero-length file and nothing more. This driver already refuses to block
 * the Expert for the SWITCH (it defers through the Notification Manager rather than
 * sleeping); the same instinct applies here, and a directory lookup is the cheapest
 * persistent signal available.
 *
 * ⚠⚠⚠ THE DELETE HAPPENS FIRST, AND ONLY A SUCCESSFUL DELETE AUTHORISES THE SWITCH.
 * Switch-then-delete looks equivalent and is the dangerous order: the switch makes the
 * card vanish mid-request, so the delete might never run, the flag would persist, and
 * the switcher would claim the card on EVERY subsequent boot -- a permanently dead
 * keyboard, for a reason invisible without file surgery. This way the worst case is
 * "pairing did not start", which is visible and one click from a retry. */
/* ---- the self-healing marker (v1.3) --------------------------------------------
 *
 * ★★★★★★ WHY THE OPT-IN FLAG WENT AWAY, AND WHAT REPLACES ITS SAFETY.
 *
 * Until v1.2 this extension only claimed the card when the panel had asked, one boot at
 * a time. That existed for ONE reason: owning the card meant the user could not type,
 * because the driver had no way to deliver a keystroke. ⇒ THAT EXPIRED 2026-09-14 --
 * driver v10.6 typed into SimpleText through our own stack with sane auto-repeat.
 *
 * ⚠⚠ BUT THE FLAG'S REAL JOB WAS NOT ASKING PERMISSION, IT WAS SELF-HEALING. It is
 * consumed BEFORE the switch, so whatever goes wrong, the NEXT BOOT IS CLEAN. Simply
 * switching unconditionally throws that away: past this point we own the card with no
 * release path, so a failure means a dead keyboard -- and a restart does not recover
 * it, it RE-RUNS THE SAME FAILURE.
 *
 * ⇒ AND THAT IS NOT HYPOTHETICAL. Driver v10.4 did not load at all (a GetResource call
 * on the load path). Under the flag that cost one boot. Unconditional, it would have
 * been a keyboard-less machine EVERY boot until the extension was physically removed --
 * which needs a keyboard.
 *
 * ⇒ SO THE MARKER INVERTS THE FLAG AND AUTOMATES IT:
 *
 *     marker present at boot -> the PREVIOUS boot switched and the driver never
 *                               confirmed -> DECLINE, and the keyboard works
 *     marker absent          -> write it, then switch
 *     driver binds           -> driver DELETES it (BT_ClearSwitchMarker)
 *
 * A broken driver therefore costs EXACTLY ONE bad boot and then falls back on its own.
 * That is strictly better than the flag, where a broken driver cost a boot AND had to
 * be re-armed by hand.
 *
 * ⚠⚠⚠ WRITTEN BEFORE THE SWITCH, NEVER AFTER -- the same ordering argument that governs
 * the force flag below. The switch makes the card vanish mid-request, so anything done
 * afterwards might never run, and a marker that failed to appear would leave a failing
 * boot looking like a healthy one forever.
 *
 * ⚠ EVERY FAILURE PATH STILL DECLINES. Preferences unreachable, marker unreadable,
 * marker uncreatable: all mean "do not switch", which is where the keyboard works. */
static Boolean SwMarkerPresentOrUncreatable(void)
{
    FSSpec spec;
    short  vRefNum;
    long   dirID;
    OSErr  err;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                     &vRefNum, &dirID);
    if (err != noErr) { SwNote(kSwMarkErr, (unsigned long)(long)err); return true; }

    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Switch Attempted", &spec);
    if (err == noErr) {                      /* it is there: last boot did not confirm */
        SwBump(kSwMarkStale);
        /* ★★★★★★ v1.5 -- CONSUME THE MARKER AS WE DECLINE ON IT. THIS IS THE LATCH FIX.
         *
         * ⚠⚠ Until 1.5 this returned true and LEFT THE FILE THERE, so "fall back for one
         * boot" fell back forever: the only thing that ever deleted it was
         * BT_ClearSwitchMarker, which runs when the driver comes up -- i.e. exactly the
         * event that had not happened. bt_keyfile.c's own note called this out ("IF THIS
         * NEVER RUNS, THE FALLBACK LATCHES FOREVER") and called it the safe direction,
         * but the shipped 'vers' string has promised ONE boot since 1.3, and a user who
         * hit a single bad boot got Bluetooth silently disabled on every boot after it
         * with no way to know why. Measured 2026-09-17: even with Gate 2 passing, the
         * first boot still needed File > Turn On Pairing At Restart to break out of this.
         * 1.4's stale-ref retry cannot help here -- it needs kSwTried > 0, and this
         * decline happens BEFORE any attempt, so the retry can never arm.
         *
         * ⇒ Deleting it here makes the fallback last exactly one boot, as advertised:
         *      boot N    switch, driver does not come up -> marker survives
         *      boot N+1  marker found -> DECLINE (keyboard works via proxy) + clear
         *      boot N+2  no marker -> try again
         * Bounded in both directions. A broken setup alternates rather than sticking:
         * never permanently disabled, and never a permanently dead keyboard. That beats
         * both of the states this can otherwise reach.
         *
         * ⚠ WHY NOT a persistent "the driver worked here once" token instead, which would
         * let it retry freely: that state goes stale the moment the driver is removed, and
         * a stale token would claim the card every boot with nothing to drive it -- a
         * permanently dead keyboard, worse than anything the current code can do. Rejected
         * on that basis, not overlooked.
         *
         * ⚠ A FAILED DELETE STILL LATCHES, and says so rather than pretending: kSwMarkErr
         * carries the error and kSwMarkCleared stays 0, which is the pair BTCheck prints.
         * ⚠ TASK LEVEL, like the FindFolder/FSpCreate either side of it -- this function
         * has one caller (SwInitialize) and must keep having one. */
        err = FSpDelete(&spec);
        if (err != noErr) SwNote(kSwMarkErr, (unsigned long)(long)err);
        else              SwBump(kSwMarkCleared);
        return true;
    }
    if (err != fnfErr) { SwNote(kSwMarkErr, (unsigned long)(long)err); return true; }

    /* Absent, which is the healthy case. Create it BEFORE returning, so the switch that
     * follows is already covered if it never completes. */
    err = FSpCreate(&spec, 'BTcp', 'BTmk', smSystemScript);
    if (err != noErr) { SwNote(kSwMarkErr, (unsigned long)(long)err); return true; }
    /* ★ v15.4: and correct a marker left by an older build, which would otherwise keep
     * creator 'OS9B' and its generic icon forever -- this file is only ever created, never
     * rewritten, so there is no other moment to fix it. Task level: this runs at startup,
     * and it already calls FSpCreate two lines up. */
    {
        FInfo fi;
        if (FSpGetFInfo(&spec, &fi) == noErr && fi.fdCreator != 'BTcp') {
            fi.fdCreator = 'BTcp';
            (void)FSpSetFInfo(&spec, &fi);
        }
    }
    SwBump(kSwMarkWrote);
    return false;
}

static Boolean SwWantsPairMode(void)
{
    FSSpec spec;
    short  vRefNum;
    long   dirID;
    OSErr  err;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                     &vRefNum, &dirID);
    if (err != noErr) {
        /* ⚠ NOT an error worth acting on: no Preferences folder reachable means no
         * flag, which means do not switch. Recorded so the log can tell "nobody asked"
         * apart from "we could not find out". */
        SwNote(kSwFlagErr, (unsigned long)(long)err);
        return false;
    }
    err = FSMakeFSSpec(vRefNum, dirID, "\pBluetooth Pair At Restart", &spec);
    if (err == fnfErr) return false;            /* the ordinary case: nobody asked */
    if (err != noErr) {
        SwNote(kSwFlagErr, (unsigned long)(long)err);
        return false;
    }

    SwBump(kSwFlagSeen);
    err = FSpDelete(&spec);
    if (err != noErr) {
        /* ⚠ CONSUMED OR NOTHING. An undeletable flag would fire again next boot and
         * every boot after, so refuse to switch on it at all. */
        SwNote(kSwFlagErr, (unsigned long)(long)err);
        return false;
    }
    return true;
}

/* ★★★★★★ v1.4, 2026-09-17 -- OVERRIDE THE MARKER WHEN THE LAST ATTEMPT DIED ON A STALE
 * DEVICE REF. This is the EHCI-coexistence fix, and it is a fix to THIS file rather than
 * to the USB 2.0 stack, which is where four builds' worth of looking went first.
 *
 * ⚠⚠ WHAT WAS MEASURED, because the numbers are the whole argument (2026-09-17,
 * BTCheck v99.65, both runs the SAME 'sw12' binary -- see docs/RELEASE-GATES.md):
 *
 *                            no EHCI extension      EHCI extension present
 *     Initialize calls       1                      2
 *     matched the proxy card 1                      2
 *     pair-mode flag consumed 1                     1
 *     switch attempts        7                      1
 *     last immediate err     0x00000001 (pending)   0xFFFFE4AA  (-6998)
 *
 * -6998 is kUSBUnknownDeviceErr, "device ref not recognised" -- NOT the card refusing
 * anything. With EHCI present the A1044 is enumerated and offered to us TWICE, because
 * its root port changes hands (the USB 2.0 INIT claims it, the UIM later cedes it back).
 * SwCsrModeSwitch takes its usbReference from gDevice captured at MATCH time and issues
 * the request from the task-level trampoline, LATER -- and a re-enumeration builds a
 * fresh driver instance with fresh globals, which is exactly why kSwTried has to live in
 * the shared block. So match 1 consumes the pair-mode flag and then fires at a ref that
 * is already dead, and match 2 arrives holding a LIVE ref with no flag left and a marker
 * in the way, and stands down. The card never leaves HID-proxy.
 *
 * ⇒ Match 2 is the one that can actually succeed. Let it.
 *
 * ⚠⚠ WHY THIS IS NOT THE FIX I FIRST REACHED FOR, recorded so it is not reached for
 * again: the obvious move is to re-arm the pair-mode flag on an immediate error. That
 * would put FSpCreate inside SwCsrModeSwitch -- and SwCsrModeSwitch is called from
 * SwSwitchCompletion ("KEEP SENDING, up to seven", the chained retry), which is
 * INTERRUPT LEVEL. The File Manager below task level on this platform is a silent hard
 * hang with no NMI [[reference_os9_no_filemgr_at_interrupt]]. This version touches no
 * file at all: it reads the shared block, which is already the thing designed to survive
 * the re-enumeration.
 *
 * ⚠ CAPPED AT ONE OVERRIDE PER BOOT, and the cap is the safety, not tidiness. From the
 * decline site onward we return noErr and OWN the card; if the switch then fails we are
 * squatting on a card we never switched and the A1016 has no proxy to talk through, i.e.
 * a dead keyboard for this boot. Declining is what hands it back to Mac OS. So this buys
 * exactly one more attempt with a fresh ref -- the one the measurement says is live --
 * and then the fail-safe returns. kSwTried's own 7-attempt bound still applies on top.
 *
 * ⚠ IT BUMPS A COUNTER AS A SIDE EFFECT, like SwMarkerPresentOrUncreatable above it
 * creates the marker as a side effect. Called exactly once, from the decline condition,
 * where the short-circuit order matters (see the comment there). Do not add a caller.
 *
 * ⚠ TASK LEVEL is not required here -- it reads memory and nothing else -- but its one
 * caller is SwInitialize, which is task level anyway. */
/* ★★★★★★ v1.6, 2026-09-19 -- THE STAND-DOWN LASTS THE BOOT, NOT THE MATCH.
 *
 * ⚠⚠ MEASURED, and it is a defect 1.5 introduced. With the USB 2.0 extension present the
 * A1044 is offered to us TWICE in one boot (the root port changes hands, the card
 * re-enumerates -- see SwRetryAfterStaleRef for the numbers). 1.5 made
 * SwMarkerPresentOrUncreatable DELETE the stale marker as it declined on it, which is
 * right, but the second match then found no marker, wrote a fresh one and SWITCHED.
 * One boot, both outcomes:
 *
 *     matched the proxy card       2
 *     ⚠⚠ STALE MARKER - declined   1     <- match 1 declined and consumed it
 *     ⭐ marker written, switching  1     <- match 2 found none and went ahead
 *
 * ⇒ The fallback lasted one MATCH instead of one boot, which is not what the 'vers'
 * string promises and not what protects anybody. It happened to be harmless on the test
 * machine because the driver does come up -- but the whole purpose of the marker is the
 * case where it CANNOT, and there the user would get the card switched anyway and a dead
 * keyboard. A known path back to the failure state is not shippable
 * [[feedback_no_partial_fixes_that_reproduce_the_symptom]].
 *
 * ⇒ So the decision becomes sticky for the session. kSwIdleDecline already counts it and
 * already lives in the shared block -- which exists precisely because re-enumeration
 * hands the next instance fresh globals -- so nothing new has to be stored.
 *
 * ⚠ PURE, unlike the two predicates either side of it: no file access, no side effect,
 * nothing to spend. It is safe to call from anywhere and safe to call twice.
 * ⚠ The pair-mode flag still outranks it -- that is the user's deliberate override and
 * it is tested first, so a menu click still forces a switch on a boot we stood down on. */
static Boolean SwDeclinedThisBoot(void)
{
    return (Boolean)(gSw != NULL && gSw[kSwIdleDecline] != 0);
}

static Boolean SwRetryAfterStaleRef(void)
{
    if (gSw == NULL)                                  return false;
    if (gSw[kSwRefRetry] > 0)                         return false;  /* one shot, spent */
    if (gSw[kSwTried] == 0)                           return false;  /* nothing tried yet */
    if (gSw[kSwTried] >= kSwMaxTries)                 return false;  /* budget gone */
    if (gSw[kSwImmErr] == 0)                          return false;  /* noErr: it went out */
    if (gSw[kSwImmErr] == (unsigned long)kUSBPending) return false;  /* still in flight */
    SwBump(kSwRefRetry);
    return true;
}

static OSStatus SwInitialize(USBDeviceRef device, USBDeviceDescriptorPtr pDesc,
                             UInt32 busPower)
{
    (void)busPower;

    SwEnsureBlock();            /* ⚠ BEFORE any Bump/Note. Task level. */
    SwBump(kSwInit);
    gDevice = device;

    /* ⚠⚠ VERIFY WHAT WE WERE ACTUALLY HANDED. The descriptor pins VID and PID, but
     * this project has been caught before assuming the Expert honours a rule as
     * documented on this stack -- a missing pad byte once bound the wrong device, and
     * a class-0 rule would have claimed the user's keyboard. Sending a vendor control
     * request to a device that is NOT the A1044 is exactly the kind of thing that must
     * be impossible rather than unlikely, so it is checked here as well. */
    if (pDesc == nil) {
        SwBump(kSwSeenOther);
        return noErr;                   /* nothing to identify; do nothing */
    }
    {
        UInt16 vid = USBToHostWord(pDesc->vendor);
        UInt16 pid = USBToHostWord(pDesc->product);

        if (vid != kA1044Vendor || pid != kA1044Proxy) {
            SwBump(kSwSeenOther);
            SwNote(kSwOtherVid, ((unsigned long)vid << 16) | (unsigned long)pid);
            /* ⚠ Refuse it rather than silently sitting on it. Holding a device we did
             * not mean to claim keeps its real driver from ever getting it. */
            return kUSBDeviceBusy;
        }
        SwBump(kSwSeenProxy);
    }
    /* ★★★★★ ALREADY SWITCHED ONCE THIS BOOT? THEN THIS IS A HANDBACK. STAND DOWN.
     *
     * ⚠⚠ MEASURED FAILURE, not a precaution. Driver 8.5's "Hand Bluetooth Back to Mac
     * OS" works -- the card really does return to 05AC:1000 on command, same session,
     * no reboot. But the keyboard did not come back, and this block said why:
     *
     *     Initialize calls        2
     *     matched the proxy card  2      <- we matched the handed-back card AGAIN
     *     switch attempts         7      <- budget already spent at boot
     *
     * So we re-claimed the card and then did nothing with it, because kSwMaxTries
     * lives in this shared block precisely so it survives re-enumeration. A device
     * held by a driver that will not drive it is a device its real driver never gets
     * -- which is the exact sentence already written above for the wrong-VID case,
     * unapplied to this one.
     *
     * ⇒ Once we have switched successfully, our job for this boot is DONE. Any later
     * appearance of the card at 1000 is a deliberate handback, and OS 9's own
     * USBHIDKeyboardModule is the driver that should have it.
     *
     * ⚠ Gated on the VERDICT, not on the attempt count. "We spent our budget" and "we
     * succeeded" are different facts: a switch that failed seven times should not
     * silently become a stand-down, because then a card stuck at 1000 would look
     * identical to a successful handback and the log would say nothing about which.
     *
     * ⚠ THE UNVERIFIED PART, stated plainly: whether the Expert offers a DECLINED
     * device on to the next matching driver on this stack. kUSBDeviceBusy is the
     * documented refusal and bt_probe.c uses it the same way, but this project has
     * been caught assuming documented USB behaviour here before. What IS measured is
     * that the Expert re-ran matching at all -- "Initialize calls 2" is that proof --
     * so there is a list to continue down. If the keyboard still does not come back,
     * declining is not enough and the switcher must not be resident at handback time. */
    if (gSw != NULL && gSw[kSwVerdict] == kVerdictSwitched) {
        SwBump(kSwStoodDown);
        USBExpertStatus(device,
            "\pOS9BTSwitch: already switched this boot - leaving the card to Mac OS", 0);
        return kUSBDeviceBusy;
    }


    /* ★★★★★★ IS PAIRING MODE ACTUALLY WANTED? IF NOT, LEAVE THE CARD ALONE.
     *
     * This is the last gap closed. Until v1.2 the switcher claimed the card at EVERY
     * boot simply because it was installed, which took away the working keyboard from
     * startup until the user handed it back -- the cost the header of this file has
     * warned about since it was written, and the reason it called itself a DIAGNOSTIC.
     *
     * ⇒ Now: idle at boot, keyboard works, and the card is claimed only for the ONE
     * boot after the panel asks for it.
     *
     * ★★★★★★ ⏭ AND THE PREMISE BEHIND THAT DESIGN EXPIRED ON 2026-09-14. This whole
     * idle-at-boot arrangement exists for ONE reason: owning the card meant the user
     * could not type, because our stack had no way to deliver a keystroke. That is no
     * longer true. Driver v10.6 typed `asdf` into SimpleText through our own transport,
     * decode, keycode table and PostEvent, and held `g` repeated sanely.
     *
     * ⇒ SO PERMANENT OWNERSHIP IS NOW POSSIBLE, and it is REQUIRED for the actual goal:
     * the Consumer page (volume, mute, eject) exists only in report protocol over our
     * own HIDP stack, which the card's HID proxy structurally cannot deliver -- it
     * truncates byte 8. A card that reverts to proxy mode at every boot can never carry
     * the media keys.
     *
     * ⚠⚠ DO NOT SIMPLY FLIP THIS TO "ALWAYS SWITCH" YET. Three things below still
     * assume the proxy is the safe fallback, and each needs a deliberate answer first:
     *   - every failure path falls back to "do not switch" BECAUSE that is where the
     *     keyboard works. With permanent ownership, a failed switch means a dead
     *     keyboard rather than a degraded one, and the hole named at the bottom of this
     *     function (we own the card from the flag check onward, with no release path)
     *     stops being theoretical.
     *   - M5 delivers ordinary keys today; the MEDIA keys are decoded but not yet
     *     injected, so full-time ownership currently trades volume/eject-through-proxy
     *     for nothing until that lands.
     *   - pairing has not been re-verified since the v9.8 security change.
     * This is a product-shape decision, and it belongs in docs/RELEASE-GATES.md with
     * the user, not in a comment that quietly changes behaviour.
     *
     * ⚠⚠ EVERY FAILURE PATH FALLS BACK TO "DO NOT SWITCH", which is the state where
     * the keyboard works. That is not a coincidence, it is the whole design:
     *
     *   - Preferences folder unreachable  -> no flag  -> decline -> keyboard works
     *   - flag absent                     -> decline -> keyboard works
     *   - flag present but UNDELETABLE    -> decline -> keyboard works
     *
     * ⚠⚠⚠ AND THE DELETE HAPPENS BEFORE THE SWITCH, NEVER AFTER. Order is the whole
     * safety argument. Switch-then-delete looks equivalent and is not: the switch makes
     * the card vanish mid-request, so the delete might never run, the flag would
     * persist, and the switcher would then claim the card on EVERY SUBSEQUENT BOOT --
     * a keyboard that is dead forever, for a reason invisible without file surgery.
     * Delete-first makes the worst case "pairing did not start", which is visible,
     * recoverable, and one menu click away from being retried.
     * [[feedback_no_partial_fixes_that_reproduce_the_symptom]] */
    /* ⚠ ONE HOLE, PRE-EXISTING AND KNOWINGLY LEFT: from here on we return noErr and
     * therefore OWN the card. If the switch itself then failed, we would be squatting
     * on a card we never switched and the keyboard would be dead for THIS boot -- there
     * is no "release" after a successful Initialize. That was equally true of 1.0 and
     * 1.1; what is new is that a user action now leads here, so it is worth naming.
     * The switch has never failed on this card (verdict SWITCHED every run, -6911 on
     * the first of seven attempts), and the cost is one restart, which is also the
     * recovery. Not worth building a release path for on that evidence. */
    /* ★★★ v1.3: SWITCH BY DEFAULT, WITH A SELF-HEALING MARKER. The panel's menu item
     * still exists and still forces a switch -- that is the override for a marker that
     * has latched, and the one case where the user overrules the safety. */
    /* ★★★ v1.4: THE ORDER OF THESE IS LOAD-BEARING, because three of them have side
     * effects and the && short-circuits.
     *   1. SwWantsPairMode() runs ALWAYS and consumes the flag if present -- unchanged,
     *      and it is what keeps the menu item an override over everything below.
     *   2. v1.6: SwDeclinedThisBoot() comes next and is PURE. Once we have stood down in
     *      this session we stand down for the rest of it, without re-testing anything.
     *   3. SwRetryAfterStaleRef() only runs when no flag was set, and spends its
     *      one-per-boot override only when it actually grants one.
     *   4. SwMarkerPresentOrUncreatable() CREATES the marker, so it must not run on any
     *      override path -- nor on the sticky-decline path, which is the point of 1.6:
     *      match 1 cleared the marker and match 2 must not put a new one back. */
    if (!SwWantsPairMode()
        && (SwDeclinedThisBoot()
            || (!SwRetryAfterStaleRef() && SwMarkerPresentOrUncreatable()))) {
        SwBump(kSwIdleDecline);
        USBExpertStatus(device,
            "\pOS9BTSwitch: declined - last boot did not confirm, card left to Mac OS", 0);
        return kUSBDeviceBusy;
    }

    USBExpertStatus(device, "\pOS9BTSwitch: HID-proxy A1044 - deferring CSR mode switch", 0);

    /* ⚠ SwDeferInit MUST come first and MUST be here: NewNMUPP is a Mixed Mode
     * allocator and is task-level only. */
    SwDeferInit();
    gSwitchPending = 1;
    SwDeferRequest();
    return noErr;
}

static OSStatus SwFinalize(USBDeviceRef device, USBDeviceDescriptorPtr pDesc)
{
    (void)device; (void)pDesc;
    SwBump(kSwFinal);

    /* ⭐ A Finalize count of 1 with verdict SWITCHED is the whole success signature:
     * the card accepted the request, left the bus as 1000, and the Expert tore us down
     * so the other extension could claim it as 8204.
     *
     * ⚠ NOTHING IS FREED HERE. The block outlives every instance by design -- it is
     * what the next instance adopts to know how many attempts have already happened,
     * and what BTCheck reads after the fact. gNMUpp is deliberately kept too: a
     * DisposeNMUPP here would race a notification still in the queue. */
    return noErr;
}

static OSStatus SwNotify(UInt32 notification, void *pointer, UInt32 refCon)
{
    (void)pointer; (void)refCon;
    SwBump(kSwNotifyC);
    SwNote(kSwNotifyCode, (unsigned long)notification);
    return noErr;
}
