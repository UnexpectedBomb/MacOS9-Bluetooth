/*
 *  hci_transport_os9.c  --  BTstack's hci_transport_t over the OS 9 USB transport.
 *
 *  This is the whole reason M0 was built with a two-hook seam. BTstack wants an
 *  hci_transport_t; we already have BT_SendHCICommand (control pipe) and
 *  BT_SendACL (bulk OUT), both validated on hardware, plus two completions that
 *  can call upward. So this file is genuinely glue and contains no protocol.
 *
 *  DIRECTION OF TRAVEL
 *  -------------------
 *      BTstack  --send_packet-->  BT_SendHCICommand / BT_SendACL  --> controller
 *      controller --> IntCompletion / AclInCompletion --> BT_DeliverPacket
 *                 --> the handler BTstack registered --> BTstack
 *
 *  Everything here runs at secondary interrupt level, like the rest of the
 *  driver. The preprocessed audit over the vendored compile set reports zero
 *  forbidden primitives reachable from these paths; see docs/M2-DESIGN.md §3b.
 *
 *  ⚠ WHAT THIS FILE REPLACES. Our own src/hci.c drives a bring-up sequence of
 *  its own (Reset, Read_Local_Version, Read_BD_ADDR, ..., Inquiry) which was M1
 *  and is hardware-validated. BTstack's hci.c does the same job and assumes it
 *  owns the controller. THE TWO CANNOT BOTH RUN. Retiring ours is a separate
 *  step, deliberately not bundled into this commit, so that M1 stays working and
 *  bisectable while this file is brought up.
 */

#include "btstack_config.h"
#include "hci_transport.h"
#include "hci.h"              /* hci_get_state -- BTstack's, unambiguous now */
#include "bluetooth.h"          /* HCI_*_PACKET constants */

#include <stddef.h>          /* NULL -- BTstack headers do not guarantee it */

#include "bt_pump.h"
#include "bt_btstack.h"

/* Provided by bt_probe.c. Declared here rather than including hci.h, so this
 * file does not drag USB.h into BTstack's include world. Signatures must match
 * src/hci.h exactly. */
extern long BT_SendHCICommand(const void *pkt, unsigned long len);
/* v17.8: the inquiry's page-scan-rep-mode + clock offset for an address (bt_btstack.c),
 * so the Create_Connection patch below pages at the device's scan phase, not blind. */
extern int  BT_PageParamsFor(const unsigned char *addr6le,
                             unsigned char *psrm, unsigned short *clkoff);
extern long BT_SendACL(const void *pkt, unsigned long len);
extern int  BT_CanSendCommand(void);
extern int  BT_CanSendACL(void);
extern int  BT_SendWasAccepted(long err);

unsigned long gTxCmd, gTxAcl, gTxRefused, gRxEvent, gRxAcl, gTxDone;

/* ★★★ THE EVENT RING -- the counterpart to bt_probe.c's opcode ring, and the answer
 * to the question that ring raised.
 *
 * The opcode ring showed the A1044 being sent, three times over:
 *     0x0409 Accept_Connection_Request
 *     0x041B Read_Remote_Supported_Features
 * and then nothing. Authentication_Requested is never sent, because hci.c:8096 gates
 * it on BONDING_RECEIVED_REMOTE_FEATURES, and hci.c sets that flag ONLY in the
 * HCI_EVENT_READ_REMOTE_SUPPORTED_FEATURES_COMPLETE handler. The controller answered
 * 0x041B with a Command Status of status 0 and credit 0 -- accepted, in progress --
 * and then, apparently, never completed it.
 *
 * "Apparently" is the word this ring exists to delete. We had no record of a single
 * event code, so "the completion never arrived" was inference from an absence.
 *
 * ⚠⚠ RECORDED HERE, IN THE TRANSPORT, NOT IN bt_btstack.c's HANDLER. That handler
 * sees what BTstack CHOOSES TO FORWARD, and BTstack also synthesises events of its own
 * -- BTSTACK_EVENT_*, L2CAP_EVENT_*, and the 0x6E transport-packet-sent we generate
 * ourselves. A ring built there would be a mix of controller traffic and BTstack's
 * internal chatter, and the whole question is what the CONTROLLER said. This is the
 * one place only real controller packets pass through.
 *
 * Each slot is (code << 16) | (param_len << 8) | packet[2]. Byte 2 is the status for
 * almost every HCI event, which is exactly the field in question for 0x0B -- so the
 * ring carries the answer rather than just the fact that the event happened.
 *
 * ⚠ Interrupt level: plain array stores, no allocation, nothing to serialise against.
 * The index is a read-modify-write and is deliberately unlocked for the same reason as
 * the opcode ring's -- worst case is one overwritten slot of diagnostic data, and a
 * lock at interrupt level to protect a debug counter is the more dangerous choice. */
/* ⚠ kEvtRingLen is defined ONCE, in bt_pump.h, which this file includes. The ring's
 * length is shared with the publisher in bt_probe.c and with BTCheck's reader, and a
 * second copy here is exactly the "two numbers that must agree" shape that put a
 * hand-typed 304 against a 313-word block and turned into an out-of-bounds read. */
unsigned long gEvtRing[kEvtRingLen], gEvtRingIdx, gEvtTotal;
/* Event 0x0B specifically, because it is THE question. */
unsigned long gRrsfCount, gRrsfStatus, gRrsfHandle;
unsigned long gAclCapCount;
unsigned char gAclCap[kAclCapSlots][kAclCapBytes];
/* aclread1: inbound ACL packets whose header length disagrees with the size delivered
 * (BTstack drops these), and the last one as (size << 16) | ACL length. */
unsigned long gAclRxBadLen, gAclRxBadLast;
/* v9.1: the OUTBOUND half. See the capture site in transport_send_packet -- the
 * inbound capture existed from v8.7 and its mirror did not, which is why our own
 * Connection Response has never been looked at. gAclTxCapLen keeps the FULL length
 * beside each slot, because a truncated capture of a longer packet must not read as
 * a short packet. */
unsigned long gAclTxCapCount;
unsigned char gAclTxCap[kAclCapSlots][kAclTxCapBytes];
unsigned long gAclTxCapLen[kAclCapSlots];
unsigned long gLinkUpMs, gLinkDownMs, gLinkLifeMs;
unsigned long gRrsfSentMs, gRrsfDoneMs;
/* v6.8: the handle a synthetic 0x0B is injected for, and how many were injected.
 * ⚠ gSynthRrsf is published and printed BESIDE gRrsfCount so a fabricated event can
 * never be mistaken for one the controller actually sent. */
unsigned long gLastConnHandle, gSynthRrsf;
/* ★★★★★★ v9.7: Number_Of_Completed_Packets, COUNTED rather than eyeballed.
 *
 * ⚠⚠ THE CLAIM THIS REPLACES WAS NOT AIRTIGHT. "no 0x13 in six runs" came from
 * grepping the EVENT RING, and kEvtRingLen is 16 -- only the last sixteen events are
 * published. The arc where our ACL sends happen (Connection Complete through
 * Disconnection Complete) does sit inside that window, and no 0x13 appears in it, so
 * the reading was well-supported. It was still a 16-entry window presented as a
 * whole-session fact, and Tiger's trace -- which shows one 0x13 within ~16 ms of
 * EVERY ACL send -- is too important a comparison to rest on that.
 *
 * Counted HERE, in the transport, unfiltered, before BTstack consumes the event to
 * free its buffers. gNumCompTotal sums the per-handle counts, because one event can
 * acknowledge several packets and "how many events" is not "how many packets". */
unsigned long gNumCompEvts, gNumCompTotal, gNumCompHandle, gNumCompMs;

/* The handler BTstack gives us in register_packet_handler. */
static void (*gPacketHandler)(uint8_t packet_type, uint8_t *packet, uint16_t size);

/* ★★★★ v18.2: THE SYNTHETIC-0x0B INJECTION, MADE CALLABLE ON DEMAND.
 *
 * Factored out of the inline Command-Status path in BT_DeliverPacket below (same event,
 * same all-zero features -- the long note there is why zeros are the load-bearing choice
 * and must not become a plausible-looking fabrication). The reason it is a function now:
 * a dropped ACL re-inits the connection and hci_connection_init zeroes bonding_flags
 * (hci.c:4469), wiping the BONDING_RECEIVED_REMOTE_FEATURES this event sets -- after which
 * the incoming HID L2CAP channel is stuck in WAIT_REMOTE_SUPPORTED_FEATURES and never
 * reaches CONNECTION_OPENED. The half-open RE-ASSERT in bt_btstack.c restores the flag by
 * calling this directly, without waiting for BTstack to re-issue 0x041B.
 *
 * ⚠ The caller owns the handle and the decision to inject: the inline path passes
 * gLastConnHandle on the 0x041B Command Status; the re-assert passes the live sampled
 * handle ONLY when hci_remote_features_available() reads false, so this never re-asserts
 * a flag that is already set (which on the dedicated-bonding path would re-raise
 * BONDING_SEND_AUTHENTICATE_REQUEST and re-fire an auth request).
 * ⚠ Counted in gSynthRrsf like every synthetic event, so a fabricated event always has an
 * audit trail; the re-assert counts its own firings separately (bt_btstack.c). */
void BT_InjectSyntheticFeatures(unsigned long handle)
{
    unsigned char ev[13];
    int i;
    if (handle == 0 || gPacketHandler == NULL) return;
    ev[0] = 0x0B;                                   /* READ_REMOTE_SUPPORTED_FEATURES_COMPLETE */
    ev[1] = 11;                                     /* status + handle + 8 features             */
    ev[2] = 0;                                      /* status: success                          */
    ev[3] = (unsigned char)(handle & 0xFF);
    ev[4] = (unsigned char)((handle >> 8) & 0xFF);
    for (i = 5; i < 13; i++) ev[i] = 0;             /* zeros: see the inline note below         */
    gSynthRrsf++;
    gPacketHandler(HCI_EVENT_PACKET, ev, 13);
}

/* ---- upward: controller to BTstack ------------------------------------- *
 * Called from the two USL completions in bt_probe.c. The packet is handed
 * straight up; BTstack copies what it needs to keep, which is why the buffer
 * only has to be valid for the duration of the call.
 *
 * The pump is run AFTER delivery, not before, so that whatever the handler
 * queued gets serviced in the same interrupt rather than waiting for the next
 * completion to arrive. On a link that goes quiet immediately after a packet,
 * that difference is the difference between progress and a stall. */
/* ★★★★★★ btwake1: WHAT THE MODULE HAD TO SAY WHEN THE MAC WOKE.
 *
 * With remote wakeup armed, the module wakes the Mac when it has something to deliver --
 * a key press or a mouse movement, which is the point, but equally anything else it
 * reports: a device dropping its link, a mode change, a stranger paging us. The Sleep
 * Assistant can say only "WOKEN BY USB on fn0", and fn0 also hosts the wired keyboard and
 * its mouse. So, per sleep, the FIRST packet the module delivers after the sleep demand,
 * whose device it came from, and how many packets arrived before the Expert told us of the
 * wake: one that arrives before that notification was waiting at the resume.
 *
 * Slot layout, 4 words (slot = (sleep - 1) % kWakeRecSlots):
 *   [0] bit 31 seen | bit 30 arrived BEFORE the wake notification | kind << 24 (1 event,
 *       2 ACL) | byte 2 << 16 (an event's first parameter, e.g. a status) | code (event)
 *       or connection handle (ACL)
 *   [1] 0x1000000 | the device's top 3 address bytes (0 = no device for this packet)
 *   [2] its low 3 address bytes
 *   [3] packets delivered before the wake notification (capped 0xFFFF) << 16 |
 *       ms from the demand to the first packet (capped 0xFFFF; the timebase stops in sleep)
 *
 * ⚠ Interrupt level: plain stores and a read-only walk of BTstack's connection list. */
unsigned long gWakeRec[kWakeRecSlots][4];
unsigned long gWakeRecSleeps, gWakeRecWakes;
static int           gWakeRecArmed;     /* waiting for this sleep's first packet          */
static int           gWakeRecWoke;      /* the wake notification has come for this sleep  */
static int           gWakeRecSlot;
static unsigned long gWakeRecDemandMs, gWakeRecBefore;

void BT_WakeRecDemand(void)
{
    int k;
    gWakeRecSlot = (int)(gWakeRecSleeps % kWakeRecSlots);
    gWakeRecSleeps++;
    for (k = 0; k < 4; k++) gWakeRec[gWakeRecSlot][k] = 0;
    gWakeRecBefore   = 0;
    gWakeRecWoke     = 0;
    gWakeRecDemandMs = (unsigned long)hal_time_ms();
    gWakeRecArmed    = 1;
}

void BT_WakeRecWake(void)
{
    unsigned long b;
    gWakeRecWakes++;
    if (gWakeRecSleeps == 0 || gWakeRecWoke) return;
    gWakeRecWoke = 1;
    b = (gWakeRecBefore > 0xFFFFUL) ? 0xFFFFUL : gWakeRecBefore;
    gWakeRec[gWakeRecSlot][3] = (gWakeRec[gWakeRecSlot][3] & 0xFFFFUL) | (b << 16);
}

/* An address as the event carries it (little-endian, 6 bytes) -> the 3+3 split. */
static void WakeRecAddrLE(const unsigned char *a, unsigned long *hi, unsigned long *lo)
{
    *hi = 0x1000000UL | ((unsigned long)a[5] << 16) | ((unsigned long)a[4] << 8) | a[3];
    *lo = ((unsigned long)a[2] << 16) | ((unsigned long)a[1] << 8) | a[0];
}

static void BT_WakeRecPacket(unsigned char type, const unsigned char *p, unsigned short size)
{
    unsigned long w0, hi = 0, lo = 0, ms;
    unsigned int  handle = 0xFFFFU;
    int s = gWakeRecSlot;

    if (gWakeRecSleeps == 0) return;
    if (!gWakeRecWoke) gWakeRecBefore++;
    if (!gWakeRecArmed) return;
    gWakeRecArmed = 0;

    w0 = 0x80000000UL | (gWakeRecWoke ? 0UL : 0x40000000UL);
    if (type == HCI_ACL_DATA_PACKET && size >= 2) {
        handle = (unsigned int)((p[0] | (p[1] << 8)) & 0x0FFF);
        w0 |= (2UL << 24) | handle;
    } else if (type == HCI_EVENT_PACKET && size >= 2) {
        unsigned char code = p[0];
        w0 |= (1UL << 24) | ((unsigned long)(size >= 3 ? p[2] : 0) << 16) | code;
        switch (code) {
        case 0x04: case 0x16: case 0x17: case 0x18: case 0x20:   /* BD_ADDR at [2] */
            if (size >= 8) WakeRecAddrLE(p + 2, &hi, &lo);
            break;
        case 0x07: case 0x12:                                     /* status, BD_ADDR at [3] */
            if (size >= 9) WakeRecAddrLE(p + 3, &hi, &lo);
            break;
        case 0x03:                                                /* status, handle, BD_ADDR */
            if (size >= 11) WakeRecAddrLE(p + 5, &hi, &lo);
            break;
        case 0x05: case 0x06: case 0x08: case 0x0B: case 0x0C:   /* status, handle at [3] */
        case 0x13: case 0x14:                                     /* (0x13: count, handle)  */
            if (size >= 5) handle = (unsigned int)((p[3] | (p[4] << 8)) & 0x0FFF);
            break;
        case 0x1B:                                                /* handle at [2] */
            if (size >= 4) handle = (unsigned int)((p[2] | (p[3] << 8)) & 0x0FFF);
            break;
        default:
            break;
        }
    }
    if (hi == 0 && handle != 0xFFFFU) {
        /* ⚠ Captured BEFORE BTstack sees the packet, so a Disconnection Complete's link
         * is still in the list here. bd_addr_t is MSB first. */
        hci_connection_t *c = hci_connection_for_handle((hci_con_handle_t)handle);
        if (c != NULL) {
            hi = 0x1000000UL | ((unsigned long)c->address[0] << 16)
                             | ((unsigned long)c->address[1] << 8) | c->address[2];
            lo = ((unsigned long)c->address[3] << 16)
               | ((unsigned long)c->address[4] << 8) | c->address[5];
        }
    }
    ms = (unsigned long)hal_time_ms() - gWakeRecDemandMs;
    if (ms > 0xFFFFUL) ms = 0xFFFFUL;
    gWakeRec[s][0] = w0;
    gWakeRec[s][1] = hi;
    gWakeRec[s][2] = lo;
    gWakeRec[s][3] = (gWakeRec[s][3] & 0xFFFF0000UL) | ms;
}

void BT_DeliverPacket(unsigned char type, unsigned char *packet, unsigned short size)
{
    if (gPacketHandler == NULL) return;

    /* btwake1: before anything else -- see BT_WakeRecPacket. */
    BT_WakeRecPacket(type, packet, size);

    if (type == HCI_EVENT_PACKET) {
        gRxEvent++;
        /* ⚠ 0x13 layout: [0] code, [1] param len, [2] Number_of_Handles, then per
         * handle a 16-bit handle and a 16-bit completed count. One handle is the only
         * case this link produces, and reading just the first is enough to answer the
         * question; the EVENT count is what settles it either way. */
        if (size >= 7 && packet[0] == 0x13) {
            gNumCompEvts++;
            gNumCompHandle = (unsigned long)packet[3] | ((unsigned long)packet[4] << 8);
            gNumCompTotal += (unsigned long)packet[5] | ((unsigned long)packet[6] << 8);
            gNumCompMs = hal_time_ms();
        }
    }
    else if (type == HCI_ACL_DATA_PACKET) {
        gRxAcl++;
        /* ★★★★★ CAPTURE WHAT THE PEER SAID. See bt_pump.h for why this has to live
         * here and nowhere else: L2CAP signalling on CID 1 is consumed by BTstack and
         * never reaches our packet handler, so the four packets the A1016 sent during
         * the failed negotiation were invisible.
         *
         * ⚠ FIRST N ONLY, and unfiltered. Filtering to CID 1 would need the ACL header
         * decoded here and a wrong offset would silently capture the wrong thing --
         * this file already pulls three bytes of 0x0B by hand and that is enough
         * hand-decoding. Raw bytes, read the CID out of them at analysis time:
         *     [0..1] handle+flags LE, [2..3] ACL len LE,
         *     [4..5] L2CAP len LE,    [6..7] CID LE,  [8..] payload
         * CID 0x0001 is signalling, which is the traffic in question. */
        if (gAclCapCount < kAclCapSlots) {
            unsigned short i, n = size;
            if (n > kAclCapBytes) n = kAclCapBytes;
            for (i = 0; i < n; i++) gAclCap[gAclCapCount][i] = packet[i];
            gAclCapCount++;
        }
        /* ★ aclread1: THE PACKETS BTSTACK WILL DROP. hci.c acl_handler discards any ACL
         * packet whose header length + 4 is not the delivered size, with only a
         * log_error nobody sees. That is what a buffer shared by two reads produced:
         * the A1015's Config Response start fragment, overwritten by its continuation
         * and delivered at the start fragment's size. Counted here, where the size is
         * known, so a lost packet is a number and not an inference. Expected 0. */
        if (size >= 4) {
            unsigned long alen = (unsigned long)packet[2] | ((unsigned long)packet[3] << 8);
            if (alen + 4UL != (unsigned long)size) {
                gAclRxBadLen++;
                gAclRxBadLast = ((unsigned long)size << 16) | (alen & 0xFFFFUL);
            }
        }
    }

    /* ★ The event ring, filled BEFORE the packet is handed up so the record reflects
     * arrival order even if the handler faults on the way through. */
    if (type == HCI_EVENT_PACKET && size >= 2) {
        unsigned long code = packet[0];
        unsigned long b2   = (size >= 3) ? packet[2] : 0;
        gEvtRing[gEvtRingIdx & (kEvtRingLen - 1)] =
            (code << 16) | ((unsigned long)packet[1] << 8) | b2;
        gEvtRingIdx = (gEvtRingIdx + 1) & (kEvtRingLen - 1);
        gEvtTotal++;

        /* ⭐ 0x0B, HCI_EVENT_READ_REMOTE_SUPPORTED_FEATURES_COMPLETE. Layout is
         * code, param_len, status, connection_handle(2), lmp_features(8) -- so status
         * is [2] and the handle is [3..4] little-endian.
         *
         * ⚠ Pulled out by hand rather than through btstack_event.h's accessor: this
         * file is the transport and deliberately does not include the event helpers,
         * and the offsets are three bytes of spec that the ring slot above already
         * depends on anyway.
         *
         * ⚠ 0x100 | status, the block's convention. A plain 0 is a VALID status here
         * -- success -- and could not be told from "never arrived", which is the exact
         * ambiguity that has already cost this project two runs. */
        if (code == 0x0B && size >= 5) {
            gRrsfCount++;
            gRrsfDoneMs = hal_time_ms();
            gRrsfStatus = 0x100UL | b2;
            gRrsfHandle = (unsigned long)packet[3] | ((unsigned long)packet[4] << 8);
        }
        /* The handle the synthetic 0x0B below needs. Taken from Connection Complete
         * (0x03) with status 0, which is the only event that establishes one. */
        if (code == 0x03 && b2 == 0 && size >= 5) {
            gLastConnHandle = (unsigned long)packet[3] | ((unsigned long)packet[4] << 8);
            /* ★ FIRST link up only. Four links come and go in a session and the
             * question is how long ONE of them lives; overwriting on every connect
             * would silently measure the last one against the first disconnect. */
            if (gLinkUpMs == 0) gLinkUpMs = hal_time_ms();
        }
        /* ⚠ And cleared on Disconnection Complete, so a stale handle can never be used
         * to inject a synthetic event for a link that no longer exists. That is
         * precisely the failure the real 0x0B exhibited (status 2, Unknown Connection
         * Identifier) and there is no reason to reproduce it ourselves. */
        if (code == 0x05 && size >= 5) {
            gLastConnHandle = 0;
            /* ★★★ THE HEADLINE NUMBER. Reason 8 is a supervision timeout, so the
             * LIFETIME is what separates "the peer stopped answering after a normal
             * negotiation" (~20 s, the default supervision window) from "something
             * killed it" (a second or two). Three wrong theories about this link were
             * all argued without it.
             *
             * ⚠ FIRST pairing only, matched to gLinkUpMs above, and never recomputed
             * -- a later disconnect against the first connect would report a lifetime
             * spanning links that were not the same link. */
            if (gLinkDownMs == 0) {
                gLinkDownMs = hal_time_ms();
                if (gLinkUpMs != 0 && gLinkDownMs >= gLinkUpMs)
                    gLinkLifeMs = gLinkDownMs - gLinkUpMs;
            }
        }
    }

    gPacketHandler((uint8_t)type, (uint8_t *)packet, (uint16_t)size);

    /* ★★★ SYNTHESISE THE COMPLETION THE A1044 WITHHOLDS.
     *
     * ⚠⚠⚠ THIS FABRICATES AN HCI EVENT THE CONTROLLER DID NOT SEND. Read the whole
     * note before touching it, and read the two gates' note in bt_pump.h first.
     *
     * WHY LEVEL_0 ALONE IS NOT ENOUGH -- found in the vendored source BEFORE staging,
     * not after a wasted run. l2cap.c puts an INCOMING channel into
     * L2CAP_STATE_WAIT_REMOTE_SUPPORTED_FEATURES *unconditionally* (l2cap.c:3246); the
     * LEVEL_0 bypass exists only on the OUTGOING path (l2cap.c:2449). l2cap_run just
     * waits in that state, and the security check LEVEL_0 would satisfy sits
     * downstream of it in a function whose first line is
     * `if (channel->state != L2CAP_STATE_WAIT_REMOTE_SUPPORTED_FEATURES) return;`.
     * So lowering the level changes a gate that is never reached.
     *
     * And the A1044 accepts Read_Remote_Supported_Features and never completes it
     * while the link is alive -- v6.7's ring caught the Disconnection Complete
     * arriving BEFORE a 0x0B whose status was 2, Unknown Connection Identifier.
     *
     * ⇒ There is no configuration knob for this. Patching vendored BTstack was the
     * obvious alternative and was rejected: the pin file names an exact commit and a
     * patch would be silently lost at the next bump, which is the same class of defect
     * that has already wiped generated documentation and clobbered sanitisation in
     * this project. Doing it in OUR transport keeps the change where it can be seen.
     *
     * ⭐ ALL-ZERO FEATURE BYTES, AND THAT IS THE LOAD-BEARING CHOICE, not laziness.
     * hci.c:2806 reads exactly three bits out of this field:
     *   features[6] bit 3  SSP Controller -- left CLEAR so
     *                      gap_ssp_supported_on_both_sides() stays false, which is
     *                      what skips l2cap.c:2469's Security-Mode-4 disconnect of a
     *                      non-SDP PSM on an unencrypted link. Setting it would get
     *                      the link dropped instead, defeating the whole probe.
     *   features[7] bit 7  Extended features -- left CLEAR so BTstack does not then
     *                      ask for page 1, which would be a SECOND command this
     *                      controller may also refuse to complete.
     *   features[3] bit 7  eSCO -- irrelevant, no SCO here.
     * Zeros are therefore the minimum lie that unblocks the gate and provokes nothing
     * else, rather than a plausible-looking fabrication.
     *
     * ⚠ Injected on the Command STATUS for 0x041B, which is exactly where the real
     * completion should have gone -- not on Connection Complete, so the sequence
     * BTstack sees stays in the right order. The handle comes from the last
     * Connection Complete we saw; one link at a time is all this diagnostic covers.
     *
     * ⚠ AFTER the real packet is delivered, so BTstack processes the Command Status
     * first and its own bookkeeping is consistent before the synthetic event lands.
     *
     * ⚠ COUNTED. A fabricated event that is not counted is a lie with no audit trail,
     * and BTCheck prints this next to the real 0x0B arrivals so no future reader can
     * mistake one for the other. */
    if (kHidSynthFeatures && type == HCI_EVENT_PACKET && size >= 6
        && packet[0] == 0x0F                                   /* Command Status  */
        && packet[2] == 0                                      /* accepted        */
        && packet[4] == 0x1B && packet[5] == 0x04               /* opcode 0x041B  */
        && gLastConnHandle != 0 && gPacketHandler != NULL) {
        BT_InjectSyntheticFeatures(gLastConnHandle);    /* v18.2: shared with the re-assert */
    }

    BT_Pump();
    /* Poll AFTER the pump. Mirroring only from the event handler left the block a
     * pump behind, so the first run reported "run-loop iterations 1" when the real
     * count was 2 -- a stale number in the one place we look. */
    BT_StackPoll((unsigned long)hci_get_state());
}

/* ---- upward: "that packet has gone" ------------------------------------- *
 * ⚠ THIS IS REQUIRED, and its absence is what stalled the first hardware run.
 *
 * hci.c decides we are an ASYNCHRONOUS transport by a single test:
 *     static int hci_transport_synchronous(void){
 *         return hci_stack->hci_transport->can_send_packet_now == NULL;
 *     }
 * We do provide can_send_packet_now, so BTstack keeps ownership of the packet
 * buffer after send_packet returns and waits to be told the transfer finished.
 * Until it is told, hci_packet_buffer_reserved stays true and it cannot build the
 * next command.
 *
 * Observed exactly that: BTstack sent HCI_Reset, the controller answered Command
 * Complete with status 0, and then nothing -- one packet each way, state stuck at
 * INITIALIZING. event_handler already calls hci_run() at its end, so the stack was
 * being asked to run; it simply had no buffer to run with.
 *
 * The event carries no parameters: code 0x6E, length 0. */
void BT_NotifyPacketSent(void)
{
    static unsigned char sent_event[2] = { 0x6E /* HCI_EVENT_TRANSPORT_PACKET_SENT */, 0 };

    if (gPacketHandler == NULL) return;
    gTxDone++;
    gPacketHandler(HCI_EVENT_PACKET, sent_event, 2);
    BT_Pump();
    BT_StackPoll((unsigned long)hci_get_state());
}

/* ---- hci_transport_t --------------------------------------------------- */

static void transport_init(const void *transport_config)
{
    (void)transport_config;
    /* Nothing to do. The USB Expert has already matched the device, M0.1 has
     * opened all three pipes and armed both readers by the time BTstack exists.
     * There is no device to open here and no baud rate to negotiate. */
}

static int transport_open(void)
{
    /* Also nothing: the pipes are open before BTstack is initialised, because
     * the Expert drives our lifetime and not the other way round. Returning 0
     * for success is what BTstack expects. */
    return 0;
}

static int transport_close(void)
{
    gPacketHandler = NULL;
    /* The pipes belong to ProbeFinalize, which the Expert calls on removal.
     * Aborting them here would fight that ownership. */
    return 0;
}

static void transport_register_packet_handler(
        void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size))
{
    gPacketHandler = handler;
}

/* BTstack asks before every send. Answering honestly is what stops it queueing
 * into a parameter block that is still in flight: each USBPB can carry exactly
 * one transaction at a time, so "free" means the previous completion has run. */
static int transport_can_send_packet_now(uint8_t packet_type)
{
    switch (packet_type) {
    case HCI_COMMAND_DATA_PACKET: return BT_CanSendCommand();
    case HCI_ACL_DATA_PACKET:     return BT_CanSendACL();
    default:                      return 0;   /* no SCO: HID needs none */
    }
}

static int transport_send_packet(uint8_t packet_type, uint8_t *packet, int size)
{
    long err;

    if (size <= 0) return -1;

    switch (packet_type) {
    case HCI_COMMAND_DATA_PACKET:
        if (!BT_CanSendCommand()) { gTxRefused++; return -1; }
        /* ★ v17.8: feed the inquiry's clock offset + page-scan mode into a Create_Connection
         * (opcode 0x0405) so an old CSR dongle pages the device at its scan phase instead of
         * blind. BTstack hardcodes clock offset 0 (hci.c:7924), which runs out the 10.24 s
         * pairing-window page timeout on the DBT-120's slow page scan -- the 0x04 PAGE TIMEOUT
         * of runs 4-8. Command layout: opcode(2) len(1) BD_ADDR(6) PacketType(2) PSRM(1)
         * Rsvd(1) ClockOffset(2) RoleSwitch(1), so PSRM is packet[11] and ClockOffset (LE) is
         * packet[13..14]. Patched only when the inquiry saw this address; otherwise BTstack's
         * values stand. ⚠ a1044pair1: the A1044 now pages a mouse from Pair as well, so this
         * runs there too (it always did for the SDP query's connections). */
        if (size >= 16 && packet[0] == 0x05 && packet[1] == 0x04 && packet[2] == 0x0D) {
            unsigned char  psrm = 0;
            unsigned short clk  = 0;
            if (BT_PageParamsFor(&packet[3], &psrm, &clk)) {
                packet[11] = psrm;
                packet[13] = (unsigned char)( clk       & 0xFF);
                packet[14] = (unsigned char)((clk >> 8) & 0xFF);
            }
        }
        gTxCmd++;
        err = BT_SendHCICommand(packet, (unsigned long)size);
        break;
    case HCI_ACL_DATA_PACKET:
        if (!BT_CanSendACL()) { gTxRefused++; return -1; }
        gTxAcl++;
        /* ★★★★★ v9.1: CAPTURE WHAT WE SEND. Five driver versions have analysed the
         * peer's silence and not one has looked at the bytes we send it.
         *
         * ⚠⚠ THE ASYMMETRY WAS THE BLIND SPOT. gAclCap is filled in BT_DeliverPacket,
         * the RECEIVE path, so every capture so far has been inbound -- three
         * Connection Requests from the keyboard, over and over. Meanwhile
         * `ACL packets sent 6` with 6 completions and status 0 says we answer every
         * one of them and our bytes reach the wire. So the peer receives something
         * from us and stops, and "something" has never been examined.
         *
         * ⇒ The obvious candidate is a Connection Response carrying result PENDING
         * (0x0001) rather than SUCCESS (0x0000). L2CAP sends pending when it must
         * defer for security, and a pending response obliges us to send a FINAL
         * response later -- which, if security never resolves, we never do. The peer
         * would then be waiting correctly and the fault would be entirely ours.
         * `last open status 0x69` (L2CAP RTX timeout) is consistent with that.
         *
         * Captured BEFORE the send so a refused write still shows what we tried.
         * Same slot geometry and byte packing as the inbound capture, so BTCheck's
         * existing ACLB() decoder reads both with one helper. */
        if (gAclTxCapCount < kAclCapSlots) {
            int i, n = size;
            if (n > kAclTxCapBytes) n = kAclTxCapBytes;
            for (i = 0; i < n; i++) gAclTxCap[gAclTxCapCount][i] = packet[i];
            gAclTxCapLen[gAclTxCapCount] = (unsigned long)size;   /* FULL length */
            gAclTxCapCount++;
        }
        err = BT_SendACL(packet, (unsigned long)size);
        break;
    default:
        return -1;
    }

    /* Whether the USL accepted the transaction is a USB question, so bt_probe.c
     * answers it with the same immediateError() rule the rest of the driver uses.
     * This file deliberately holds no USL error constants of its own. */
    return BT_SendWasAccepted(err) ? 0 : -1;
}

/* UART-only extensions. NULL is the documented way to say "not applicable", and
 * they are listed explicitly rather than omitted so it is clear the omission is
 * deliberate. set_sco_config stays NULL because HID has no SCO. */
static const hci_transport_t hci_transport_os9 = {
    "OS9-USB-H2",
    &transport_init,
    &transport_open,
    &transport_close,
    &transport_register_packet_handler,
    &transport_can_send_packet_now,
    &transport_send_packet,
    NULL,       /* set_baudrate  -- USB, not UART */
    NULL,       /* reset_link    -- H5/BCSP only  */
    NULL,       /* set_sco_config -- no SCO       */
};

const hci_transport_t * hci_transport_os9_instance(void)
{
    return &hci_transport_os9;
}
