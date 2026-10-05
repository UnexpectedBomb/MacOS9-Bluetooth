/*
 *  bt_inject.c  --  M5 step 2c: a decoded key into Mac OS 9's event stream.
 *
 *  Structured as a transcription of Apple's PostADBKeyToMac (usb-ddk/Examples/
 *  KeyboardModule/KeyIn.c:452) with its steps in Apple's order, because the order is
 *  load-bearing: the KeyMap must be current before KeyTranslate reads the modifiers out
 *  of it, and the repeat globals must be cleared before the new key's are written.
 *
 *  ⚠ This file IS in scripts/level-audit.py's FILES list.
 */
/* ⚠ REQUIRED, and the reason is in CMakeLists.txt: this target builds with
 * -DTYPE_BOOL=1, which tells MacTypes.h that the compiler has a real bool and to
 * skip its own `enum { false = 0, true = 1 }`. Sound.h pulls in MacWindows.h, which
 * USES true/false at file scope -- so without stdbool they are undeclared and the
 * build fails inside an Apple header with no mention of this file. */
#include <stdbool.h>
#include <MacTypes.h>
#include <Events.h>
#include <Resources.h>
#include <MacMemory.h>
#include <Sound.h>       /* Get/SetDefaultOutputVolume, Set/GetSoundOutputInfo -- M6 */
#include <Components.h>  /* FindNextComponent -- the sdev output device          */
#include <Patches.h>     /* NGetTrapAddress -- v11.7, Apple's own volume route   */
#include <Traps.h>       /* _Unimplemented = 0xA89F                              */
#include <MixedMode.h>   /* CallUniversalProc                                    */
#include <CursorDevices.h> /* the mouse path -- see BT_InjectMouse               */

#include "bt_bootreport.h"
#include "bt_inject.h"
#include "bt_defer.h"    /* the sanctioned interrupt -> task hop */

unsigned long gInjInitRuns, gInjKchrOk, gInjKchrErr, gInjEvents, gInjPosted;
unsigned long gInjNoVk, gInjNoKchr, gInjNoChar, gInjLastUsage, gInjLastVk, gInjPostErr;
unsigned long gInjMapClobber, gInjMapLastSys, gInjMapLastOur;

/* ⚠ PostEvent IS declared, in Events.h:562, as
 *     EXTERN_API( OSErr ) PostEvent(EventKind eventNum, UInt32 eventMsg)
 * so it is deliberately NOT redeclared here. My first draft declared it anyway, with
 * `short` for EventKind, and the compiler rejected the conflict outright -- which is
 * the good case: a self-declaration that merely DIFFERED in a way C tolerates would
 * have linked and then misbehaved at interrupt level with nothing to read afterwards.
 * If a Toolbox call seems to be missing, grep for it starting at the line beginning
 * with its name; these headers wrap the return type onto the previous line.
 *
 * ⚠⚠ KeyTranslate GENUINELY IS ABSENT from every Retro68 PPC header -- searched the
 * whole include tree -- while being exported by InterfaceLib, verified with `strings`
 * on libInterfaceLib.a. So this one declaration is unavoidable. It is the Inside
 * Macintosh signature, and the types are spelled as the Toolbox spells them so the
 * compiler checks what it can. */
extern pascal UInt32 KeyTranslate(const void *transData, UInt16 keyCode, UInt32 *state);

/* ---- low memory ------------------------------------------------------------------
 *
 * ⚠⚠ THESE FOUR ADDRESSES ARE APPLE'S OWN, open-coded in KeyIn.c:72-75 exactly as they
 * are here. They are NOT guesses and they are NOT from my memory of Inside Macintosh:
 * Retro68's LowMem.h either omits these accessors or offers them as TWOWORDINLINE 68K
 * opcodes, which do nothing on PowerPC, which is why Apple's own PPC driver open-codes
 * them too. */
#define kLMKeyMap      ((Ptr)0x0174)      /* KeyMap, 16 bytes  */
#define kLMKeyLast     (*(short *)0x0184)
#define kLMKeyTime     (*(long  *)0x0186)
#define kLMKeyRepTime  (*(long  *)0x018A)

/* ★★★★★★ APPLE'S THREE EXTRA WRITES: OMITTED, THEN RESTORED, AND THEY WERE THE FIX.
 *
 * ⚠⚠ THIS BLOCK USED TO ARGUE AT LENGTH FOR LEAVING THEM OUT, and ended "do not add
 * these three writes speculatively now -- the addresses are still unsourced." Both
 * halves of that were wrong, and the record is kept because the mistake is instructive.
 *
 * PostADBKeyToMac also does LMSetKbdVars(0), LMSetKbdLast(FakeADBAddr) and
 * LMSetKbdType(FakeKBDType). I omitted them on the grounds that their addresses were
 * unavailable "from any authoritative source on this machine". They were available the
 * whole time, in two places I had already opened:
 *     LMSetKbdVars  -- Apple's own KeyIn.c:71, #define ... (*(short *)0x0216)
 *     LMSetKbdLast  -- Events.h:841, TWOWORDINLINE(0x11DF, 0x0218)  => 0x0218
 *     LMSetKbdType  -- Events.h:866, TWOWORDINLINE(0x11DF, 0x021E)  => 0x021E
 * LowMem.h does not declare them; it says, in the comment I had read, "The following
 * functions were moved to Events.h" -- a header this file already includes.
 *
 * ⇒ AND THE CLOSING ARGUMENT WAS WORSE THAN THE OMISSION. v10.6 measured that typing
 * and auto-repeat were fine without them and concluded "this gap is CLOSED". That
 * tested REPEAT, which is what KbdVars governs -- and then generalised to KbdLast and
 * KbdType, which govern which keyboard and which type the OS thinks produced the event.
 * Caps Lock was broken for four builds on exactly that difference. A measurement closes
 * the thing it measured, not its neighbours.
 * [[feedback_control_must_exercise_the_change]]
 *
 * ⭐ RESTORED in v11.7 and CONFIRMED on hardware the same day: Caps Lock capitalises.
 * The v11.7 instrument also ruled out the alternative explanation outright -- "KeyMap
 * clobbered between keys 0", so our shadow was never being overwritten and persistence
 * was never the problem. The writes themselves were the fix. */

/* ⚠ Apple's constants, not addresses: the ADB address and keyboard type it reports in
 * the event message. 16 is an address no real ADB keyboard uses, which is the point --
 * software reading it can tell this key did not come from the ADB bus. Included so our
 * event message matches Apple's field for field; diverging here would be a difference
 * with no reason behind it. */
#define kFakeADBAddr  16

static UInt32   gKeyMap[4];        /* our shadow of the system KeyMap */
static UInt32   gKeyTransState;    /* KeyTranslate's dead-key state   */
static UInt8   *gKCHR;
static Handle   gKCHRHandle;

void BT_InjectInit(void)
{
    if (gKCHR != 0) return;                /* already have it */
    gInjInitRuns++;

    /* ⚠ TASK LEVEL. GetResource is the Resource Manager. Apple does exactly this once,
     * in InitUSBKeyboard, and caches the dereferenced pointer so that KeyTranslate at
     * interrupt level never touches a handle. HLock is what makes that dereference
     * legitimate rather than a dangling pointer waiting for a heap compaction.
     * ⚠ Resource 0 is the US layout; Apple's comment notes the ADB Manager handles
     * layout selection differently, and this driver does not attempt to. */
    gKCHRHandle = GetResource('KCHR', 0);
    if (gKCHRHandle == 0 || *gKCHRHandle == 0) {
        gInjKchrErr = 0x100UL | (unsigned long)(unsigned short)ResError();
        return;
    }
    HLock(gKCHRHandle);
    gKCHR     = (UInt8 *)*gKCHRHandle;
    gInjKchrOk = 1;
}

/* Set or clear one bit of the shadow KeyMap. Apple's SetBit, kept local so this file
 * has no dependency on theirs. Bit order is the KeyMap's own: byte index is
 * bit >> 3, and within a byte the low bit is the lowest-numbered key. */
static void KeyMapSet(short bit, int down)
{
    UInt8 *p = (UInt8 *)gKeyMap;
    if (bit < 0 || bit > 127) return;
    if (down) p[bit >> 3] |=  (UInt8)(1 << (bit & 7));
    else      p[bit >> 3] &= (UInt8)~(1 << (bit & 7));
}

void BT_InjectKeyEvent(unsigned short ev)
{
    unsigned char usage = (unsigned char)(ev & 0x00FF);
    int           down  = (ev & BT_KEYEV_UP) ? 0 : 1;
    unsigned char vk;
    UInt32        msg;
    UInt16        code;

    gInjEvents++;
    gInjLastUsage = 0x100UL | (unsigned long)usage;

    vk = BT_UsageToVirtualKey(usage);
    if (vk == BT_VK_NONE) { gInjNoVk++; return; }
    gInjLastVk = 0x100UL | (unsigned long)vk;

    /* ⚠ Apple drops anything above 127 as "not handled by MacOS". Every entry in our
     * table is BT_VK_NONE or <= 0x7F and there is a host test asserting exactly that,
     * so this cannot fire today -- it is kept because the table could be regenerated
     * from a different source, and a silent out-of-range KeyMap write is not a failure
     * mode worth leaving open. */
    if (vk > 127) { gInjNoVk++; return; }

    /* ⚠⚠ FAIL OPEN, AND ASK FOR THE RESOURCE WHILE FAILING. Without KCHR there is
     * nothing to translate with, so count it and stop rather than posting a fabricated
     * event -- but also request the task-level hop that CAN acquire it.
     *
     * ⇒ WHY IT IS ACQUIRED LAZILY AT ALL: GetResource is the Resource Manager, so it
     * cannot run here (secondary interrupt level), and v10.4 proved it cannot run on
     * the load path either -- calling it from ValidateHW stopped the fragment loading.
     * Apple's driver never touches the Resource Manager at load either; it acquires on
     * demand (KBDHIDEmulation.c:70). This is that, with the demand being a real
     * keystroke.
     *
     * ⚠ BT_DeferRequest is interrupt-safe and COALESCES, so calling it on every
     * dropped key is free -- one notification is queued no matter how many keys are
     * pressed before it is serviced. The cost is that the first keystroke or two are
     * counted here instead of typed, which is visible in kWInjNoKchr rather than
     * mysterious. */
    if (gKCHR == 0) { gInjNoKchr++; BT_DeferRequest(); return; }

    /* --- Apple's order, from here down ------------------------------------------ */

    /* Stop any repeat in progress before the new key's state replaces it. */
    kLMKeyLast = 0;

    /* ⭐⭐ v11.7 INSTRUMENT: DID OUR KEYMAP SURVIVE SINCE THE LAST KEYSTROKE?
     *
     * This is the one question the v11.6 run could not answer. Caps Lock's toggle is
     * PROVEN to run -- 24 state changes in that log -- and the bit arithmetic is
     * verified against Apple's own SetBit (KeyIn.c: index/8, 1 << index%8), which puts
     * virtual key 57 in bit 1 of word 1, exactly where the modifier fold shifts it to
     * 0x0400 = alphaLock. Every step checks out and capitalization still fails, so the
     * remaining suspect is PERSISTENCE: we write our shadow over low memory and
     * something else -- the wired keyboard's own driver, or the OS -- writes it back
     * before the next letter is translated. Nothing in the driver could see that.
     *
     * ⚠ Compare BEFORE we overwrite, or the question answers itself. */
    {
        const UInt8 *sys = (const UInt8 *)kLMKeyMap;
        UInt8 ours = ((const UInt8 *)gKeyMap)[7];   /* byte 7 holds vk 56..63: shift,
                                                     * caps, option, control */
        if (sys[7] != ours) gInjMapClobber++;
        gInjMapLastSys = 0x1000000UL | (unsigned long)sys[7];
        gInjMapLastOur = 0x1000000UL | (unsigned long)ours;
    }

    /* Our shadow first, then the system's copy: KeyTranslate reads the modifier state
     * out of the KeyMap, so it must be current BEFORE the call below. */
    KeyMapSet((short)vk, down);
    BlockMoveData((Ptr)gKeyMap, kLMKeyMap, (Size)sizeof(gKeyMap));

    /* ⚠⚠ APPLE'S THREE WRITES, RESTORED. I dropped these with the note that their
     * addresses were "unsourceable" -- and that was simply false. LMSetKbdVars is
     * defined in Apple's own KeyIn.c:71 as *(short*)0x0216, and LMSetKbdLast /
     * LMSetKbdType are declared in Events.h:841/:866 (TWOWORDINLINE 0x11DF, 0x0218 and
     * 0x021E), which this file already includes. A retracted claim is an untested
     * to-do, not a closed one [[feedback_retracted_claims_are_a_todo_list]].
     *
     * ⚠ These tell the OS WHICH keyboard produced the event and what type it is. I am
     * NOT claiming they fix Caps Lock -- the instrument above is what will say. They go
     * in because Apple does them and my reason for omitting them was wrong. */
    *(short *)0x0216 = 0;                   /* LMSetKbdVars(0) -- stop repeating */
    *(unsigned char *)0x0218 = kFakeADBAddr; /* LMSetKbdLast  */
    *(unsigned char *)0x021E = 2;            /* LMSetKbdType = Apple Extended    */

    /* ⚠ THE MODIFIER BITS ARE FOLDED INTO THE KEYCODE, not passed separately.
     * KeyTranslate takes a 16-bit value: the virtual keycode in the low 7 bits, bit 7
     * set for a key UP, and the modifier state in the high byte. Apple builds it from
     * gKeyMap[1], which is where the modifier keys live in the KeyMap. This expression
     * is Apple's, transcribed rather than re-derived -- the shifts are not obvious and
     * getting them wrong would break shifted characters in a way that looks like a
     * layout problem. */
    code = (UInt16)(vk
                    | ((gKeyMap[1] << 9) & 0x00FE00)
                    | ((gKeyMap[1] >> 7) & 0x000100)
                    | (down ? 0 : 0x0080));

    msg = KeyTranslate(gKCHR, code, &gKeyTransState);

    /* ⚠⚠ TWO CHARACTERS, TWO EVENTS. KeyTranslate returns a pair packed into one long:
     * a dead-key sequence can complete and produce both the accent and the base
     * character in a single call. Apple posts both halves, high first. Posting only one
     * would silently drop the second character of every accented keystroke. */
    if (msg & 0xFFFF0000UL) {
        UInt32 event = (msg & 0xFF000000UL)
                     | (((UInt32)kFakeADBAddr << 16) & 0x00FF0000UL)
                     | (((UInt32)vk << 8) & 0x0000FF00UL)
                     | ((msg >> 16) & 0x000000FFUL);
        OSErr err;
        if (down) {
            UInt32 ticks = TickCount();
            kLMKeyTime    = (long)ticks;
            kLMKeyRepTime = (long)ticks;
            kLMKeyLast    = (short)(event & 0xFFFFUL);
        }
        err = PostEvent(down ? keyDown : keyUp, event);
        gInjPosted++;
        if (err != noErr) gInjPostErr = 0x100UL | (unsigned long)(unsigned short)err;
    }
    if (msg & 0x0000FFFFUL) {
        UInt32 event = ((msg << 16) & 0xFF000000UL)
                     | (((UInt32)kFakeADBAddr << 16) & 0x00FF0000UL)
                     | (((UInt32)vk << 8) & 0x0000FF00UL)
                     | (msg & 0x000000FFUL);
        OSErr err;
        if (down) {
            UInt32 ticks = TickCount();
            kLMKeyTime    = (long)ticks;
            kLMKeyRepTime = (long)ticks;
            kLMKeyLast    = (short)(event & 0xFFFFUL);
        }
        err = PostEvent(down ? keyDown : keyUp, event);
        gInjPosted++;
        if (err != noErr) gInjPostErr = 0x100UL | (unsigned long)(unsigned short)err;
    }
    if ((msg & 0xFFFFFFFFUL) == 0) gInjNoChar++;
}

/* ================================================================================
 *  M6: THE MEDIA KEYS. See bt_inject.h for the measurement that justifies this.
 * ================================================================================ */

unsigned long gMedPresses, gMedRuns, gMedVolUp, gMedVolDn, gMedMute, gMedEject;
unsigned long gMedGetErr, gMedSetErr, gMedLastVol, gMedMuted, gMedDropped;

/* ⚠ ONE STEP = 0x20 OF 0x0100, i.e. eight presses from silence to full. That is the
 * granularity the audible test used (0x20 / 0x60 / 0xC0 were plainly distinct), so it
 * is a measured feel rather than a guess. */
/* ⚠⚠ 0x10, NOT 0x20, AND IT IS MEASURED. The wired Apple Pro Keyboard (M7803) was used
 * as the instrument: three of its volume-up presses moved hvol from 0x0080 to 0x00B0,
 * i.e. 0x30 for three presses -- EXACTLY 0x10 each. That is 16 steps from silence to
 * full, and matching the reference keyboard is better than the 0x20 I had guessed. */
#define kVolStep    0x10L
#define kVolMax     0x0100L          /* kFullVolume */

/* ⚠ volatile: written at interrupt level, read at task level. Without it the compiler
 * may keep the pending mask in a register across the task-level read and service a
 * press that has already been superseded -- or miss one entirely. */
static volatile unsigned char gMedPending;

/* ⚠ The 'sdev' sound output device, found ONCE at task level and cached. BTCheck
 * measured exactly one on this machine. FindNextComponent is the Component Manager and
 * belongs nowhere near an interrupt; it is called from BT_MediaServiceAtTask, which is
 * the defer trampoline's task-level half. */
static Component gSndDev;
unsigned long gMedHwSetErr, gMedHwReadBack, gMedDevFound, gMedDevId;

/* ================================================================================
 *  v11.7: APPLE'S ACTUAL VOLUME ROUTE, DISASSEMBLED RATHER THAN GUESSED
 * ================================================================================
 * ⚠⚠ THE COMMENT BELOW USED TO SAY Apple's handler was "NOT AVAILABLE TO US" and that
 * the next candidate was siHardwareVolume. Both statements were wrong, and the second
 * cost three hardware cycles. What follows is read out of Apple's own code.
 *
 * MEASURED, 2026-09-15, from the PEF container at offset 378672 of the shipping
 * `USB Device Extension` (NOT 370896 -- that offset is not a container boundary, and
 * the "44 imports including AESend and LaunchApplication" recorded in
 * docs/M6-VOLUME-MECHANISM.md came from mis-parsing it. The real container imports 35
 * symbols and NOT ONE Apple Event call, so the Apple-Event theory is dead.)
 *
 * DoSoundUpButton, in full, is:
 *      proc = GetToolboxTrapAddress(0xABEC);
 *      CallUniversalProc(proc, 0xE0, &block);
 * and nothing else. procInfo 0xE0 decodes as pascal, 2-byte result, one 4-byte
 * pointer argument:  pascal short (*)(void *).
 *
 * The blocks are constants in Apple's TOC, three pairs, chosen by the key state.
 * Apple's own KeyIn.c:24 defines DOWN 0 / UP 1, and the branch takes the r3==0 (DOWN)
 * leg to the SECOND member -- so 7 is the press and 6 is the release:
 *
 *      volume up     press {7,0x12,0}   release {6,0x12,0}
 *      volume down   press {7,0x11,0}   release {6,0x11,0}
 *      mute          press {7,0x0A,0}   release {6,0x0A,0}
 *
 * ⭐ THIS IS WHY IT MATTERS MORE THAN A NEW API: Apple's shim is a DRIVER-CONTEXT
 * fragment, exactly like ours. The v11.6 run proved the Component Manager will not
 * serve us here (driver and app hold the identical 'sdev' token, 0x00010027, and get
 * different answers). Apple never asks it to. One trap does the whole job -- the level
 * change AND the on-screen feedback -- which is precisely the pair the user reports
 * missing. No helper application is required, and the design fork is closed.
 *
 * ⚠ THE GATE IS APPLE'S TOO, not caution I invented: KeyboardShimInitialize calls its
 * own TrapAvailable(0xABEC) -- NGetTrapAddress compared against 0xA89F
 * (_Unimplemented) -- and stores the result. An unimplemented trap resolves to the
 * _Unimplemented handler, so calling it blind would run the wrong code. */
#define kMedVolTrap  0xABEC
#define kMedProcInfo 0x000000E0UL     /* pascal, 2-byte result, one 4-byte pointer */

typedef struct { unsigned short kind; unsigned short which; unsigned long rsvd; } MedBlk;

/* ⚠⚠⚠ 6 IS THE PRESS AND 7 IS THE RELEASE. v11.7 SHIPPED THESE TRANSPOSED AND THE
 * MACHINE RAN AWAY -- the volume rose and fell by itself until the wired keyboard sent
 * a proper release. Worth writing down exactly how I got it wrong.
 *
 * I derived the mapping from Apple's KeyIn.c:24 (DOWN 0 / UP 1) plus the branch in
 * DoSoundUpButton, and concluded r3==0 meant DOWN. That was an assumption about the
 * PARAMETER'S MEANING, and I never checked the callers. The callers say otherwise:
 *      cmpwi r9,0xE9        ; HID Consumer 'Volume Increment' present in this report?
 *      li    r3,1  ;  bl DoSoundUpButton     <- usage IS held
 *   ...
 *      cmpwi r26,0          ; usage NOT seen this frame?
 *      li    r3,0  ;  bl DoSoundUpButton     <- usage is RELEASED
 * ⇒ r3 is "is this key currently down", NOT the DOWN/UP constant. r3!=0 (held) takes
 * the r1+0x38 leg, which holds the block copied from r2+0x218 = {6,...}.
 *
 * ⇒ 6 = key down, 7 = key up. Shipping them swapped meant every tap ENDED ON A PRESS,
 * so the system held the key down and auto-repeated it forever. That is the entire
 * reported bug, and one caller disassembly would have caught it before the build.
 * [[feedback_second_verification_pass_before_every_build]] */
#define kMedPress   6
#define kMedRelease 7
#define kMedVolUp   0x12
#define kMedVolDn   0x11
#define kMedMute    0x0A

unsigned long gMedTrapOk, gMedTrapCalls, gMedTrapRc;
static UniversalProcPtr gMedTrapProc;

/* Apple's TrapAvailable, transcribed. ⚠ Task level only -- NGetTrapAddress is a trap. */
static int MedTrapAvailable(unsigned short trapNum)
{
    return NGetTrapAddress(trapNum, ToolTrap)
        != NGetTrapAddress((unsigned short)_Unimplemented, ToolTrap);
}

/* One edge. ⚠ The block is STATIC, not a stack local -- CallUniversalProc hands the
 * pointer to code we do not control, and this project has already been bitten once
 * this session by passing a dying frame to something that kept the pointer. */
static void MedSend(unsigned short kind, unsigned short which)
{
    static MedBlk blk;
    if (gMedTrapProc == 0) return;
    blk.kind = kind; blk.which = which; blk.rsvd = 0;
    gMedTrapCalls++;
    gMedTrapRc = 0x100UL | (unsigned long)(unsigned short)
        CallUniversalProc(gMedTrapProc, kMedProcInfo, &blk);
}

/* One complete keystroke, for the three keys where a tap is the whole gesture. */
static void MedTapKey(unsigned short which)
{
    MedSend(kMedPress, which);
    MedSend(kMedRelease, which);
}

/* ================================================================================
 *  EJECT, AND WHY IT IS NOT A TAP
 * ================================================================================
 * ⚠⚠ THE NOTE THAT USED TO SIT AT THE BOTTOM OF THIS FILE WAS WRONG ON ITS FACTS.
 * It said eject "means the File Manager and the drive queue, not the Sound Manager",
 * and deferred the work on that basis. Apple's DoEjectButton does nothing of the kind:
 * it uses the SAME trap 0xABEC as the three sound keys, with {6,0x42}. The system
 * decides what to eject, exactly as it does for the wired keyboard, so the risk is the
 * risk of pressing Eject -- not of us walking the drive queue.
 *
 * ⭐ BUT IT IS NOT A TAP, AND A TAP WOULD SILENTLY DO NOTHING. Apple sends the press,
 * stores TickCount, and sends the release from KeyboardSystemTaskPatch only once
 * SIXTY TICKS have passed:
 *      lwz r12,0(r31) ; addi r0,r12,60 ; cmplw r3,r0 ; ble exit
 *      addi r12,r2,0xF8          <- the {7,0x42} release block
 * One second, synthesised regardless of how long the user actually held the key. That
 * is hold-to-eject, and the system will not act on a shorter press.
 *
 * ⚠⚠⚠ THE HAZARD THIS CREATES IS THE ONE WE JUST SHIPPED A BUG FOR. A press with no
 * release leaves the key logically DOWN and the system auto-repeats it -- which for
 * eject means the tray cycling, not just a wandering volume. And BT_DeferRequest is
 * ONE-SHOT and event-driven, NOT periodic: if nothing else asks for task level, a
 * naively deferred release is never sent at all. So the release re-arms its own wake-up
 * until it has actually gone out, and a second press cannot start while one is pending.
 * [[feedback_guards_must_not_latch]] -- gEjectDueTick is cleared on the path that sends,
 * so it cannot strand itself. */
#define kMedEject       0x42        /* Apple's shared TOC constant is {7,0x42}.
                                     * ⚠ 0x44 is the sibling code, selected by a byte
                                     * this driver has no equivalent of; if 0x42 turns
                                     * out to be the wrong drive, 0x44 is the one flip
                                     * to try. Published so the log says which ran. */
#define kEjectHoldTicks 60          /* Apple's own figure, not a guess */

unsigned long gEjectPress, gEjectRelease, gEjectWhich;
static unsigned long gEjectDueTick;  /* 0 = nothing pending */

/* Task level only. Returns 1 while a release is still owed, so the caller re-arms. */
static int MedEjectServiceAtTask(void)
{
    if (gEjectDueTick == 0) return 0;
    if ((long)((unsigned long)TickCount() - gEjectDueTick) < 0) return 1;  /* wrap-safe */
    MedSend(kMedRelease, kMedEject);
    gEjectRelease++;
    gEjectDueTick = 0;
    return 0;
}
static long gMedPreMute;             /* the level to restore on unmute */

void BT_MediaKeyPressed(unsigned char bitsPressed)
{
    if (bitsPressed == 0) return;
    gMedPresses++;
    /* ⚠ OR rather than overwrite: two keys can rise in one report, and a press must not
     * be lost because another arrived in the same 11 bytes. If a service is still
     * queued the bits simply accumulate, which is counted so the log can show it. */
    if (gMedPending != 0) gMedDropped++;
    gMedPending = (unsigned char)(gMedPending | bitsPressed);
    /* Interrupt-safe and coalescing -- one notification however many keys are pressed
     * before it is serviced. Same trampoline as KCHR acquisition. */
    BT_DeferRequest();
}

void BT_MediaServiceAtTask(void)
{
    unsigned char want;
    long vol = 0;
    OSErr err;

    /* ⚠ TAKE AND CLEAR IN ONE STEP, before any Sound Manager call. Those calls can take
     * real time, and a press arriving during them must queue a NEW request rather than
     * be dropped as already-handled or serviced twice. */
    /* ⚠⚠ THE EJECT RELEASE IS SERVICED BEFORE THE `no keys pending` RETURN, and that
     * placement is the whole safety property. A release that is owed must go out on a
     * pass where nothing was pressed -- which is every pass after the press. Put this
     * below the early return and the key stays down until the user happens to press
     * something else, which is the stuck-key runaway again. */
    if (MedEjectServiceAtTask()) BT_DeferRequest();   /* still owed: come back */

    want = gMedPending;
    if (want == 0) return;

    /* Found once, on the first key press rather than at init: this is task level, and
     * the first press is the earliest moment we are certainly here. */
    if (gSndDev == 0) {
        ComponentDescription cd;
        cd.componentType         = siHardwareVolume ? FOUR_CHAR_CODE('sdev') : 0;
        cd.componentSubType      = 0;
        cd.componentManufacturer = 0;
        cd.componentFlags        = 0;
        cd.componentFlagsMask    = 0;
        gSndDev = FindNextComponent(0, &cd);
        if (gSndDev != 0) gMedDevFound = 1;
        /* ⭐ Publish the token itself, not just "found". BTCheck prints the one IT gets
         * from the identical lookup; the two numbers side by side are what separate "the
         * driver found a different component" from "the driver found the right component
         * and cannot use it from here." See the note at kWMedDevId in bt_probe.c. */
        gMedDevId = (unsigned long)gSndDev;
        /* ⭐ v11.7: resolve Apple's trap once, here, at task level -- same place and
         * same reason the component lookup lives here. */
        gMedTrapOk = (unsigned long)MedTrapAvailable(kMedVolTrap);
        if (gMedTrapOk)
            gMedTrapProc = (UniversalProcPtr)
                NGetTrapAddress((unsigned short)kMedVolTrap, ToolTrap);
    }
    gMedPending = 0;
    gMedRuns++;

    /* ★★★★★★ APPLE'S ROUTE, AND IT RUNS BEFORE THE SOUND MANAGER IS EVEN CONSULTED.
     *
     * ⚠ v11.7/v11.8 had this branch BELOW the GetDefaultOutputVolume call, so a machine
     * where that errors -- the Mac Mini G4 returns $8001 for it -- would have skipped
     * Apple's trap entirely on the way to a fallback that cannot work either. The trap
     * needs nothing from the Sound Manager, so it must not be gated behind it. Found by
     * re-reading the control flow rather than by a run, which is the cheap way.
     * [[feedback_second_verification_pass_before_every_build]] */
    if (gMedTrapProc != 0) {
        if (want & BT_CONSUMER_MUTE)   { gMedMute++;  MedTapKey(kMedMute); }
        if (want & BT_CONSUMER_VOL_UP) { gMedVolUp++; MedTapKey(kMedVolUp); }
        if (want & BT_CONSUMER_VOL_DN) { gMedVolDn++; MedTapKey(kMedVolDn); }
        /* ⭐ EJECT: press now, release one second from now. Guarded so a second press
         * cannot start a new hold while one is outstanding -- otherwise two quick
         * presses would leave a press with no matching release. */
        if (want & BT_CONSUMER_EJECT) {
            gMedEject++;
            if (gEjectDueTick == 0) {
                MedSend(kMedPress, kMedEject);
                gEjectPress++;
                gEjectWhich = 0x100UL | (unsigned long)kMedEject;
                gEjectDueTick = (unsigned long)TickCount() + kEjectHoldTicks;
                if (gEjectDueTick == 0) gEjectDueTick = 1;  /* never alias "idle" */
                BT_DeferRequest();
            }
        }
        return;
    }

    err = GetDefaultOutputVolume(&vol);
    gMedGetErr = 0x100UL | (unsigned long)(unsigned short)err;
    if (err != noErr) return;        /* fail open: no volume read, no volume written */

    /* ⚠ ONE VALUE, BOTH CHANNELS. The long is (left << 16) | right and this driver does
     * not implement balance -- it reads the LEFT channel, moves it, and writes the same
     * level to both. Treating the channels independently would silently destroy any
     * balance the user had set in the Sound control panel. */
    {
        long lvl = (vol >> 16) & 0xFFFFL;

        /* ⚠⚠ MUTE IS COUNTED AND DELIBERATELY INERT FOR NOW. It is the one action that
         * can leave the machine SILENT with a sticky flag, and v10.8 did exactly that --
         * the user had to fix it by hand. Until the write is confirmed to move the real
         * volume (the 'hvol' experiment), mute stays out: a wrong volume step is a
         * nuisance the user can hear and correct, a wrong mute is silence they have to
         * diagnose. Restoring it is uncommenting this block.
         * [[feedback_no_partial_fixes_that_reproduce_the_symptom]] */
        /* ★★★★★ MUTE IS LIVE AGAIN AS OF v11.2, because the write is now PROVEN.
         * v11.1 moved the system volume from 0x70 to full and 'hvol' -- an independent
         * hardware selector read before BTCheck touched anything -- agreed. Mute was
         * held back only while the write was unconfirmed, on the grounds that a wrong
         * mute is silence the user has to diagnose. A mute that demonstrably toggles is
         * not that: the same key undoes it. */
        /* ★★★★★★ v11.7: APPLE'S ROUTE FIRST, AND IT SHORT-CIRCUITS THE REST.
         *
         * When the trap is there, do exactly what Apple does and NOTHING ELSE. The
         * Sound Manager arithmetic below stays only as the fallback for a machine where
         * the trap is absent -- it is measured inert on this one, and running both would
         * have our stale software value fighting whatever the trap sets. Apple makes no
         * sound call at all, and that is the point. */
        if (want & BT_CONSUMER_MUTE) {
            gMedMute++;
            if (gMedMuted) {
                lvl = gMedPreMute;           /* unmute: back to where we found it */
                gMedMuted = 0;
            } else {
                gMedPreMute = lvl;
                lvl = 0;
                gMedMuted = 1;
            }
        }

        /* ⚠ A VOLUME KEY WHILE MUTED WOULD UNMUTE FIRST, which is what every other
         * system does and what pressing volume-up on a silent machine plainly means.
         * gMedMuted is never set while mute is inert, so this is dormant rather than
         * dead -- it is the half of the mute feature that does not risk silence, and
         * it comes back working the moment the block above does. */
        if (gMedMuted) { lvl = gMedPreMute; gMedMuted = 0; }
        if (want & BT_CONSUMER_VOL_UP)   { gMedVolUp++; lvl += kVolStep; }
        if (want & BT_CONSUMER_VOL_DN)   { gMedVolDn++; lvl -= kVolStep; }

        if (lvl < 0)        lvl = 0;
        if (lvl > kVolMax)  lvl = kVolMax;

        vol = ((lvl & 0xFFFFL) << 16) | (lvl & 0xFFFFL);

        /* ⚠⚠⚠ THE WRITE IS DELIBERATELY NOT MADE, AND THIS IS A HARM FIX, NOT A PAUSE.
         *
         * v10.8 called SetDefaultOutputVolume(vol) here. MEASURED on hardware: 36 media
         * presses produced 36 clean writes, every call returned noErr -- and NO volume
         * bar appeared and NOTHING audibly changed. The user's WIRED USB keyboard does
         * both on the same machine at the same time, so the mechanism exists and this is
         * not it.
         *
         * ⇒ AND IT ACTIVELY HARMED: a mute press set output to ZERO and nothing restores
         * it, so the session ended with the machine silent and the user had to fix it by
         * hand. A key that does nothing is a disappointment; a key that silences the
         * machine and cannot undo it is a defect. Leaving the call in while it is known
         * wrong is the "no partial fixes that reproduce the symptom" rule.
         * [[feedback_no_partial_fixes_that_reproduce_the_symptom]]
         *
         * ⚠ WHAT MY BEEP TEST ACTUALLY PROVED, and it was less than I read into it:
         * SysBeep RESPECTS the default output volume. It did NOT prove that value is the
         * system volume other audio obeys, because the same process set the level and
         * made the sound -- the oracle exercised a neighbour of the thing under test.
         *
         * ⇒ THE PATH ABOVE IS ALL PROVEN AND STAYS: radio, L2CAP, report protocol, byte
         * 8, rising-edge detection, the interrupt-to-task hop, and the clamping and
         * mute-state logic. Only the final call is missing, and restoring it is ONE LINE
         * once the right API is known. The counters keep proving the path meanwhile.
         *
         * ⚠ APPLE'S OWN HANDLER IS NOT AVAILABLE TO US: DoSoundUpButton, DoSoundDownButton,
         * DoSoundMuteButton and DoEjectButton are INTERNAL to the USBKeyboardSupport
         * fragment -- its PEF export table carries only TheUSBShimDescription, USBShim,
         * gShimData, InvokeExternalButtonCallback, RegisterButtonCallback and
         * UnregisterButtonCallback, and none of those causes a button, they only report
         * one. Parsed from the container at offset 370896, not inferred from strings.
         *
         * ⏭ NEXT CANDIDATE: siHardwareVolume via Set/GetSoundOutputInfo on the 'sdev'
         * output component -- the route the Mac Mini tested SEPARATELY and which is still
         * untested here. BTCheck probes it. */
        /* ★★★★★★ v11.0: THE WRITE IS BACK, FOR VOLUME ONLY, AND THE REASON IS NEW DATA.
         *
         * ⭐ BTCheck v99.33 measured siHardwareVolume ('hvol') on the one 'sdev' output
         * device: 0x00700070, err 0000 -- and GetDefaultOutputVolume returned THE SAME
         * 0x00700070, which is the level the user had just set by hand in the Sound
         * control panel. ⇒ BOTH APIs READ THE REAL SYSTEM VOLUME. They are two views of
         * one value, not two different volumes, so "we are setting the wrong volume" was
         * the wrong conclusion.
         *
         * ⇒ AND v10.8'S WRITES PROBABLY DID WORK: that session ended with the machine
         * genuinely silent, which is our mute write taking effect. What certainly did
         * NOT happen is the on-screen volume bar, and "no audible change" may have been
         * the missing bar plus nothing playing to hear.
         *
         * ⚠ THAT IS A HYPOTHESIS AND THE NEXT RUN IS THE EXPERIMENT. It needs no ear and
         * no bar: press volume up a few times, then read 'hvol' in BTCheck. If the
         * NUMBER MOVED, the write works and only the feedback UI is missing. If it did
         * not, the write is inert and the default-output route is dead after all. */
        /* ★★★★★★ WRITE THE HARDWARE SELECTOR, NOT JUST THE SOFTWARE COPY.
         *
         * ⚠⚠ THE ASYMMETRY IS THE WHOLE BUG, AND IT IS THE ONE MY OWN NOTES WARN ABOUT:
         * READ EVIDENCE IS NOT WRITE EVIDENCE. We had only ever READ 'hvol'. Writing
         * SetDefaultOutputVolume moved the value that BOTH readers report -- and moved
         * nothing audible. Measured with the wired M7803 as the instrument: when the
         * REAL volume changes, hvol and GetDefaultOutputVolume both follow it. So those
         * APIs read the real level faithfully; it is the WRITE that stops at a software
         * copy the hardware never consults.
         * [[feedback_measure_every_direction_before_enabling]]
         *
         * ⇒ SetSoundOutputInfo with siHardwareVolume is the write counterpart of the
         * read that demonstrably tracks the hardware. That is the untested direction and
         * the reason this build exists.
         *
         * ⚠ BOTH WRITES ARE MADE, hardware first. If the hardware write is the one that
         * works, the software copy should follow it anyway -- but leaving the Sound
         * Manager's own value stale would make every later READ disagree with the
         * hardware, and this driver reads it on the next key press to compute the step.
         * Cheap insurance against a half-updated pair. */
        if (gSndDev != 0) {
            long hw = vol;
            OSErr eH = SetSoundOutputInfo(gSndDev, siHardwareVolume, &hw);
            gMedHwSetErr = 0x100UL | (unsigned long)(unsigned short)eH;
            hw = 0;
            if (GetSoundOutputInfo(gSndDev, siHardwareVolume, &hw) == noErr)
                gMedHwReadBack = 0x1000000UL | (unsigned long)(hw & 0xFFFFFFL);
        }
        err = SetDefaultOutputVolume(vol);
        gMedSetErr  = 0x100UL | (unsigned long)(unsigned short)err;
        gMedLastVol = 0x1000000UL | (unsigned long)(vol & 0xFFFFFFL);  /* what we WOULD write */
    }

    /* ⚠⚠ EJECT IS COUNTED AND NOT ACTED ON, DELIBERATELY. Apple's DoEjectButton sits
     * beside the three sound handlers but does something entirely different -- it
     * ejects removable media, which means the File Manager and the drive queue, not the
     * Sound Manager. It is a separate piece of work with its own risks (ejecting the
     * wrong volume, or one with unsaved data) and it is not going to be bolted on here
     * without its own investigation. The counter proves the key arrives. */
    if (want & BT_CONSUMER_EJECT) gMedEject++;
}

/* ================================================================================
 *  THE MOUSE, for the A1015.
 * ================================================================================
 *
 * ⭐ PRIOR ART, COPIED RATHER THAN DERIVED: usb-ddk/Examples/MouseModule is Apple's own
 * OS 9 USB mouse driver, and HIDEmulation.c:88-107 is the whole recipe. It uses the
 * CURSOR DEVICE MANAGER -- not PostEvent, not low-memory MTemp pokes -- and its report
 * handler does exactly four things, all reproduced below:
 *
 *   1. refuse to call anything without a live CursorDevicePtr;
 *   2. CursorDeviceMove ONLY when a delta is nonzero;
 *   3. mask buttons to 0x07 and call CursorDeviceButtons ONLY ON CHANGE;
 *   4. remember the button state for the next diff.
 *
 * ⚠ (3) is not an optimisation. Re-sending an unchanged button mask on every report
 * makes the Cursor Device Manager see a fresh press each time, which on a mouse held
 * still with a button down is a click autorepeat nobody asked for. Apple diffs; so do we.
 *
 * ⚠⚠ LEVEL SPLIT, and it is the same one this file already uses for the KCHR and the
 * 'sdev'. CursorDeviceMove/Buttons are called from Apple's USB completion routine, i.e.
 * secondary interrupt level, which is exactly our context -- so those are safe here.
 * CursorDeviceNewDevice ALLOCATES and is task-level only, so the device is created on
 * the defer trampoline's task-level half and the first reports are dropped until it
 * exists. A mouse sends hundreds of reports a second; losing the first few costs
 * nothing, and creating it at ProbeInitialize instead would register a phantom cursor
 * device on every machine that never attaches a mouse.
 */

/* ⚠ volatile: written at interrupt level, read at task level. Same reason as gMedPending
 * -- without it the compiler may cache the flag in a register across the task-level read
 * and the device is never created, or is created twice. */
static volatile short  gCurWant;        /* a report arrived with no device yet */
static CursorDevicePtr gCurDev;
static unsigned char   gCurButtons;     /* remembered mask, for the change diff */

long gCurMoveErr, gCurBtnErr;   /* 0x10000 | OSErr, first failure only */
long gCurAccelErr, gCurButtonsErr, gCurUpiErr;  /* v16.2: the setup calls' verdicts */
long gCurAccelUsed;      /* v16.4: the acceleration we inherited, Fixed */
long gCurDevPtr;         /* v16.4: our CursorDevicePtr, so BTCheck can name our row */
unsigned long gMouseReports, gMouseMoves, gMouseBtnChanges, gMouseDropped;
unsigned long gMouseWheelSeen, gCurNewErr, gCurCreated;

/* ⚠ INTERRUPT LEVEL. Plain arithmetic plus two Cursor Device Manager traps; no
 * allocation, no File Manager, no Resource Manager. */
void BT_InjectMouse(short dx, short dy, unsigned char buttons, short wheel)
{
    unsigned char b = (unsigned char)(buttons & 0x07);

    gMouseReports++;
    /* ⚠ COUNTED, NOT INJECTED. Apple's own mouse module has no wheel path and the
     * Cursor Device Manager exposes no wheel call, so there is nothing to deliver it
     * through -- and the A1015, the only mouse in scope, has no wheel at all. Counting
     * it means a device that DOES send one shows up in the log instead of the field
     * being silently dropped and later rediscovered. */
    if (wheel != 0) gMouseWheelSeen++;

    if (gCurDev == 0) {
        /* No device yet: ask for the task-level hop that can make one, and drop this
         * report. BT_DeferRequest only ENQUEUES and coalesces, so calling it on every
         * dropped report is safe and costs one notification. */
        gMouseDropped++;
        gCurWant = 1;
        BT_DeferRequest();
        return;
    }

    /* ⚠ KEEP THE RETURN. These used to be discarded, and this project's own rule says
     * not to: if the cursor still does not move, "we called Move 800 times" and "Move
     * refused 800 times" look identical in a log, and telling them apart would cost a
     * whole hardware cycle. First nonzero only -- the rest would be the same error. */
    if (dx != 0 || dy != 0) {
        OSErr e;
        gMouseMoves++;
        e = CursorDeviceMove(gCurDev, (long)dx, (long)dy);
        if (e != noErr && gCurMoveErr == 0) gCurMoveErr = 0x10000L | (long)(short)e;
    }
    if (b != gCurButtons) {
        OSErr e;
        gMouseBtnChanges++;
        e = CursorDeviceButtons(gCurDev, (short)b);
        if (e != noErr && gCurBtnErr == 0) gCurBtnErr = 0x10000L | (long)(short)e;
        gCurButtons = b;
    }
}

/* ⚠ TASK LEVEL ONLY, from the defer response beside BT_MediaServiceAtTask.
 * CursorDeviceNewDevice allocates. */
void BT_MouseServiceAtTask(void)
{
    CursorDevicePtr dev = 0;

    if (!gCurWant || gCurDev != 0) return;
    gCurWant = 0;

    if (CursorDeviceNewDevice(&dev) != noErr || dev == 0) {
        gCurNewErr++;
        return;                       /* ⚠ fail open: gCurDev stays 0, reports keep
                                       * being dropped and counted, nothing crashes */
    }
    /* Apple's setup, same order AND -- as of v16.3 -- genuinely the same values.
     * Acceleration 1<<16 is Fixed 1.0, matching MouseModule.c; the Mouse control panel's
     * own tracking setting then applies on top.
     *
     * ⚠ KEEP THE RETURNS. These were all (void) and this project's own rule says not to
     * discard what the hardware answers. It matters more now that the resolution below
     * is load-bearing: a refused UnitsPerInch would leave OS 9 on its default and look
     * exactly like the bug we just fixed. */
    /* ★★★★★★ INHERIT THE MACHINE'S ACCELERATION INSTEAD OF HARDCODING ONE.
     *
     * MEASURED 2026-10-03 by walking every CursorDevice on the MDD: the two devices
     * that were already there read acceleration 0.50, and the only one at 1.0 was OURS
     * -- the constant copied from Apple's sample. 1.0 is what MouseModule.c uses at
     * CREATION; 0.50 is what this machine actually runs, because the Mouse control
     * panel's tracking preference has been applied to the devices that existed when the
     * user set it. A device created later, as ours is at every connect, never receives
     * it and is permanently the odd one out.
     *
     * So: copy it from a device that is already here. That makes the Bluetooth mouse
     * follow the Mouse control panel like every other pointer on the machine, which is
     * a better answer than any constant -- including the right constant, because the
     * user can change their mind and we would be wrong again.
     *
     * ⚠ SKIP OURSELVES. We are already in the list by this point; copying our own
     * freshly-defaulted value would be an elaborate way to write 1.0.
     * ⚠ FALLING BACK TO 1.0 WHEN ALONE IS CORRECT, NOT A CONSOLATION. There is no API
     * to read the machine's tracking preference -- CursorDevices.h exposes setters,
     * enumeration and the public struct, and nothing else -- so a Bluetooth mouse that
     * is the ONLY pointer cannot discover it, and neither can Apple: MouseModule.c
     * hardcodes 1.0 for every USB mouse. The Mouse control panel is what pushes the
     * user's choice onto devices afterwards, which is why the devices on this machine
     * read 0.50 when Apple's creation default is 1.0. So alone we start where every
     * other new pointer starts and the control panel corrects us like any of them;
     * inheritance is a nicety that saves the user touching the slider, not a
     * requirement.
     *
     * ⚠⚠ UNVERIFIED, AND SAY SO: WHICH ROW IN THE 99.96 DUMP WAS OURS IS NOT KNOWN.
     * Two devices read 400 u/in, cntButtons comes "from ADB reg 1" so it does not
     * reflect our SetButtons(3), and the commit that introduced this inheritance
     * asserted ours was the 1.0 one after the preceding message had flagged the
     * identification as impossible. That was an overreach.
     *     If ours was the 1.0 row  -> this change is the fix.
     *     If ours was the 0.50 row -> then the 1.0 device is the WIRED mouse, which the
     *                                 user calls perfect, 1.0 is fine, and the remaining
     *                                 sluggishness is NOT acceleration at all.
     * ✅ SETTLED 2026-10-03 by the marker, and NEITHER branch was right. The dump grew
     * from three devices to four: ours was the new one, so in the earlier dump NONE of
     * the three was ours -- the mouse had not connected when it was taken. We inherited
     * 0.50 correctly, cntButtons DOES reflect SetButtons (ours reads 3, so that earlier
     * doubt was also wrong), and the device still at 1.0 is simply one nobody has
     * adjusted since it was created.
     *
     * ⇒ The inheritance was the right change for a reason I had not actually
     * established. Worth keeping as a reminder that "the fix worked" is not evidence
     * that the reasoning behind it did. */
    {
        CursorDevicePtr scan = NULL;
        Fixed inherited = (Fixed)(1L << 16);
        short guard = 0;
        if (CursorDeviceNextDevice(&scan) == noErr) {
            while (scan != NULL && guard < 8) {
                if (scan != dev && scan->acceleration != 0) {
                    inherited = scan->acceleration;
                    break;
                }
                guard++;
                if (CursorDeviceNextDevice(&scan) != noErr) break;
            }
        }
        gCurAccelUsed = (long)inherited;
        gCurAccelErr  = (long)CursorDeviceSetAcceleration(dev, inherited);
    }
    gCurButtonsErr = (long)CursorDeviceSetButtons(dev, 3);
    (void)CursorDeviceButtonOp(dev, 0, kButtonSingleClick, 0L);
    (void)CursorDeviceButtonOp(dev, 1, kButtonSingleClick, 0L);
    (void)CursorDeviceButtonOp(dev, 2, kButtonSingleClick, 0L);

    /* ★★★★★★ 400, WHICH IS WHAT APPLE FEEDS EVERY USB MOUSE ON OS 9.
     *
     * Prior art, found 2026-10-03 and sitting in this repo the whole time:
     * usb-ddk/Examples/MouseModule/MouseModule.c:654-659 --
     *     myMousePB.unitsPerInch = (Fixed)(400<<16);     // the default
     *     ... (Fixed)(100<<16) for ONE Chicony device, 046e:6782
     * with CursorDeviceSetAcceleration(1<<16) and SetButtons(3) beside it. That is the
     * whole configuration, and it is the one to mirror.
     *
     * ⚠ THIS IS NOT THE SENSOR'S RESOLUTION AND IS NOT MEANT TO BE. A measured swipe
     * put the A1015 at 681 cpi (|dx| 6814 over 10 inches, |dy| 233, so a straight pull).
     * Reporting that true figure made small drags precise and large gestures SLUGGISH --
     * because the Cursor Device Manager divides deltas by this number to get speed, and
     * its curves are ADB-era: the struct documents `resolution` as coming "from ADB reg
     * 1". Apple feeds every USB mouse a conventional 400 regardless of its real sensor,
     * so 681 told the curve the pointer was moving at 59% of its true speed and it
     * stopped accelerating.
     *
     * Both failures are the same error in opposite directions:
     *     200  = half of 400   -> speed over-reported  -> over-accelerated, imprecise
     *     681  = 1.7x of 400   -> speed under-reported -> under-accelerated, sluggish
     *     400  = Apple's value -> behaves like every other mouse on the machine
     *
     * ⚠ AND 200 WAS NEVER APPLE'S NUMBER, despite the comment above once claiming "same
     * values". The order and the acceleration matched the sample; the resolution did
     * not, and nobody checked. */
    gCurUpiErr = (long)CursorDeviceUnitsPerInch(dev, (Fixed)(400L << 16));

    /* ⚠ PUBLISH LAST. Until this store, BT_InjectMouse at interrupt level sees no device
     * and drops reports -- which is correct. Storing the pointer before the device is
     * configured would let an interrupt use a half-set-up device. */
    gCurDevPtr = (long)dev;
    gCurDev = dev;
    gCurCreated++;
}

/* ⚠ TASK LEVEL, from ProbeFinalize. */
void BT_MouseFinalize(void)
{
    CursorDevicePtr dev = gCurDev;
    if (dev == 0) return;
    /* ⚠ CLEAR FIRST, then dispose. An interrupt arriving between the two must see 0 and
     * drop its report, never a pointer to a device being torn down -- the same ordering
     * rule as the CSM's icon suite and for the same reason. */
    gCurDev = 0;
    gCurButtons = 0;
    (void)CursorDeviceDisposeDevice(dev);
}
