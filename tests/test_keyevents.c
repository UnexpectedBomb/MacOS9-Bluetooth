/*
 *  test_keyevents.c  --  host tests for the report -> press/release diff (M5 step 2b).
 *
 *  ⚠ WHY THIS IS THE MOST IMPORTANT OF THE THREE SUITES. The decoder either accepts a
 *  report or does not, and a wrong answer is loud. This state machine can be subtly
 *  wrong and still look fine: it will type, mostly, and then emit a phantom keystroke
 *  under a condition the user cannot reproduce on demand -- fast typing, a chord, a
 *  key held across a rollover. Those are the exact cases below, because they are the
 *  ones that would otherwise be found by a person losing work to them.
 *
 *  Run:  tests/run-tests.sh
 */
#include "../src/bt_bootreport.h"
#include <stdio.h>

static int gFail = 0;

static void check(int cond, const char *what)
{
    if (!cond) { printf("  FAIL  %s\n", what); gFail++; }
    else       { printf("  ok    %s\n", what); }
}

/* Build a report directly rather than through the decoder: these tests are about the
 * diff, and going through the decoder would make a decoder bug look like a diff bug. */
static BTHidReport rep(unsigned char mods, const unsigned char *keys, short n,
                       short rollover)
{
    BTHidReport r;
    short i;
    r.mods = mods;
    for (i = 0; i < BT_BOOT_MAX_KEYS; i++) r.keys[i] = (i < n) ? keys[i] : 0;
    r.nKeys = n; r.rollover = rollover;
    r.consumer = 0; r.consumerRaw = 0; r.hasConsumer = 0;
    return r;
}

static int has(const unsigned short *ev, int n, unsigned short want)
{
    int i;
    for (i = 0; i < n; i++) if (ev[i] == want) return 1;
    return 0;
}

int main(void)
{
    unsigned short ev[BT_MAX_KEY_EVENTS];
    BTKeyState st;
    int n;

    printf("report -> press/release diff\n");

    /* --- a key goes down, stays down, comes up --------------------------------- */
    {
        unsigned char a[1] = { 0x04 };            /* 'a' */
        BTHidReport down = rep(0, a, 1, 0);
        BTHidReport up   = rep(0, 0, 0, 0);

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &down, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == 0x04, "first report: 'a' down");

        n = BT_KeyEvents(&st, &down, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "held: the SAME report produces nothing");

        n = BT_KeyEvents(&st, &up, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == (0x04 | BT_KEYEV_UP), "'a' up");

        n = BT_KeyEvents(&st, &up, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "idle: nothing again");
    }

    /* --- ⭐⭐ SLOT REORDERING MUST PRODUCE NOTHING ------------------------------
     * The HID spec says array order is indeterminate, so a keyboard may report the same
     * two keys in either order between reports. A positional diff would emit a release
     * and a press for both keys -- doubling characters while the user types a chord.
     * This is the single most important assertion in the file. */
    {
        unsigned char ab[2] = { 0x04, 0x05 };
        unsigned char ba[2] = { 0x05, 0x04 };
        BTHidReport r1 = rep(0, ab, 2, 0);
        BTHidReport r2 = rep(0, ba, 2, 0);

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &r1, ev, BT_MAX_KEY_EVENTS);
        check(n == 2, "two keys down together");
        n = BT_KeyEvents(&st, &r2, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "⭐ SAME keys in the OTHER slot order produce NO events");
    }

    /* --- one of two released, order independent -------------------------------- */
    {
        unsigned char ab[2] = { 0x04, 0x05 };
        unsigned char b[1]  = { 0x05 };
        BTHidReport r1 = rep(0, ab, 2, 0);
        BTHidReport r2 = rep(0, b, 1, 0);

        BT_KeyStateInit(&st);
        (void)BT_KeyEvents(&st, &r1, ev, BT_MAX_KEY_EVENTS);
        n = BT_KeyEvents(&st, &r2, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == (0x04 | BT_KEYEV_UP),
              "releasing one of two reports only that one");
    }

    /* --- swap: one key replaced by another in a single report ------------------ */
    {
        unsigned char a[1] = { 0x04 };
        unsigned char b[1] = { 0x05 };
        BTHidReport r1 = rep(0, a, 1, 0);
        BTHidReport r2 = rep(0, b, 1, 0);

        BT_KeyStateInit(&st);
        (void)BT_KeyEvents(&st, &r1, ev, BT_MAX_KEY_EVENTS);
        n = BT_KeyEvents(&st, &r2, ev, BT_MAX_KEY_EVENTS);
        check(n == 2 && has(ev, n, 0x04 | BT_KEYEV_UP) && has(ev, n, 0x05),
              "a swap yields one release and one press");
        /* ⚠ ORDER MATTERS: the release must come first, so an application never sees
         * the new key arrive before the old one has gone. */
        check(ev[0] == (0x04 | BT_KEYEV_UP) && ev[1] == 0x05,
              "⭐ the RELEASE is emitted before the press");
    }

    /* --- modifiers ------------------------------------------------------------- */
    {
        BTHidReport none  = rep(0, 0, 0, 0);
        BTHidReport shift = rep(BT_MOD_LSHIFT, 0, 0, 0);
        BTHidReport both  = rep((unsigned char)(BT_MOD_LSHIFT | BT_MOD_RGUI), 0, 0, 0);

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &shift, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == 0xE1, "left shift down is usage 0xE1");
        n = BT_KeyEvents(&st, &both, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == 0xE7, "adding right command is usage 0xE7");
        n = BT_KeyEvents(&st, &none, ev, BT_MAX_KEY_EVENTS);
        check(n == 2 && has(ev, n, 0xE1 | BT_KEYEV_UP) && has(ev, n, 0xE7 | BT_KEYEV_UP),
              "releasing both modifiers reports both");
    }

    /* --- ⭐⭐ ROLLOVER: NO PHANTOM BURST ----------------------------------------
     * With more keys held than the report can carry, every slot reads ErrorRollOver and
     * the decoder yields nKeys 0 with rollover set. Diffing that naively would release
     * every held key, then press them all again when the rollover cleared: a burst of
     * phantom keystrokes exactly while the user is typing fastest. */
    {
        unsigned char abc[3] = { 0x04, 0x05, 0x06 };
        BTHidReport held = rep(0, abc, 3, 0);
        BTHidReport roll = rep(0, 0, 0, 1);        /* nKeys 0, rollover set */

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &held, ev, BT_MAX_KEY_EVENTS);
        check(n == 3, "three keys down");

        n = BT_KeyEvents(&st, &roll, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "⭐ a ROLLOVER report emits NO key events");

        /* And the state must have been preserved, so the keys still being held do not
         * come back as fresh presses when the rollover clears. */
        n = BT_KeyEvents(&st, &held, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "⭐⭐ after rollover clears, the held keys are NOT re-pressed");

        /* Releasing for real still works afterwards. */
        {
            BTHidReport up = rep(0, 0, 0, 0);
            n = BT_KeyEvents(&st, &up, ev, BT_MAX_KEY_EVENTS);
            check(n == 3, "and a real release afterwards still reports all three");
        }
    }

    /* --- modifiers ARE still diffed during rollover ----------------------------- */
    {
        unsigned char a[1] = { 0x04 };
        BTHidReport held = rep(0, a, 1, 0);
        BTHidReport roll = rep(BT_MOD_LSHIFT, 0, 0, 1);

        BT_KeyStateInit(&st);
        (void)BT_KeyEvents(&st, &held, ev, BT_MAX_KEY_EVENTS);
        n = BT_KeyEvents(&st, &roll, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == 0xE1,
              "a modifier pressed DURING rollover is still reported");
    }

    /* --- keys already held at attach time are reported as presses --------------- */
    {
        unsigned char a[1] = { 0x04 };
        BTHidReport held = rep(BT_MOD_LCTRL, a, 1, 0);
        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &held, ev, BT_MAX_KEY_EVENTS);
        check(n == 2 && has(ev, n, 0xE0) && has(ev, n, 0x04),
              "first report reports what is already held, key and modifier");
    }

    /* --- the event buffer is never overrun -------------------------------------- */
    {
        unsigned char six[6] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
        BTHidReport all = rep(0xFF, six, 6, 0);   /* 8 modifiers + 6 keys = 14 events */
        unsigned short small[3];
        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &all, small, 3);
        check(n == 3, "a small buffer clamps rather than overruns");

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &all, ev, BT_MAX_KEY_EVENTS);
        check(n == 14, "8 modifiers and 6 keys fit in BT_MAX_KEY_EVENTS");
    }

    /* --- null arguments --------------------------------------------------------- */
    {
        BTHidReport r = rep(0, 0, 0, 0);
        BT_KeyStateInit(&st);
        check(BT_KeyEvents(0, &r, ev, BT_MAX_KEY_EVENTS) == 0, "null state rejected");
        check(BT_KeyEvents(&st, 0, ev, BT_MAX_KEY_EVENTS) == 0, "null report rejected");
        check(BT_KeyEvents(&st, &r, 0, BT_MAX_KEY_EVENTS) == 0, "null buffer rejected");
        check(BT_KeyEvents(&st, &r, ev, 0) == 0, "zero maxEvents rejected");
        BT_KeyStateInit(0);   /* must not crash */
        check(1, "BT_KeyStateInit(0) is survivable");
    }

    /* --- every event maps to something postable, or is deliberately unmapped ----- */
    {
        unsigned char a[1] = { 0x04 };
        BTHidReport r1 = rep(BT_MOD_LSHIFT, a, 1, 0);
        int i, allOk = 1;
        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &r1, ev, BT_MAX_KEY_EVENTS);
        for (i = 0; i < n; i++) {
            unsigned char usage = (unsigned char)(ev[i] & 0x00FF);
            if (BT_UsageToVirtualKey(usage) == BT_VK_NONE) allOk = 0;
        }
        check(allOk, "shift and 'a' both have Mac virtual keycodes");
    }

    /* --- ⭐⭐ CAPS LOCK IS A TOGGLE, NOT A KEY ----------------------------------
     * Measured on hardware 2026-09-15: Caps Lock did nothing and its LED never lit,
     * because we posted press-then-release and Mac OS saw it engage and instantly
     * disengage. Apple's PostUSBKeyToMac keeps the toggle, remaps the PRESS to a
     * down-or-up according to the NEW state, and swallows the release. */
    {
        unsigned char caps[1] = { BT_USAGE_CAPSLOCK };
        BTHidReport down = rep(0, caps, 1, 0);
        BTHidReport up   = rep(0, 0, 0, 0);

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &down, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == BT_USAGE_CAPSLOCK,
              "⭐ caps ON: first press posts a DOWN");
        check(st.capsOn == 1 && (st.leds & BT_LED_CAPSLOCK) && st.ledsChanged,
              "⭐ caps ON: toggle set and the LED bit went with it");

        n = BT_KeyEvents(&st, &up, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "⭐⭐ the RELEASE is swallowed -- this is the whole bug");
        check(st.capsOn == 1, "and the toggle survives the release");

        n = BT_KeyEvents(&st, &down, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == (BT_USAGE_CAPSLOCK | BT_KEYEV_UP),
              "⭐ caps OFF: the SECOND press posts the UP");
        check(st.capsOn == 0 && !(st.leds & BT_LED_CAPSLOCK),
              "caps OFF: toggle and LED bit both cleared");

        n = BT_KeyEvents(&st, &up, ev, BT_MAX_KEY_EVENTS);
        check(n == 0 && st.ledsChanged == 0,
              "a release changes no LED and posts nothing");
    }

    /* --- caps lock does not disturb ordinary keys held with it ------------------ */
    {
        unsigned char both[2] = { BT_USAGE_CAPSLOCK, 0x04 };
        BTHidReport r1 = rep(0, both, 2, 0);
        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &r1, ev, BT_MAX_KEY_EVENTS);
        check(n == 2 && has(ev, n, BT_USAGE_CAPSLOCK) && has(ev, n, 0x04),
              "caps and 'a' in one report both report");
    }

    /* --- ⭐⭐ LEFT/RIGHT MODIFIERS SHARE A VIRTUAL KEYCODE ----------------------
     * So releasing one while the other is held must NOT post a key-up: it would tell
     * Mac OS that Shift is up while the user is still holding the other Shift, and the
     * next character would come out lowercase mid-word. Apple guards the same case. */
    {
        BTHidReport bothShift = rep((unsigned char)(BT_MOD_LSHIFT | BT_MOD_RSHIFT), 0, 0, 0);
        BTHidReport onlyRight = rep(BT_MOD_RSHIFT, 0, 0, 0);
        BTHidReport none      = rep(0, 0, 0, 0);

        BT_KeyStateInit(&st);
        n = BT_KeyEvents(&st, &bothShift, ev, BT_MAX_KEY_EVENTS);
        check(n == 2, "both shifts down report two presses");

        n = BT_KeyEvents(&st, &onlyRight, ev, BT_MAX_KEY_EVENTS);
        check(n == 0, "⭐⭐ releasing LEFT shift while RIGHT is held posts NOTHING");

        n = BT_KeyEvents(&st, &none, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == (0xE5 | BT_KEYEV_UP),
              "⭐ releasing the LAST shift finally posts the up");
    }

    /* --- but a non-twin modifier release is unaffected -------------------------- */
    {
        BTHidReport two = rep((unsigned char)(BT_MOD_LSHIFT | BT_MOD_LCTRL), 0, 0, 0);
        BTHidReport one = rep(BT_MOD_LCTRL, 0, 0, 0);
        BT_KeyStateInit(&st);
        (void)BT_KeyEvents(&st, &two, ev, BT_MAX_KEY_EVENTS);
        n = BT_KeyEvents(&st, &one, ev, BT_MAX_KEY_EVENTS);
        check(n == 1 && ev[0] == (0xE1 | BT_KEYEV_UP),
              "releasing shift while CONTROL is held still posts the shift up");
    }

    printf(gFail ? "\nFAILED: %d\n" : "\nall passed\n", gFail);
    return gFail ? 1 : 0;
}
