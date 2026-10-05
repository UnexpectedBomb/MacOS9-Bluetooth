/*
 * bt_hidident.h -- who owns "the connection" when there are two of them.
 *
 * ⚠ DEPENDENCY-FREE ON PURPOSE, exactly like bt_bootreport.c: tests/run-tests.sh
 * compiles it with the HOST compiler, so a mistake in here costs a second instead of
 * a reboot. Do not add includes.
 *
 * gBhCid / gBhPeerAddr / gBhLive predate the second device. Four consumers read them
 * as "the connection": the Caps LED write, bh_reconnect_timeout, the battery probe,
 * and the connect sweep's collision guard. While a keyboard was the only possibility,
 * assigning them from whatever connected was correct. Once an A1015 is on the radio it
 * is not -- and the failure is silent and one-sided: the mouse works, the keyboard
 * quietly loses its LED and spends its bounded reconnect budget paging a device that
 * was never disconnected.
 *
 * These two predicates are that decision, pulled out of the event handler so the edge
 * cases are asserted by tests/test_hidident.c rather than argued about in a comment.
 */
#ifndef BT_HIDIDENT_H
#define BT_HIDIDENT_H

/* Should a connection with newCid take over the keyboard identity?
 *
 *   curCid     the identity today; 0 means nobody holds it
 *   curCidLive nonzero if curCid still names a device with a live channel
 *   newCid     the cid that is connecting
 *
 * Yes when the identity is free or stale, or when this IS the identity reconnecting.
 * No when a live device already holds it -- a second device never steals it. Role
 * learning in BhRecordReport still has the last word, because only a decoded report
 * proves what a device actually is.
 */
int BT_IdentityAdopt(unsigned long curCid, int curCidLive, unsigned long newCid);

/* Should a close of closedCid clear the identity (gBhCid, gBhLive)?
 * Only the identity's own close does. This is the one that bit: an A1015 naps every
 * few minutes, and an ungated clear turned each nap into a dead Caps LED and a
 * connect into a live object.
 */
int BT_IdentityClear(unsigned long closedCid, unsigned long curCid);

#endif /* BT_HIDIDENT_H */
