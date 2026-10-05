/*
 *  bt_btstack.h  --  the two calls bt_probe.c makes into the BTstack world.
 *
 *  Kept deliberately tiny and free of BTstack types so that bt_probe.c, which
 *  includes USB.h, never has to include BTstack's headers. The reverse is also
 *  true: bt_btstack.c never includes USB.h. Those two include worlds clash (see
 *  the TYPE_BOOL note in the build) and keeping them apart is cheaper than
 *  reconciling them.
 */
#ifndef OS9BT_BTSTACK_H
#define OS9BT_BTSTACK_H

/* Bring BTstack up and ask it to power on the controller. Called from
 * ConfigDone once all three pipes are open and both readers are armed.
 *
 * ⚠ Replaces HCI_Start(). BTstack's hci.c runs its own bring-up sequence and
 * assumes it owns the controller, so our M1 sequence must not also run. */
void BT_StackStart(void);

/* Mirror the glue's counters into the driver's counter block. Implemented in
 * bt_probe.c because that is where the block lives; called from the BTstack side
 * after each delivery. `state` is BTstack's own hci_get_state(). */
void BT_StackPoll(unsigned long state);

#endif /* OS9BT_BTSTACK_H */
