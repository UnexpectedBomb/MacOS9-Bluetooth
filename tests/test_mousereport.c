/*
 *  test_mousereport.c -- BT_DecodeMouseReport, proven with the HOST compiler.
 *
 *  ⚠ The A1015's real wire format is UNMEASURED (no mouse has ever reported to this
 *  stack), so these cases are the STANDARD boot-mouse forms rather than observed ones.
 *  That is exactly why the decoder refuses ambiguous shapes instead of guessing: the
 *  keyboard's fifth framing was rejected by every branch of its table until it was
 *  measured, and 38 reports were thrown away in one session while the keyboard reported
 *  itself live and typed nothing. These tests pin the forms we DO commit to, and the
 *  refusals we deliberately make, so a hardware run can only surprise us in one
 *  direction: a form we do not yet accept, which the counters will name.
 */
#include <stdio.h>
#include <string.h>
#include "../src/bt_bootreport.h"

static int fails = 0;

static void ok(int cond, const char *what)
{
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

static void expect(const unsigned char *p, unsigned short len, int want,
                   int dx, int dy, int buttons, int wheel, int hasWheel,
                   const char *what)
{
    BTMouseReport m;
    int got;
    memset(&m, 0xEE, sizeof(m));
    got = BT_DecodeMouseReport(p, len, &m);
    if (got != want) {
        printf("  FAIL %s: decode returned %d, wanted %d\n", what, got, want);
        fails++;
        return;
    }
    if (!want) { printf("  ok   %s (refused, as intended)\n", what); return; }
    if (m.dx != dx || m.dy != dy || m.buttons != buttons
        || m.wheel != wheel || m.hasWheel != hasWheel) {
        printf("  FAIL %s: dx=%d dy=%d btn=%02X wheel=%d hasWheel=%d "
               "(wanted %d %d %02X %d %d)\n", what,
               m.dx, m.dy, m.buttons, m.wheel, m.hasWheel,
               dx, dy, buttons, wheel, hasWheel);
        fails++;
        return;
    }
    printf("  ok   %s\n", what);
}

int main(void)
{
    printf("=== accepted forms ===\n");
    {
        /* bare 3: buttons dx dy */
        unsigned char r[3] = { 0x00, 0x05, 0x03 };
        expect(r, 3, 1, 5, 3, 0x00, 0, 0, "bare 3, move right+down");
    }
    {
        /* framed 4: A1 + buttons dx dy */
        unsigned char r[4] = { 0xA1, 0x01, 0x0A, 0xF6 };
        expect(r, 4, 1, 10, -10, 0x01, 0, 0,
               "framed 4, left button held, right+UP");
    }
    {
        /* bare 4: buttons dx dy wheel */
        unsigned char r[4] = { 0x02, 0x00, 0x00, 0x01 };
        expect(r, 4, 1, 0, 0, 0x02, 1, 1, "bare 4, right button + wheel up");
    }
    {
        /* framed 5: A1 + buttons dx dy wheel */
        unsigned char r[5] = { 0xA1, 0x04, 0xFF, 0xFF, 0xFF };
        expect(r, 5, 1, -1, -1, 0x04, -1, 1,
               "framed 5, middle button, up-left, wheel down");
    }

    printf("\n=== SIGN EXTENSION, the bug that would only move down-right ===\n");
    {
        unsigned char r[3] = { 0x00, 0x80, 0x80 };
        expect(r, 3, 1, -128, -128, 0x00, 0, 0, "0x80 is -128, not +128");
    }
    {
        unsigned char r[3] = { 0x00, 0x7F, 0x7F };
        expect(r, 3, 1, 127, 127, 0x00, 0, 0, "0x7F is +127");
    }

    printf("\n=== ★ THE A1015'S REAL FORM, measured 2026-09-22 ===\n");
    {
        /* This was REFUSED until the mouse's first session named it: 767 of 767 reports
         * came in as 0xA1 0x02 + three while the mouse was awake and STATIONARY. A
         * button byte reads 0x00 at rest, the A1015 has no right button to hold, and
         * the A1016 sends 0xA1 0x01 + nine on this same stack -- the same report-ID
         * convention. So 0x02 is the mouse's REPORT ID and the payload follows it.
         *
         * ⚠ These cases pin the BYTE POSITIONS, which is the half that can silently
         * regress: read one byte to the left and dx picks up the button bits. */
        unsigned char r[5] = { 0xA1, 0x02, 0x00, 0x10, 0x20 };
        expect(r, 5, 1, 0x10, 0x20, 0, 0, 0, "A1 02: payload starts at byte 2");
    }
    {
        unsigned char r[5] = { 0xA1, 0x02, 0x01, 0xF0, 0xFF };
        expect(r, 5, 1, -16, -1, 1, 0, 0, "A1 02: left button, and both deltas negative");
    }
    {
        /* The report actually seen at rest, all 767 of them. */
        unsigned char r[5] = { 0xA1, 0x02, 0x00, 0x00, 0x00 };
        expect(r, 5, 1, 0, 0, 0, 0, 0, "A1 02: the at-rest report decodes to no motion");
    }

    printf("\n=== deliberate refusals ===\n");
    {
        /* ⚠ A 5-byte 0xA1 report whose second byte is NOT the mouse's report ID is
         * still the older framed reading, with a wheel. Kept so the commitment above is
         * narrow: it applies to id 0x02, not to every 5-byte report. */
        unsigned char r[5] = { 0xA1, 0x01, 0x10, 0x20, 0x01 };
        expect(r, 5, 1, 0x10, 0x20, 1, 1, 1, "A1 01 + 4 is still the framed wheel form");
    }
    {
        unsigned char r[2] = { 0x00, 0x01 };
        expect(r, 2, 0, 0, 0, 0, 0, 0, "2 bytes is too short");
    }
    {
        unsigned char r[8] = { 0 };
        expect(r, 8, 0, 0, 0, 0, 0, 0, "8 bytes is a keyboard report, not a mouse");
    }
    {
        BTMouseReport m;
        ok(BT_DecodeMouseReport(NULL, 3, &m) == 0, "NULL report is refused");
        ok(BT_DecodeMouseReport((const unsigned char *)"\0\0\0", 3, NULL) == 0,
           "NULL output is refused");
    }

    printf("\n=== *out is untouched on refusal (no invented input) ===\n");
    {
        BTMouseReport m;
        unsigned char r[2] = { 0x00, 0x01 };
        memset(&m, 0, sizeof(m));
        m.dx = 4242;
        (void)BT_DecodeMouseReport(r, 2, &m);
        ok(m.dx == 4242, "a refused report leaves stale data, not zeroed data");
    }

    printf("\n=== button mask is confined to the three declared bits ===\n");
    {
        /* Padding bits set: a boot mouse should not do this, but if one does we must
         * not hand phantom buttons to the Cursor Device Manager. */
        unsigned char r[3] = { 0xFF, 0x00, 0x00 };
        expect(r, 3, 1, 0, 0, 0x07, 0, 0, "0xFF masks down to the low three bits");
    }

    if (fails) { printf("\n%d FAILURE(S)\n", fails); return 1; }
    printf("\nall mouse decoder tests passed\n");
    return 0;
}
