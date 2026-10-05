/*
 *  bt_bootreport.c  --  the boot keyboard report decoder. Pure, and tested off-target.
 *
 *  ⚠ NO INCLUDES BEYOND ITS OWN HEADER, on purpose. tests/test_bootreport.c compiles
 *  this exact file with the HOST compiler, so the decoder is proven against known byte
 *  patterns before any keyboard exists to send them. Adding a dependency here breaks
 *  that and removes the only pre-hardware coverage M4 has.
 *
 *  ⚠ This file IS in scripts/level-audit.py's FILES list. It is pure arithmetic and
 *  cannot reach a forbidden primitive, but a source file that the audit does not walk
 *  is exactly the blind spot reference_static_audit_blind_spots warns about, and
 *  "obviously safe" is how a file stays unaudited until it isn't.
 */
#include "bt_bootreport.h"

int BT_DecodeBootKeyboard(const unsigned char *p, unsigned short len,
                          BTBootReport *out)
{
    short i, n = 0;
    short roll = 0;

    if (p == 0 || out == 0) return 0;

    /* Accept both the framed form and a bare report: which one arrives depends on
     * whether the caller has already stripped the HIDP header, and making the decoder
     * tolerant of both means the framing decision cannot silently break decoding. */
    if (len == 9 && p[0] == HIDP_DATA_INPUT) {
        p++;
        len--;
    }

    /* ⚠ EXACTLY 8, and nothing else. A decoder that half-fills its output from a short
     * packet is how a keystroke gets invented out of noise -- and on this stack a
     * malformed packet is a real possibility, not a hypothetical: the transport has
     * delivered truncated reads before. *out is left untouched on failure so a caller
     * that ignores the return value sees stale data rather than fabricated data. */
    if (len != 8) return 0;

    /* byte 1 is reserved. Deliberately NOT checked for zero: the spec reserves it, but
     * real keyboards have been observed putting OEM data there, and rejecting a report
     * over a byte we do not use would drop every keystroke from such a device. */

    for (i = 0; i < BT_BOOT_MAX_KEYS; i++) {
        unsigned char k = p[2 + i];

        if (k == 0x00) continue;        /* empty slot */

        /* ⚠⚠ 0x01..0x03 ARE NOT KEYS. 0x01 is ErrorRollOver, which a keyboard puts in
         * EVERY slot when more keys are held down than the boot report can carry; 0x02
         * is POSTFail and 0x03 ErrorUndefined. Treating 0x01 as a usage ID would inject
         * six phantom keypresses at precisely the moment the user is typing fastest,
         * which is both the worst time and the hardest to reproduce deliberately. */
        if (k <= 0x03) { roll = 1; continue; }

        out->keys[n++] = k;
    }

    /* Written only once the input is known good, and the tail is zeroed so a caller
     * reading keys[] beyond nKeys sees nothing rather than the previous report's. */
    out->mods = p[0];
    for (i = n; i < BT_BOOT_MAX_KEYS; i++) out->keys[i] = 0;
    out->nKeys    = n;
    out->rollover = roll;
    return 1;
}

/* ---- M5 step 1: report protocol, which is the Consumer page --------------------
 *
 * The 9-byte report's bytes 0..7 ARE the boot layout, so the key half is decoded by
 * the function above rather than duplicated here. Duplicating it would mean the
 * rollover rule (0x01..0x03 are not keys) had two homes, and the one that got fixed
 * would not be the one that ran. */
int BT_DecodeHidReport(const unsigned char *p, unsigned short len,
                       BTHidReport *out)
{
    BTBootReport      boot;
    const unsigned char *rep = 0;   /* the 9-byte report, once framing is stripped  */
    short             i;

    if (p == 0 || out == 0) return 0;

    /* ⚠ NO LONGER DISJOINT BY LENGTH, AND THAT CHANGED IN 14.0 -- read this before
     * adding a form. Two accepted forms are now 10 bytes long, so the table is disjoint
     * by length OR BY THE FRAMING BYTE. That is safe here, and only here, because in
     * both 10-byte forms byte 0 is STRUCTURALLY FIXED rather than data: 0x01 is a report
     * ID and 0xA1 is the HIDP framing constant, so neither can ever be the other. It
     * would NOT be safe for a form whose first byte is a modifier or a keycode, which is
     * exactly the ambiguity the header's note refuses a bare 9-byte report for. If you
     * add a form, say which of length or first byte separates it, and prove that byte
     * cannot be data.
     *
     * Checked most-specific first. */
    if (len == 11 && p[0] == HIDP_DATA_INPUT && p[1] == 0x01) {
        rep = p + 2;                     /* framed, with report ID          */
    } else if (len == 10 && p[0] == 0x01) {
        rep = p + 1;                     /* report ID only, header stripped */
    } else if ((len == 9 && p[0] == HIDP_DATA_INPUT) || len == 8
               || (len == 10 && p[0] == HIDP_DATA_INPUT && p[1] == 0x01)) {
        /* ★★★★★★ BOOT PROTOCOL, IN THREE FRAMINGS, AND THE THIRD COST A DEAD KEYBOARD.
         *
         * ⚠⚠ `A1 01` + 8 was measured off the wire on 2026-09-20 and REJECTED by every
         * branch of this table: too short for the framed report-protocol form (which
         * needs 9 payload bytes) and the wrong first byte for the bare one. BTCheck's own
         * log said so -- `first payload: A1 01 00 00 28 00 00 00 00 00`,
         * `reports accepted 0`, `⚠ Data arrived and the decoder REJECTED it` -- and 38
         * reports were thrown away in one session. 0x28 is Return. The keyboard connected,
         * reported itself live, and typed nothing.
         *
         * WHERE IT COMES FROM: driver 13.9's retry connects OUTGOING, and an outgoing
         * REPORT-mode connect sends no SET_PROTOCOL -- hid_host.c sets set_protocol for
         * BOOT mode only on that path (962, 992) and for REPORT only on the incoming one
         * (697). So the keyboard stays in boot protocol, an 8-byte payload, while the
         * channel still frames with the report ID. Framed with 8 bytes is the one
         * combination of {framed, bare} x {8, 9} this table never had.
         *
         * ⚠ A keyboard decoded through here has NO media keys, and that is the protocol's
         * doing rather than a defect: an 8-byte boot report has no Consumer page to carry
         * them. hasConsumer is set to 0 below, truthfully. Making the retry path request
         * REPORT protocol is a separate, larger change.
         *
         * Delegate wholesale after stripping whatever framing this form carries. */
        const unsigned char *bp = p;
        unsigned short       bn = len;
        if (len == 10) { bp = p + 2; bn = 8; }   /* strip the A1 01 */
        if (!BT_DecodeBootKeyboard(bp, bn, &boot)) return 0;
        out->mods = boot.mods;
        for (i = 0; i < BT_BOOT_MAX_KEYS; i++) out->keys[i] = boot.keys[i];
        out->nKeys       = boot.nKeys;
        out->rollover    = boot.rollover;
        out->consumer    = 0;
        out->consumerRaw = 0;
        out->hasConsumer = 0;
        return 1;
    } else {
        return 0;
    }

    /* Report protocol. Decode bytes 0..7 through the boot decoder by handing it the
     * bare 8-byte prefix -- the layouts are identical, which is the finding that made
     * M5 small. ⚠ Its `len != 8` guard is why this passes 8 and not 9: byte 8 is ours
     * to interpret and must not reach a decoder that would reject the whole report
     * for being one byte too long. */
    if (!BT_DecodeBootKeyboard(rep, 8, &boot)) return 0;

    out->mods = boot.mods;
    for (i = 0; i < BT_BOOT_MAX_KEYS; i++) out->keys[i] = boot.keys[i];
    out->nKeys       = boot.nKeys;
    out->rollover    = boot.rollover;
    out->consumerRaw = rep[8];
    out->consumer    = (unsigned char)(rep[8] & 0x0F);
    out->hasConsumer = 1;
    return 1;
}

/* ---- M5 step 2: USB HID usage -> Mac OS virtual keycode -------------------------
 *
 * ⭐ PROVENANCE, because a 256-entry table copied by eye is exactly how the A1016
 * descriptor decode nearly went wrong. This was EXTRACTED PROGRAMMATICALLY from
 * Apple's own OS 9 USB keyboard driver, usb-ddk/Examples/KeyboardModule/KeyIn.c,
 * table USBKMAP[256], and the extraction SELF-VALIDATED: every entry in that file
 * carries its own index as a comment, and all 256 were checked to be sequential
 * before a byte was emitted.
 *
 * ⚠⚠ AND THE VALIDATION EARNED ITS KEEP ON THE FIRST TRY. A naive parse silently
 * dropped two entries -- usage 0x25 is commented "8 *", whose literal asterisk ended
 * the comment early for a regex that excluded '*'. The table would have been SHIFTED
 * BY ONE from 0x25 onward: every letter after '8' would have typed the wrong
 * character, in a way no compiler could catch and only a human at the keyboard would
 * notice. 254 entries where 256 were expected is what caught it.
 *
 * The mapping itself is a factual correspondence between two published hardware
 * numbering schemes, not an algorithm. 0xFF means "no Mac virtual keycode", which
 * covers the unassigned usages AND the ones Mac OS has no key for.
 *
 * ⚠ Both left and right modifiers map to the SAME virtual keycode (control 0x3B,
 * shift 0x38, option 0x3A, command 0x37) because Mac OS 9's KeyMap has one bit per
 * modifier, not one per side. That is Apple's choice in this table, not a lossy
 * simplification of ours, and it means a report distinguishing left from right
 * cannot be round-tripped. Nothing in Mac OS 9 consumes that distinction.
 */
static const unsigned char kUsageToVirtualKey[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x0B, 0x08, 0x02,   /* 00-07 */
    0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25,   /* 08-0F */
    0x2E, 0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11,   /* 10-17 */
    0x20, 0x09, 0x0D, 0x07, 0x10, 0x06, 0x12, 0x13,   /* 18-1F */
    0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D,   /* 20-27 */
    0x24, 0x35, 0x33, 0x30, 0x31, 0x1B, 0x18, 0x21,   /* 28-2F */
    0x1E, 0x2A, 0xFF, 0x29, 0x27, 0x32, 0x2B, 0x2F,   /* 30-37 */
    0x2C, 0x39, 0x7A, 0x78, 0x63, 0x76, 0x60, 0x61,   /* 38-3F */
    0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F, 0x69, 0x6B,   /* 40-47 */
    0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x79, 0x7C,   /* 48-4F */
    0x7B, 0x7D, 0x7E, 0x47, 0x4B, 0x43, 0x4E, 0x45,   /* 50-57 */
    0x4C, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,   /* 58-5F */
    0x5B, 0x5C, 0x52, 0x41, 0xFF, 0x6E, 0x7F, 0x51,   /* 60-67 */
    0x69, 0x6B, 0x71, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 68-6F */
    0x5B, 0x5C, 0x52, 0x41, 0xFF, 0xFF, 0x7F, 0x4C,   /* 70-77 */
    0x69, 0x6B, 0x71, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 78-7F */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 80-87 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 88-8F */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 90-97 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 98-9F */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* A0-A7 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* A8-AF */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* B0-B7 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* B8-BF */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* C0-C7 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* C8-CF */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* D0-D7 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* D8-DF */
    0x3B, 0x38, 0x3A, 0x37, 0x3B, 0x38, 0x3A, 0x37,   /* E0-E7 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* E8-EF */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* F0-F7 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* F8-FF */
};

/* Returns the Mac OS virtual keycode for a USB HID Keyboard/Keypad usage, or
 * BT_VK_NONE (0xFF) when the usage has no Mac equivalent.
 *
 * ⚠ The caller MUST test for BT_VK_NONE. Apple's PostADBKeyToMac additionally drops
 * anything above 127 as "not handled by MacOS"; every value in this table is either
 * 0xFF or <= 0x7F, so that check is structurally satisfied here -- but do not rely on
 * that if the table is ever regenerated from a different source. */
unsigned char BT_UsageToVirtualKey(unsigned char usage)
{
    return kUsageToVirtualKey[usage];
}

/* ---- M5 step 2b: reports -> press/release events ------------------------------- */

void BT_KeyStateInit(BTKeyState *st)
{
    short i;
    if (st == 0) return;
    st->valid     = 0;
    st->prev.mods = 0;
    for (i = 0; i < BT_BOOT_MAX_KEYS; i++) st->prev.keys[i] = 0;
    st->prev.nKeys       = 0;
    st->prev.rollover    = 0;
    st->prev.consumer    = 0;
    st->prev.consumerRaw = 0;
    st->prev.hasConsumer = 0;
    st->capsOn      = 0;
    st->leds        = 0;
    st->ledsChanged = 0;
}

/* ⚠ Modifier usages are 0xE0..0xE7, and the LEFT/RIGHT pair for one modifier differs by
 * exactly 4: 0xE0 left control / 0xE4 right control, and so on. They map to the SAME Mac
 * virtual keycode, because Mac OS 9's KeyMap has one bit per modifier rather than one
 * per side -- see kUsageToVirtualKey.
 *
 * ⇒ SO RELEASING ONE WHILE THE OTHER IS STILL HELD MUST NOT POST A KEY-UP. It would tell
 * Mac OS that Shift is up while the user is still holding the other Shift, and the next
 * character would come out lowercase mid-word. Apple guards the same case in
 * IsModifierKeyAlreadyDown (KeyIn.c). */
static int TwinStillDown(unsigned char mods, short bit)
{
    short twin = (bit < 4) ? (short)(bit + 4) : (short)(bit - 4);
    return (mods & (1 << twin)) ? 1 : 0;
}

/* Is `usage` present in the first n entries of keys[]? The decoder compacts keys with
 * no gaps, so this is a plain membership test -- which is exactly what Apple's O(n^2)
 * cross-comparison amounts to once the array has no holes in it. */
static int KeyHeld(const unsigned char *keys, short n, unsigned char usage)
{
    short i;
    for (i = 0; i < n; i++) if (keys[i] == usage) return 1;
    return 0;
}

int BT_KeyEvents(BTKeyState *st, const BTHidReport *cur,
                 unsigned short *events, int maxEvents)
{
    int   n = 0;
    short i;
    unsigned char changed;

    if (st == 0 || cur == 0 || events == 0 || maxEvents <= 0) return 0;

    /* ---- modifiers, byte 0. Diffed even during rollover: it is a bitmap in its own
     * byte and stays meaningful when the key array does not. */
    changed = (unsigned char)(cur->mods ^ st->prev.mods);
    if (changed) {
        for (i = 0; i < 8; i++) {
            if ((changed & (1 << i)) == 0) continue;
            if (n >= maxEvents) break;
            /* ⚠ A RELEASE IS SUPPRESSED WHILE THE TWIN IS STILL HELD -- see
             * TwinStillDown. The PRESS is always reported: pressing the second Shift
             * while the first is down is harmless to report, but releasing one is not. */
            if ((cur->mods & (1 << i)) == 0 && TwinStillDown(cur->mods, i)) continue;
            /* Usages 0xE0..0xE7 are the eight modifiers, in bit order. */
            events[n++] = (unsigned short)(0xE0 + i) |
                          ((cur->mods & (1 << i)) ? 0 : BT_KEYEV_UP);
        }
        st->prev.mods = cur->mods;
    }

    /* ---- ⚠ ROLLOVER: leave the key half of the state ALONE and emit nothing for it.
     * See the header. Returning here rather than falling through is the whole point --
     * st->prev.keys keeps describing what is actually held. */
    if (cur->rollover) {
        st->valid = 1;
        return n;
    }

    /* ---- releases: in the previous set, absent from the current one. Emitted BEFORE
     * presses so that a key which moved slots -- or a chord released and re-pressed in
     * one report -- never produces a press the application sees before the matching
     * release of the same usage. */
    st->ledsChanged = 0;
    for (i = 0; i < st->prev.nKeys; i++) {
        if (n >= maxEvents) break;
        if (KeyHeld(cur->keys, cur->nKeys, st->prev.keys[i])) continue;
        /* ⚠⚠ CAPS LOCK'S RELEASE IS SWALLOWED ENTIRELY. The toggle was already
         * expressed when it was pressed; posting an up here would immediately undo it,
         * which is exactly what made Caps Lock do nothing on hardware. Apple returns
         * early on the same case. */
        if (st->prev.keys[i] == BT_USAGE_CAPSLOCK) continue;
        events[n++] = (unsigned short)st->prev.keys[i] | BT_KEYEV_UP;
    }

    /* ---- presses: in the current set, absent from the previous one. ⚠ On the first
     * report st->prev.nKeys is 0, so everything held at attach time is a press. */
    for (i = 0; i < cur->nKeys; i++) {
        if (n >= maxEvents) break;
        if (KeyHeld(st->prev.keys, st->prev.nKeys, cur->keys[i])) continue;
        if (cur->keys[i] == BT_USAGE_CAPSLOCK) {
            /* ⭐ THE TOGGLE, AND ITS DIRECTION IS THE NEW STATE rather than the physical
             * press. Caps Lock ON posts a DOWN and leaves it down; Caps Lock OFF posts
             * the UP. That is what makes the KeyMap bit -- which KeyTranslate reads to
             * decide case -- stay set for as long as the light is on. */
            st->capsOn = (short)(st->capsOn ? 0 : 1);
            st->leds = (unsigned char)(st->capsOn ? (st->leds | BT_LED_CAPSLOCK)
                                                  : (st->leds & ~BT_LED_CAPSLOCK));
            st->ledsChanged = 1;
            events[n++] = (unsigned short)BT_USAGE_CAPSLOCK |
                          (st->capsOn ? 0 : BT_KEYEV_UP);
            continue;
        }
        events[n++] = (unsigned short)cur->keys[i];
    }

    /* ⚠ COMMITTED ONLY HERE, and only for a non-rollover report. The tail is zeroed so
     * a later read past nKeys cannot see a ghost from an older, longer report. */
    for (i = 0; i < cur->nKeys; i++) st->prev.keys[i] = cur->keys[i];
    for (i = cur->nKeys; i < BT_BOOT_MAX_KEYS; i++) st->prev.keys[i] = 0;
    st->prev.nKeys = cur->nKeys;
    st->valid = 1;
    return n;
}

/* ---- the boot mouse ------------------------------------------------------------
 *
 * See bt_bootreport.h for the accepted forms, why length 4 is disambiguated by byte 0,
 * and why the 0xA1 0x02 five-byte form is refused rather than guessed at.
 */
int BT_DecodeMouseReport(const unsigned char *p, unsigned short len,
                         BTMouseReport *out)
{
    const unsigned char *r;
    short                payload;

    if (p == 0 || out == 0) return 0;

    if (len == 4 && p[0] == HIDP_DATA_INPUT) {
        r = p + 1; payload = 3;              /* framed, no wheel */
    } else if (len == 5 && p[0] == HIDP_DATA_INPUT && p[1] == 0x02) {
        /* ★★★★★ MEASURED 2026-09-22, and this branch used to REFUSE. The reasoning for
         * refusing was that 0x02 is also a legitimate button byte (right held), so a
         * report ID could not be told from data and the two readings put the deltas one
         * byte apart. Three pieces of evidence from the A1015's first real session
         * settle it, none of them available when that comment was written:
         *
         *  1. 767 of 767 reports arrived as 5 bytes with p[1] == 0x02 while the mouse
         *     was awake and STATIONARY. A button byte reads 0x00 at rest. It cannot be
         *     0x02 in every report of an idle session.
         *  2. The A1015 is a ONE-BUTTON mouse. It has no right button to hold.
         *  3. The A1016 on this same stack, in this same protocol mode, sends
         *     A1 01 + nine bytes -- HIDP header, REPORT ID, payload. Report 0x02 is the
         *     mouse's ID in exactly that convention.
         *
         * ⇒ A1 | 02 | buttons dx dy. No wheel, which matches a mouse that has none.
         * If this is somehow still wrong, kWMouseRidOther counts any 5-byte report
         * whose p[1] is NOT 0x02, and the dx/dy min-max rows say whether the deltas
         * landed in the right bytes -- the next log answers it without a guess. */
        r = p + 2; payload = 3;              /* report-ID framed, no wheel */
    } else if (len == 5 && p[0] == HIDP_DATA_INPUT) {
        r = p + 1; payload = 4;              /* framed, with wheel */
    } else if (len == 3) {
        r = p; payload = 3;                  /* bare */
    } else if (len == 4) {
        r = p; payload = 4;                  /* bare, with wheel */
    } else {
        return 0;
    }

    /* ⚠ SIGN-EXTEND EXPLICITLY. The deltas are signed 8-bit and these bytes are
     * unsigned char, so a plain assignment turns every leftward movement into a large
     * rightward one -- the cursor would only ever travel down and right. Casting via
     * signed char is the conversion the report format actually specifies. */
    out->buttons  = (unsigned char)(r[0] & 0x07);
    out->dx       = (short)(signed char)r[1];
    out->dy       = (short)(signed char)r[2];
    out->wheel    = (payload == 4) ? (short)(signed char)r[3] : 0;
    out->hasWheel = (short)(payload == 4);
    return 1;
}

/* ---- UTF-8 -> MacRoman -----------------------------------------------------------
 *
 * See the header for why this exists: Bluetooth names are UTF-8, DrawString is MacRoman,
 * and v14.8 stored the raw bytes so "fw800's keyboard" drew as "fw800,Aos keyboard".
 *
 * ⚠ The table is DELIBERATELY SMALL and covers what device names actually contain: the
 * Latin-1 accented letters, and the punctuation Apple's own default names use. It is not
 * a Unicode implementation and does not pretend to be -- anything outside it becomes a
 * visible '?', which says "this character did not survive" instead of implying the name
 * is corrupt.
 */
typedef struct { unsigned short cp; unsigned char mac; } BTUniMap;

static const BTUniMap kUniToMac[] = {
    /* Latin-1 letters, in MacRoman's own order for the high range */
    { 0x00C4, 0x80 }, { 0x00C5, 0x81 }, { 0x00C7, 0x82 }, { 0x00C9, 0x83 },
    { 0x00D1, 0x84 }, { 0x00D6, 0x85 }, { 0x00DC, 0x86 }, { 0x00E1, 0x87 },
    { 0x00E0, 0x88 }, { 0x00E2, 0x89 }, { 0x00E4, 0x8A }, { 0x00E3, 0x8B },
    { 0x00E5, 0x8C }, { 0x00E7, 0x8D }, { 0x00E9, 0x8E }, { 0x00E8, 0x8F },
    { 0x00EA, 0x90 }, { 0x00EB, 0x91 }, { 0x00ED, 0x92 }, { 0x00EC, 0x93 },
    { 0x00EE, 0x94 }, { 0x00EF, 0x95 }, { 0x00F1, 0x96 }, { 0x00F3, 0x97 },
    { 0x00F2, 0x98 }, { 0x00F4, 0x99 }, { 0x00F6, 0x9A }, { 0x00F5, 0x9B },
    { 0x00FA, 0x9C }, { 0x00F9, 0x9D }, { 0x00FB, 0x9E }, { 0x00FC, 0x9F },
    { 0x00DF, 0xA7 }, { 0x00B0, 0xA1 }, { 0x00E6, 0xBE }, { 0x00F8, 0xBF },
    { 0x00C6, 0xAE }, { 0x00D8, 0xAF },
    /* ⭐ The punctuation that caused this. Apple's default device names use a CURLY
     * apostrophe, and MacRoman has the real character -- 0xD5 -- so this is a faithful
     * mapping and not an ASCII downgrade. */
    { 0x2018, 0xD4 }, { 0x2019, 0xD5 },      /* ' '  */
    { 0x201C, 0xD2 }, { 0x201D, 0xD3 },      /* " "  */
    { 0x2013, 0xD0 }, { 0x2014, 0xD1 },      /* en/em dash */
    { 0x2026, 0xC9 },                        /* ellipsis   */
    { 0x00A0, 0x20 },                        /* nbsp -> plain space */
};
#define kUniMapCount ((int)(sizeof(kUniToMac) / sizeof(kUniToMac[0])))

int BT_Utf8ToMacRoman(const unsigned char *src, unsigned short srcLen,
                      unsigned char *dst, unsigned short dstMax)
{
    unsigned short i = 0, o = 0;

    if (src == 0 || dst == 0) return 0;

    while (i < srcLen && o < dstMax) {
        unsigned char  b = src[i];
        unsigned long  cp;
        unsigned short need;

        if (b < 0x80) { dst[o++] = b; i++; continue; }   /* plain ASCII */

        /* Decode one sequence. ⚠ A truncated or malformed sequence must not be
         * interpreted -- it is consumed as ONE byte and emitted as '?', so a bad stream
         * can neither loop forever nor swallow the rest of the name. */
        if      ((b & 0xE0) == 0xC0) { need = 1; cp = (unsigned long)(b & 0x1F); }
        else if ((b & 0xF0) == 0xE0) { need = 2; cp = (unsigned long)(b & 0x0F); }
        else if ((b & 0xF8) == 0xF0) { need = 3; cp = (unsigned long)(b & 0x07); }
        else                         { dst[o++] = '?'; i++; continue; }

        /* ⚠ The continuation bytes live at i+1 .. i+need, so a complete sequence needs
         * i+need <= srcLen-1. Written as the strict comparison rather than with an
         * srcLen-1 that would underflow when srcLen is 0. */
        if ((unsigned short)(i + need) >= srcLen) { dst[o++] = '?'; i++; continue; }
        {
            unsigned short k;
            int bad = 0;
            for (k = 1; k <= need; k++) {
                if ((src[i + k] & 0xC0) != 0x80) { bad = 1; break; }
                cp = (cp << 6) | (unsigned long)(src[i + k] & 0x3F);
            }
            if (bad) { dst[o++] = '?'; i++; continue; }
        }
        i = (unsigned short)(i + need + 1);

        /* Map it, or say plainly that it did not survive. */
        {
            int k, hit = -1;
            if (cp < 0x80) hit = -2;                      /* overlong: treat as ASCII */
            else for (k = 0; k < kUniMapCount; k++)
                if (kUniToMac[k].cp == (unsigned short)cp) { hit = k; break; }
            if (hit == -2)      dst[o++] = (unsigned char)cp;
            else if (hit >= 0)  dst[o++] = kUniToMac[hit].mac;
            else                dst[o++] = '?';
        }
    }
    return (int)o;
}
