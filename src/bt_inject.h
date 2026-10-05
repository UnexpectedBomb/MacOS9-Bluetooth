/*
 *  bt_inject.h  --  M5 step 2c: putting a decoded key into Mac OS 9's event stream.
 *
 *  ⭐ THE RECIPE IS APPLE'S, from its own OS 9 USB keyboard driver:
 *  usb-ddk/Examples/KeyboardModule/KeyIn.c, PostADBKeyToMac() at :452. See
 *  bt_bootreport.h for the full call chain and the proof that Apple runs it from a USB
 *  completion, i.e. secondary interrupt level -- which is where our reports arrive too.
 *
 *  ⚠ THIS FILE IS NOT HOST-TESTABLE, and that is the whole reason the decode, the
 *  keycode table and the press/release diff live in bt_bootreport.c instead. Everything
 *  that could be proven off-target already has been (169 assertions). What is left here
 *  is Toolbox calls and low-memory writes, which can only be exercised on the machine.
 *  Keep it thin for exactly that reason: every line added here is a line that cannot be
 *  tested until a reboot.
 */
#ifndef OS9BT_INJECT_H
#define OS9BT_INJECT_H

/* ⚠⚠ TASK LEVEL ONLY. Acquires the KCHR resource, which is the Resource Manager and is
 * task-level only -- Apple does the same once in InitUSBKeyboard (KeyIn.c:355) and
 * caches the dereferenced pointer so the Resource Manager is never reached from
 * interrupt level. Call from ProbeInitialize, which bt_probe.c documents as "task level
 * -- the Expert's first call into us".
 *
 * Safe to call more than once; the second call is a no-op. */
void BT_InjectInit(void);

/* Post one key event. `ev` is a USB usage, OR'd with BT_KEYEV_UP for a release --
 * exactly what BT_KeyEvents() produces, so the two compose without translation.
 *
 * ⚠ Safe at secondary interrupt level, and safe to call before BT_InjectInit has
 * succeeded: with no KCHR it counts the attempt and returns rather than posting. That
 * is deliberate -- a keyboard that does nothing is recoverable, and this project's
 * standing rule is that recovery paths fail open.
 * [[reference_os9_recovery_paths_fail_open]] */
void BT_InjectKeyEvent(unsigned short ev);

/* Counters, mirrored into the driver's block. Every one of these exists because the
 * only way to see inside this file on the target is the log. */
extern unsigned long gInjInitRuns;    /* BT_InjectInit calls that did work         */
extern unsigned long gInjKchrOk;      /* 1 once KCHR is held                       */
extern unsigned long gInjKchrErr;     /* 0x100 | ResError(), if it failed          */
extern unsigned long gInjEvents;      /* events handed to BT_InjectKeyEvent        */
extern unsigned long gInjPosted;      /* PostEvent calls actually made             */
extern unsigned long gInjNoVk;        /* usages with no Mac virtual keycode        */
extern unsigned long gInjNoKchr;      /* events dropped because KCHR was missing   */
extern unsigned long gInjNoChar;      /* KeyTranslate produced no character at all */
extern unsigned long gInjLastUsage;   /* 0x100 | the last usage seen               */
extern unsigned long gInjLastVk;      /* 0x100 | the virtual keycode it mapped to  */
/* v15.1: the Cursor Device Manager's own verdict, 0x10000 | OSErr, first failure only.
 * Discarding these is how "we called Move 800 times" and "Move refused 800 times"
 * become indistinguishable in a log. */
extern long gCurMoveErr, gCurBtnErr;
extern long gCurAccelErr, gCurButtonsErr, gCurUpiErr;
extern long gCurAccelUsed, gCurDevPtr;
extern unsigned long gInjPostErr;     /* 0x100 | the last nonzero PostEvent result */

/* ================================================================================
 *  M6: THE MEDIA KEYS.
 * ================================================================================
 *
 * ⭐ MEASURED 2026-09-15 on this MDD: the documented Sound Manager volume API works.
 * GetDefaultOutputVolume/SetDefaultOutputVolume return noErr, the value round-trips
 * without clamping at 0x20 / 0x60 / 0xC0, and THE BEEPS AUDIBLY GOT LOUDER. The
 * restore to 0x80 was audible too -- the user reported the trailing beep as "a bit
 * softer than the third" without knowing a restore had happened, which is an accidental
 * control confirming the whole path.
 * ⚠ The Mac Mini G4 errors $8001 on the same call (Mini-G4-Audio TIER3_FINDINGS.md).
 * This is a per-machine fact and must not be generalised.
 *
 * ⚠⚠ THE VOLUME CHANGE RUNS AT TASK LEVEL, NOT WHERE THE KEY ARRIVES. Consumer bits
 * reach us in a USB completion at secondary interrupt level, and the Sound Manager is
 * nowhere documented as interrupt-safe.
 *
 * ⇒ AND APPLE'S OWN OS 9 MEDIA-KEY HANDLER DEFERS TOO. Its USB Device Extension carries
 * PatchSystemTask and KeyboardSystemTaskPatch beside DoSoundUpButton/DoSoundDownButton/
 * DoSoundMuteButton/DoEjectButton, and patching SystemTask is the classic way to get
 * task level out of driver context. We use bt_defer.c's trampoline, which is the same
 * idea and is already proven -- it is how KCHR is acquired.
 */

/* Called at SECONDARY INTERRUPT LEVEL with the Consumer bits that have just been
 * PRESSED (a rising edge, not the raw nibble). Records the request and asks for a
 * task-level hop; does no Sound Manager work itself. */
void BT_MediaKeyPressed(unsigned char bitsPressed);

/* ⚠ TASK LEVEL ONLY. Performs whatever BT_MediaKeyPressed queued. Called from
 * BT_DeferredSwitchIfPending, alongside BT_InjectInit. */
void BT_MediaServiceAtTask(void);

/* ---- the mouse, for the A1015 --------------------------------------------------
 *
 * Delivered through the CURSOR DEVICE MANAGER, copying usb-ddk/Examples/MouseModule
 * (HIDEmulation.c:88-107) rather than inventing a path. See bt_inject.c for the level
 * split and why buttons are diffed rather than re-sent.
 *
 * BT_InjectMouse is INTERRUPT-SAFE: deltas and buttons only, no allocation. It drops
 * the report and requests a task-level hop while no cursor device exists yet.
 * BT_MouseServiceAtTask creates that device and is TASK LEVEL ONLY.
 * BT_MouseFinalize disposes it, also task level.
 *
 * ⚠ `wheel` is COUNTED, NOT DELIVERED: the Cursor Device Manager has no wheel call and
 * the A1015 has no wheel. It is carried this far so a device that sends one appears in
 * the log rather than being silently discarded. */
void BT_InjectMouse(short dx, short dy, unsigned char buttons, short wheel);
void BT_MouseServiceAtTask(void);
void BT_MouseFinalize(void);

extern unsigned long gMedPresses;     /* rising edges handed to us                  */
extern unsigned long gMedRuns;        /* task-level services performed              */
extern unsigned long gMedVolUp, gMedVolDn, gMedMute, gMedEject;
extern unsigned long gMedGetErr;      /* 0x100 | last GetDefaultOutputVolume error  */
extern unsigned long gMedSetErr;      /* 0x100 | last SetDefaultOutputVolume error  */
extern unsigned long gMedLastVol;     /* 0x100xxxx | the last value we wrote        */
extern unsigned long gMedMuted;       /* 1 while muted by us                        */
extern unsigned long gMedDropped;     /* presses that arrived with one still queued */
extern unsigned long gMedHwSetErr;    /* 0x100 | SetSoundOutputInfo(siHardwareVolume) */
extern unsigned long gMedHwReadBack;  /* 0x1000000 | hvol read straight back          */
extern unsigned long gMedDevFound;    /* 1 = an 'sdev' output device was located       */
extern unsigned long gMedDevId;       /* ⭐ the Component token, to compare with BTCheck's */
/* v11.7: Apple's own volume route (trap 0xABEC). See the long note in bt_inject.c. */
extern unsigned long gMedTrapOk;      /* 1 = TrapAvailable(0xABEC), Apple's own gate     */
extern unsigned long gMedTrapCalls;   /* taps issued through Apple's trap                */
extern unsigned long gMedTrapRc;      /* 0x100 | the trap's pascal short result          */
/* v11.9: eject is a HELD key -- press, then release 60 ticks later. */
extern unsigned long gEjectPress;     /* holds started                                   */
extern unsigned long gEjectRelease;   /* holds ended. MUST equal gEjectPress at rest      */
extern unsigned long gEjectWhich;     /* 0x100 | the code used (0x42, or 0x44 if flipped) */
/* v11.7: the Caps Lock instrument -- did our KeyMap survive to the next keystroke?      */
extern unsigned long gInjMapClobber;  /* entries where low memory disagreed with us      */
extern unsigned long gInjMapLastSys;  /* 0x1000000 | low byte of the system KeyMap word 1 */
extern unsigned long gInjMapLastOur;  /* 0x1000000 | low byte of OUR shadow word 1        */

#endif /* OS9BT_INJECT_H */
