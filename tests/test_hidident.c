/*
 *  test_hidident.c -- the keyboard identity survives a second device.
 *
 *  These are the exact scenarios the A1015 introduces. Every one of them was reasoned
 *  about in a comment first; this file is what makes them true. The one that matters
 *  most is "the mouse naps": an Apple wireless mouse disconnects every few minutes, and
 *  before v15.0 each nap cleared gBhCid and gBhLive -- which killed the keyboard's Caps
 *  LED (it gates on gBhCid != 0) and sent bh_reconnect_timeout to page a keyboard that
 *  had never disconnected, spending its bounded retry budget on a live object.
 */
#include <stdio.h>
#include "../src/bt_hidident.h"

static int fails = 0;

static void ok(int cond, const char *what)
{
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fails++;
}

#define KBD   0x0041UL
#define MOUSE 0x0042UL

int main(void)
{
    printf("=== adopting the identity ===\n");
    ok(BT_IdentityAdopt(0, 0, KBD) == 1,
       "first device takes a free identity");
    ok(BT_IdentityAdopt(KBD, 1, MOUSE) == 0,
       "⭐ a mouse does NOT steal it from a live keyboard");
    ok(BT_IdentityAdopt(KBD, 0, MOUSE) == 1,
       "a stale identity is taken over (keyboard already gone)");
    ok(BT_IdentityAdopt(KBD, 1, KBD) == 1,
       "the identity reconnecting under its own cid is not a steal");
    ok(BT_IdentityAdopt(KBD, 1, 0) == 0,
       "cid 0 is never adopted -- it is the 'no connection' sentinel");
    ok(BT_IdentityAdopt(0, 0, 0) == 0,
       "...even when the identity is free");

    printf("=== clearing it on a close ===\n");
    ok(BT_IdentityClear(KBD, KBD) == 1,
       "the keyboard's own close clears it");
    ok(BT_IdentityClear(MOUSE, KBD) == 0,
       "⭐⭐ THE MOUSE NAPPING DOES NOT clear the keyboard's identity");
    ok(BT_IdentityClear(MOUSE, 0) == 1,
       "with no identity held, clearing stays a no-op (pre-v15.0 path)");

    printf("=== the full A1015 sequence, in order ===\n");
    {
        unsigned long cur = 0;        /* gBhCid */
        int           live = 0;       /* is cur's device connected? */

        /* keyboard pages us at boot */
        if (BT_IdentityAdopt(cur, live, KBD)) { cur = KBD; live = 1; }
        ok(cur == KBD, "1. keyboard connects -> identity is the keyboard");

        /* the sweep connects the mouse */
        if (BT_IdentityAdopt(cur, live, MOUSE)) cur = MOUSE;
        ok(cur == KBD, "2. mouse connects  -> identity UNCHANGED");

        /* the mouse goes to sleep */
        if (BT_IdentityClear(MOUSE, cur)) { cur = 0; live = 0; }
        ok(cur == KBD, "3. mouse sleeps    -> identity UNCHANGED (the regression)");

        /* the mouse wakes and reconnects with a fresh cid */
        if (BT_IdentityAdopt(cur, live, MOUSE + 1)) cur = MOUSE + 1;
        ok(cur == KBD, "4. mouse returns   -> identity STILL the keyboard");

        /* now the keyboard itself disconnects */
        if (BT_IdentityClear(KBD, cur)) { cur = 0; live = 0; }
        ok(cur == 0, "5. keyboard closes -> identity released");

        /* and the mouse, already connected, may now take it */
        if (BT_IdentityAdopt(cur, live, MOUSE + 1)) { cur = MOUSE + 1; live = 1; }
        ok(cur == MOUSE + 1, "6. identity is free -> the live mouse may hold it");
    }

    printf(fails ? "\nHID IDENTITY TESTS FAILED\n" : "\nall HID identity tests passed\n");
    return fails ? 1 : 0;
}
