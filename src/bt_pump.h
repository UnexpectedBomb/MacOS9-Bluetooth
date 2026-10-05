/*
 *  bt_pump.h  --  the seam between the OS 9 USB transport and BTstack.
 *
 *  Three small contracts, deliberately in their own header so neither side has
 *  to include the other's world: bt_probe.c knows nothing about BTstack's
 *  headers, and the BTstack glue knows nothing about USB.h.
 */
#ifndef OS9BT_PUMP_H
#define OS9BT_PUMP_H

/* ---- provided by btstack_run_loop_os9.c -------------------------------- */

/* Perform ONE run-loop iteration. Called from the USB completions in
 * bt_probe.c, which is the only thing that drives BTstack forward on this
 * platform. Safe to call from secondary interrupt level; not reentrant, and it
 * drops and counts a nested call rather than corrupting BTstack's lists. */
void BT_Pump(void);

/* Milliseconds, from Microseconds(). Also BTstack's HAVE_EMBEDDED_TIME_MS hook. */
unsigned long hal_time_ms(void);

/* ---- provided by hci_transport_os9.c ----------------------------------- */

/* Feed one HCI packet up into BTstack. Called from the interrupt-IN completion
 * for events and the bulk-IN completion for ACL data.
 *   type: HCI_EVENT_PACKET (0x04) or HCI_ACL_DATA_PACKET (0x02)
 * The buffer must stay valid only for the duration of the call. */
void BT_DeliverPacket(unsigned char type, unsigned char *packet, unsigned short size);

/* Tell BTstack a send has completed. REQUIRED for an asynchronous transport:
 * without it BTstack never releases its packet buffer and stops after one
 * command. Call from the command and bulk-OUT completions, AFTER clearing the
 * busy flag so can_send_packet_now answers true when hci_run asks. */
void BT_NotifyPacketSent(void);

/* ---- counters, read out through the driver's counter block ------------- */
extern unsigned long gPumpCalls;        /* iterations performed              */
extern unsigned long gPumpReentered;    /* nested calls dropped by the guard */
extern unsigned long gTxCmd, gTxAcl;    /* packets handed to the transport   */
extern unsigned long gTxDone;           /* sends reported complete to BTstack */
extern unsigned long gTxRefused;        /* send attempted with no pipe free  */
extern unsigned long gRxEvent, gRxAcl;  /* packets delivered upward          */

/* ---- M2b: L2CAP / inquiry / SDP, provided by bt_btstack.c --------------- *
 * Mirrored into the counter block by BT_StackPoll() in bt_probe.c, which owns
 * the word numbering. ⚠ Every *Status / *Rc here is stored as 0x100 | value so
 * that a zero word means "never received" rather than "succeeded" -- the M1
 * kRecInqComplete ambiguity, fixed structurally. */
/* ⚠ The recovery after a refused start. A non-zero value means BTstack was told an
 * inquiry was still active and we asked it to cancel -- see BT_ScanStart. */
extern unsigned long gInqStopRc;

/* ---- M4 step 1: the incoming HID control channel (docs/M4-DESIGN.md §10) --------
 * ⚠ Deliberately granular. "No keyboard connected" has at least five distinct causes --
 * the listen was refused, nothing paged us, something paged us on the wrong PSM, we
 * accepted and the channel failed to open, or it opened and closed again -- and one
 * boolean cannot separate them. Each of these answers exactly one of those. */
extern unsigned long gHidListenRc, gHidIncoming, gHidAccepted, gHidDeclined;
extern unsigned long gHidOpened, gHidClosed, gHidOpenStatus, gHidLastPsm;
extern unsigned long gHidCtrlCid, gHidPeerHi, gHidPeerLo;
/* ★★★★★ v8.6: THE REPORT-PROTOCOL EXPERIMENT.
 *
 * Every previous LEVEL_0 run opened the HID control channel and then SAID NOTHING --
 * "incoming connections 2, accepted 2, 4 ACL packets sent and completed" and then the
 * peer went quiet and the channel died at 0x69 RTX timeout. BTCheck's own text has
 * named the missing step all along: a HID device that has accepted a control channel
 * is waiting for SET_PROTOCOL.
 *
 * ⭐ And we send HIDP_SET_PROTOCOL_REPORT (0x71), not _BOOT (0x70). REPORT protocol is
 * where the Consumer page (volume, eject) and the battery-strength feature report live;
 * boot protocol is the flattened 8-byte form the card's proxy already gives us and the
 * reason those keys do not work. Same keyboard, its other personality. */
extern unsigned long gHidIntrListenRc, gHidIntrCid, gHidIntrOpened;
extern unsigned long gHidSetProtoTried, gHidSetProtoRc, gHidHandshake;
extern unsigned long gHidCtrlDataPkts, gHidIntrDataPkts;
extern unsigned long gAutoAuthTried, gAutoAuthRc, gAutoAuthHandle;
/* v8.9: block freshness. See BT_PublishFromTimer -- on a quiet radio nothing else
 * refreshes the block, and a frozen block reads exactly like a hardware answer. */
extern unsigned long gPublishRuns, gPublishMs;
void BT_PublishFromTimer(void);
/* v9.2: the Authentication Complete ring. 4 is enough -- links arrive one per
 * keypress burst and three per run has been typical. Packed
 * 0x1000000 | (status << 16) | handle, so an unfilled slot is distinguishable from
 * a genuine status 0 on handle 0. */
#define kAuthRingSlots 4
extern unsigned long gAuthRing[kAuthRingSlots], gAuthRingMs[kAuthRingSlots],
                     gAuthRingCount;
/* v9.0: the controller's own Scan_Enable, read back rather than assumed. Bit 1 is
 * page scan -- without it a bonded keyboard cannot reach us at all, and "nothing
 * paged us" could not be told from "we were not listening". */
extern unsigned long gScanEnaRc, gScanEnaSends, gScanEnaFirst, gScanEnaLast,
                     gScanEnaReads;
void BT_TryScanEnableProbe(void);
extern unsigned long gL2Init;           /* 1 = l2cap_init, 2 = + sdp_client_init */
extern unsigned long gInqState;         /* 0 none, 1 started, 2 complete         */
extern unsigned long gInqStartRc, gInqResults, gInqComplStatus;
extern unsigned long gTgtAddrHi, gTgtAddrLo, gTgtCoD, gTgtPick;
extern unsigned long gHciConnStatus, gHciConnHandle;
extern unsigned long gSdpIssued, gSdpQueryRc, gSdpAttrBytes, gSdpRecords;
extern unsigned long gSdpComplete, gSdpStatus;
extern unsigned long gL2capEvts, gLastUnkEvt;
extern unsigned long gStoredKeyRc, gStoredKeyCap;
/* v6.5's gate instrument -- see BT_TryStoredKeyProbe for why each one exists. */
extern unsigned long gSendRc, gGateFirst, gGateLast, gAclSlots;
extern unsigned long gConnPeerHi, gConnPeerLo;
void BT_SampleSendGates(void);
/* v6.6. ⚠ gRlkAddr holds ADDRESSES ONLY; the link keys in that event are secrets and
 * are deliberately never copied out of it. See the 0x15 case in bt_btstack.c. */
extern unsigned long gRlkEvents, gRlkKeys, gRlkAddr[4][2];
/* ★★ v8.2: the store is read MORE THAN ONCE now -- a delete re-reads it, so one boot
 * can show the count before and after. gRlkAddr therefore describes a PASS, and the
 * 0x15 handler clears it when the pass number changes. Without that the second read
 * would APPEND to the first and the panel would keep showing an address the card no
 * longer holds -- the deleted row would never disappear. */
extern unsigned long gRlkPasses;         /* Read_Stored_Link_Key sends ACCEPTED       */
extern unsigned long gStoredKeyCapFirst; /* the bring-up read, kept across a re-read  */
extern unsigned long gConnReqs, gConnReqHi, gConnReqLo, gConnReqCoD, gConnReqType;
/* ★★★ v8.2: EVERY address that has PAGED US, not just the most recent one.
 *
 * ⚠ This exists because of a gap that would have made the A1016 test impossible. The
 * panel builds its rows from three sources: inquiry responses, our own key file, and
 * the addresses the CARD has keys for. Delete the A1016's stored key -- the whole
 * point of the test -- and it is in none of them, so there is no row to select and
 * no way to click Pair on it.
 *
 * ⭐ And a keyboard that has lost its host PAGES; it does not sit waiting to be
 * discovered. The v91 run is the proof: the A1016 paged us (kWConnReqs 1, CoD
 * 0x002540) in the very same run where the inquiry found no peripheral at all. The
 * one source that sees it was the one source the panel could not draw. */
#define kPagedSlots 4
extern unsigned long gPagedCount;             /* distinct addresses that paged us     */
extern unsigned long gPagedAddr[kPagedSlots][3];   /* hi, lo, class-of-device         */
extern unsigned long gAuthComps, gLkNotifs, gLastCmdStatus;
/* v6.7's event ring, filled in hci_transport_os9.c's BT_DeliverPacket -- see the note
 * there for why it lives in the transport and not in the BTstack handler. */
#define kEvtRingLen 16
extern unsigned long gEvtRing[kEvtRingLen], gEvtRingIdx, gEvtTotal;
extern unsigned long gRrsfCount, gRrsfStatus, gRrsfHandle;
/* ★★★★★ v8.7: THE TWO THINGS THREE WRONG THEORIES ABOUT THIS LINK ALL LACKED.
 *
 * 1. WHAT THE PEER ACTUALLY SAID. The v8.6 run measured `ACL read completions 4` --
 *    the A1016 sent us four packets while the channel was failing to open -- and we
 *    have no idea what was in them, because L2CAP SIGNALLING travels on CID 1 and goes
 *    to BTstack internally. It never reaches our packet handler, so gHidFirstData
 *    (which only sees channel data) captured nothing. The TRANSPORT sees every ACL
 *    packet, and that is the only place this can be caught.
 *
 * 2. HOW LONG THE LINK LIVED. Disconnect reason 8 is a SUPERVISION TIMEOUT, and
 *    whether the link lasted 2 seconds or 20 splits the hypothesis space in half: a
 *    20-second life means the peer stopped answering at the baseband after a normal
 *    negotiation attempt, a 2-second one means something killed it. There has never
 *    been a timestamp on any of these events.
 *
 * ⚠ hal_time_ms() rather than TickCount(): it is Microseconds()-based, already used by
 * BTstack on this platform, and proven safe at this level.
 * [[reference_os9_delay_granularity_trap]] */
#define kAclCapSlots 4
#define kAclCapBytes 16
extern unsigned long gAclCapCount;
extern unsigned char gAclCap[kAclCapSlots][kAclCapBytes];
/* v9.1: the OUTBOUND capture, the mirror the inbound one never had. Every run since
 * v8.7 has examined what the peer says and none has examined what we answer.
 *
 * ⚠⚠ WIDER THAN THE INBOUND CAPTURE, ON PURPOSE, and the first draft got this wrong.
 * The field this instrument exists to read is the Connection Response RESULT, which
 * sits at BYTE 16 -- exactly one past the end of a 16-byte capture. Shipping that
 * would have spent a hardware boot capturing everything except the answer. 24 bytes
 * reaches RESULT (16..17) and STATUS (18..19) with room for a Config Request's
 * options. The inbound constant stays 16 so the block's existing word numbering does
 * not move. */
#define kAclTxCapBytes 24
extern unsigned long gAclTxCapCount;
extern unsigned char gAclTxCap[kAclCapSlots][kAclTxCapBytes];
extern unsigned long gAclTxCapLen[kAclCapSlots];
extern unsigned long gLinkUpMs, gLinkDownMs, gLinkLifeMs;
extern unsigned long gRrsfSentMs, gRrsfDoneMs;
/* ---- v6.8: the LEVEL_0 probe and the HID data path ---------------------------- */

/* ⚠⚠ TWO GATES, AND THE FIRST VERSION WRONGLY MADE THEM ONE.
 *
 * v6.8 shipped them behind a single switch with a comment claiming "neither works
 * alone". THE MEASUREMENT REFUTED THAT, and the two turned out to pull in opposite
 * directions:
 *
 *   kHidSynthFeatures  is the load-bearing half. Injecting the
 *                      Read_Remote_Supported_Features completion the A1044 withholds
 *                      sets BONDING_RECEIVED_REMOTE_FEATURES, which is what releases
 *                      BOTH l2cap.c's WAIT_REMOTE_SUPPORTED_FEATURES state AND
 *                      hci.c:8096's authentication gate. It works alone.
 *
 *   kHidLevel0         was called COUNTERPRODUCTIVE, and ⚠⚠ THAT VERDICT RESTS ON A
 *                      PREMISE NOW KNOWN TO BE FALSE -- see the correction block at the
 *                      end of this comment. At LEVEL_0 nothing calls
 *                      gap_request_security_level, so no authentication is requested,
 *                      the link is never encrypted -- and the A1016, said to be paired
 *                      under SSP, then refuses to answer our L2CAP Connection Response.
 *                      Measured: incoming connections 2, accepted 2, 4 ACL packets
 *                      sent and completed, and the channel died with 0x69
 *                      L2CAP_CONNECTION_RESPONSE_RESULT_RTX_TIMEOUT -- the peer heard
 *                      us and said nothing. LEVEL_0 removed the very thing the
 *                      keyboard requires.
 *
 * ⇒ The combination worth running is LEVEL_2 *with* the synthetic event: security IS
 * requested, authentication can now proceed past the features gate, and the
 * controller already holds this keyboard's link key (Max_Num_Keys 16,
 * Num_Keys_Read 2) so it may authenticate without asking us at all.
 *
 * ⚠ All-zero synthetic features stays load-bearing at LEVEL_2 too: with the SSP bit
 * clear, l2cap.c:2469's Security-Mode-4 disconnect of an unencrypted non-SDP PSM is
 * skipped, so raising the level cannot trip it before authentication runs.
 *
 * ⚠⚠⚠ kHidSynthFeatures MUST BE 0 IN ANYTHING THAT SHIPS: it feeds the stack an event
 * the controller never sent. That has not changed and never will.
 *
 * ⚠⚠ BUT THE SECOND HALF OF THIS VERDICT IS RETRACTED (v9.8). It read: "kHidLevel0
 * must be 0 in anything that ships too, and on this evidence there is no reason to set
 * it to 1 again at all." Both clauses are now contradicted by direct measurement of
 * the working reference on THIS card and THIS keyboard:
 *     defaults read com.apple.Bluetooth -> AuthenticationEnable 0, EncryptionEnable 0
 *     system_profiler                   -> "Requires Authentication: No"
 * and Tiger reconnects the A1016 in 2-3 s with volume and eject working. LEVEL_0 is
 * not a diagnostic shortcut; it is what the reference implementation does.
 *
 * ⇒ kHidLevel0 = 1 IS THEREFORE A SHIPPING CANDIDATE, and the v9.7 evidence says the
 * opposite setting cannot ship at all: LEVEL_2 wedges the LMP channel on this
 * controller (see bt_btstack.c:1843) and no link survives it.
 *
 * ⚠ THE TRADE-OFF IS REAL AND IS THE USER'S CALL, NOT MINE: an unauthenticated,
 * unencrypted HID link means keystrokes cross the air in the clear, readable by
 * anything listening nearby. Apple shipped exactly that on this hardware, so matching
 * it is defensible and is the stated goal of this project -- but it is a security
 * property of the product and belongs in docs/RELEASE-GATES.md as a decision, not
 * buried in a header comment.
 *
 * ===========================================================================
 *  ★ BOTH GATES ARE NOW 0 (v8.4), AND HERE IS THE PREDICTION THAT MAKES IT
 *    A TEST RATHER THAN A HOPE
 * ===========================================================================
 *
 * ⚠ THE RISK IS REAL AND WORTH STATING PLAINLY. Everything above was written for M4,
 * the HID host role -- but the synthetic event releases hci.c:8096's AUTHENTICATION
 * gate as well, and authentication is the path the goal was proven through. The v94
 * run that paired the A1016 shows `0x0B arrivals 1, of which SYNTHESISED 1`. The
 * successful pairing DID have this event in it. Whether it DEPENDED on it is exactly
 * what this build asks.
 *
 * ⭐ PREDICTION: PAIRING SURVIVES. BT_PairDevice does not go through gap_*; it sends
 * Authentication_Requested with a raw hci_send_cmd:
 *
 *     hci_send_cmd(&hci_authentication_requested, gLastConnHandle)
 *
 * That is a direct command submission, so hci.c's authentication gate -- the thing
 * BONDING_RECEIVED_REMOTE_FEATURES releases -- is not in our path at all. The gate
 * governs BTstack DECIDING to authenticate; we never ask it to decide.
 *
 * ⭐ PREDICTION: M4 STALLS, and that costs nothing. Without the injection l2cap.c
 * stays in WAIT_REMOTE_SUPPORTED_FEATURES, so the HID channel will not open. It never
 * did anyway (0x69 RTX timeout), and the proxy -- not M4 -- is what makes a paired
 * keyboard type. Losing a path that never worked, in exchange for shipping honest
 * code, is not a loss.
 *
 * ⚠ It also will NOT hit l2cap.c:2469's Security-Mode-4 disconnect: with no features
 * event at all, l2cap never reaches that check, it waits before it. Stalls, does not
 * disconnect.
 *
 * ⇒ DISCRIMINATOR: a fresh pairing with `0x0B arrivals 0 / SYNTHESISED 0` beside
 * `legacy PIN requests 1 / ANSWERED 1` and `put_link_key 1`. If pairing instead dies
 * with no Auth Complete, the prediction is refuted and the raw-send reasoning above is
 * wrong -- say so and read hci.c:8096 properly rather than reaching for the gate.
 *
 * ===========================================================================
 *  ✅ VALIDATED 2026-09-07, AND IT REFUTED THE GATE'S OWN PREMISE
 * ===========================================================================
 *
 * ⭐ THE PREDICTION HELD. A Pixel paired cleanly on driver v8.4 with the gate at 0:
 * `legacy PIN requests 1 / ANSWERED 1`, `link key requests 0`, `Auth Complete 0`,
 * `encryption enabled 1`, `put_link_key 1`, `records written 3`, and
 * `Write_Stored_Link_Key sent 1 / keys written 1`. The raw hci_send_cmd path does not
 * pass through hci.c's authentication gate, exactly as argued.
 *
 * ⚠⚠ AND EVERYTHING ABOVE ABOUT "the completion the A1044 withholds" IS FALSE. That
 * run recorded `0x0B arrivals 3` with ZERO synthetic events. The card sends them.
 *
 * The claim came from BTCheck printing gSynthRrsf as "of which SYNTHESISED" beside
 * gRrsfCount, as though it were a subset. They are independent counters -- one counts
 * real events at the transport's entry point, the other counts direct injections that
 * never pass through it. "arrivals 3, of which SYNTHESISED 3" actually meant three
 * REAL completions AND three fabricated duplicates: the stack was fed each one twice,
 * and the log read as though the card had sent none. One command in, one completion
 * from the card, one injection from us -- which is why the two numbers always matched.
 *
 * ⇒ SO THE GATE WAS NEVER FILLING IN A MISSING EVENT. It was supplying a DUPLICATE
 * with all-zero feature bytes to make l2cap believe the peer had no SSP, dodging
 * l2cap.c:2469's Security Mode 4 disconnect. Not "the card is silent" but "lie about
 * the peer" -- a much stronger reason it could never ship. Both gates are 0.
 *
 * ===========================================================================
 *  ⚠⚠⚠ CORRECTION 2026-09-08: TWO PREMISES IN THIS FILE WERE FALSE, AND THE
 *      REAL M4 BLOCKER IS A DEADLOCK IN BTSTACK, NOT A MISSING EVENT
 * ===========================================================================
 *
 * FALSE PREMISE 1 -- "the completion the A1044 withholds". The card sends it. What it
 * sends is `0x0B ... status 0x02` = ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER, and it
 * arrives AFTER the link has already gone. Measured, event ring, twice identically:
 *
 *     0x04 Connection Request -> 0x03 Connection Complete (st 0, handle 0x002E)
 *     -> 0x0F Command Status (our 0x041B accepted)
 *     -> 0x05 DISCONNECTION COMPLETE          <== the link dies here
 *     -> 0x0B features complete, status 0x02  <== answer arrives after, handle gone
 *
 * FALSE PREMISE 2 -- "the A1016, paired under SSP". ⚠ IMPOSSIBLE ON THIS CARD. It is
 * HCI 2 / LMP 2 = Bluetooth 1.2, and SSP needs LMP 4. There is no SSP pairing anywhere
 * in this system, so the reason given for abandoning LEVEL_0 cannot be the real one.
 *
 * ⇒ THE ACTUAL BLOCKER IS A CIRCULAR WAIT, entirely inside BTstack's policy:
 *
 *     l2cap has an incoming HID channel at LEVEL_2, so it calls
 *       gap_request_security_level        (measured: security requests made 2)
 *         -> sets BONDING_SEND_AUTHENTICATE_REQUEST
 *           -> hci.c:8096 REFUSES to authenticate until
 *              BONDING_RECEIVED_REMOTE_FEATURES is set
 *             -> so hci.c:8117 sends Read_Remote_Supported_Features first
 *               -> which never completes while the link is alive
 *                 -> link supervision expires, disconnect reason 8
 *                   -> and hci.c's 0x0B handler then bails at
 *                      `if (!conn) break;` because the Disconnection Complete
 *                      already freed the connection object
 *
 * ⚠ BTstack's error handling is NOT at fault: its 0x0B case calls
 * hci_handle_remote_features_received even on a nonzero status -- the `if (!packet[2])`
 * guards only the PARSING. The flag would have been set. The connection was simply
 * gone by then.
 *
 * ⇒ AND THE FEATURES READ IS NOT REQUIRED BY THE PROTOCOL. Apple's own OS 9 stack,
 * on EVT_CONN_COMPLETE, sends an L2CAP Connection Request for PSM 1 IMMEDIATELY --
 * no Read_Remote_Supported_Features, no Authentication_Requested, no authentication at
 * all (vendor/bt-control-center/USB Bluetooth/HCI/HCI_Events.c). It is BTstack policy,
 * not a requirement. ⚠ Apple's stack also has NO Connection Request handler, so it
 * never accepts an incoming link -- there is no OS 9 prior art for the responder path.
 *
 * ⇒ SO THE SYNTHETIC EVENT WAS A DEADLOCK BREAKER, not a substitute for a missing
 * event. That is why it was load-bearing, and why removing it restored the stall. It
 * must still never ship -- but the thing to replace it with is a policy change, not a
 * lie.
 *
 * ⏭ THE EXPERIMENT THAT HAS NEVER BEEN RUN, and it is one build:
 *   LEVEL_0 (Apple's shape: no auth, straight to L2CAP) *TOGETHER WITH* sending
 *   SET_PROTOCOL on the control channel when it opens. Every previous LEVEL_0 run
 *   opened the channel -- "incoming connections 2, accepted 2, 4 ACL packets sent and
 *   completed" -- and then SAID NOTHING, so of course the peer went quiet and the
 *   channel died at 0x69 RTX timeout. BTCheck's own text has said so all along: "for a
 *   HID device usually means it is waiting for SET_PROTOCOL, the step 2 this build does
 *   not send yet."
 *   ⭐ And send HIDP_SET_PROTOCOL_REPORT (0x71), not _BOOT (0x70): report protocol is
 *   where the Consumer page and the battery feature report live. See bt_bootreport.h. */
/* ⚠⚠⚠ kHidLevel0 IS 1 IN THIS BUILD -- A DIAGNOSTIC, NOT SHIPPABLE.
 *
 * At LEVEL_0 the HID channels demand no security, so l2cap never calls
 * gap_request_security_level, nothing sets BONDING_SEND_AUTHENTICATE_REQUEST, hci.c
 * never needs remote features, and THE DEADLOCK CANNOT FORM. That is the point: it is
 * Apple's own shape (their OS 9 stack authenticates nothing and goes straight to
 * L2CAP), and it is the only configuration in which the control channel has ever
 * actually opened on this hardware.
 *
 * ⚠ The link is then UNENCRYPTED, which is why this must go back to 0 before anything
 * ships. A HID link carrying keystrokes in clear is not a shippable default.
 * ⚠ And kHidSynthFeatures STAYS 0 -- the whole point is to remove the deadlock
 * legitimately rather than fabricate an event to break it. */
#define kHidSynthFeatures 0
/* ★★★★★★ v12.7: kHidLevel0 GOES TO 0, AND THE PREMISE HAS FINALLY CHANGED.
 *
 * ⚠⚠ THE COMMENT ABOVE IS STILL TRUE ABOUT v9.6 AND v9.7 AND IS NOT BEING WAVED AWAY.
 * LEVEL_2 really did deadlock then, and LEVEL_0 really was the only configuration in
 * which the control channel ever opened. What changed is the thing those runs could not
 * have had: A LINK KEY.
 *
 * Every one of those experiments ran with NO BOND IN EXISTENCE anywhere -- not on the
 * card, not in our database, not on the keyboard. Asking for LEVEL_2 in that state does
 * not "turn on encryption"; it forces a whole pairing mid-link, which is what needed
 * remote features, which is what deadlocked. The 2026-09-15 run changed the world:
 *
 *     inquiry responses 2, of those PERIPHERAL 1     the scan FOUND the keyboard
 *     legacy PIN requests 4 / ANSWERED 4             0000, four times
 *     Link Key Notifications 4, put_link_key 4
 *     key stored for 00:0A:95:xx:xx:xx               ⭐ THE A1016 IS BONDED TO US
 *     Encryption Change 0 = success
 *
 * ⇒ AND THEN IT STILL DID NOT TYPE, for a reason the log states exactly:
 *     page requests (0x04)   11      it pages us now, repeatedly
 *     Connection Requests    11      the ACL link comes up every time
 *     incoming connections    0      no HID channel EVER arrives
 *     control channels OPEN   0
 *     disconnect reason      19      0x13, the REMOTE side hung up
 *     Auth Completes          1      the pairing only -- none of the 11 reconnects
 * The keyboard connects, waits for an encrypted link, does not get one, and leaves.
 * Eleven times. That is precisely what the retracted paragraph at the top of this file
 * predicted, measured on a keyboard that is now properly bonded.
 *
 * ⇒ With a key held by the keyboard, by us, and by the card, a security request should
 * trigger ENCRYPTION from the stored key rather than a fresh pairing -- a different and
 * far cheaper operation than the one that deadlocked.
 *
 * ⚠ THIS IS AN EXPERIMENT AND ITS FAILURE MODE IS KNOWN AND BOUNDED: if the deadlock
 * returns, the control channel stops opening -- which is where we already are, since it
 * has never opened for this keyboard. There is nothing to lose that is not already lost,
 * and [[feedback_retracted_claims_are_a_todo_list]] says a retracted claim is untested
 * rather than closed. This tests it. */
#define kHidLevel0        0
/* ★★★★★ v8.8: AUTHENTICATE AN INBOUND LINK OURSELVES.
 *
 * The v8.7 run answered the question three theories had missed. Per link the A1016
 * sends EXACTLY ONE packet -- a well-formed L2CAP Connection Request for PSM 0x0011,
 * id 0x27, SCID 0x005B -- we answer it with two ACL packets that reach the wire, and
 * IT NEVER SPEAKS AGAIN. The link then lives out the full supervision window,
 * 20004 ms to the millisecond, and times out.
 *
 * ⚠ That also corrected me: on v8.6 I read `ACL read completions 4` as "the peer
 * answered four times" and called the wants-encryption theory refuted. That run had
 * FOUR LINKS. Four links x one packet each is not a conversation, and the per-link
 * view shows the pattern plainly.
 *
 * ⇒ The HID profile has a Host authenticate and encrypt BEFORE opening the control
 * channel, and this keyboard enforces it: it gets our Connection Response on an
 * unencrypted link and declines to continue.
 *
 * ⭐ SO NEITHER SECURITY LEVEL IS THE ANSWER:
 *     LEVEL_2 -> the link would be encrypted, but hci.c gates authentication on
 *                remote features, that read outlives the link, deadlock.
 *     LEVEL_0 -> no deadlock and the incoming connection is finally VISIBLE, but
 *                nothing encrypts the link and the keyboard refuses.
 *
 * ⇒ Keep LEVEL_0 for the gate, and drive authentication OURSELVES with a raw
 * hci_send_cmd -- which bypasses hci.c:8096 entirely, exactly as BT_PairDevice
 * already does. Encryption then follows with no help from us: BTstack sets
 * BONDING_SEND_ENCRYPTION_REQUEST itself on Authentication Complete status 0
 * (hci.c:5064), so one command is the whole change.
 *
 * ⭐ And every piece is already proven ON THIS KEYBOARD: that same raw send produced
 * `Auth Complete 0` and `encryption enabled 1` during the successful pairing, and its
 * link key is in the card's own store, so no PIN is needed.
 *
 * ⚠ DIAGNOSTIC. It authenticates, unprompted, any link whose peer has PAGED us this
 * session -- see the ring test at the fire site for why membership in gPagedAddr[] is
 * the condition and what it does and does not exclude. Authenticating on the user's
 * behalf is not a decision a shipping driver should make for them. Both this and
 * kHidLevel0 go to 0 before anything ships, and kProbeGateMarker now carries this
 * gate so the artifact says which build it is. */
#define kHidAutoAuth      0

/* ★★★★★ kHidLongSupervision -- STOP READING THE WRONG TIMEOUT.
 *
 * ⚠⚠ v9.2 measured the masking exactly. The link lived 20000 ms and the
 * Authentication Complete arrived at 1706356, two milliseconds AFTER the
 * Disconnection Complete at 1706354: the controller held our request for the whole
 * lifetime and closed it with 0x02, Unknown Connection Identifier.
 *
 * One fact, two opposite worlds, and no way to tell them apart:
 *   (a) the controller never started the LMP challenge, or
 *   (b) it did, and the KEYBOARD never answered it.
 * The LMP response timeout is 30 s; link supervision is 20 s. So (b) can never
 * surface -- the link always dies first and 0x22 LMP RESPONSE TIMEOUT is never
 * emitted. EVERY run in this project has been reading the shorter timeout and
 * mistaking its expiry for the answer.
 *
 * ⇒ Write 40 s of supervision on link-up, longer than the LMP timer, and the two
 * worlds separate themselves: 0x22 blames the keyboard, 0x02 blames the controller.
 *
 * ⚠ DIAGNOSTIC. A dead link held for 40 s is not shippable behaviour, and this goes
 * to 0 with kHidLevel0 and kHidAutoAuth before anything ships. kProbeGateMarker
 * carries it so the artifact says which build it is. */
#define kHidLongSupervision 0
extern unsigned long gSuperTimeoutTried, gSuperTimeoutRc, gSuperTimeoutHandle;

/* ★★★★★★ kHidLinkPolicy -- READ THE LINK POLICY, THEN PERMIT SNIFF.
 *
 * v9.3 delivered the verdict: Read_Remote_Supported_Features completes with status
 * 0x08 CONNECTION TIMEOUT, three times. The peer does not answer LMP on a link we
 * accepted. Six versions of security work were downstream of that.
 *
 * ⭐ The prior-art read found a mechanism that would explain it. BTstack never
 * assigns default_link_policy_settings a default, and Write_Default_Link_Policy is
 * only sent when gap_set_default_link_policy_settings() is called -- which we never
 * call. So every link runs on whatever policy the controller defaults to, and we
 * have never measured it. A HID keyboard enters SNIFF immediately (that is how a
 * 2003 keyboard gets months from four AAs); if policy disallows sniff the controller
 * must refuse LMP_sniff_req, and transactions queued behind that refusal time out.
 * Tiger works because any real host permits sniff. BTstack's own gap.h calls
 * role-switch|sniff "the common value".
 *
 * ⚠⚠ BUT "WE NEVER SET IT" IS NOT "IT IS 0", and that distinction is the whole
 * Scan_Enable lesson: page scan was assumed for six versions and only the read-back
 * settled it. So this READS the policy first and publishes what it finds, THEN
 * writes. If the controller already permitted sniff, the read says so and the
 * hypothesis dies without a second boot.
 *
 * ⚠ SNIFF ONLY (0x0004), not role switch -- role switch was eliminated by reading
 * at v9.3 and enabling it here would put the variable straight back in.
 *
 * ⚠ DIAGNOSTIC, and it goes to 0 with the others before anything ships. */
#define kHidLinkPolicy 0

/* ★★★★★★ kHidUseBtstackHost -- STOP RE-DERIVING A WORKING IMPLEMENTATION.
 *
 * vendor/btstack/src/classic/hid_host.c is 1440 lines, is listed at
 * vendor/btstack-files.txt:45, and has therefore been LINKED INTO EVERY BUILD THIS
 * PROJECT HAS EVER SHIPPED with hid_host_init() never once called. We hand-rolled
 * the same job and spent eleven versions re-deriving its decisions one command at a
 * time, getting several of them wrong: MTU 672 where it uses 0xffff, a forced
 * security level where it uses gap_get_security_level(), accepting PSM 0x11
 * immediately where it waits, and no notion of HID_v1.1.1 §5.2.2 (both control AND
 * interrupt channels shall always be opened).
 *
 * ⭐ hid_host_accept_connection() takes the PROTOCOL MODE, so passing
 * HID_PROTOCOL_MODE_REPORT is the Consumer-page unlock v8.6 built SET_PROTOCOL
 * (0x71) by hand to reach -- including the handshake and the boot fallback.
 *
 * ⚠⚠ SINGLE VARIABLE, DELIBERATELY. Every other diagnostic gate goes to 0 for this
 * run except kHidLongSupervision, which is kept because it is what makes an LMP
 * failure VISIBLE (0x22 or 0x08 instead of a link silently dying at 20 s) and
 * because leaving exactly ONE command in the v9.4 queue is itself a discriminator:
 * if that single command goes out, the 12/12 refusal was contention; if it is
 * refused again, the queue has a real bug independent of load.
 *
 * ⚠⚠ THIS PARAGRAPH WAS WRONG AND IT COST v9.6 AND v9.7. It said "kHidLevel0 becomes
 * IRRELEVANT when this is on: our registration never runs, and hid_host uses BTstack's
 * global level (default LEVEL_2). Set to 0 for clarity."
 *
 * ⇒ THE OPPOSITE IS TRUE. kHidLevel0 is MORE load-bearing with the canonical host, not
 * less, and it now reaches THREE sites:
 *     1. gap_set_security_level() before hid_host_init  -- v9.6; hid_host.c:1180-1181
 *        reads the global level AT REGISTRATION, so the ordering is load-bearing
 *     2. hid_host's two l2cap_register_service levels   -- consequence of 1
 *     3. the gap_request_security_level call on Connection Complete -- v9.8
 * Site 3 was hardcoded LEVEL_2 and silently overrode sites 1 and 2 for two builds:
 * L2CAP correctly answered SUCCESS at LEVEL_0 while that call separately demanded
 * LEVEL_2, wedging the LMP channel. See the long note at bt_btstack.c:1843.
 *
 * ⚠ THE LESSON, and it is the same one as v9.5: a gate is only as real as the sites it
 * actually reaches, and a comment asserting a gate is irrelevant is a claim about the
 * whole call graph. Grep for every use before believing it.
 *
 * ⚠ NOT purely diagnostic, unlike the others. If this works it is the SHIPPING
 * path -- less of our code, not more. */
#define kHidUseBtstackHost 1
extern unsigned char gHidDescStore[256];
extern unsigned long gBtstackHostInit, gBhIncoming, gBhCid, gBhAcceptRc;
extern unsigned long gGapLevel;
/* v9.7: what the controller claims for ACL buffering. Read beside the 0x13 count:
 * "claims N buffers and never returns one" is a different fault from "discarded". */
extern unsigned long gBufSizeRc, gAclBufLen, gAclBufNum;
extern unsigned long gBhOpened, gBhOpenedStatus, gBhOpenedMs;
extern unsigned long gBhClosed, gBhClosedMs, gBhSetProtoRsp, gBhDescAvail;
extern unsigned long gBhReports, gBhOtherEvts;
/* v13.8: the half-open watchdog. gBhIncomingMs is the accept-at timestamp that did not
 * exist before, so the accept->open gap is finally measurable from a log. */
extern unsigned long gBhIncomingMs, gBhFailedOpens, gBhFailedStatus;
extern unsigned long gBhReconnTried, gBhReconnRc;
/* v13.9: the maintained "a HID channel is open right now" flag. Read it; never derive it
 * from gBhCid, from gBhOpened/gBhClosed, or from the open/close timestamps -- all three
 * of those were tried and all three were wrong in different states. */
extern unsigned long gBhLive;
/* v14.1: the battery probe. Raw payloads, not decoded -- see the note in bt_btstack.c,
 * and note that report IDs 68/69 in the same table are DESTRUCTIVE and never sent. */
extern unsigned long gBatSends, gBatResponses;
extern unsigned long gBatPctRc, gBatPctHs, gBatPctLen, gBatPctVal;
extern unsigned long gBatStRc,  gBatStHs,  gBatStLen,  gBatStVal;

/* ★★★★★★ kHidOutgoingConnect -- THE CALL WE HAVE NEVER MADE.
 *
 * ⚠⚠ hid_host_accept_connection is the only HID connect call in bt_btstack.c.
 * hid_host_connect is never called, so this stack has only ever ACCEPTED a HID channel:
 * the device has to come to us. That single omission is the suspected cause of two
 * things this project has been writing down as facts about the hardware.
 *
 *   1. THE POWER-CYCLE STEP AFTER PAIRING. A freshly paired A1016 sits in pairing mode
 *      with no reason to page anyone. Measured 2026-09-16: incoming connections 0 after
 *      a successful pair, 1 after switching it off and on, nothing else changed. The
 *      power cycle does not fix the keyboard -- it turns it into a BONDED device, which
 *      reconnects on its own, and then we accept.
 *   2. THE CONNECT BUTTON. It clears our refusal and then waits, because nothing in the
 *      driver can page the device. I described that to the user as "the keyboard is the
 *      initiator on this stack", which was true of our implementation and which I
 *      presented as though it were true of Bluetooth.
 *
 * ⚠ WHAT TIGER DOES AFTER A FRESH PAIR IS NOT CAPTURED. docs/TIGER-HCI-TRACE.md is one
 * RECONNECT -- no PIN, no Link_Key_Request, no Link_Key_Notification -- and in it the
 * keyboard initiates, so Tiger is an acceptor on that path too. Nothing here may be
 * justified by "Tiger does X"; this is our hypothesis about our own omission.
 *
 * ⚠ A GATE, not an unconditional change, because it is a new outgoing L2CAP connection
 * on a card this project has knocked off the bus three times. 0 restores exactly the
 * behaviour every run to date has had, without a rebuild-from-scratch decision. */
#define kHidOutgoingConnect 1

/* v13.3: what the outgoing connect did. See kHidOutgoingConnect. */
extern unsigned long gHidOutTried, gHidOutRc, gHidOutCid;
/* v13.4: the fresh-bond latch. See the note at HCI_EVENT_LINK_KEY_NOTIFICATION. */
extern unsigned long gJustBonded, gHidOutArmed;
/* v13.7: key-file opens that FAILED -- see the long note in bt_keyfile.c. */
extern unsigned long gKfNoFile;

/* v9.4: the per-link command queue. See BT_LinkCmdEnqueue for why a burst of
 * commands from Connection Complete starved the auth send at v9.3. 6 slots is two
 * more than the four commands a link needs, so an overflow means a real defect
 * rather than a tight fit. kLinkCmdMaxTries bounds the retry PER ENTRY: an
 * unbounded retry on a dead link would spin for the life of the driver. */
#define kLinkCmdSlots     6
#define kLinkCmdMaxTries 50      /* 50 ticks x 100 ms = 5 s per entry */
#define kLinkCmdSupervision 1
#define kLinkCmdPolicyRead  2
#define kLinkCmdPolicyWrite 3
#define kLinkCmdAuth        4
extern unsigned char gLinkCmdQ[kLinkCmdSlots];
extern unsigned long gLinkCmdHead, gLinkCmdCount, gLinkCmdTries, gLinkCmdHandle;
extern unsigned long gLinkCmdEnq, gLinkCmdSent, gLinkCmdRefused, gLinkCmdDropped;
extern unsigned long gPolicyReadRc, gPolicyReadSends, gPolicyReadBack, gPolicyReads;
extern unsigned long gPolicyWriteRc, gPolicyWriteSends;
extern unsigned long gModeChanges, gModeLast, gModeLastMs;
extern unsigned long gSniffKbdSlots, gSniffMseSlots, gSniffOtherSlots;
extern unsigned long gMouseRatePeak;
extern unsigned long gMouseAbsDx, gMouseAbsDy;
extern unsigned long gRoleChanges, gRoleStatus, gRoleNew, gRoleMs;
/* ---- M5 step 1: the Consumer page ---------------------------------------------
 * ⚠ THE RING RECORDS TRANSITIONS ONLY. The keyboard sends idle reports constantly,
 * so storing every byte-8 value would fill this with zeros before a key was pressed
 * and the instrument would measure its own noise.
 *
 * ⚠⚠ IT WAS 8 AND 8 WAS ONE SHORT, measured. The comment here used to claim "8 slots
 * is enough for four keys pressed once each WITH their releases, which is exactly the
 * test" -- and the arithmetic is 1 baseline 00 + 4 x (set, clear) = NINE. The v10.5 run
 * recorded 00 04 00 08 00 02 00 01 and then "transitions LOST past the ring 1": the
 * ring filled exactly as EJECT was pressed, so eject's RELEASE is the one transition
 * that got away, and eject is the one key whose relative-vs-absolute behaviour is still
 * unmeasured. One slot.
 *
 * ⇒ 12, not 9. The extra three are for a stray transition before the deliberate test
 * begins, which costs 12 bytes and removes the class of failure entirely rather than
 * making the ring exactly big enough for the test I happen to have in mind. */
#define kHidConsumerRing 12
extern unsigned long gHidConsumerReports;  /* reports that carried byte 8 at all   */
extern unsigned long gHidConsumerSeen;     /* OR of every nonzero nibble observed  */
extern unsigned long gHidConsumerPadBits;  /* ⚠ reports where bits 4..7 were set   */
extern unsigned long gHidConsumerRing[kHidConsumerRing];
extern unsigned long gHidConsumerRingCount, gHidConsumerRingLost, gHidConsumerLast;
/* ⚠ Reports in which a key was actually HELD. A decode that succeeds on an all-zero
 * release report is not evidence of a keystroke. */
extern unsigned long gHidKeyReports;
extern unsigned long gInjCalls, gInjEventsSeen;
extern unsigned long gLedSends, gLedRc, gLedRsp;
extern unsigned long gCapsOnMirror;
void BT_LinkCmdEnqueue(unsigned char what, unsigned long handle);
void BT_LinkCmdReset(void);
void BT_LinkCmdPump(void);
extern unsigned long gLastConnHandle, gSynthRrsf;
/* v9.7: Number_Of_Completed_Packets, counted in the transport. Tiger gets one per
 * ACL send on this exact card; we appear to get none, and that is why the peer never
 * hears our Connection Response. See docs/TIGER-HCI-TRACE.md. */
extern unsigned long gNumCompEvts, gNumCompTotal, gNumCompHandle, gNumCompMs;

/* ⚠ kHidCapBytes is defined ONCE, here, and shared by the buffer in bt_btstack.c, the
 * publisher in bt_probe.c and BTCheck's reader -- two numbers that must agree is the
 * shape that put a hand-typed 304 against a 313-word block and turned into an
 * out-of-bounds read. */
#define kHidCapBytes 16
extern unsigned long gHidLevel;
extern unsigned char gHidFirstData[kHidCapBytes];
extern unsigned long gHidFirstDataLen, gHidFirstDataFull, gHidDataPkts, gHidLastDataLen;
extern unsigned long gHidDecodeRc, gHidDecodeOk, gHidRollovers;
extern unsigned long gMouseDecodeOk, gUnclaimedReports, gUnclaimedLen, gUnclaimedB0;
extern unsigned long gMouseReports, gMouseMoves, gMouseBtnChanges, gMouseDropped;
extern unsigned long gMouseWheelSeen, gCurNewErr, gCurCreated;
/* the scan filter: set at task level in ProbeInitialize, read at interrupt level */
extern unsigned long gShowAllDevices, gInqFiltered;

/* the low-battery warning: policy in bt_keyfile.c, Notification Manager in bt_defer.c */
extern unsigned long gBatWarnings, gAlertsPosted, gAlertsDropped, gAlertErr;
extern unsigned long gNmFlushes, gNmFlushErr;   /* the device-names file */
/* two-device HID (15.0) */
extern unsigned long gHidDevOpens, gHidDevCloses, gHidDevFull;
/* v16.1: the sweep's globals are gone with it. */
/* v15.1: the A1015 report-ID reading, kept under measurement. */
extern unsigned long gBatTargets, gBatGiveUps;
extern short gMouseDxMin, gMouseDxMax, gMouseDyMin, gMouseDyMax;
extern unsigned long gMouseBtnMask, gMouseRidOther;
/* ⚠ SIX words per slot, 12 total. Renamed from BT_HidDevSnapshot(out8) in v15.3 so
 * that a caller still passing a 4-word stride fails to compile instead of reading
 * into the next slot. */
void BT_HidDevSnapshot12(unsigned long *out12);
extern unsigned long gHidLastMods, gHidLastNKeys, gHidLastKey0;

/* ---- v7.0: the security instrument ------------------------------------------- *
 * ⚠ kHandlerRingLen is its own length, defined ONCE here and shared by the ring in
 * bt_btstack.c, the publisher in bt_probe.c and BTCheck's reader. */
#define kHandlerRingLen 16
extern unsigned long gSecEvtCount, gSecEvtLevel, gSecEvtStatus, gSecEvtHandle;
extern unsigned long gSecLvlFirst, gSecLvlLast, gRemFeatFirst, gRemFeatLast, gLiveSamples;
extern unsigned long gSampledHandle, gSampledHciHandle, gSampledHandleMax, gSampledHciMax;
extern unsigned long gHandlerEvtRing[kHandlerRingLen], gHandlerEvtIdx, gHandlerEvtTotal;
void BT_TryStoredKeyProbe(void);
extern unsigned long gScanMode;
extern unsigned long gDiscReason, gDiscHandle, gDiscCount;
extern unsigned long gIoCapReqs, gUserConfReqs, gPinReqs, gAuthComplete;
extern unsigned long gSspAuto, gLinkKeyReqs;
extern unsigned long gSimplePairing, gEncryptChange, gEncryptOn;
extern unsigned long gSdpRetries, gSdpRetryRc, gSdpNotReady;
extern unsigned long gSdpEventsAny, gSdpLastEvt;
extern unsigned long gConnOks, gSdpOks, gAllocFails;

/* ---- what the inquiry actually SAW, by class ------------------------------ *
 * ⚠ The responder table holds only 4 while real runs see 12-15 answers, so these
 * tallies -- incremented for EVERY responder -- are the only reliable way to ask
 * "did a keyboard answer?" without a GUI. */
extern unsigned long gInqPeriph, gInqPhones, gInqComputers, gInqOther;
/* ⚠ Audio/Video (major class 0x04) used to fall into gInqOther, so the only classes
 * this code distinguished were the three we happened to own. Counted separately now,
 * because "did the headphones answer?" should be a fact and not a shrug. */
extern unsigned long gInqAudio;

/* ---- M0.7: the ACL reader parks while no link exists ------------------------- *
 * Provided by bt_probe.c, called from the HCI event handler on connection and
 * disconnection complete.
 *
 * Run 51 measured what polling an idle bulk-IN pipe costs on this stack: an empty
 * read leaves the pipe halted, the re-arm is then REFUSED, and a stall clear is
 * genuinely required to proceed -- 286 empty reads, 286 refused arms, 285 clears and
 * 573 arms in one session, all at interrupt level. With no ACL link there is nothing
 * to receive, so the poll is pure waste.
 *
 * ⚠ BT_AclLinkUp arms the reader immediately. It is called from the connection event,
 * which arrives on the INTERRUPT pipe -- never disarmed -- so the wake-up path cannot
 * be lost. Safe at interrupt level: both do counter arithmetic and one USBBulkRead. */
void BT_AclLinkUp(void);
void BT_AclLinkDown(void);

/* ---- M6: the scan channel, provided by bt_btstack.c -------------------------- *
 * docs/SCAN-DESIGN.md. BT_ScanStart clears the responder table and starts a fresh
 * inquiry; BT_ScanPublish copies the table into the shared block, four words per
 * entry, and returns how many it wrote.
 *
 * ⚠ Both are called from the driver's command handler at interrupt level, and both
 * only touch the responder arrays and gap_inquiry_start -- the same shape as every
 * other gap_* call this stack already makes from an event handler. */
unsigned long BT_ScanStart(void);
short         BT_ScanPublish(unsigned long *blk, short base, short maxEntries);

/* ---- M7: which addresses we hold link keys for, from bt_linkkey_db.c ------------
 * ⚠ A count (gLkStored) cannot answer "is THIS row paired?", and run 49 showed why it
 * matters: a paired phone stops advertising, answers no inquiry, and was therefore
 * invisible in the panel entirely. Published addresses let a bond be listed WITHOUT a
 * scan. Two words per entry, same packing as BT_ScanPublish. */
short         BT_KeyPublish(unsigned long *blk, short base, short maxEntries);
/* ⚠ Bit N = the Nth published key was handed to the controller this session, which is
 * the ONLY evidence the peer still honours the bond. Holding a key proves nothing: run
 * 50 had 3 keys stored, 0 put_link_key calls and 3 loaded from file, one of them for a
 * phone whose own UI reported pairing FAILED. Valid only after BT_KeyPublish. */
extern unsigned long gLkHandedMask;

/* ---- the control panel's Delete button, from bt_linkkey_db.c -------------------
 * Forgets a bond by address. Returns 1 if an entry was removed, 0 if that address held
 * no key -- ⚠ the distinction matters, because the panel must not report success for a
 * bond it never had. Sets gLkDirty, so the deferred task-level flush persists the
 * removal; without that the bond would reappear from the key file on the next boot. */
int BT_DeleteBondByAddr(unsigned long hi, unsigned long lo);
/* ---- M3.7: initiator pairing, and the write that makes it useful ---------------- */
int  BT_PairDevice(unsigned long hi, unsigned long lo);
/* ⚠ Called ONLY from db_put_link_key's success path. See its note. */
void BT_WriteKeyToController(const unsigned char *addr, const unsigned char *key);
/* v12.0: the pairing safety net. Declared HERE rather than reached through
 * bt_linkkey_db.h because bt_probe.c does not include the BTstack headers that one
 * pulls in, and the signature uses plain bytes precisely so it need not. */
int  BT_LinkKeyArchive(unsigned long hi, unsigned long lo);
int  BT_DisconnectByAddr(unsigned long hi, unsigned long lo);
int  BT_ForgetPager(unsigned long hi, unsigned long lo);
int  BT_LinkKeyArchivedCount(void);
/* v12.8: the "stay disconnected" state. One slot, RAM only, cleared by a restart. */
void BT_SetBlocked(unsigned long hi, unsigned long lo, int on);
int  BT_IsBlocked(unsigned long hi, unsigned long lo);
extern unsigned long gBlockHi, gBlockLo, gBlockActive, gBlockDrops;
/* v12.9: live connections as a LIST, because one peer global is not one. */
int  BT_ConnHandleForAddr(unsigned long hi, unsigned long lo);
void BT_SampleLiveConns(void);
#define kLiveSlots 2
extern unsigned long gLiveCount, gLiveAddr[kLiveSlots][2];
/* v13.0: the device's OWN name, asked over a live link. See bt_btstack.c. */
#define kNameSlots 2
#define kNameChars 24
void BT_RequestNameIfUnknown(unsigned long hi, unsigned long lo);
extern unsigned long gNameCount, gNameAddr[kNameSlots][2], gNameReqs, gNameOks;
extern unsigned long gNameDeferred;  /* name asks skipped because a scan was running */
extern unsigned long gInqRetries, gInqRecovered;
extern unsigned char gNameText[kNameSlots][kNameChars];
extern unsigned long gPagedForgot;
extern unsigned long gDiscReqs, gDiscRc;
extern unsigned long gLkArchived;
int  BT_LinkKeyFindByAddr(unsigned long hi, unsigned long lo,
                          unsigned char *addr6, unsigned char *key16,
                          unsigned char *type);
extern unsigned long gRlkCaptured;   /* keys copied out of the card's own store */
extern unsigned long gLkCaptured;    /* the database's count of the same        */
extern unsigned long gRestoreTried;  /* restore-to-controller commands issued   */
extern unsigned long gPairAsked, gPairRc, gPairAddrHi, gPairAddrLo;
/* ⚠ Defined ONCE and shared by the ring, the publisher and BTCheck's reader. */
#define kBadCmdRingLen 4
extern unsigned long gFirstBadCmdStatus, gBadCmdCount;
extern unsigned long gPairPath, gLinkWasUp;
extern unsigned long gArmed, gArmedHi, gArmedLo, gArmedFired, gArmedRc;
extern unsigned long gSuppCmds[2], gSuppCmdsGot;
extern unsigned long gLocalVer, gLocalMfr;
/* ⚠ Delete_All_Flag is ALWAYS 0 in this driver. See BT_DeleteStoredKey. */
int BT_DeleteStoredKey(unsigned long hi, unsigned long lo);
extern unsigned long gDelStoredTried, gDelStoredRc, gDelStoredDone;
extern unsigned long gDelStoredHi, gDelStoredLo;
extern unsigned long gBadCmdRing[kBadCmdRingLen], gBadCmdIdx;
extern unsigned long gBondDone, gBondStatus, gWroteKeyTried, gWroteKeyRc, gWroteKeyDone;

/* ---- M8: the On/Off control, provided by bt_btstack.c ---------------------------
 * ⚠ "Off" clears page scan and inquiry scan -- not discoverable, not connectable. It
 * does NOT power the radio down; that is hci_power_control, a teardown path this stack
 * has never exercised. Same two calls bring-up already makes, so the path is proven. */
unsigned long BT_SetRadio(int on);
extern unsigned long gRadioOn;
extern unsigned long gInqPeriphAddrHi, gInqPeriphAddrLo, gInqPeriphCoD;
extern unsigned long gRespStored, gRespDisplaced;

/* ---- M3.1: the RAM link key store, provided by bt_linkkey_db.c ----------- *
 * ⚠ gLkDirty is the handshake the task-level flush will consume at §6.4 of
 * docs/M3-DESIGN.md. It is maintained now so the file half does not have to
 * retrofit this logic. */
extern unsigned long gLkGets, gLkHits, gLkPuts, gLkDeletes;
extern unsigned long gLkStored, gLkEvicted, gLkDirty;
extern unsigned long gLkLastAddrHi, gLkLastAddrLo, gLkLastType;
extern unsigned long gSecReqs;   /* gap_request_security_level calls made */

/* ---- M3.7: LEGACY PIN PAIRING ------------------------------------------------ *
 * ⚠ Until v3.2 HCI_EVENT_PIN_CODE_REQUEST was counted and ignored, so legacy
 * pairing could never complete: BTstack waits for the application to call
 * gap_pin_code_response and we never called it. Era-appropriate Apple keyboards
 * predate SSP and want exactly this path, so the A1016 that M4 is built around
 * could not have paired without it. gPinReqs above counts the asks; these count
 * and describe the answers. */
extern unsigned long gPinAnswered, gPinRespRc, gPinAddrHi, gPinAddrLo;

#endif /* OS9BT_PUMP_H */
