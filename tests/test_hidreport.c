/*
 *  test_hidreport.c  --  host-compiled tests for the REPORT-PROTOCOL decoder (M5 step 1).
 *
 *  ⚠ WHY THIS EXISTS, and it is the same reason as test_bootreport.c but sharper now.
 *  The Consumer page is the goal this project has chased since M4, and the decoder is
 *  the ONE part of M5 that is pure arithmetic -- so it is the one part that can be
 *  proven before it ever reaches a machine that costs a reboot per attempt and has no
 *  debugger. Eleven builds went into getting these bytes to arrive at all; none of them
 *  should be spent finding out that the decoder drops them.
 *
 *  ⭐ THE FIXTURES ARE REAL WIRE BYTES, not invented patterns:
 *    - the all-zero report is what driver v10.0 actually captured
 *      (docs/source/btcheck-v99.19-*.log, "first payload: A1 01 00 ...")
 *    - the four key reports are Tiger's, on the same card and keyboard
 *      (docs/TIGER-HCI-TRACE.md)
 *
 *  Run:  tests/run-tests.sh
 */
#include "../src/bt_bootreport.h"
#include <stdio.h>
#include <string.h>

static int gFail = 0;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL  %s\n", what); gFail++; }
    else       { printf("  ok    %s\n", what); }
}

/* Pre-filled with junk so "left UNTOUCHED on failure" is testable rather than
 * asserted. Same trick as the boot report tests. */
static BTHidReport poisoned(void)
{
    BTHidReport r;
    memset(&r, 0xEE, sizeof(r));
    return r;
}

/* One Consumer key, framed exactly as the wire carries it. */
static void consumerCase(unsigned char byte8, unsigned char expect, const char *what)
{
    unsigned char rep[11] = { 0xA1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    BTHidReport r = poisoned();
    char label[96];

    rep[10] = byte8;
    sprintf(label, "%s (byte 8 = 0x%02X)", what, byte8);
    check(BT_DecodeHidReport(rep, 11, &r) == 1 &&
          r.hasConsumer == 1 && r.consumer == expect && r.nKeys == 0 && r.mods == 0,
          label);
}

int main(void)
{
    printf("report-protocol decoder (M5 step 1)\n");

    /* --- the bit assignments, straight from the descriptor ---------------------- */
    check(BT_CONSUMER_EJECT  == 0x01, "Eject is bit 0");
    check(BT_CONSUMER_MUTE   == 0x02, "Mute is bit 1");
    check(BT_CONSUMER_VOL_UP == 0x04, "Volume Up is bit 2");
    check(BT_CONSUMER_VOL_DN == 0x08, "Volume Down is bit 3");

    /* --- ⭐ THE EXACT REPORT v10.0 CAPTURED ON HARDWARE ------------------------- */
    {
        unsigned char rep[11] = { 0xA1, 0x01, 0,0,0,0,0,0,0,0, 0x00 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(rep, 11, &r) == 1 &&
              r.mods == 0 && r.nKeys == 0 && r.rollover == 0 &&
              r.consumer == 0 && r.consumerRaw == 0 && r.hasConsumer == 1,
              "v10.0's captured idle report decodes, hasConsumer 1");
    }

    /* --- ⭐ TIGER'S FOUR KEYS, byte for byte ------------------------------------ */
    consumerCase(0x04, BT_CONSUMER_VOL_UP, "Tiger's VOLUME UP");
    consumerCase(0x08, BT_CONSUMER_VOL_DN, "Tiger's VOLUME DOWN");
    consumerCase(0x02, BT_CONSUMER_MUTE,   "Tiger's MUTE");
    consumerCase(0x01, BT_CONSUMER_EJECT,  "Tiger's EJECT");

    /* --- keys and a Consumer bit in the SAME report ----------------------------- */
    {
        /* Return (0x28) held with left shift, and volume up at the same time. */
        unsigned char rep[11] = { 0xA1, 0x01, BT_MOD_LSHIFT, 0x00,
                                  0x28, 0, 0, 0, 0, 0, 0x04 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(rep, 11, &r) == 1 &&
              r.mods == BT_MOD_LSHIFT && r.nKeys == 1 && r.keys[0] == 0x28 &&
              r.consumer == BT_CONSUMER_VOL_UP,
              "keys and a Consumer bit coexist in one report");
    }

    /* --- two Consumer bits at once --------------------------------------------- */
    consumerCase(0x0C, (unsigned char)(BT_CONSUMER_VOL_UP | BT_CONSUMER_VOL_DN),
                 "both volume bits");

    /* --- ⚠ THE PADDING NIBBLE. Bits 4..7 are declared CONSTANT; if a device ever
     * sets them, consumerRaw must preserve that fact while consumer masks it, so the
     * descriptor decode can be caught being wrong instead of silently papered over. */
    {
        unsigned char rep[11] = { 0xA1, 0x01, 0,0,0,0,0,0,0,0, 0xF4 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(rep, 11, &r) == 1 &&
              r.consumer == BT_CONSUMER_VOL_UP && r.consumerRaw == 0xF4,
              "high nibble masked out of consumer but kept in consumerRaw");
    }

    /* --- the header-stripped 10-byte form -------------------------------------- */
    {
        unsigned char rep[10] = { 0x01, 0,0,0,0,0,0,0,0, 0x02 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(rep, 10, &r) == 1 &&
              r.consumer == BT_CONSUMER_MUTE && r.hasConsumer == 1,
              "10-byte form (report ID, no HIDP header)");
    }

    /* --- BOOT protocol still works through the same entry point ---------------- */
    {
        unsigned char framed[9] = { 0xA1, BT_MOD_LCTRL, 0x00, 0x04, 0,0,0,0,0 };
        unsigned char bare[8]   = { BT_MOD_RALT, 0x00, 0x05, 0,0,0,0,0 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(framed, 9, &r) == 1 &&
              r.mods == BT_MOD_LCTRL && r.nKeys == 1 && r.keys[0] == 0x04 &&
              r.hasConsumer == 0 && r.consumer == 0 && r.consumerRaw == 0,
              "9-byte framed BOOT report, hasConsumer 0");
        r = poisoned();
        check(BT_DecodeHidReport(bare, 8, &r) == 1 &&
              r.mods == BT_MOD_RALT && r.nKeys == 1 && r.keys[0] == 0x05 &&
              r.hasConsumer == 0,
              "8-byte bare BOOT report, hasConsumer 0");
    }

    /* --- ⚠⚠ THE AMBIGUITY THAT DROVE THE DESIGN --------------------------------
     * A 9-byte buffer beginning 0xA1 is the framed BOOT form. It must NOT be read as a
     * bare report-protocol report, because a modifier byte of 0xA1 is perfectly legal
     * and the two would be indistinguishable. This is why the report-protocol forms
     * REQUIRE the report ID.
     *
     * ⭐ AND THE ASSERTION IS SHARPER THAN "hasConsumer == 0". The trailing 0x0F sits
     * in KEYCODE SLOT 5 under the boot layout, so a correct decode reports it as the
     * key 'l' -- TWO keys held. Reading the same byte as a Consumer nibble would
     * instead yield one key and all four media bits set at once. The two readings give
     * visibly different output, which is what makes this a test rather than a
     * restatement of the code. (My first draft of this fixture asserted nKeys == 1 and
     * the test correctly failed.) */
    {
        unsigned char nine[9] = { 0xA1, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0x0F };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(nine, 9, &r) == 1 && r.hasConsumer == 0 &&
              r.consumer == 0 && r.consumerRaw == 0 &&
              r.nKeys == 2 && r.keys[0] == 0x04 && r.keys[1] == 0x0F,
              "9 bytes starting 0xA1 is BOOT: byte 8 is a KEYCODE, not a Consumer bit");
    }

    /* --- rollover propagates through the report-protocol path ------------------ */
    {
        /* 0x01 in every slot is ErrorRollOver, not six keypresses. */
        unsigned char rep[11] = { 0xA1, 0x01, 0x00, 0x00,
                                  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x04 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(rep, 11, &r) == 1 &&
              r.rollover == 1 && r.nKeys == 0 &&
              r.consumer == BT_CONSUMER_VOL_UP,
              "ErrorRollOver rejected as keys, Consumer bit still read");
    }

    /* --- rejections, each leaving *out UNTOUCHED -------------------------------- */
    {
        struct { unsigned char b[16]; unsigned short len; const char *why; } bad[] = {
            { { 0xA1, 0x02, 0,0,0,0,0,0,0,0,0 }, 11, "11 bytes with the WRONG report ID" },
            { { 0xA2, 0x01, 0,0,0,0,0,0,0,0,0 }, 11, "11 bytes with a non-DATA header" },
            { { 0x02, 0,0,0,0,0,0,0,0,0 },       10, "10 bytes with the wrong report ID" },
            /* ⚠⚠ REMOVED IN 14.0, AND IT WAS A CORRECT RULE ABOUT A WRONG PREMISE.
             * This table used to assert that `A1 01` + 8 at len 10 must be REFUSED --
             * "10 bytes that are really framed" -- on the reasoning that a framed
             * report-protocol report needs 9 payload bytes, so a framed 8 is malformed.
             * Defensible, and contradicted by the hardware: the A1016 sends exactly that
             * shape when it is in BOOT protocol with report-ID framing, which is what the
             * 13.9 retry path leaves it in. Measured 2026-09-20, 38 reports, all rejected,
             * keyboard silent. The acceptance is asserted at the end of this file against
             * the literal wire bytes, and the "len 10 with a first byte that is neither
             * 0x01 nor 0xA1" case there keeps the strictness this entry was protecting.
             * A test that encodes an assumption about a DEVICE is only as good as the
             * assumption. */
            { { 0xA1, 0x01, 0,0,0,0,0,0,0,0,0,0 }, 12, "12 bytes (one too many)"        },
            { { 0xA1, 0x01, 0,0,0,0,0,0,0 },      7, "7 bytes (short)"                 },
            { { 0 },                              0, "zero length"                     },
            { { 0xA1 },                           1, "one byte"                        },
        };
        unsigned i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            BTHidReport r = poisoned();
            BTHidReport save = r;
            check(BT_DecodeHidReport(bad[i].b, bad[i].len, &r) == 0 &&
                  memcmp(&save, &r, sizeof(r)) == 0, bad[i].why);
        }
    }

    /* --- null arguments -------------------------------------------------------- */
    {
        BTHidReport r = poisoned();
        unsigned char rep[11] = { 0xA1, 0x01, 0,0,0,0,0,0,0,0,0 };
        check(BT_DecodeHidReport(0, 11, &r) == 0,  "null pointer rejected");
        check(BT_DecodeHidReport(rep, 11, 0) == 0, "null output rejected");
    }

    /* --- the tail of keys[] is zeroed, not left from the previous report -------- */
    {
        unsigned char many[11] = { 0xA1, 0x01, 0x00, 0x00,
                                   0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x00 };
        unsigned char one[11]  = { 0xA1, 0x01, 0x00, 0x00,
                                   0x04, 0, 0, 0, 0, 0, 0x00 };
        BTHidReport r = poisoned();
        short k, clean = 1;
        check(BT_DecodeHidReport(many, 11, &r) == 1 && r.nKeys == 6, "six keys held");
        check(BT_DecodeHidReport(one, 11, &r) == 1 && r.nKeys == 1, "then one key");
        for (k = r.nKeys; k < BT_BOOT_MAX_KEYS; k++) if (r.keys[k] != 0) clean = 0;
        check(clean, "keys[] tail zeroed, no ghosts from the previous report");
    }

    /* ★★★★★★ THE FRAMED BOOT FORM: A1 01 + 8. MEASURED OFF THE WIRE, NOT INVENTED.
     *
     * ⚠⚠ These are the literal bytes from BTCheck v99.71's 2026-09-20 run (banked as
     * logs/BTCheck_v99.71_2026-09-20_GATE1_REPASS_*.log):
     *
     *     first payload bytes 10
     *     first payload: A1 01 00 00 28 00 00 00 00 00
     *     reports accepted 0
     *     ⚠ Data arrived and the decoder REJECTED it.
     *
     * THIRTY-EIGHT reports arrived that session and the decoder threw away every one,
     * so the keyboard connected, showed as live, and typed nothing. 0x28 is Return --
     * the user's keypress at the Keychain prompt.
     *
     * WHY THIS SHAPE EXISTS: the 13.9 retry connects OUTGOING, and an outgoing
     * REPORT-mode connect sends no SET_PROTOCOL (hid_host.c sets set_protocol for BOOT
     * only on that path). So the keyboard stays in boot protocol -- an 8-byte payload --
     * while the channel still frames with the report ID. Framed, 8 bytes: the one
     * combination of {framed, bare} x {8, 9} the table never had.
     *
     * ⚠ The log was read TWICE before anyone looked at these rows. The warning was
     * printed both times. */
    {
        unsigned char ret[10] = { 0xA1, 0x01, 0x00, 0x00,
                                  0x28, 0x00, 0x00, 0x00, 0x00, 0x00 };
        unsigned char mod[10] = { 0xA1, 0x01, 0x02, 0x00,
                                  0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(ret, 10, &r) == 1,
              "framed boot A1 01 + 8 is accepted (the 09-20 wire bytes)");
        check(r.nKeys == 1 && r.keys[0] == 0x28,
              "  and it decodes as Return (0x28)");
        check(r.hasConsumer == 0,
              "  with NO consumer data -- boot protocol has no Consumer page");
        r = poisoned();
        check(BT_DecodeHidReport(mod, 10, &r) == 1 && r.mods == 0x02 && r.keys[0] == 0x04,
              "framed boot carries modifiers too (shift+a)");
    }

    /* ⚠⚠ AND THE AMBIGUITY THE HEADER WARNS ABOUT MUST STAY CLOSED. Two forms are now
     * 10 bytes long, so the table is no longer disjoint by length alone -- it is
     * disjoint by the FIRST BYTE, and that is only safe because in both forms byte 0 is
     * structurally fixed rather than data: 0x01 is a report ID, 0xA1 is a framing
     * constant, and neither can be the other. These two cases assert exactly that. */
    {
        unsigned char bare[10] = { 0x01, 0x00, 0x00,
                                   0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        unsigned char junk[10] = { 0x55, 0x01, 0x00, 0x00,
                                   0x28, 0x00, 0x00, 0x00, 0x00, 0x00 };
        BTHidReport r = poisoned();
        check(BT_DecodeHidReport(bare, 10, &r) == 1,
              "bare 01 + 9 at len 10 still decodes (not shadowed by the new form)");
        r = poisoned();
        check(BT_DecodeHidReport(junk, 10, &r) == 0,
              "len 10 with a first byte that is neither 0x01 nor 0xA1 is REFUSED");
    }

    printf(gFail ? "\nFAILED: %d\n" : "\nall passed\n", gFail);
    return gFail ? 1 : 0;
}
