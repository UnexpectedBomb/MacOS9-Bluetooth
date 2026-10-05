/*
 *  btstack_config.h  --  BTstack configuration for the Mac OS 9 port.
 *
 *  Every line here is load-bearing, and most of them exist to make a forbidden
 *  code path not compile. This driver runs ENTIRELY at secondary interrupt level
 *  after ProbeInitialize returns, so scripts/level-audit.py forbids allocators,
 *  the File Manager, the Toolbox, blocking waits and anything that can abort.
 *  The audit over the vendored subset (docs/M2-DESIGN.md §3a) found exactly two
 *  forbidden paths, and both are switched off from this file.
 *
 *  ⚠ Do not add options casually. Every ENABLE_* is a new region of third-party
 *  code running below task level, and the audit must be re-run when one changes.
 */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* ---- time ---------------------------------------------------------------- *
 * We supply hal_time_ms(). Microseconds() is callable from our contexts and is
 * what the eSATA driver already uses in its data path; TickCount() at 16.7 ms is
 * too coarse for protocol timers. */
#define HAVE_EMBEDDED_TIME_MS

/* ---- Classic only -------------------------------------------------------- *
 * This is Bluetooth Classic HID. ENABLE_BLE is deliberately UNDEFINED, and that
 * is what deletes le_device_db_info, gap_get_persistent_irk and
 * hci_iso_stream_create, all three of which the audit found reachable and all
 * three of which sit inside #ifdef ENABLE_BLE (verified, hci.c lines 7383-7399
 * and 248). Do not define it to "keep options open": it would pull the entire LE
 * stack, the security manager and elliptic-curve crypto below task level. */
#define ENABLE_CLASSIC

/* ---- NO DYNAMIC ALLOCATION ---------------------------------------------- *
 * HAVE_MALLOC is deliberately UNDEFINED. That single omission deletes all 33
 * plain free() sites and their matching malloc() sites in btstack_memory.c,
 * which was one of only two forbidden hits the audit found. With it undefined,
 * every MAX_NR_* below becomes a plain static array.
 *
 * ⚠ If HAVE_MALLOC is ever defined, this driver hangs. Not "might": the Memory
 * Manager is not reentrant from interrupt level. */
/* #define HAVE_MALLOC  <- NEVER */

/* ---- NO LOGGING ---------------------------------------------------------- *
 * log_info / log_debug / log_error route into hci_dump, which can do real I/O.
 * We cannot touch the File Manager and have no task level to defer it to.
 * Leaving all three undefined compiles them to nothing.
 * ENABLE_CONTROLLER_DUMP_PACKETS is the other forbidden hit the audit found: it
 * guards an sprintf. Leave it off. */
/* #define ENABLE_LOG_INFO */
/* #define ENABLE_LOG_DEBUG */
/* #define ENABLE_LOG_ERROR */
/* #define ENABLE_PRINTF_HEXDUMP */
/* #define ENABLE_CONTROLLER_DUMP_PACKETS */

/* ---- ASSERTS OFF --------------------------------------------------------- *
 * With ENABLE_BTSTACK_ASSERT undefined, btstack_debug.h compiles
 * btstack_assert(c) to {(void)(c);} and btstack_unreachable() to the same. Both
 * appeared in the audit's unclassified list; both are resolved here rather than
 * by writing a handler, because the alternative handler would have to do
 * something at interrupt level and there is nothing safe for it to do. */
/* #define ENABLE_BTSTACK_ASSERT */

/* ---- buffers ------------------------------------------------------------- *
 * HCI_ACL_PAYLOAD_SIZE is OUR buffer size, not the controller's advertised
 * limit. 1021 is the Classic maximum.
 *
 * ⚠ Do NOT set this from the controller's Read_Buffer_Size answer. The dongle we
 * have reports 679, which is an odd figure, and it is a counterfeit CSR whose
 * self-reported limits are not trustworthy as bounds on what it might send. The
 * transport's own ACL read is separately capped at its 2048-byte static buffer
 * for the same reason. */
#define HCI_ACL_PAYLOAD_SIZE 1021

/* ---- static pool sizes --------------------------------------------------- *
 * ⚠ THESE WERE ESTIMATES. ONE OF THEM HAS NOW BEEN MEASURED, THE HARD WAY.
 *
 * The warning that used to sit here said: "too small and a connection fails in a
 * way that looks like a protocol bug. Pool exhaustion must be instrumented in the
 * counter block, not discovered as a mystery." Run 20 proved it exactly, and it WAS
 * discovered as a mystery -- two counters read `86` and the value had to be decoded
 * by hand to `0x56` = `BTSTACK_MEMORY_ALLOC_FAILED` (bluetooth.h:286), emitted from
 * hci.c:8435 as a SYNTHESISED CONNECTION_COMPLETE when
 * create_connection_for_bd_addr_and_type cannot allocate.
 *
 * One controller, one keyboard. L2CAP channels: SDP query, then HID control
 * (PSM 0x11) and HID interrupt (PSM 0x13), so three is the working minimum. */

/* ⚠ 2, NOT 1, AND THE REASON IS STRUCTURAL RATHER THAN GENEROUS.
 *
 * "One keyboard" implies one connection, which is what made 1 look correct. But a
 * connection object is not released the instant the link drops -- BTstack keeps it
 * while the disconnection is processed -- so a *replacement* connection to the same
 * peer legitimately overlaps the outgoing one. With a pool of 1 that overlap is
 * indistinguishable from a protocol failure, which is precisely how run 20 ended up
 * showing BTSTACK_MEMORY_ALLOC_FAILED on an otherwise perfect run.
 *
 * This matters beyond the retry that triggered it: at M4 a keyboard that drops and
 * reconnects -- sleep, out of range, battery pull -- hits the same overlap. A pool of
 * 1 would make normal reconnection fail intermittently. 2 costs one hci_connection_t
 * of BSS. */
/* ★★★ SIZED FOR TWO HID DEVICES AT ONCE (15.0), which is the whole scope: one keyboard
 * and one mouse. The A1015 run proved the old numbers were the blocker -- the mouse
 * bonded and could not open a HID connection because there was no second slot.
 *
 *   HCI connections  : one ACL link per device.
 *   L2CAP channels   : ⚠ TWO PER HID DEVICE -- control (PSM 0x11) and interrupt (0x13).
 *                      So two devices need FOUR, and 3 was short even for a keyboard
 *                      plus a transient SDP query. 6 leaves headroom for that query and
 *                      one reconnect overlapping a teardown.
 *   L2CAP services   : per listening PSM, not per device. Still 2.
 *   HID connections  : one per device.
 *
 * ⚠⚠ THESE ARE STATIC POOL SIZES, NOT AN ALLOCATOR. HAVE_MALLOC stays undefined -- the
 * level audit's note is blunt about it ("if HAVE_MALLOC is ever defined, this driver
 * hangs -- not 'might'"), so raising these grows a static array and reaches no memory
 * manager. That is the property that makes this safe to do at all. */
#define MAX_NR_HCI_CONNECTIONS          2
#define MAX_NR_L2CAP_CHANNELS           6
#define MAX_NR_L2CAP_SERVICES           2
#define MAX_NR_HID_HOST_CONNECTIONS     2
#define MAX_NR_SERVICE_RECORD_ITEMS     0   /* SDP client, not a server */

/* ---- deliberately absent, with reasons ---------------------------------- *
 * MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES -- we register NO link key database.
 *   hci.c guards every use with `if (!hci_stack->link_key_db) return;` (lines
 *   566, 572, 578), and they are struct members rather than extern symbols, so
 *   registering nothing means no calls and no link errors.
 *   ⚠ CONSEQUENCE, and it is user-visible: link keys are not persisted, so a
 *   pairing does not survive a reboot and the keyboard must be re-paired each
 *   time. Revisit at M3. Persisting them needs the File Manager, which needs
 *   task level, which this driver does not have -- except that ProbeInitialize
 *   and ProbeFinalize DO run at task level, so reading keys at init and flushing
 *   at finalize is the one available window. Note that a power cut then loses
 *   whatever was not flushed.
 * MAX_NR_RFCOMM_* / AVDTP / AVRCP / HFP -- serial and audio profiles, not HID.
 * MAX_NR_SM_LOOKUP_ENTRIES / WHITELIST / GATT -- LE only.
 */

#endif /* BTSTACK_CONFIG_H */
