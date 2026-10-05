/*
 *  test_keymap.c  --  host-compiled tests for the USB usage -> Mac virtual keycode table.
 *
 *  ⚠⚠ THE ORACLE IS DELIBERATELY INDEPENDENT OF THE TABLE'S SOURCE. The table was
 *  extracted programmatically from Apple's USBKMAP[256]; asserting it against that same
 *  extraction would only prove the generator ran. Every expectation below is written
 *  from the published USB HID usage IDs and the published Mac OS virtual keycodes --
 *  two separate documented numbering schemes -- so a fault in the extraction shows up
 *  as a disagreement rather than as a matching pair of errors.
 *  [[feedback_test_content_not_return_codes]] -- a data-path test needs an oracle.
 *
 *  ⭐ AND THERE IS A SPECIFIC REGRESSION GUARD AT USAGE 0x25. A naive parse of Apple's
 *  file dropped two entries, because 0x25's comment is "8 *" and the literal asterisk
 *  closed the comment early. That would have shifted the table by one from 0x25 to the
 *  end: every letter after '8' typing the wrong character, invisible to the compiler,
 *  and findable only by a human at a keyboard. The digits below pin that boundary.
 *
 *  Run:  tests/run-tests.sh
 */
#include "../src/bt_bootreport.h"
#include <stdio.h>

static int gFail = 0;

static void expect(unsigned char usage, unsigned char vk, const char *what)
{
    unsigned char got = BT_UsageToVirtualKey(usage);
    if (got != vk) {
        printf("  FAIL  usage 0x%02X -> 0x%02X, expected 0x%02X  (%s)\n",
               usage, got, vk, what);
        gFail++;
    } else {
        printf("  ok    usage 0x%02X -> 0x%02X  %s\n", usage, vk, what);
    }
}

int main(void)
{
    int i;

    printf("USB usage -> Mac virtual keycode\n");

    /* --- the alphabet, usages 0x04..0x1D in USB order ---------------------------
     * Mac virtual keycodes are famously NOT alphabetical; that scramble is the
     * whole reason a table exists, and it makes these strong assertions. */
    {
        static const unsigned char vk[26] = {
            0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, /* A..I */
            0x26, 0x28, 0x25, 0x2E, 0x2D, 0x1F, 0x23, 0x0C, 0x0F, /* J..R */
            0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06        /* S..Z */
        };
        char label[8];
        for (i = 0; i < 26; i++) {
            sprintf(label, "'%c'", 'A' + i);
            expect((unsigned char)(0x04 + i), vk[i], label);
        }
    }

    /* --- ⭐ THE DIGITS, which straddle the parse bug's boundary ------------------
     * USB orders these 1,2,...,9,0 starting at 0x1E. Usage 0x25 is '8' -- the entry
     * whose "8 *" comment broke the first extraction -- so 0x25/0x26/0x27 are the
     * three that would have been wrong, and the ones after them shifted. */
    expect(0x1E, 0x12, "'1'");
    expect(0x1F, 0x13, "'2'");
    expect(0x20, 0x14, "'3'");
    expect(0x21, 0x15, "'4'");
    expect(0x22, 0x17, "'5'");
    expect(0x23, 0x16, "'6'");
    expect(0x24, 0x1A, "'7'");
    expect(0x25, 0x1C, "'8'  ⭐ the entry the bad parse dropped");
    expect(0x26, 0x19, "'9'  ⭐ first entry that would have shifted");
    expect(0x27, 0x1D, "'0'");

    /* --- the keys that make a keyboard usable ---------------------------------- */
    expect(0x28, 0x24, "Return");
    expect(0x29, 0x35, "Escape");
    expect(0x2A, 0x33, "Delete/Backspace");
    expect(0x2B, 0x30, "Tab");
    expect(0x2C, 0x31, "Space");
    expect(0x2D, 0x1B, "'-'");
    expect(0x2E, 0x18, "'='");
    expect(0x2F, 0x21, "'['");
    expect(0x30, 0x1E, "']'");
    expect(0x33, 0x29, "';'");
    expect(0x34, 0x27, "'\\''");
    expect(0x36, 0x2B, "','");
    expect(0x37, 0x2F, "'.'");
    expect(0x38, 0x2C, "'/'");
    expect(0x39, 0x39, "Caps Lock");

    /* --- arrows, which a keyboard driver is judged on -------------------------- */
    expect(0x4F, 0x7C, "Right arrow");
    expect(0x50, 0x7B, "Left arrow");
    expect(0x51, 0x7D, "Down arrow");
    expect(0x52, 0x7E, "Up arrow");

    /* --- function keys, non-contiguous on the Mac side ------------------------- */
    expect(0x3A, 0x7A, "F1");
    expect(0x3B, 0x78, "F2");
    expect(0x3C, 0x63, "F3");
    expect(0x3D, 0x76, "F4");
    expect(0x3E, 0x60, "F5");
    expect(0x3F, 0x61, "F6");
    expect(0x40, 0x62, "F7");
    expect(0x41, 0x64, "F8");
    expect(0x42, 0x65, "F9");
    expect(0x43, 0x6D, "F10");
    expect(0x44, 0x67, "F11");
    expect(0x45, 0x6F, "F12");

    /* --- modifiers. ⚠ LEFT AND RIGHT SHARE A VIRTUAL KEYCODE, deliberately: Mac OS
     * 9's KeyMap has one bit per modifier, not one per side. Asserted so that a future
     * "fix" giving them distinct codes fails here and has to justify itself. */
    expect(0xE0, 0x3B, "left control");
    expect(0xE4, 0x3B, "RIGHT control -- same code as left, on purpose");
    expect(0xE1, 0x38, "left shift");
    expect(0xE5, 0x38, "RIGHT shift -- same code as left");
    expect(0xE2, 0x3A, "left option");
    expect(0xE6, 0x3A, "RIGHT option -- same code as left");
    expect(0xE3, 0x37, "left command");
    expect(0xE7, 0x37, "RIGHT command -- same code as left");

    /* --- ⚠ THE FOUR NON-KEYS. 0x00 is "no event" and 0x01..0x03 are ErrorRollOver,
     * POSTFail and ErrorUndefined. The decoder already filters them, but a table that
     * mapped them to real keys would inject six phantom keystrokes during fast typing
     * if anything ever bypassed the decoder. Defence in depth, cheaply. */
    expect(0x00, BT_VK_NONE, "no event");
    expect(0x01, BT_VK_NONE, "ErrorRollOver");
    expect(0x02, BT_VK_NONE, "POSTFail");
    expect(0x03, BT_VK_NONE, "ErrorUndefined");

    /* --- the structural invariant Apple's poster relies on ---------------------
     * PostADBKeyToMac drops anything above 127 as "not handled by MacOS". Every entry
     * must therefore be either BT_VK_NONE or <= 0x7F, or a usage would silently do
     * nothing after passing every check above. */
    {
        int bad = 0;
        for (i = 0; i < 256; i++) {
            unsigned char v = BT_UsageToVirtualKey((unsigned char)i);
            if (v != BT_VK_NONE && v > 0x7F) { bad++; printf("    0x%02X -> 0x%02X\n", i, v); }
        }
        if (bad) { printf("  FAIL  %d entries are >0x7F and not BT_VK_NONE\n", bad); gFail++; }
        else       printf("  ok    every entry is BT_VK_NONE or <= 0x7F\n");
    }

    /* --- and the table is populated at all, so an all-0xFF regression is caught -- */
    {
        int mapped = 0;
        for (i = 0; i < 256; i++)
            if (BT_UsageToVirtualKey((unsigned char)i) != BT_VK_NONE) mapped++;
        if (mapped != 118) {
            printf("  FAIL  %d usages mapped, expected 118\n", mapped);
            gFail++;
        } else {
            printf("  ok    118 usages mapped, 138 deliberately unmapped\n");
        }
    }

    printf(gFail ? "\nFAILED: %d\n" : "\nall passed\n", gFail);
    return gFail ? 1 : 0;
}
