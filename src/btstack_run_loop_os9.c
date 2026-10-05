/*
 *  btstack_run_loop_os9.c  --  BTstack run loop for Mac OS 9, driven from
 *                             USB completions instead of owning a thread.
 *
 *  WHY THIS FILE EXISTS INSTEAD OF platform/embedded
 *  -------------------------------------------------
 *  BTstack's embedded run loop is built around a task-level loop woken by
 *  interrupts: `btstack_run_loop_embedded_execute()` is a `while` loop, and
 *  `execute_once()` ENDS BY SLEEPING via hal_cpu_enable_irqs_and_sleep().
 *
 *  This driver has no task-level context. After ProbeInitialize returns, every
 *  line runs in a USB Services Library completion at secondary interrupt level.
 *  There is no thread to own a loop and nothing may ever sleep.
 *
 *  A background application could have supplied that context, and it was
 *  seriously considered and rejected: Bluetooth HID has to work before the
 *  desktop loads, so the stack must live in an extension. See docs/M2-DESIGN.md
 *  and the standing constraint it records.
 *
 *  So: `execute` is never called. Our USL completions call BT_Pump() instead,
 *  which performs exactly one run-loop iteration by calling the three
 *  btstack_run_loop_base_* workers directly -- deliberately NOT execute_once(),
 *  because that is where the sleep is.
 *
 *  This was made safe by measurement, not by hope. The preprocessed audit over
 *  the vendored compile set with this exact configuration reports ZERO forbidden
 *  primitives reachable from these roots: no allocator, no File Manager, no
 *  Toolbox, no blocking wait, nothing that can abort.
 *  See docs/M2-DESIGN.md §3b and logs/BTstack_audit_run2_*.
 *
 *  ⚠ KNOWN COST OF THIS DESIGN. Timers only advance when a completion happens to
 *  fire. On a quiet link a protocol timeout will expire late or not at all. If
 *  that bites -- M3 pairing timeouts are the likely place -- the fix is a Time
 *  Manager task that does nothing but call BT_Pump(). That is still interrupt
 *  level, so it changes nothing about the audit or about what ships.
 */

#include "btstack_config.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"

#include <Timer.h>          /* Microseconds -- declared here, NOT in OSUtils.h */

#include "bt_pump.h"

/* ---- time --------------------------------------------------------------- *
 * HAVE_EMBEDDED_TIME_MS means BTstack asks us for milliseconds.
 *
 * Microseconds() rather than TickCount(): ticks are 16.7 ms, which is coarser
 * than protocol timers want, and this project already relies on Microseconds()
 * in the eSATA data path so it is known callable from these contexts.
 *
 * The 64-bit microsecond count is reduced to a 32-bit millisecond count, which
 * wraps about every 49.7 days. BTstack's timer comparisons are wrap-safe, and a
 * driver that has been resident for 49 days without a bus reset is not the case
 * to optimise for. */
uint32_t hal_time_ms(void)
{
    UnsignedWide us;
    Microseconds(&us);
    /* (hi * 2^32 + lo) / 1000, kept in 32 bits without a 64-bit divide:
     * 2^32 / 1000 = 4294967.296, so hi contributes hi * 4294967 plus a small
     * correction that is irrelevant at millisecond resolution. */
    return (uint32_t)(us.lo / 1000UL) + (uint32_t)(us.hi * 4294967UL);
}

static uint32_t os9_get_time_ms(void)
{
    return hal_time_ms();
}

/* ---- the pump ----------------------------------------------------------- */

static volatile int gInPump = 0;     /* completions can nest; see below */
static int          gRunLoopReady = 0;

unsigned long gPumpCalls;            /* read out through the counter block */
unsigned long gPumpReentered;

/* One run-loop iteration, called from a USL completion.
 *
 * ⚠ NOT REENTRANT, and it must not be. A USL completion can fire while we are
 * inside BTstack -- sending a packet from within a callback completes later, but
 * an immediate error path can re-enter sooner than that. Re-entering BTstack's
 * linked lists mid-walk would corrupt them. The guard drops the nested call and
 * counts it; the work is not lost, because the outer iteration is still walking
 * the same data sources and the next completion pumps again.
 *
 * Deliberately no sleep, no wait, no yield. Compare
 * btstack_run_loop_embedded_execute_once(), which ends in
 * hal_cpu_enable_irqs_and_sleep(). */
void BT_Pump(void)
{
    if (!gRunLoopReady) return;
    if (gInPump) { gPumpReentered++; return; }
    gInPump = 1;
    gPumpCalls++;

    btstack_run_loop_base_poll_data_sources();
    btstack_run_loop_base_execute_callbacks();
    btstack_run_loop_base_process_timers(hal_time_ms());

    gInPump = 0;
}

/* ---- btstack_run_loop_t ------------------------------------------------- *
 * Everything here delegates to the base implementation, which is plain list
 * manipulation with no allocation. Only `execute` and the thread-related members
 * differ, and they differ because we have no thread. */

static void os9_init(void)
{
    btstack_run_loop_base_init();
    gRunLoopReady = 1;
}

static void os9_add_data_source(btstack_data_source_t *ds)
{
    btstack_run_loop_base_add_data_source(ds);
}

static bool os9_remove_data_source(btstack_data_source_t *ds)
{
    return btstack_run_loop_base_remove_data_source(ds);
}

static void os9_enable_data_source_callbacks(btstack_data_source_t *ds, uint16_t cb)
{
    btstack_run_loop_base_enable_data_source_callbacks(ds, cb);
}

static void os9_disable_data_source_callbacks(btstack_data_source_t *ds, uint16_t cb)
{
    btstack_run_loop_base_disable_data_source_callbacks(ds, cb);
}

static void os9_set_timer(btstack_timer_source_t *ts, uint32_t timeout_in_ms)
{
    ts->timeout = hal_time_ms() + timeout_in_ms + 1;
}

static void os9_add_timer(btstack_timer_source_t *ts)
{
    btstack_run_loop_base_add_timer(ts);
}

static bool os9_remove_timer(btstack_timer_source_t *ts)
{
    return btstack_run_loop_base_remove_timer(ts);
}

/* ⚠ NEVER CALLED, and must not be. BTstack calls execute() only if the port's
 * main() hands control to the run loop, which ours does not: the USB Expert owns
 * our lifetime and BT_Pump() is how iterations happen. Left as a no-op rather
 * than a loop so that a future caller stalls visibly instead of hanging the
 * machine inside a completion. */
static void os9_execute(void)
{
    /* deliberately empty -- see the comment above */
}

static void os9_dump_timer(void)
{
    /* logging is compiled out; nothing to dump */
}

/* An interrupt asking the run loop to look at its data sources. For the embedded
 * port this sets a flag for the task-level loop to notice. We ARE the interrupt
 * context, so the honest implementation is to do the iteration now. */
static void os9_poll_data_sources_from_irq(void)
{
    BT_Pump();
}

/* No threads, so "on the main thread" means "here". The callback is queued and
 * the next pump runs it, which keeps the ordering BTstack expects rather than
 * calling it inline from wherever we happen to be. */
static void os9_execute_on_main_thread(btstack_context_callback_registration_t *cb)
{
    btstack_run_loop_base_add_callback(cb);
}

static void os9_trigger_exit(void)
{
    /* nothing to exit: there is no loop to leave */
}

static const btstack_run_loop_t btstack_run_loop_os9 = {
    &os9_init,
    &os9_add_data_source,
    &os9_remove_data_source,
    &os9_enable_data_source_callbacks,
    &os9_disable_data_source_callbacks,
    &os9_set_timer,
    &os9_add_timer,
    &os9_remove_timer,
    &os9_execute,
    &os9_dump_timer,
    &os9_get_time_ms,
    &os9_poll_data_sources_from_irq,
    &os9_execute_on_main_thread,
    &os9_trigger_exit,
};

const btstack_run_loop_t * btstack_run_loop_os9_get_instance(void)
{
    return &btstack_run_loop_os9;
}
