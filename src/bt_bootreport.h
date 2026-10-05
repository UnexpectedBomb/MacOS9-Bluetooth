/*
 *  bt_bootreport.h  --  HID-over-Bluetooth framing constants and the boot keyboard
 *                      report decoder.
 *
 *  ⚠⚠ DELIBERATELY DEPENDENCY-FREE. No BTstack, no USB.h, no Toolbox, no Retro68
 *  headers -- nothing but plain C types. That is the whole point: this is the ONE part
 *  of M4 that can be compiled and proven with the HOST compiler before the A1016
 *  keyboard is on the desk (tests/test_bootreport.c). Every other part of the HID path
 *  needs a keyboard to exercise, and this project has spent a week measuring what
 *  untested code costs.
 *
 *  Do not add an include here. If this file ever needs one, the test stops being
 *  runnable off-target and the reason for its existence is gone.
 */
#ifndef OS9BT_BOOTREPORT_H
#define OS9BT_BOOTREPORT_H

/* ---- HID over BR/EDR transaction framing (HIDP) ---------------------------------
 * Every control-channel message is one header byte:
 *
 *     header = (transaction_type << 4) | parameter
 *
 * so SET_PROTOCOL selecting boot protocol is 0x70, and a HANDSHAKE reply is
 * 0x00 | result. Reports arrive on the INTERRUPT channel as DATA transactions,
 * 0xA0 | report_type, input reports therefore being 0xA1. */
#define HIDP_TRANS_HANDSHAKE     0x0
#define HIDP_TRANS_HID_CONTROL   0x1
#define HIDP_TRANS_GET_REPORT    0x4
#define HIDP_TRANS_SET_REPORT    0x5
#define HIDP_TRANS_GET_PROTOCOL  0x6
#define HIDP_TRANS_SET_PROTOCOL  0x7
#define HIDP_TRANS_DATA          0xA

#define HIDP_PROTOCOL_BOOT       0x0
#define HIDP_PROTOCOL_REPORT     0x1
#define HIDP_REPORT_TYPE_INPUT   0x1

/* HANDSHAKE result codes, carried in the header's low nibble. */
#define HIDP_HS_SUCCESSFUL       0x0
#define HIDP_HS_NOT_READY        0x1
#define HIDP_HS_ERR_INVALID_ID   0x2
#define HIDP_HS_ERR_UNSUPPORTED  0x3
#define HIDP_HS_ERR_INVALID_PARM 0x4
#define HIDP_HS_ERR_UNKNOWN      0xE
#define HIDP_HS_ERR_FATAL        0xF

/* The composed bytes, so callers never open-code the shift. */
#define HIDP_HDR(trans, parm)    ((unsigned char)(((trans) << 4) | ((parm) & 0x0F)))
#define HIDP_SET_PROTOCOL_BOOT   HIDP_HDR(HIDP_TRANS_SET_PROTOCOL, HIDP_PROTOCOL_BOOT)
/* ★★★★★ 0x71 -- AND THIS ONE BYTE IS THE WHOLE CONSUMER-PAGE QUESTION.
 *
 * HIDP_PROTOCOL_REPORT has been defined above since M4 was designed and never used,
 * because M4 deliberately aimed at BOOT protocol -- it was trying to reproduce what
 * the card's proxy already does. That was the right target then and it is the wrong
 * one now.
 *
 * ⭐ REPORT protocol is where everything the proxy cannot carry lives: the Consumer
 * page (volume, eject), and the battery-strength feature report on the Generic Device
 * page. A keyboard in boot protocol sends a fixed 8-byte report drawing only on
 * Keyboard/Keypad (0x07); the SAME keyboard in report protocol sends its native
 * reports. Nothing about the hardware changes -- only which of its two personalities
 * it is speaking.
 *
 * ⇒ So the media keys and the battery level are ONE transaction apart, not one
 * milestone apart, ONCE a HID control channel is open. See docs/M4-DESIGN.md for what
 * still blocks that channel and why the previous diagnosis of it was wrong. */
#define HIDP_SET_PROTOCOL_REPORT HIDP_HDR(HIDP_TRANS_SET_PROTOCOL, HIDP_PROTOCOL_REPORT)
#define HIDP_DATA_INPUT          HIDP_HDR(HIDP_TRANS_DATA, HIDP_REPORT_TYPE_INPUT)

/* ---- the decoded boot keyboard report ------------------------------------------ */
#define BT_BOOT_MAX_KEYS 6

/* Modifier bits, byte 0 of the report. */
#define BT_MOD_LCTRL   0x01
#define BT_MOD_LSHIFT  0x02
#define BT_MOD_LALT    0x04
#define BT_MOD_LGUI    0x08
#define BT_MOD_RCTRL   0x10
#define BT_MOD_RSHIFT  0x20
#define BT_MOD_RALT    0x40
#define BT_MOD_RGUI    0x80

typedef struct {
    unsigned char mods;                      /* modifier bitmap, byte 0        */
    unsigned char keys[BT_BOOT_MAX_KEYS];    /* usage IDs, compacted, no gaps  */
    short         nKeys;                     /* how many of keys[] are valid   */
    short         rollover;                  /* ⚠ too many keys held; see .c   */
} BTBootReport;

/* Returns 1 and fills *out on a well-formed report; returns 0 and leaves *out
 * UNTOUCHED on anything malformed. Accepts either the framed form (0xA1 + 8 bytes)
 * or a bare 8-byte report. */
int BT_DecodeBootKeyboard(const unsigned char *p, unsigned short len,
                          BTBootReport *out);

/* ================================================================================
 *  M5 step 1: THE REPORT-PROTOCOL DECODER, which is the Consumer page.
 * ================================================================================
 *
 * ⭐ MEASURED 2026-09-10, driver v10.0, and this is not a design from the descriptor
 * any more -- it is the wire:
 *
 *     first payload: A1 01 00 00 00 00 00 00 00 00 00      (11 bytes)
 *
 * A1 = HIDP DATA/input, 01 = REPORT ID 1, then NINE bytes. Tiger's capture of the four
 * keys is the same 11 bytes with byte 8 set (docs/TIGER-HCI-TRACE.md):
 *
 *     a1 01 00 00 00 00 00 00 00 00 04   VOLUME UP
 *     a1 01 00 00 00 00 00 00 00 00 08   VOLUME DOWN
 *     a1 01 00 00 00 00 00 00 00 00 02   MUTE
 *     a1 01 00 00 00 00 00 00 00 00 01   EJECT
 *
 * ⇒ Bytes 0..7 of the 9-byte report are EXACTLY the boot layout -- modifiers,
 * reserved, six keycodes -- and byte 8 is the whole Consumer page. So this decoder is
 * the boot decoder plus one byte, which is what docs/A1016-REPORT-DESCRIPTOR.md
 * predicted and why M5 needs no HID descriptor parser for this device.
 *
 * ⚠ FIXED LAYOUT MEANS THIS DEVICE ONLY. A different keyboard has a different
 * descriptor. Hardcoding is correct for a keyboard-specific driver and wrong for a
 * general one; this project is building the former.
 */
#define BT_CONSUMER_EJECT   0x01   /* bit 0 -- ⚠ RELATIVE in the descriptor (81 06) */
#define BT_CONSUMER_MUTE    0x02   /* bit 1 -- ⚠ RELATIVE                          */
#define BT_CONSUMER_VOL_UP  0x04   /* bit 2 -- absolute (81 02)                    */
#define BT_CONSUMER_VOL_DN  0x08   /* bit 3 -- absolute                            */

/* ⚠⚠ RELATIVE vs ABSOLUTE IS A REAL DIFFERENCE AND IT IS STILL UNMEASURED.
 *
 * The descriptor marks Eject and Mute RELATIVE (81 06) and the two volume keys
 * ABSOLUTE (81 02). An absolute bit reports HELD STATE, so press is a 0->1 edge and
 * release is 1->0. A relative bit reports a TRANSITION, so the device is expected to
 * set it for one report and clear it by itself.
 *
 * ⇒ WHAT WE DO NOT KNOW: whether the A1016 actually clears the relative bits. Tiger's
 * capture shows the four PRESS reports and no releases, because that is all that was
 * extracted from it -- absence of a release in the doc is not evidence of absence on
 * the wire.
 *
 * ⇒ SO THIS DECODER TAKES NO POSITION. It hands back the raw nibble and the caller
 * decides. Getting it wrong in here would bake a guess into the one component that is
 * provable off-target, and two of the four keys would misbehave in a way that looks
 * like a radio problem. The driver carries a byte-8 history ring so ONE hardware run
 * settles it with real data. */

typedef struct {
    /* Identical names and meanings to BTBootReport, so a caller can handle both
     * personalities of this keyboard through one code path. */
    unsigned char mods;
    unsigned char keys[BT_BOOT_MAX_KEYS];
    short         nKeys;
    short         rollover;
    /* The Consumer page. `consumer` is masked to the four declared bits. */
    unsigned char consumer;
    /* ⚠ THE UNMASKED BYTE, kept deliberately. Bits 4..7 are declared CONSTANT padding
     * (75 01 95 04 / 81 01). If this ever differs from `consumer` then the descriptor
     * decode in docs/A1016-REPORT-DESCRIPTOR.md is wrong about the padding, and that
     * is worth knowing loudly rather than silently masking away. */
    unsigned char consumerRaw;
    short         hasConsumer;   /* 1 = this report carried byte 8 at all */
} BTHidReport;

/* ONE ENTRY POINT FOR BOTH PERSONALITIES. Returns 1 and fills *out; returns 0 and
 * leaves *out UNTOUCHED on anything malformed. Accepted forms, disjoint by length so
 * no report can be misclassified:
 *
 *     11 bytes  0xA1 0x01 + 9   report protocol, HIDP-framed   hasConsumer = 1
 *     10 bytes  0x01 + 9        report protocol, header stripped
 *      9 bytes  0xA1 + 8        BOOT, HIDP-framed              hasConsumer = 0
 *      8 bytes  bare 8          BOOT                           hasConsumer = 0
 *
 * ⚠ THE REPORT ID IS REQUIRED for the report-protocol forms, and that is not
 * pedantry: a bare 9-byte report-protocol report would be indistinguishable from the
 * framed boot form (0xA1 + 8), since a modifier byte of 0xA1 is legal. The descriptor
 * declares Report ID 1, so every report-protocol report carries it, and demanding it
 * removes the ambiguity instead of guessing at it. */
int BT_DecodeHidReport(const unsigned char *p, unsigned short len,
                       BTHidReport *out);

/* ================================================================================
 *  M5 step 2: USB HID usage -> Mac OS virtual keycode.
 * ================================================================================
 *
 * ⭐⭐ AND M4-DESIGN §9's PREMISE IS RETRACTED BY THIS. It recorded that Apple's OS X
 * driver hands a report descriptor to the OS's HID family and lets it parse and
 * inject, that OS 9 has no equivalent provider API, and therefore that "M5 had to be
 * solved without prior art".
 *
 * ⇒ THE FIRST TWO CLAUSES ARE TRUE AND THE CONCLUSION IS FALSE. Apple's own OS 9 USB
 * keyboard driver does not use a provider model either -- it translates and posts, by
 * hand, and the whole recipe ships in usb-ddk/Examples/KeyboardModule:
 *
 *     USB interrupt-IN completion
 *       -> KeyboardCompletionProc            KeyboardModule.c:266, case
 *                                            kReadInterruptPipe at :378
 *         -> NotifyRegisteredHIDUser         KBDHIDEmulation.c:168, diffs the report
 *                                            against the previous one and emits
 *                                            press/release usages
 *           -> USBDemoKeyIn                  KeyIn.c:644
 *             -> PostUSBKeyToMac             KeyIn.c:367, usage -> virtual keycode
 *               -> PostADBKeyToMac           KeyIn.c:452
 *                    LMSetKeyMap / LMSetKbdLast / LMSetKbdType
 *                    KeyTranslate(KCHRptr, ...)
 *                    PostEvent(keyDown|keyUp, event)
 *
 * ⚠⚠ NOTE THE EXECUTION LEVEL, which is the fact that makes this usable: Apple calls
 * KeyTranslate and PostEvent from a USB COMPLETION ROUTINE -- secondary interrupt
 * level. Verified by reading the call graph rather than assuming: KeyboardCompletionProc
 * is the ONLY function defined between KeyboardModule.c:260 and :400, so :378 is
 * unambiguously inside it. Our own HID reports arrive at the same level.
 *
 * ⚠ The one task-level dependency is the KCHR resource. Apple acquires it once in
 * InitUSBKeyboard (KeyIn.c:355) with GetResource + HLock and caches the dereferenced
 * pointer, so the Resource Manager is never touched from interrupt level. Our
 * equivalent is ProbeInitialize, which bt_probe.c:3366 documents as "task level -- the
 * Expert's first call into us"; bt_defer.c is the fallback if that ever changes.
 */
#define BT_VK_NONE 0xFF

unsigned char BT_UsageToVirtualKey(unsigned char usage);

/* ---- M5 step 2b: reports -> press/release events -------------------------------
 *
 * A HID keyboard reports STATE, not events: "these keys are down now". Turning that
 * into keyDown/keyUp means diffing against the previous report, and the algorithm is
 * Apple's from KBDHIDEmulation.c:168 with its two non-obvious rules kept intact.
 *
 * ⚠⚠ RULE 1: THE KEY DIFF IS A SET COMPARISON, NOT SLOT BY SLOT. Apple's comment
 * cites the HID spec appendix C, pp. 73-74: "The order of keycodes in array fields has
 * no significance [...] If two or more keys are pressed in one report, their order is
 * indeterminate." A positional diff would therefore report spurious presses and
 * releases whenever a keyboard reshuffled its array -- while typing quickly, which is
 * both the worst time and the hardest to reproduce on purpose.
 *
 * ⚠⚠ RULE 2: DURING ROLLOVER THE KEY DIFF IS SKIPPED ENTIRELY. When more keys are held
 * than the report can carry, the keyboard fills every slot with ErrorRollOver (0x01).
 * Diffing that would emit a release for every held key, then a press for every one of
 * them again when the rollover cleared -- a burst of phantom keystrokes at exactly the
 * moment the user is typing fastest. Modifiers ARE still diffed, because byte 0 is a
 * bitmap independent of the key array and stays valid.
 *
 * ⇒ AND THAT IS WHY THE STATE IS OWNED HERE rather than left to the caller. Rule 2
 * means the remembered report must NOT be updated from a rollover report, or the held
 * set is lost and the next ordinary report emits the phantom burst anyway. A caller
 * that did `prev = cur` unconditionally would reintroduce the bug the rule exists to
 * prevent, so no caller is given the chance.
 */
#define BT_KEYEV_UP        0x8000   /* OR'd into an event to mark a release */
/* Apple's worst case, from its own comment: 4 modifiers up + 4 down + 6 keys up +
 * 6 down. Sized to it so a full-hand release cannot silently drop events. */
#define BT_MAX_KEY_EVENTS  20

/* Usage of the Caps Lock key, which this layer treats specially -- see BT_KeyEvents. */
#define BT_USAGE_CAPSLOCK  0x39

/* LED bits in the A1016's 8-bit OUTPUT report, from its own descriptor
 * (docs/A1016-REPORT-DESCRIPTOR.md): five LEDs, NumLock first. */
#define BT_LED_NUMLOCK     0x01
#define BT_LED_CAPSLOCK    0x02
#define BT_LED_SCROLLLOCK  0x04

typedef struct {
    BTHidReport prev;
    short       valid;      /* 0 until the first report has been seen */
    /* ⚠ CAPS LOCK IS A TOGGLE, NOT A KEY, and treating it as a key is why it did
     * nothing on hardware: we posted press-then-release, so Mac OS saw it engage and
     * instantly disengage. Apple's PostUSBKeyToMac (KeyIn.c:395) keeps the toggle
     * itself, remaps the PRESS to a down-or-up according to the new state, and SWALLOWS
     * the release entirely. This is that state. */
    short       capsOn;
    /* The LED bitmap as last computed. The caller sends it to the keyboard when it
     * changes; this layer only decides what it should be. */
    unsigned char leds;
    short       ledsChanged;   /* 1 when the last call altered `leds` */
} BTKeyState;

/* Zero a state block. Equivalent to memset 0, but named so the intent is visible and
 * so a caller need not include <string.h> in driver context. */
void BT_KeyStateInit(BTKeyState *st);

/* Diff `cur` against the remembered report, writing up to maxEvents events and
 * returning how many were written. Each event is a USB usage, OR'd with BT_KEYEV_UP
 * for a release. Updates the remembered report per the rules above.
 *
 * ⚠ On the FIRST report (valid == 0) the previous state is taken as empty, so keys
 * already held when we attach are reported as presses. That is deliberate: the
 * alternative is a key that is down at connect time never generating anything and
 * never being releasable. */
int BT_KeyEvents(BTKeyState *st, const BTHidReport *cur,
                 unsigned short *events, int maxEvents);

/* ================================================================================
 *  THE BOOT MOUSE, for the A1015.
 * ================================================================================
 *
 * HID boot protocol 2 (Mouse) is a three-byte report: a button bitmap, then a signed
 * X delta, then a signed Y delta. A fourth signed byte carries a wheel when the device
 * has one. Deltas are RELATIVE, which is what the Cursor Device Manager wants.
 *
 * ⚠⚠ THE A1015'S ACTUAL WIRE FORMAT IS UNMEASURED. No mouse has ever reported to this
 * stack, so the forms below are the STANDARD ones rather than observed ones -- and the
 * keyboard taught this project exactly what that is worth: five framings of a
 * *documented* report existed, and the fifth (`A1 01` + 8) was rejected by every branch
 * of the keyboard table until it was measured off the wire, costing a session in which
 * 38 reports were thrown away while the keyboard reported itself live and typed nothing.
 *
 * ⇒ So this decoder ACCEPTS ONLY UNAMBIGUOUS FORMS AND THE CALLER RECORDS THE REST.
 * A rejected report's length and leading bytes go into the counter block, so ONE
 * hardware run says what the A1015 really sends instead of another round of guessing.
 *
 * Accepted:
 *     3 bytes  buttons dx dy            bare boot report
 *     4 bytes  0xA1 + buttons dx dy     HIDP-framed boot report
 *     4 bytes  buttons dx dy wheel      bare, with a wheel
 *     5 bytes  0xA1 + buttons dx dy wheel     (when byte 1 is NOT 0x02)
 *     5 bytes  0xA1 0x02 + buttons dx dy      ★ THE A1015, measured 2026-09-22
 *
 * ⚠ LENGTH 4 IS TWO DIFFERENT FORMS and the discriminator is byte 0 == 0xA1. That is
 * safe for the same structural reason the keyboard table relies on: in a BOOT mouse
 * report the button byte's bits 3..7 are declared padding and are zero, so 0xA1
 * (buttons 0, 5 and 7) cannot be a legitimate button byte. If a device is ever seen
 * setting those bits, this assumption breaks and the counters will show it.
 *
 * ★ THE 0xA1 0x02 FORM WAS REFUSED UNTIL IT WAS MEASURED, and that was the right call
 * at the time: it could be a report ID of 2 followed by a 3-byte payload, OR a framed
 * 4-byte payload whose button byte is 0x02 (the right button held). The two readings
 * put the deltas one byte apart, so guessing wrong would fling the cursor on every
 * right-click. The A1015's first session resolved it: 767 of 767 reports carried
 * 0xA1 0x02 while the mouse was awake and STATIONARY -- a button byte reads 0x00 at
 * rest -- the A1015 has no right button to hold, and the A1016 on this same stack
 * sends 0xA1 0x01 + nine, which is the same report-ID convention with the keyboard's
 * ID. It is now accepted as report ID 2 + buttons/dx/dy, and the driver counts any
 * 5-byte report whose byte 1 is NOT 0x02 so the assumption stays under measurement.
 */
typedef struct {
    short         dx, dy;      /* signed, as sent; relative */
    short         wheel;       /* signed; 0 when the form carries none */
    unsigned char buttons;     /* bit0 left, bit1 right, bit2 middle */
    short         hasWheel;    /* 1 = this report carried a wheel byte */
} BTMouseReport;

/* Returns 1 and fills *out; returns 0 and leaves *out UNTOUCHED on anything this
 * decoder will not commit to. Same contract as the keyboard decoders, for the same
 * reason: a caller that ignores the result sees stale data, never invented data. */
int BT_DecodeMouseReport(const unsigned char *p, unsigned short len,
                         BTMouseReport *out);

/* ================================================================================
 *  UTF-8 -> MacRoman, for device names.
 * ================================================================================
 *
 * ⚠⚠ THE BLUETOOTH SPEC SAYS DEVICE NAMES ARE UTF-8. OS 9's DrawString renders
 * MacRoman. Driver 14.8 stored the controller's bytes verbatim, so the A1016's own name
 * -- "fw800's keyboard", with a U+2019 curly apostrophe -- reached the screen as
 * "fw800,Aos keyboard": the three UTF-8 bytes E2 80 99 drawn as three MacRoman glyphs.
 * Not a file-format fault and not a transport fault; a conversion that was never done.
 *
 * ⭐ ONE CONVERSION, AT THE POINT OF STORAGE, so the counter block, the names file, the
 * control panel and the CSM all hold text OS 9 can draw. Converting in each consumer
 * would be three chances to forget.
 *
 * ⚠ MacRoman HAS the characters that matter, so this is a real mapping rather than a
 * strip: U+2019 -> 0xD5 is a proper right single quote, not an ASCII substitute.
 *
 * ⚠ AN UNMAPPED CODE POINT BECOMES '?', NEVER A RAW BYTE. Passing an unconvertible byte
 * through is exactly what produced the mojibake -- a visible '?' says "this character did
 * not survive", which is honest, where a garbage glyph implies the name itself is broken.
 *
 * ⚠ The output is never longer than the input (multi-byte in, single-byte out), so an
 * in-place conversion can never overflow the source buffer. dstMax is still honoured.
 *
 * Returns the number of bytes written; does NOT NUL-terminate (the caller knows its
 * buffer). Pure arithmetic and a table: safe at interrupt level, and tested off-target
 * in tests/test_utf8.c against the bytes measured on hardware. */
int BT_Utf8ToMacRoman(const unsigned char *src, unsigned short srcLen,
                      unsigned char *dst, unsigned short dstMax);

#endif /* OS9BT_BOOTREPORT_H */
