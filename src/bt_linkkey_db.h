/*
 *  bt_linkkey_db.h  --  the one entry point into the RAM link key store.
 *
 *  Kept free of BTstack types where possible, but the return type IS a BTstack
 *  type, so only bt_btstack.c may include this. bt_probe.c reads the counters
 *  through bt_pump.h instead, which is why they live there and not here.
 */
#ifndef OS9BT_LINKKEY_DB_H
#define OS9BT_LINKKEY_DB_H

/* ⚠ BTstack typedefs an ANONYMOUS struct, so there is no struct tag to
 * forward-declare. Including the real header is the only correct option, which is
 * also why only bt_btstack.c may include this file. */
#include "classic/btstack_link_key_db.h"

/* Pass to hci_init() in place of the NULL this port used up to v1.10.
 *
 * ⚠ Must be handed to hci_init BEFORE BTstack can be asked for a key, and every
 * callback behind it runs at SECONDARY INTERRUPT LEVEL -- see bt_linkkey_db.c. */
const btstack_link_key_db_t * bt_linkkey_db_os9_instance(void);

/* ---- the seam the FILE half uses (src/bt_keyfile.c) ----------------------- *
 * ⚠ These are called at TASK LEVEL only, from ProbeInitialize / ProbeFinalize /
 * the NM response. The db callbacks above are the interrupt-level side. Keeping
 * both in one file is deliberate: the RAM table is the single source of truth and
 * only one file may touch its internals. */

/* Insert a record read from disk. Does NOT set the dirty flag -- loading is not a
 * change, and marking it dirty would make the first flush rewrite what we just
 * read. */
/* v12.0: capture a key the CONTROLLER already holds into our database + key file.
 * Returns 1 only if something changed. Does NOT write back to the controller. */
/* v12.3: demote a live entry to archived (key kept, row dropped). Card-delete only. */
int  BT_LinkKeyArchive(unsigned long hi, unsigned long lo);
int  BT_LinkKeyGetByIndexEx(int i, unsigned char *addr6, unsigned char *key16,
                            unsigned char *type, int wantArchived);
extern unsigned long gLkArchived;

int  BT_LinkKeyFindByAddr(unsigned long hi, unsigned long lo,
                          unsigned char *addr6, unsigned char *key16,
                          unsigned char *type);
int  BT_LinkKeyCapture(const unsigned char *addr6, const unsigned char *key16,
                       unsigned char type);
extern unsigned long gLkCaptured;

void BT_LinkKeyLoadOne(const unsigned char *addr6, const unsigned char *key16,
                       unsigned char type);

int  BT_LinkKeyCount(void);
int  BT_LinkKeyGetByIndex(int i, unsigned char *addr6, unsigned char *key16,
                          unsigned char *type);

int  BT_LinkKeyIsDirty(void);
void BT_LinkKeyClearDirty(void);

/* The local controller's address, as reported by BTstack through
 * set_local_bd_addr. Returns 0 if BTstack has not told us yet.
 * ⚠ A key is bonded against the LOCAL address, so a stored file from a DIFFERENT
 * dongle must be discarded rather than offered. */
int  BT_LinkKeyGetLocalAddr(unsigned char *addr6);

#endif /* OS9BT_LINKKEY_DB_H */
