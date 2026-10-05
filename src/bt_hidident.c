/*
 * bt_hidident.c -- see bt_hidident.h. Dependency-free: no includes but its own header.
 */
#include "bt_hidident.h"

int BT_IdentityAdopt(unsigned long curCid, int curCidLive, unsigned long newCid)
{
    /* 0 is "no connection", never an identity. Adopting it would make every consumer's
     * `gBhCid != 0` test -- the Caps LED's in particular -- read as "connected". */
    if (newCid == 0) return 0;

    /* The identity reconnecting under the same cid is not a steal. */
    if (curCid == newCid) return 1;

    /* Free, or pointing at a device that is gone. */
    if (curCid == 0 || !curCidLive) return 1;

    /* A live device holds it. Leave it alone. */
    return 0;
}

int BT_IdentityClear(unsigned long closedCid, unsigned long curCid)
{
    /* Nobody holds it; clearing is a harmless no-op and keeps the pre-v15.0 path. */
    if (curCid == 0) return 1;

    return closedCid == curCid;
}
