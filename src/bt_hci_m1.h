/*
 *  bt_hci_m1.h  --  OS9-Bluetooth  |  M1 HCI command/event layer (device-agnostic).
 *
 *  Sits above the USB transport in bt_probe.c. The transport provides two hooks
 *  (send a command, log a line); this layer drives a controller bring-up
 *  sequence and parses the events that come back. All standard HCI, so it works
 *  identically on an external CSR dongle and a Mini's internal CSR module.
 */
/* !! RENAMED from hci.h at M2, and the name matters. vendor/btstack/src/hci.h
 * exists too, and a quote-include searches the including file'"'"'s directory FIRST, so
 * every file in src/ writing #include "hci.h" silently got OURS instead of
 * BTstack'"'"'s. bt_btstack.c did exactly that and only compiled because BTstack'"'"'s
 * header arrived anyway through a transitive include. That is luck, not design. */
#ifndef OS9BT_HCI_M1_H
#define OS9BT_HCI_M1_H

/* !! stdbool BEFORE USB.h. The build defines TYPE_BOOL=1 so MacTypes.h skips its
 * own `enum { false = 0, true = 1 }`, which BTstack needs; but USB.h then uses
 * `true` itself (USB.h:1969) and nothing would define it. stdbool supplies the
 * macros, satisfying both, and every translation unit agrees bool is C99 bool. */
#include <stdbool.h>
#include <USB.h>

/* ---- provided by the transport (bt_probe.c) ---------------------------- */
extern OSStatus BT_SendHCICommand(const void *pkt, UInt32 len);  /* control-pipe OUT */
extern OSStatus BT_SendACL(const void *pkt, UInt32 len);         /* bulk-OUT, for L2CAP */
extern int      BT_CanSendCommand(void);   /* is the command PB free?  */
extern int      BT_CanSendACL(void);       /* is the bulk-OUT PB free? */
extern int      BT_SendWasAccepted(OSStatus err);  /* queued or done, vs refused */
extern void     BT_Log(const void *pstr, UInt32 value);          /* USB Prober log   */

/* ---- the counter-block hook -------------------------------------------- *
 * BT_Log goes to the USB Expert log, which on this machine carries nothing we
 * post, for reasons still unexplained after four hardware runs. So every value
 * worth reading is ALSO written into the driver's counter block, where BTCheck
 * can read it out of the System heap. These slot numbers are shared between
 * hci.c and bt_probe.c and must match bt-check/bt_check.c's decoder. */
enum {
    kRecResetStatus = 0,
    kRecHciVersion,
    kRecManufacturer,        /* 10 (0x000A) = Cambridge Silicon Radio          */
    kRecBdAddrHi,            /* top 3 bytes; a SHARED value across units means  */
    kRecBdAddrLo,            /* a counterfeit CSR clone                         */
    kRecAclLen,
    kRecAclCount,
    kRecSetMaskStatus,
    kRecScanEnStatus,
    kRecCmdStatusOpcode,
    kRecInqResponses,
    kRecInqDevHi,
    kRecInqDevLo,
    kRecInqComplete,         /* status byte of Inquiry Complete                 */
    kRecLastOtherEvent,      /* event code of anything unrecognised             */
    kRecCount
};
extern void     BT_Record(UInt32 which, UInt32 value);

/* ---- provided by this layer (called by the transport) ------------------ */
void HCI_Start(void);                               /* begin the M1 sequence      */
void HCI_HandleEvent(const UInt8 *evt, UInt32 len); /* feed an interrupt-IN event */

#endif /* OS9BT_HCI_M1_H */
