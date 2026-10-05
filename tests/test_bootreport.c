/*
 *  test_bootreport.c  --  host-compiled tests for the boot keyboard report decoder.
 *
 *  ⚠ WHY THIS EXISTS. Every other part of M4 needs an A1016 keyboard on the desk, a
 *  reboot per attempt and no debugger. The decoder is the one piece that is pure
 *  arithmetic, so it is the one piece that can be PROVEN before the hardware arrives --
 *  and this project has spent a week learning the cost of shipping untested code to a
 *  machine that can only be examined through a log file.
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

/* A report struct pre-filled with junk, so "left untouched on failure" is testable
 * rather than merely asserted. */
static BTBootReport poisoned(void)
{
    BTBootReport r;
    memset(&r, 0xEE, sizeof(r));
    return r;
}

int main(void)
{
    printf("boot report decoder\n");

    /* --- a bare 8-byte report: 'a' with left shift ------------------------------ */
    {
        unsigned char rep[8] = { BT_MOD_LSHIFT, 0x00, 0x04, 0, 0, 0, 0, 0 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 8, &r) == 1, "bare 8-byte report accepted");
        check(r.mods == BT_MOD_LSHIFT,                "modifier decoded");
        check(r.nKeys == 1,                           "one key");
        check(r.keys[0] == 0x04,                      "keycode 0x04");
        check(r.rollover == 0,                        "no rollover");
        check(r.keys[1] == 0 && r.keys[5] == 0,       "unused slots zeroed");
    }

    /* --- the FRAMED form, 0xA1 + 8 -------------------------------------------- */
    {
        unsigned char rep[9] = { HIDP_DATA_INPUT, 0x00, 0x00, 0x05, 0, 0, 0, 0, 0 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 9, &r) == 1, "framed 0xA1 report accepted");
        check(r.nKeys == 1 && r.keys[0] == 0x05,      "framed payload decoded");
    }

    /* --- gaps are COMPACTED, not preserved ------------------------------------ */
    {
        unsigned char rep[8] = { 0, 0, 0x04, 0x00, 0x06, 0x00, 0x08, 0x00 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 8, &r) == 1, "sparse slots accepted");
        check(r.nKeys == 3,                           "three keys found");
        check(r.keys[0] == 0x04 && r.keys[1] == 0x06 && r.keys[2] == 0x08,
                                                      "keys compacted in order");
    }

    /* --- all six slots, every modifier ---------------------------------------- */
    {
        unsigned char rep[8] = { 0xFF, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 8, &r) == 1, "six keys accepted");
        check(r.nKeys == 6,                           "all six decoded");
        check(r.mods == 0xFF,                         "all modifiers set");
    }

    /* --- ⚠ ROLLOVER must not become six phantom keypresses -------------------- */
    {
        unsigned char rep[8] = { 0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 8, &r) == 1, "rollover report accepted");
        check(r.nKeys == 0,                           "rollover yields NO keys");
        check(r.rollover == 1,                        "rollover flagged");
    }

    /* --- POSTFail / ErrorUndefined are likewise not keys ---------------------- */
    {
        unsigned char rep[8] = { 0, 0, 0x02, 0x03, 0x04, 0, 0, 0 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 8, &r) == 1, "0x02/0x03 report accepted");
        check(r.nKeys == 1 && r.keys[0] == 0x04,      "only the real key survives");
        check(r.rollover == 1,                        "error codes flag rollover");
    }

    /* --- reserved byte 1 is IGNORED, not validated ---------------------------- */
    {
        unsigned char rep[8] = { 0, 0x5A, 0x04, 0, 0, 0, 0, 0 };
        BTBootReport r = poisoned();
        check(BT_DecodeBootKeyboard(rep, 8, &r) == 1, "non-zero reserved byte accepted");
        check(r.nKeys == 1,                           "key still decoded");
    }

    /* --- malformed input leaves *out UNTOUCHED -------------------------------- */
    {
        unsigned char rep[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
        BTBootReport r;
        unsigned char save[sizeof(BTBootReport)];
        short len;
        static const short bad[] = { 0, 1, 7, 9, 10, 64 };
        unsigned i;
        for (i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
            char label[64];
            len = bad[i];
            /* len 9 with a bare (unframed) first byte must also be rejected. */
            r = poisoned();
            memcpy(save, &r, sizeof(r));
            sprintf(label, "len %d rejected and *out untouched", len);
            check(BT_DecodeBootKeyboard(rep, (unsigned short)len, &r) == 0 &&
                  memcmp(save, &r, sizeof(r)) == 0, label);
        }
    }

    /* --- null arguments ------------------------------------------------------- */
    {
        BTBootReport r = poisoned();
        unsigned char rep[8] = {0};
        check(BT_DecodeBootKeyboard(0, 8, &r) == 0,   "null pointer rejected");
        check(BT_DecodeBootKeyboard(rep, 8, 0) == 0,  "null output rejected");
    }

    /* --- the composed header constants match the spec ------------------------- */
    check(HIDP_SET_PROTOCOL_BOOT == 0x70, "SET_PROTOCOL(boot) is 0x70");
    check(HIDP_DATA_INPUT        == 0xA1, "DATA(input) is 0xA1");

    printf(gFail ? "\nFAILED: %d\n" : "\nall passed\n", gFail);
    return gFail ? 1 : 0;
}
