/*
 *  bt_linkkey_db.c  --  a btstack_link_key_db_t backed by a fixed RAM array.
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  Up to v1.10 this port passed NULL as the link key database:
 *  `hci_init(hci_transport_os9_instance(), NULL)`. hci.c guards every use with
 *  `if (!hci_stack->link_key_db) return;`, so that was safe rather than broken --
 *  but it meant the link key produced by a successful pairing was thrown away.
 *
 *  Run 18 paired and encrypted a link for the first time; runs 19-21 repeated it.
 *  In every one of those runs `link key requests` read ZERO, which is exactly what
 *  an empty store produces: the peer never bothers asking for a key we have never
 *  been able to offer, and each session re-pairs from scratch.
 *
 *  ⚠ THAT IS FATAL TO THE DELIVERABLE, NOT MERELY UNTIDY. A Bluetooth keyboard that
 *  must be re-paired after every boot cannot be re-paired, because you need a
 *  keyboard to do it. The standing requirement is that support is resident and
 *  working BEFORE the desktop loads. Persistence is what makes that possible.
 *
 *  ⚠⚠ THE CRUX: THESE CALLBACKS RUN AT SECONDARY INTERRUPT LEVEL.
 *
 *  `get_link_key` is called from hci.c while it handles HCI_EVENT_LINK_KEY_REQUEST,
 *  and `put_link_key` while it handles a Link Key Notification. Both arrive through
 *  our interrupt-IN completion. So NOTHING HERE MAY TOUCH THE FILE MANAGER, the
 *  Memory Manager, or the Toolbox. A naive "open the prefs file in put_link_key" is
 *  a silent hard hang with no NMI and no log -- the failure this project has hit
 *  three times (EHCI r18, n4, r95).
 *
 *  This file is therefore deliberately ONLY the RAM half of the design in
 *  docs/M3-DESIGN.md §3b:
 *
 *      RAM table  (here)      interrupt level   serves BTstack, sets a dirty flag
 *      File       (later)     task level        loaded once, written back when dirty
 *
 *  The file half arrives at §6.3-6.4 via the NMInstall defer-to-task-level
 *  trampoline. Until then a bond survives a session but not a reboot, which is a
 *  strictly better position than v1.10 and is independently testable: pair,
 *  disconnect, reconnect WITHOUT re-pairing.
 *
 *  No allocation anywhere. Fixed array, linear scan, 8 entries.
 */

#include "btstack_config.h"

#include "bluetooth.h"                  /* bd_addr_t, link_key_t, LINK_KEY_LEN */
#include "classic/btstack_link_key_db.h"

#include <string.h>                     /* memcpy / memcmp -- pure */

#include "bt_pump.h"                    /* the counters bt_probe.c mirrors      */
#include "bt_linkkey_db.h"
#include "bt_defer.h"

/* ---- capacity ------------------------------------------------------------ *
 * One keyboard is the goal. Eight covers a keyboard, a mouse and experimentation
 * while costing 8 * ~60 bytes of BSS, and it is a fixed array precisely because
 * there is no allocator available at interrupt level. Exhaustion is COUNTED rather
 * than silent -- see gLkEvicted. */
#define BT_MAX_LINK_KEYS 8

typedef struct {
    bd_addr_t       addr;
    link_key_t      key;
    link_key_type_t type;
    int             used;
    unsigned long   seq;        /* for oldest-wins eviction; see put_link_key */
    /* ⚠ Set only when the CONTROLLER asks for this key by address -- see
     * db_get_link_key. Deliberately NOT persisted: it is evidence about this session,
     * and writing it to the file would turn it back into the same unfounded claim the
     * file already makes. */
    int             handedOut;
    /* ★★★★ v12.3: ARCHIVED -- the key is retained for Restore ONLY.
     *
     * An archived entry is NOT live: db_get_link_key will not offer it, M7 will not
     * publish it, and the control panel will not show a row for it. It exists so that
     * removing the CARD's copy of a pairing stays reversible without also leaving a
     * ghost row in the device list, which is what made the panel useless as an
     * instrument: a deleted keyboard kept reading "Paired" and there was no way to
     * tell its real state by looking.
     *
     * ⚠ The two deletes are DELIBERATELY different and must stay so:
     *   "Forget this Bluetooth device"      -> db_delete_link_key, WIPES the key.
     *                                          The user means forget it.
     *   "Remove this pairing from the module" -> BT_LinkKeyArchive, keeps the key.
     *                                          The user means change the CARD. */
    int             archived;
} BTLinkKeyEntry;

static BTLinkKeyEntry gKeys[BT_MAX_LINK_KEYS];
static unsigned long  gSeq;

/* Counters, mirrored into the driver's block by BT_StackPoll in bt_probe.c. The
 * dirty flag is what the task-level flush will consume at §6.4; it is maintained
 * now so the file half does not have to touch this logic later. */
unsigned long gLkGets, gLkHits, gLkPuts, gLkDeletes, gLkStored, gLkEvicted, gLkDirty;
/* v12.0: keys captured from the controller's own store. See BT_LinkKeyCapture.
 * gRestoreTried is defined here rather than in bt_probe.c so that every link-key
 * counter has one home; bt_probe.c only increments it. */
unsigned long gLkCaptured;
unsigned long gRestoreTried;
/* v12.3: entries demoted to archived by a card-side delete. */
unsigned long gLkArchived;
/* Bit N set = the Nth PUBLISHED key has been handed to the controller this session.
 * See db_get_link_key: that is the only evidence a bond is mutual. */
unsigned long gLkHandedMask;

/* ⚠ WHAT WE ACTUALLY HOLD, not just how many. Run 22 showed 1 key stored and 0
 * lookups, which left "is the key stored against the RIGHT address?" unanswerable --
 * and a key filed under a wrong or zeroed address would miss every future request
 * while the count still looked healthy. These make the store's content visible. */
unsigned long gLkLastAddrHi, gLkLastAddrLo, gLkLastType;

static BTLinkKeyEntry * find_entry(const bd_addr_t addr)
{
    int i;
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
        if (gKeys[i].used && memcmp(gKeys[i].addr, addr, sizeof(bd_addr_t)) == 0)
            return &gKeys[i];
    }
    return NULL;
}

/* ---- btstack_link_key_db_t ---------------------------------------------- */

static void db_open(void)
{
    /* Deliberately empty. There is no file to open here: at §6.4 the file will be
     * read at ProbeInitialize (task level) BEFORE hci_init runs, because hci_init
     * takes this db pointer and BTstack may ask for a key at any time afterwards.
     * Opening anything from db_open would be opening it at interrupt level. */
}

static void db_close(void)
{
    /* Also empty, and for the same reason. The flush belongs to ProbeFinalize and
     * to the dirty-triggered trampoline, both of which are task level. */
}

static bd_addr_t gLocalAddr;
static int       gHaveLocalAddr;

static void db_set_local_bd_addr(bd_addr_t bd_addr)
{
    /* ⚠ NOT IGNORED WHEN THE FILE HALF LANDS. BTstack provides this hook precisely
     * for "Bluetooth Controllers that can be swapped, e.g. USB Dongles on desktop
     * systems", which is exactly our case: keys bonded through one dongle are
     * useless through another, since the peer bonded against that dongle's BD_ADDR.
     * The stored file must therefore be keyed by local address too, and a mismatch
     * must discard rather than offer stale keys.
     *
     * ⚠ NO LONGER A STUB. bt_keyfile.c compares this against the local address in
     * the stored header and DISCARDS the whole file on a mismatch, because a key
     * bonded through another dongle is not merely useless -- offering it makes the
     * peer's authentication fail in a way that looks like our bug. */
    memcpy(gLocalAddr, bd_addr, sizeof(bd_addr_t));
    gHaveLocalAddr = 1;
}

static int db_get_link_key(bd_addr_t bd_addr, link_key_t link_key,
                           link_key_type_t * type)
{
    BTLinkKeyEntry *e;

    gLkGets++;
    e = find_entry(bd_addr);
    if (e == NULL) return 0;

    memcpy(link_key, e->key, LINK_KEY_LEN);
    if (type != NULL) *type = e->type;
    gLkHits++;
    /* ★★ THIS IS THE ONLY EVIDENCE A BOND IS REAL, so record it per entry.
     *
     * Holding a key proves nothing. Run 50: keys stored 3, put_link_key 0, records
     * LOADED 3 -- every key came off disk from an earlier session, and one of them was
     * for a phone whose own UI said pairing had FAILED. A Bluetooth bond is mutual; our
     * half alone is worthless if the peer discarded its half, and the panel was
     * labelling all three "Paired" on exactly that non-evidence.
     *
     * Reaching here means the CONTROLLER asked for a key by this address, which only
     * happens when the peer initiated an authenticated link and named it. That is a
     * fact about the peer's belief, not ours. */
    e->handedOut = 1;
    return 1;
}

static void db_put_link_key(bd_addr_t bd_addr, link_key_t link_key,
                            link_key_type_t type)
{
    BTLinkKeyEntry *e;
    int i, victim = -1;

    gLkPuts++;

    /* Update in place if we already know this peer. Re-pairing the same device must
     * not consume a second slot. */
    e = find_entry(bd_addr);
    if (e == NULL) {
        for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
            if (!gKeys[i].used) { e = &gKeys[i]; break; }
        }
    }
    if (e == NULL) {
        /* ⚠ FULL. Evict the OLDEST rather than refusing, because refusing would make
         * a new pairing silently fail while the block still looked healthy. Eviction
         * is counted so "why did my keyboard forget me" has an answer in the data.
         * With 8 slots and one keyboard this should never fire; if it does, that is
         * the finding. */
        for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
            if (victim < 0 || gKeys[i].seq < gKeys[victim].seq) victim = i;
        }
        e = &gKeys[victim];
        gLkEvicted++;
    }

    memcpy(e->addr, bd_addr, sizeof(bd_addr_t));
    memcpy(e->key, link_key, LINK_KEY_LEN);
    e->type = type;
    e->used = 1;
    e->seq  = ++gSeq;

    /* ★★★★ AND PUSH IT INTO THE CARD'S OWN STORE -- the step the narrowed goal turns
     * on, and the reason Read_Stored_Link_Key was measured three runs ago.
     *
     * The A1044 keeps link keys in its firmware (Max_Num_Keys 16, currently holding
     * 2), and its on-chip proxy stack reconnects a device using THAT store, with no
     * host involved. So a key that only lives here is a key the card cannot use: the
     * device would pair under OS 9 and then stop working the moment we hand the card
     * back to proxy mode.
     *
     * ⚠⚠ THIS IS THE ONLY PLACE IT IS CALLED FROM, DELIBERATELY. The address and key
     * are ones a real pairing just produced, sitting in our own database -- never
     * speculative, never from user input, never for an address we did not just bond
     * with. Write_Stored_Link_Key mutates state that survives a reboot and that Tiger
     * shares; overwriting a slot the user needs would take away a keyboard that
     * currently works.
     *
     * ⚠ Interrupt level. It sends one HCI command through the same async path
     * everything else here uses, and takes no lock and no allocation. */
    BT_WriteKeyToController(e->addr, e->key);

    gLkLastAddrHi = ((unsigned long)bd_addr[0] << 16) | ((unsigned long)bd_addr[1] << 8)
                  |  (unsigned long)bd_addr[2];
    gLkLastAddrLo = ((unsigned long)bd_addr[3] << 16) | ((unsigned long)bd_addr[4] << 8)
                  |  (unsigned long)bd_addr[5];
    /* 0x100 | type so a stored COMBINATION_KEY (type 0) is distinguishable from
     * "nothing stored" -- the same rule as every status word in the block. */
    gLkLastType   = 0x100UL | (unsigned long)type;

    /* The file half consumes this at §6.4. Flushing on DIRTY rather than only at
     * ProbeFinalize is deliberate: a power cut or an unclean unload would otherwise
     * lose the key and force exactly the re-pair the store exists to prevent. */
    gLkDirty = 1;

    /* ⚠ ASK FOR TASK LEVEL, do not try to reach it. BT_DeferRequest only enqueues a
     * Notification Manager record; the actual file write happens later, at task
     * level, in bt_defer.c's response proc. Calling the File Manager from here would
     * be a silent hard hang. */
    BT_DeferRequest();

    {
        int n = 0;
        for (i = 0; i < BT_MAX_LINK_KEYS; i++) if (gKeys[i].used) n++;
        gLkStored = (unsigned long)n;
    }
}

static void db_delete_link_key(bd_addr_t bd_addr)
{
    BTLinkKeyEntry *e;
    int i, n = 0;

    gLkDeletes++;
    e = find_entry(bd_addr);
    if (e != NULL) {
        /* Wipe the key bytes, not just the used flag. This is key material sitting
         * in the System heap where a heap scan can reach it -- BTCheck itself walks
         * the System heap looking for our magic, which is proof enough that other
         * code can too. */
        memset(e->key, 0, LINK_KEY_LEN);
        memset(e->addr, 0, sizeof(bd_addr_t));
        e->used = 0;
        e->seq  = 0;
        gLkDirty = 1;

        /* ⚠⚠ AND ASK FOR THE FLUSH. THIS LINE WAS MISSING AND IT IS THE WHOLE BUG.
         *
         * db_put_link_key sets gLkDirty and then calls BT_DeferRequest to get task
         * level; this function set the flag and asked for nothing. So the RAM store
         * forgot the bond and the FILE never did -- and the next launch loaded it
         * straight back, showing "Bond on file" for a device the user had deleted
         * several times. Exactly the symptom reported.
         *
         * ⭐ The header comment forty lines below already SAID this was required:
         * "without it the bond would reappear on the next boot from the key file and
         * the button would look broken in the most confusing possible way." The
         * requirement was written down and the code did not do it. A comment is not
         * an implementation.
         *
         * ⚠ Enqueue only, safe from interrupt level, and identical to what the put
         * path has always done from this same context -- so this is restoring
         * symmetry, not adding a new mechanism. */
        BT_DeferRequest();
    }
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) if (gKeys[i].used) n++;
    gLkStored = (unsigned long)n;
}

/* ---- iteration ----------------------------------------------------------- *
 * BTstack's iterator carries a single void* context. We store an index in it
 * rather than a pointer, so there is nothing to allocate and nothing to free --
 * which is why iterator_done has no work to do. */

static int db_iterator_init(btstack_link_key_iterator_t * it)
{
    if (it == NULL) return 0;
    it->context = (void *)0;
    return 1;
}

static int db_iterator_get_next(btstack_link_key_iterator_t * it,
                                bd_addr_t bd_addr, link_key_t link_key,
                                link_key_type_t * type)
{
    long i;

    if (it == NULL) return 0;
    for (i = (long)it->context; i < BT_MAX_LINK_KEYS; i++) {
        if (!gKeys[i].used) continue;
        memcpy(bd_addr, gKeys[i].addr, sizeof(bd_addr_t));
        memcpy(link_key, gKeys[i].key, LINK_KEY_LEN);
        if (type != NULL) *type = gKeys[i].type;
        it->context = (void *)(i + 1);
        return 1;
    }
    it->context = (void *)(long)BT_MAX_LINK_KEYS;
    return 0;
}

static void db_iterator_done(btstack_link_key_iterator_t * it)
{
    (void)it;       /* nothing allocated, so nothing to release */
}

/* ⚠ FIELD ORDER IS BTstack's, NOT ALPHABETICAL OR CONVENIENT. This is a plain
 * struct of function pointers with no designated initialisers in the header's own
 * style, so a reordered member binds the wrong function silently. The order below
 * matches btstack_link_key_db.h exactly: open, set_local_bd_addr, close, get, put,
 * delete, iterator_init, iterator_get_next, iterator_done. */
static const btstack_link_key_db_t bt_linkkey_db_os9 = {
    &db_open,
    &db_set_local_bd_addr,
    &db_close,
    &db_get_link_key,
    &db_put_link_key,
    &db_delete_link_key,
    &db_iterator_init,
    &db_iterator_get_next,
    &db_iterator_done,
};

/* ---- the seam the FILE half uses. TASK LEVEL callers only. --------------- *
 * ⚠ These read and write the same array the interrupt-level callbacks use. On OS 9
 * that is safe WITHOUT a lock for a reason worth stating: secondary interrupt
 * handlers are serialised against each other, and task-level code is interrupted BY
 * them rather than racing them, so a task-level reader can be interrupted mid-scan
 * but never see a half-written entry -- each field write is a single store, and an
 * entry only becomes visible when `used` is set last. If that ordering ever changes,
 * this comment is the thing to revisit. */

/* ★★★★★★ v12.0: CAPTURE A KEY THE CONTROLLER ALREADY HOLDS.
 *
 * This is NOT db_put_link_key and NOT BT_LinkKeyLoadOne, and the differences are the
 * whole reason it exists:
 *
 *  - unlike BT_LinkKeyLoadOne (file -> RAM at startup) it UPDATES an existing entry
 *    rather than only filling an empty slot. Capture runs against a database that may
 *    already hold this address, and appending would leave two entries for one peer with
 *    find_entry silently shadowing one of them.
 *  - unlike BT_LinkKeyLoadOne it sets gLkDirty, because a captured key that never
 *    reaches the file is not a safety net at all -- it would vanish on the next boot,
 *    which is exactly when it is needed.
 *  - ⚠⚠ unlike db_put_link_key it does NOT call BT_WriteKeyToController. The key CAME
 *    FROM the controller; writing it straight back would be a pointless HCI command
 *    issued from inside event handling, and it would make the card's store a source
 *    that feeds itself.
 *
 * ⚠ Returns 1 only when something actually changed, so the caller's counter measures
 * captures rather than sightings. An identical key seen on every read pass must not
 * re-dirty the file forever. */
/* ★★★★ v12.3: demote a live entry to ARCHIVED, keeping the key for Restore.
 *
 * ⚠ This deliberately does NOT wipe the key, which is the one thing
 * db_delete_link_key is careful to do. The difference is the user's intent: this
 * path runs when they removed the pairing from the CARD, and the whole point of
 * keeping our copy is to be able to put it back. db_delete_link_key stays as it is
 * for the case where they really do mean forget.
 *
 * Returns 1 if an entry was demoted. */
/* ★★★★ v12.6: THE ARCHIVED COUNT, RECOUNTED RATHER THAN TALLIED.
 *
 * ⚠⚠ gLkArchived read 0 across a delete whose archive DEMONSTRABLY WORKED -- the key
 * came back when Restore was used, which it could not have done had the entry been
 * wiped. So the archive functions and the event counter does not, for a reason I could
 * not pin down: no duplicate word number, no double Note, correct linkage, same Note
 * group as counters that do work.
 *
 * ⇒ Rather than keep hunting a counter, publish the STATE instead of the EVENT. A
 * recount of what the array actually holds cannot drift from what the array holds,
 * which is the property the tally failed to have. gLkStored has been maintained this
 * way since the beginning and has never been wrong.
 *
 * ⚠ The event counter is KEPT, deliberately, and published beside this one: if the two
 * ever disagree again that disagreement is itself the diagnostic. */
int BT_LinkKeyArchivedCount(void)
{
    int i, n = 0;
    for (i = 0; i < BT_MAX_LINK_KEYS; i++)
        if (!gKeys[i].used && gKeys[i].archived) n++;
    return n;
}

int BT_LinkKeyArchive(unsigned long hi, unsigned long lo)
{
    unsigned char want[6];
    int i, n = 0, hit = 0;
    want[0] = (unsigned char)((hi >> 16) & 0xFF);
    want[1] = (unsigned char)((hi >>  8) & 0xFF);
    want[2] = (unsigned char)( hi        & 0xFF);
    want[3] = (unsigned char)((lo >> 16) & 0xFF);
    want[4] = (unsigned char)((lo >>  8) & 0xFF);
    want[5] = (unsigned char)( lo        & 0xFF);
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
        if (!gKeys[i].used) continue;
        if (memcmp(gKeys[i].addr, want, 6) != 0) continue;
        gKeys[i].used     = 0;      /* drops out of M7, the panel and get_link_key */
        gKeys[i].archived = 1;      /* but the key stays, for Restore              */
        gLkArchived++;
        gLkDirty = 1;
        BT_DeferRequest();
        hit = 1;
        break;
    }
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) if (gKeys[i].used) n++;
    gLkStored = (unsigned long)n;
    return hit;
}

/* v12.0: find a captured key by the packed address the mailbox carries.
 * hi = bytes 0..2, lo = bytes 3..5 -- the same encoding kCmdDeleteBond uses.
 * ⚠ Copies the key OUT to the caller. The only caller is the restore command, which
 * hands it straight to BT_WriteKeyToController; it must never reach the block. */
int BT_LinkKeyFindByAddr(unsigned long hi, unsigned long lo,
                         unsigned char *addr6, unsigned char *key16,
                         unsigned char *type)
{
    unsigned char want[6];
    int i;
    want[0] = (unsigned char)((hi >> 16) & 0xFF);
    want[1] = (unsigned char)((hi >>  8) & 0xFF);
    want[2] = (unsigned char)( hi        & 0xFF);
    want[3] = (unsigned char)((lo >> 16) & 0xFF);
    want[4] = (unsigned char)((lo >>  8) & 0xFF);
    want[5] = (unsigned char)( lo        & 0xFF);
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
        /* ⚠ ARCHIVED ENTRIES COUNT HERE, and that is the entire point: Restore exists
         * to reach a key that Delete removed from the live list. Searching only `used`
         * would make the archive unreachable and the feature a no-op. */
        if (!gKeys[i].used && !gKeys[i].archived) continue;
        if (memcmp(gKeys[i].addr, want, 6) != 0) continue;
        memcpy(addr6, gKeys[i].addr, 6);
        memcpy(key16, gKeys[i].key, LINK_KEY_LEN);
        *type = (unsigned char)(gKeys[i].type);
        /* ⭐ PROMOTE IT BACK. Restore is not just a write to the card -- it undoes the
         * delete, so the entry becomes live again and the row reappears as it was.
         * Without this the key would go back to the card while our own list still
         * showed nothing, which is the same "cannot tell the real state by looking"
         * problem in the other direction. */
        if (!gKeys[i].used) {
            gKeys[i].used     = 1;
            gKeys[i].archived = 0;
            gKeys[i].seq      = ++gSeq;
            gLkStored++;
            gLkDirty = 1;
            BT_DeferRequest();
        }
        return 1;
    }
    return 0;
}

int BT_LinkKeyCapture(const unsigned char *addr6, const unsigned char *key16,
                      unsigned char type)
{
    BTLinkKeyEntry *e;
    int i, victim = -1, wasUsed;

    e = find_entry((unsigned char *)addr6);
    if (e != NULL
     && e->type == (link_key_type_t)type
     && memcmp(e->key, key16, LINK_KEY_LEN) == 0) {
        return 0;                      /* already have exactly this -- nothing to do */
    }
    if (e == NULL) {
        for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
            if (!gKeys[i].used) { e = &gKeys[i]; break; }
        }
    }
    if (e == NULL) {
        for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
            if (victim < 0 || gKeys[i].seq < gKeys[victim].seq) victim = i;
        }
        e = &gKeys[victim];
        gLkEvicted++;
    }

    wasUsed = e->used;
    memcpy(e->addr, addr6, sizeof(bd_addr_t));
    memcpy(e->key, key16, LINK_KEY_LEN);
    e->type = (link_key_type_t)type;
    e->seq  = ++gSeq;
    e->used = 1;                       /* set LAST -- see the note above */
    /* ⚠⚠ MAINTAIN THE COUNT. v12.0 stored the entry and did NOT touch gLkStored, so
     * the run reported "keys stored now 1" while the flush wrote TWO records and M7
     * published TWO addresses. A diagnostic that undercounts the very thing it exists
     * to confirm is worse than no diagnostic, and this project has been bitten by a
     * stale counter before. Only count a NEW slot -- an update in place must not
     * inflate it. */
    if (!wasUsed) gLkStored++;
    gLkCaptured++;
    gLkDirty = 1;
    BT_DeferRequest();                 /* task level does the file write */
    return 1;
}

void BT_LinkKeyLoadOne(const unsigned char *addr6, const unsigned char *key16,
                       unsigned char type)
{
    int i;
    /* ⚠ Bit 7 marks a record that was ARCHIVED when it was written -- see
     * BT_LinkKeyGetByIndexEx. It must come back archived, not live, or a delete would
     * undo itself across a reboot and the ghost row returns. */
    int wasArchived = (type & 0x80) != 0;
    type = (unsigned char)(type & 0x7F);
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) {
        if (gKeys[i].used || gKeys[i].archived) continue;
        memcpy(gKeys[i].addr, addr6, sizeof(bd_addr_t));
        memcpy(gKeys[i].key, key16, LINK_KEY_LEN);
        gKeys[i].type = (link_key_type_t)type;
        gKeys[i].seq  = ++gSeq;
        gKeys[i].archived = wasArchived;
        gKeys[i].used = wasArchived ? 0 : 1;   /* set LAST -- see the note above */
        if (!wasArchived) gLkStored++;
        return;
    }
}

int BT_LinkKeyCount(void)
{
    int i, n = 0;
    for (i = 0; i < BT_MAX_LINK_KEYS; i++) if (gKeys[i].used) n++;
    return n;
}

/* ⚠⚠ THIS FEEDS BOTH THE KEY FILE AND THE PANEL'S ROWS, and those two want DIFFERENT
 * sets. The file must keep archived entries or Restore cannot survive a reboot -- the
 * one moment it is most needed. The panel must NOT show them, or the ghost row is back.
 *
 * Resolved by the `wantArchived` flag rather than by two near-identical loops: the file
 * asks for everything and distinguishes an archived record by setting bit 7 of the type
 * byte, which is free (link key types are 0..8). An older reader sees an odd type and
 * still recovers the address and key, so the format stays backward compatible. */
int BT_LinkKeyGetByIndexEx(int i, unsigned char *addr6, unsigned char *key16,
                           unsigned char *type, int wantArchived)
{
    int j, seen = 0;
    for (j = 0; j < BT_MAX_LINK_KEYS; j++) {
        if (!gKeys[j].used && !(wantArchived && gKeys[j].archived)) continue;
        if (seen == i) {
            memcpy(addr6, gKeys[j].addr, sizeof(bd_addr_t));
            memcpy(key16, gKeys[j].key, LINK_KEY_LEN);
            *type = (unsigned char)gKeys[j].type;
            if (!gKeys[j].used && gKeys[j].archived) *type |= 0x80;
            return 1;
        }
        seen++;
    }
    return 0;
}

/* The live-only view, which is what the panel and M7 publish. */
int BT_LinkKeyGetByIndex(int i, unsigned char *addr6, unsigned char *key16,
                         unsigned char *type)
{
    return BT_LinkKeyGetByIndexEx(i, addr6, key16, type, 0);
}

int  BT_LinkKeyIsDirty(void)    { return gLkDirty != 0; }
void BT_LinkKeyClearDirty(void) { gLkDirty = 0; }

int BT_LinkKeyGetLocalAddr(unsigned char *addr6)
{
    if (!gHaveLocalAddr) return 0;
    memcpy(addr6, gLocalAddr, sizeof(bd_addr_t));
    return 1;
}

const btstack_link_key_db_t * bt_linkkey_db_os9_instance(void)
{
    return &bt_linkkey_db_os9;
}

/* ★★ PUBLISH WHICH ADDRESSES WE HOLD KEYS FOR.
 *
 * The store already reported gLkStored (a count) and gLkLastAddrHi/Lo (the most recent
 * one), which is enough to say "some pairing happened" and nothing else. The control
 * panel needs to answer a different question per row -- "is THIS device paired?" -- and
 * a count cannot answer it.
 *
 * ⚠ It also fixes a real invisibility. Run 49: the user's phone paired with the dongle
 * and never appeared in the panel, because a paired phone stops advertising itself and
 * so answers no inquiry -- `inquiry responses 5, phones 0`. With the key addresses
 * published, a bonded device can be LISTED FROM THE BOND rather than only from a scan,
 * which is what Tiger's Devices tab does and what the user expected to see.
 *
 * Two words per entry, same address packing as BT_ScanPublish and RowBDAddr. Returns
 * how many were written. Called from BT_StackPoll, so interrupt level: it reads a
 * static array and writes the block, nothing more. */
short BT_KeyPublish(unsigned long *blk, short base, short maxEntries)
{
    short i, n = 0;
    if (blk == 0) return 0;
    gLkHandedMask = 0;
    for (i = 0; i < BT_MAX_LINK_KEYS && n < maxEntries; i++) {
        if (!gKeys[i].used) continue;
        /* ⚠ Bit n, matching the PUBLISHED index, not the slot index -- the panel sees
         * a compacted list and would mis-attribute the flag otherwise. */
        if (gKeys[i].handedOut) gLkHandedMask |= (1UL << n);
        blk[base + n * 2 + 0] = ((unsigned long)gKeys[i].addr[0] << 16)
                              | ((unsigned long)gKeys[i].addr[1] << 8)
                              |  (unsigned long)gKeys[i].addr[2];
        blk[base + n * 2 + 1] = ((unsigned long)gKeys[i].addr[3] << 16)
                              | ((unsigned long)gKeys[i].addr[4] << 8)
                              |  (unsigned long)gKeys[i].addr[5];
        n++;
    }
    return n;
}

/* ★★ FORGET A BOND, by address, for the control panel's Delete button.
 *
 * ⚠ Reuses db_delete_link_key rather than reimplementing the wipe. That function
 * already zeroes the KEY BYTES and not merely the used flag, which matters: this is key
 * material sitting in the System heap, and BTCheck walking that heap looking for our
 * magic is standing proof that other code can reach it. A second implementation would
 * eventually forget that.
 *
 * gLkDirty is set by the wipe, so the deferred task-level flush persists the removal --
 * without it the bond would reappear on the next boot from the key file and the button
 * would look broken in the most confusing possible way.
 *
 * Returns 1 if an entry was actually removed, 0 if that address held no key. The
 * DISTINCTION MATTERS: the panel must not report success for a bond it never had. */
int BT_DeleteBondByAddr(unsigned long hi, unsigned long lo)
{
    bd_addr_t addr;
    unsigned long before = gLkStored;

    addr[0] = (unsigned char)((hi >> 16) & 0xFF);
    addr[1] = (unsigned char)((hi >>  8) & 0xFF);
    addr[2] = (unsigned char)( hi        & 0xFF);
    addr[3] = (unsigned char)((lo >> 16) & 0xFF);
    addr[4] = (unsigned char)((lo >>  8) & 0xFF);
    addr[5] = (unsigned char)( lo        & 0xFF);

    db_delete_link_key(addr);
    return (gLkStored < before) ? 1 : 0;
}
