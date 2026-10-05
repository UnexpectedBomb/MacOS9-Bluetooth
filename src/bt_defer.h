/*
 *  bt_defer.h  --  the interrupt-to-task-level trampoline (NMInstall, nmStr = 0).
 *
 *  See bt_defer.c for the mechanism and its one real limitation (the NM queue is
 *  serviced by an application's event loop, so it may not run before the desktop).
 */
#ifndef OS9BT_DEFER_H
#define OS9BT_DEFER_H

/* ⚠ TASK LEVEL ONLY. Builds the NMUPP, which allocates a routine descriptor.
 * Call from ProbeInitialize. */
void BT_DeferInit(void);

/* Callable from SECONDARY INTERRUPT LEVEL. Only enqueues; the work happens later
 * at task level. Coalesces, so calling it repeatedly is free and safe. */
void BT_DeferRequest(void);

/* ⚠ TASK LEVEL ONLY. Call from ProbeFinalize, before the fragment goes away. */
void BT_DeferFinalize(void);

/* Provided by bt_probe.c, called by the trampoline's task-level half. Sends the CSR
 * mode switch if one is pending, and does nothing otherwise.
 *
 * This is how the switch gets Apple's IOSleep(1000) settle time without blocking the
 * USB Expert: ProbeInitialize arms it and returns immediately, and the notification is
 * serviced later, at task level, which is also where Apple's retries run.
 * See docs/M0-MODE-SWITCH.md §9. */
void BT_DeferredSwitchIfPending(void);

/* ---- a user-visible alert -------------------------------------------------------
 *
 * A SECOND Notification Manager record, separate from the defer trampoline's. That one
 * has nmStr = 0 and deliberately displays nothing; this one exists to show a string.
 *
 * BT_PostAlert takes a PASCAL string and COPIES it -- the Notification Manager holds the
 * record and text by pointer until the user dismisses the alert, so the caller's buffer
 * cannot be trusted to live that long. All three are TASK LEVEL ONLY. One alert may be
 * outstanding at a time; further posts are counted and dropped rather than corrupting
 * the queue. */
void BT_AlertInit(void);
void BT_PostAlert(const unsigned char *pstr);
void BT_AlertFinalize(void);

extern unsigned long gDeferReqs;         /* notifications installed              */
extern unsigned long gDeferRuns;         /* ★ responses that RAN at task level   */
extern unsigned long gDeferInstallErrs;  /* NMInstall refused                    */
extern unsigned long gDeferDropped;      /* coalesced away (one already queued)  */

#endif /* OS9BT_DEFER_H */
