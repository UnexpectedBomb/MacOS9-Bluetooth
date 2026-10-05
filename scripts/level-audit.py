#!/usr/bin/env python3
"""Static below-task-level audit for the OS 9 Bluetooth USB class driver.

BFSes the call graph from the BELOW-TASK-LEVEL entry points and reports every
task-level-only primitive reachable from them. Comments and string literals are
stripped FIRST: a function name inside a comment creates a phantom edge and the
audit then cries wolf.

Prescribed by claude-os9/CLAUDE.md:
    "After moving any work between execution levels, re-run the static audit."

WHY THE ENTRY POINTS ARE WHAT THEY ARE
--------------------------------------
In the OS 9 USB Services Library every transaction is asynchronous and its
`usbCompletion` proc is called at SECONDARY INTERRUPT LEVEL, not task level. So
the four completion routines below are the roots, and everything they reach --
including the whole of hci.c, which is entered from IntCompletion via
HCI_HandleEvent -- runs below task level.

`ProbeValidateHW` / `ProbeInitialize` / `ProbeFinalize` / `ProbeNotify` are
deliberately NOT roots: the USB Expert calls those at task level. But
ProbeInitialize *starts* the completion chain, so everything after the first USL
call in that chain is interrupt level. If a future change makes the Expert call
any dispatch proc from an interrupt context, add it here in the same commit.

THIS SCRIPT REPORTS TWO THINGS, and the second is the important one
-------------------------------------------------------------------
1. Hits against a FORBIDDEN list. Useful, but a blocklist only finds what someone
   already thought of.
2. EVERY external symbol reachable from interrupt level, split into known-safe
   and UNCLASSIFIED. This project's own audit scripts carry the scar: PBStatusSync
   and NMRemove each went unchecked for builds because they were simply absent from
   the list, and "an audit that cannot see a primitive is worse than no audit,
   because it is believed." An unclassified symbol is not necessarily a bug; it is
   a thing a human must look at once and then move into a list.

Usage:  python3 scripts/level-audit.py [src/*.c ...]
Exit 1 if any forbidden primitive is reachable, or if anything is unclassified.
"""
import re
import sys
import collections
import os

# --roots lets the same tool audit a DIFFERENT body of code with different entry
# points, which is how the vendored BTstack subset is checked for M2. One tool, so
# the FORBIDDEN and KNOWN_SAFE lists stay in one place and cannot drift apart.
ARGV = sys.argv[1:]
ROOTS_OVERRIDE = None
if '--roots' in ARGV:
    i = ARGV.index('--roots')
    ROOTS_OVERRIDE = [r.strip() for r in ARGV[i + 1].split(',') if r.strip()]
    del ARGV[i:i + 2]
# ⚠ THE PORT FILES BELONG HERE, NOT JUST THE TWO ORIGINALS.
#
# M2 put BTstack behind the completions, and the seam functions (BT_DeliverPacket,
# BT_NotifyPacketSent, BT_StackStart) live in hci_transport_os9.c and bt_btstack.c.
# With those files absent the BFS stopped dead at the seam, and -- worse -- the
# three seam names were misfiled as "defined in the corpus" (see the note on
# defined_in_corpus), so the audit reported CLEAN having traversed none of the
# BTstack-facing half of the driver.
FILES = ARGV or ['src/bt_probe.c', 'src/bt_hci_m1.c',
                 'src/hci_transport_os9.c', 'src/btstack_run_loop_os9.c',
                 'src/bt_btstack.c', 'src/bt_linkkey_db.c',
                 # ⚠ bt_defer.c and bt_keyfile.c are TASK-LEVEL code, and they are in
                 # this list ON PURPOSE. The point is not to audit them as interrupt
                 # code -- it is to prove they are NOT REACHABLE from an interrupt
                 # root. bt_keyfile.c is full of File Manager calls, every one of them
                 # on the FORBIDDEN list, so if the level separation ever breaks this
                 # audit turns red immediately. Leaving them out would hide exactly
                 # the failure the audit exists to catch.
                 'src/bt_defer.c', 'src/bt_keyfile.c',
                 # ⚠ IN THE LIST EVEN THOUGH IT IS PURE ARITHMETIC. A source file the
                 # audit does not walk is the blind spot reference_static_audit_blind_spots
                 # warns about, and "obviously safe" is how a file stays unaudited until
                 # it stops being safe.
                 'src/bt_bootreport.c',
                 # ⚠⚠ M5's INJECTOR, AND IT IS THE FILE THIS AUDIT MATTERS MOST FOR.
                 # It writes low memory and calls PostEvent / KeyTranslate / TickCount
                 # from SECONDARY INTERRUPT LEVEL. That is legal -- Apple's own OS 9 USB
                 # keyboard driver does exactly this from a USB completion routine, see
                 # bt_bootreport.h for the verified call chain -- but "Apple does it" is
                 # a claim about Apple's call graph, not ours, and this audit is what
                 # checks ours. In the list in the commit that creates it.
                 'src/bt_inject.c',
                 # ⚠⚠ THE SWITCHER, IN THE LIST IN THE COMMIT THAT CREATES IT.
                 #
                 # It is a SEPARATE extension and a separate PEF, so nothing in
                 # bt_probe.c can reach it and it would have been easy to argue it was
                 # out of scope. That argument is exactly the blind spot
                 # reference_static_audit_blind_spots describes: it has its own
                 # interrupt-level completion (SwSwitchCompletion), and from there it
                 # reaches USBClearPipeStallByReference, NMInstall and USBDeviceRequest.
                 # It also calls NewPtrSys in SwEnsureBlock, which is FORBIDDEN below
                 # task level -- so the one thing this audit must prove about this file
                 # is that the completion cannot reach the allocator. Unwalked, that
                 # would be an assertion; walked, it is checked.
                 'src/bt_switch.c']

# ---------------------------------------------------------------------------
# Roots: these run BELOW TASK LEVEL (USL completion procs).
# ---------------------------------------------------------------------------
ENTRIES = ['ConfigStep', 'ConfigStepAny', 'ConfigDone', 'CmdCompletion',
           'IntCompletion', 'AclInCompletion', 'AclOutCompletion',
           # ---- M0.0: the CSR mode-switch completion. Added when the switch was
           # written, because the audit had already printed "no forbidden primitive
           # is reachable" WITHOUT walking it -- a clean verdict over a function it
           # never looked at, which is the exact failure mode
           # reference_static_audit_blind_spots warns about. A new completion proc
           # MUST be added here in the same commit that creates it.
           'SwitchCompletion',
           # ---- USBBluetoothSwitch's completion, the same kind of root in a
           # different extension. Named Sw* rather than reusing SwitchCompletion
           # because this audit builds its call graph BY NAME across all the files in
           # FILES: two static functions with one name in two separately-linked PEFs
           # are fine for the linker and would silently merge two call graphs here.
           'SwSwitchCompletion',
           # ---- M6b: the control panel's entry point, and a NEW KIND OF ROOT.
           #
           # Every root above is reached from a USB completion. bt_scan_start_sih is
           # not: the panel calls the exported BTScanStart at TASK level, which hands
           # the handler to CallSecondaryInterruptHandler2, and the OS runs it at
           # secondary interrupt level. So it runs under exactly the same restrictions
           # as a completion while being unreachable from any of them by static call
           # graph -- precisely the hole that makes an audit print CLEAN over code it
           # never walked.
           #
           # ⚠ BTScanStart itself is deliberately NOT a root: it runs at task level,
           # where the restrictions do not apply, and rooting it here would drag
           # CallSecondaryInterruptHandler2's own task-level context into the
           # interrupt-level verdict and produce false positives.
           'bt_scan_start_sih',
           # M8: the On/Off control, same kind of root and the same reasoning. A new
           # SecondaryInterruptHandler2 MUST be added here in the commit that creates
           # it -- it is unreachable from any completion by static call graph while
           # running under identical restrictions.
           'bt_set_radio_sih',
           # The teardown-bisect probe's handler. A root even though it is empty and
           # temporary: the rule is that EVERY SecondaryInterruptHandler2 is rooted in
           # the commit that creates it, and an exception "because it does nothing" is
           # how the habit erodes.
           'bt_empty_sih',
           # The Delete button's handler. ⚠ This one is NOT trivially safe like the
           # others: it reaches into the link-key store, and that store's file half is
           # full of File Manager calls. Rooting it here is what proves the delete path
           # stays on the RAM side and only sets the dirty flag -- exactly the
           # separation bt_linkkey_db.c's header comment claims.
           'bt_delete_bond_sih',
           # ★★★ THE PUMP TIMER, and this is the most consequential root in the list,
           # not the least. SetPersistentTimer fires it on the driver's own schedule
           # with nobody having asked, and through BT_ServiceMailbox it reaches
           # BT_ScanStart, BT_SetRadio, BT_DeleteBondByAddr and BTstack's entire
           # hci_run send path. Every task-level-only primitive any of those can touch
           # is now reachable from a timer interrupt, so this root is what makes the
           # audit's verdict mean anything about the change that introduced it.
           # ⚠ "It only pumps" was the tempting reason to skip rooting it, which is
           # exactly the exception that erodes the habit -- same note as bt_empty_sih.
           'bt_pump_timer_sih',
           # ---- M2: the callbacks BTstack invokes THROUGH FUNCTION POINTERS.
           # Unreachable from the completions by static call graph, but they run
           # in exactly the same interrupt, and docs/M2-DESIGN.md §3b explicitly
           # discharges BTstack's ~15 unresolved function-pointer names by saying
           # "they resolve into our own code, which the driver's own audit
           # covers". That was only true once they were roots here.
           'hci_event_handler',                     # hci_add_event_handler
           'sdp_query_handler',                     # sdp_client_query_uuid16 callback
           # ---- v13.8: the half-open watchdog's two timer callbacks, reached via
           # btstack_run_loop_set_timer_handler and invoked from
           # btstack_run_loop_base_process_timers inside BT_Pump -- i.e. from
           # bt_pump_timer_sih, at secondary interrupt level.
           # ⚠⚠ ROOTED BECAUSE THE AUDIT WAS MEASURABLY BLIND TO THEM. Before this,
           # `level-audit.py` mentioned bh_stall_timeout ZERO times and still printed
           # "no forbidden primitive is reachable" -- a clean headline over a holed
           # graph, which is the exact failure reference_static_audit_blind_spots
           # records. Checked by grepping the output for the new names rather than
           # trusting the verdict. Function pointers are invisible to a static BFS, so
           # every one of them has to be named here or the verdict means nothing.
           # ⚠ v13.9 deleted bh_stall_timeout (it raced L2CAP's identical 10 s RTX timer
           # and was redundant with hid_host's own finalize). Removed from the roots in
           # the same commit: a root naming a function that no longer exists is a silent
           # no-op that makes the list look more complete than it is.
           'bh_reconnect_timeout',
           # ⚠ 15.0's connect sweep. A btstack_run_loop timer callback is reached
           # ONLY through a function pointer, so the BFS cannot find it and the
           # audit would report CLEAN over everything it touches -- the exact blind
           # spot reference_static_audit_blind_spots exists for.
           # 'bt_sweep_timeout' -- REMOVED v16.1 with the sweep itself. Left as a
           # note because a root naming a deleted function is silently useless:
           # the BFS simply finds nothing and the verdict still prints CLEAN.
           # ⚠⚠ v14.1's battery probe, AND I ADDED IT WITHOUT ROOTING IT FIRST. The
           # comment directly above says every function-pointer entry point has to be
           # named here or the verdict means nothing, and the next timer callback written
           # after that sentence was not. Caught the same way as last time -- grepping the
           # output for the new name instead of reading the headline. Root it when you add
           # it, in the same commit, not after the audit prints a reassuring INCOMPLETE.
           'bat_probe_timeout',
           # v14.2's second half, on its own tick. Checked for BEFORE reading the
           # verdict this time rather than after -- which is the habit the note above
           # was asking for.
           'bat_state_timeout',
           # ---- M3.1: btstack_link_key_db_t. hci.c calls these while handling
           # HCI_EVENT_LINK_KEY_REQUEST and Link Key Notification, both of which
           # reach us through the interrupt-IN completion. They are function
           # pointers, so nothing reaches them by static call graph.
           'db_open', 'db_close', 'db_set_local_bd_addr',
           'db_get_link_key', 'db_put_link_key', 'db_delete_link_key',
           'db_iterator_init', 'db_iterator_get_next', 'db_iterator_done',
           'transport_send_packet',                 # hci_transport_t
           'transport_can_send_packet_now',
           'transport_register_packet_handler',
           'transport_init', 'transport_open', 'transport_close',
           'os9_get_time_ms',                       # btstack_run_loop_t
           'os9_set_timer', 'os9_add_timer', 'os9_remove_timer',
           'os9_add_data_source', 'os9_remove_data_source',
           'os9_enable_data_source_callbacks', 'os9_disable_data_source_callbacks',
           'os9_poll_data_sources_from_irq', 'os9_execute_on_main_thread',
           'os9_trigger_exit']

# ---------------------------------------------------------------------------
# Task-level-only primitives. Reaching any of these below task level is a hang,
# and on this project it is a hang with no NMI and no log.
# ---------------------------------------------------------------------------
FORBIDDEN = {
    # File Manager. THE recurring bug on this project (EHCI r18, n4, r95).
    'FSWrite': 'File Manager', 'FSRead': 'File Manager',
    'FlushVol': 'File Manager', 'FSClose': 'File Manager',
    'FSpCreate': 'File Manager', 'FSpOpenDF': 'File Manager',
    # ⚠ ADDED v15.4 WITH THE CALLS THAT NEEDED THEM. They were absent, so the audit
    # reported CLEAN about FSpSetFInfo the moment it was introduced -- vacuously,
    # because it did not know the name. An audit that does not classify a primitive
    # does not clear it; it ignores it.
    'FSpGetFInfo': 'File Manager', 'FSpSetFInfo': 'File Manager',
    'FSpExchangeFiles': 'File Manager', 'FSpRename': 'File Manager',
    'FSpDelete': 'File Manager',
    'FSpDelete': 'File Manager', 'FSMakeFSSpec': 'File Manager',
    # ⚠ ADDED after switcher 1.2 put a pair-mode flag lookup in SwInitialize and
    # this table did not know FindFolder at all -- so the one call that actually
    # touches the catalog to FIND the Preferences folder was invisible to the
    # audit while its neighbours FSMakeFSSpec and FSpDelete were flagged. A table
    # that catches two of three calls in one function is a blind spot, not a
    # check. [[reference_static_audit_blind_spots]]
    'FindFolder': 'File Manager', 'FSpCreate': 'File Manager',
    'FSpGetFInfo': 'File Manager', 'FSpSetFInfo': 'File Manager',
    'FSpExchangeFiles': 'File Manager', 'FSpRename': 'File Manager',
    'FSpDelete': 'File Manager',
    'FSpOpenDF': 'File Manager', 'FSClose': 'File Manager',
    'FlushVol': 'File Manager', 'SetEOF': 'File Manager',
    'SetFPos': 'File Manager',
    'SetFPos': 'File Manager', 'GetFPos': 'File Manager',
    'PBFlushFileSync': 'File Manager', 'PBMountVol': 'File Manager',
    'fopen': 'stdio', 'fwrite': 'stdio', 'fprintf': 'stdio', 'printf': 'stdio',
    'sprintf': 'stdio (and a stack-smash risk -- use vsnprintf)',
    'vsprintf': 'stdio (stack smash -- use vsnprintf)',
    # Allocators. The Memory Manager is not reentrant from interrupt level.
    'NewPtr': 'allocator', 'NewPtrSys': 'allocator',
    'NewPtrClear': 'allocator', 'NewPtrSysClear': 'allocator',
    'DisposePtr': 'allocator', 'NewHandle': 'allocator',
    'NewHandleSys': 'allocator', 'DisposeHandle': 'allocator',
    'HLock': 'Memory Manager', 'HUnlock': 'Memory Manager',
    'NewRoutineDescriptor': 'MixedMode allocator (task level)',
    'PoolAllocateResident': 'allocator',
    # Synchronous Device Manager / File Manager PB calls: they BLOCK.
    'PBControlSync': 'synchronous Device Manager',
    'PBStatusSync': 'synchronous Device Manager',
    'PBReadSync': 'synchronous Device Manager',
    'PBWriteSync': 'synchronous Device Manager',
    'PBOpenSync': 'synchronous Device Manager',
    'InstallDriverFromMemory': 'Device Manager driver install',
    # Blocking / timing. Delay() is 16.7 ms tick granularity and yields.
    'Delay': 'Delay() yields and is 16.7 ms granular -- never below task level',
    'SysTaskAvailable': 'task level', 'SystemTask': 'task level',
    'WaitNextEvent': 'event loop', 'GetNextEvent': 'event loop',
    # UI. A modal anything below task level is an instant wedge.
    'StandardAlert': 'modal event loop', 'Alert': 'modal event loop',
    'NoteAlert': 'modal event loop', 'CautionAlert': 'modal event loop',
    'BeginSystemMode': 'system-mode dialog (task level)',
    'DrawString': 'QuickDraw', 'SysBeep': 'Sound Manager (task level)',
    # Gestalt allocates.
    'NewGestaltValue': 'Gestalt (allocates)',
    # Synchronous USL variants, if any are ever introduced.
    'USBDeviceRequestSync': 'synchronous USL call',
    # C library. Forbidden here for the same reasons as the Toolbox allocators, and
    # listed explicitly because third-party code is the likely source: BTstack with
    # HAVE_MALLOC undefined must not reach any of these.
    'malloc': 'C allocator', 'calloc': 'C allocator', 'realloc': 'C allocator',
    'free': 'C allocator', 'strdup': 'C allocator',
    'fopen': 'stdio', 'fclose': 'stdio', 'fputs': 'stdio', 'puts': 'stdio',
    'putchar': 'stdio', 'snprintf': 'stdio (formatting is fine, the I/O is not)',
    'exit': 'terminates the process', 'abort': 'terminates the process',
    'assert': 'may abort; use a logging assert',
    'setjmp': 'non-local exit', 'longjmp': 'non-local exit',
}

# ---------------------------------------------------------------------------
# Known-safe externals: verified callable from secondary interrupt level.
# Everything in the USL transaction API is asynchronous by contract -- it queues
# and returns kUSBPending, and the completion proc is what runs later.
# ---------------------------------------------------------------------------
KNOWN_SAFE = {
    # ⚠ Classified when v17.4 began calling it directly from IntCompletion. It was
    # ALREADY reachable at interrupt level -- hal_time_ms() is Microseconds() and the
    # run loop calls it from completions -- so the audit was vouching for it indirectly
    # while reporting it unclassified by name. It is a Time Manager trap that reads the
    # microsecond counter: no allocation, no File Manager, no blocking, and
    # btstack_config.h already records it as callable from our contexts.
    'Microseconds': 'Time Manager trap, reads the us counter; no alloc, no block',
    'USBDeviceRequest': 'async USL control transfer; returns kUSBPending',
    'USBControlRequest': 'async USL control transfer',
    'USBIntRead': 'async USL interrupt-IN read',
    'USBIntWrite': 'async USL interrupt-OUT write',
    'USBBulkRead': 'async USL bulk read',
    'USBBulkWrite': 'async USL bulk write',
    'USBFindNextInterface': 'async USL enumeration call',
    'USBFindNextPipe': 'async USL enumeration call',
    'USBSetConfiguration': 'async USL configuration call',
    'USBNewInterfaceRef': 'async USL call',
    'USBConfigureInterface': 'async USL call',
    'NMInstall': 'ENQUEUE ONLY -- the defer-to-task-level trampoline. Apple uses it\n                  the same way from USB completions (APPLE-UMSS-RE.md); it does not\n                  run the response proc, it only queues the record',
    'USBClearPipeStallByReference': 'async USL call; takes only a pipe ref, no PB. THE\n                                    fix for run 23 -- MacErrors.h says a stalled pipe\n                                    needs its error CLEARED before it accepts work',
    'USBAbortPipeByReference': 'async USL call',
    'USBExpertStatus': 'THE sanctioned interrupt-safe logging call (USB Prober)',
    'USBExpertStatusLevel': 'same call with an explicit level (USBServicesLib 1.2+); '
                            'same interrupt-safe path, no File Manager, no allocation',
    'USBExpertFatalError': 'interrupt-safe error report',
    'USBMakeBMRequestType': 'macro / pure arithmetic',
    'BlockMoveData': 'safe below task level (no allocation, no Memory Manager)',
    'memcpy': 'pure', 'memset': 'pure', 'memcmp': 'pure', 'strlen': 'pure',
    # ⚠ CLASSIFIED ONLY AFTER READING IT, prompted by the PostToolUse hook flagging
    # the count going 59 -> 60. hci_send_cmd_va_arg checks
    # hci_can_send_command_packet_now, reserves BTstack's STATIC hci_packet_buffer
    # (no allocation), formats into it, and hands it to our async transport. No
    # allocation, no blocking, no File Manager. It is also precedented at the exact
    # call site we use: the neighbouring branch starts an inquiry, which sends
    # commands the same way from the same context.
    'hci_send_cmd': 'static packet buffer, no allocation, async transport send',
    # ⚠ ADDED for v8.6's report-protocol experiment, and classified rather than left
    # unclassified because the hook that flags a rising ?? count is right to: the
    # 2026-09-03 SetPersistentTimer defect was exactly this delta.
    #
    # Both are BTstack API calls made from BTstack's OWN packet handler, which is the
    # context BTstack documents application code as running in -- it is single
    # threaded and the handler IS the run-loop context. Direct precedent in this same
    # handler, running at this same level since M4 was built: l2cap_accept_connection,
    # l2cap_decline_connection, hci_send_cmd, sdp_client_query_uuid16,
    # gap_pin_code_response. And this driver has been SENDING ACL from here for
    # dozens of runs ("ACL packets sent 20").
    #
    # ⚠ Neither allocates: HAVE_MALLOC is undefined in this build, so BTstack uses
    # static pools only. l2cap_send can REFUSE when the packet buffer is busy, which
    # is precisely why the send is driven from CAN_SEND_NOW rather than issued
    # directly when the channel opens.
    'l2cap_send': 'BTstack API from its own packet handler; static pools, no\n'
                  '                  allocation. Refusal is reported, not blocking',
    'l2cap_request_can_send_now_event': 'sets a flag and returns; the sanctioned way\n'
                  '                  to wait for a free packet buffer',
    'l2cap_register_service': 'registers a listener in a static table; no allocation',
    # ⚠ Also classified only after reading each one, same hook prompt (60 -> 63).
    # All four are pure reads or arithmetic over a buffer the caller already holds:
    #   HCI_OPCODE                                   macro, ((ocf) | ((ogf) << 10))
    #   hci_event_command_complete_get_command_opcode  static inline, reads 2 bytes
    #   hci_event_command_complete_get_return_parameters static inline, &event[5]
    #   little_endian_read_16                        two byte loads and a shift
    'HCI_OPCODE': 'macro / pure arithmetic',
    'hci_event_command_complete_get_command_opcode': 'pure read of the event buffer',
    'hci_event_command_complete_get_return_parameters': 'pure, returns &event[5]',
    'little_endian_read_16': 'pure',
    # ⚠ v6.5's gate instrument, classified only after reading each body in hci.c,
    # same hook prompt (59 -> 63). These are the READ-ONLY predicates BTstack itself
    # consults before every send, so calling them costs exactly what BTstack already
    # spends on our behalf in this same handler:
    #   hci_is_packet_buffer_reserved        one global bool read (hci.c:859)
    #   hci_can_send_command_packet_now      that flag + our own can_send_packet_now
    #                                        + a num_cmd_packets compare (hci.c:757)
    #   hci_can_send_acl_classic_packet_now  that flag + the slot count below
    #   hci_number_free_acl_slots_for_connection_type
    #                                        walks hci_stack->connections and does
    #                                        integer arithmetic; no allocation
    # ⚠ Both macros they lean on are no-ops in THIS build, which is why "it only
    # reads" is true rather than merely likely: btstack_config.h leaves
    # ENABLE_LOG_ERROR undefined, so log_error is (void)(0) (btstack_debug.h:146),
    # and with HAVE_ASSERT undefined btstack_assert is {(void)(condition);}
    # (btstack_debug.h:96). Neither can reach the File Manager or abort.
    'hci_is_packet_buffer_reserved': 'pure read of one BTstack global',
    'hci_can_send_command_packet_now': 'pure reads; no allocation, never blocks',
    'hci_can_send_acl_classic_packet_now': 'pure reads; no allocation, never blocks',
    'hci_number_free_acl_slots_for_connection_type':
        'walks the connection list; integer arithmetic only',
    'hci_event_connection_complete_get_bd_addr':
        'static inline reverse_bd_addr of &event[5]; pure',
    'reverse_bd_addr': 'pure byte copy',
    # ⚠ v6.6's auth-path instrument, same hook prompt (59 -> 62), each read in
    # btstack_event.h before being classified. All three are one-line static inlines
    # over a buffer the caller already holds:
    #   ..._connection_request_get_bd_addr          reverse_bytes(&event[2], .., 6)
    #   ..._connection_request_get_class_of_device  little_endian_read_24(event, 8)
    #   ..._connection_request_get_link_type        event[11]
    # ⚠ CLASSIFIED ONLY AFTER READING BOTH, prompted by the PostToolUse hook flagging the
    # count going 58 -> 60 on the 2026-09-17 outgoing-connect work (a202915). Note the
    # hook fired on a LATER, unrelated edit to bt_switch.c -- these two entered the graph
    # at a202915 and the audit had simply not been re-run since, so the delta named the
    # wrong commit. Provenance checked before classifying: neither symbol occurs in
    # bt_switch.c at all.
    #   btstack_event.h:715  reverse_bytes(&event[2], bd_addr, 6);  -- that is the whole
    #   body, a static inline. Pure read; legal at any level.
    'hci_event_link_key_request_get_bd_addr': 'pure byte copy out of the event buffer '
                                              '(btstack_event.h:715, static inline)',
    #   hid_host.c:1249 -- list lookup, then hid_host_create_connection ->
    #   btstack_memory_hid_host_connection_get(), then l2cap_create_channel or
    #   sdp_client_register_query_callback. THE LOAD-BEARING FACT is that
    #   src/btstack_config.h leaves HAVE_MALLOC undefined ("if HAVE_MALLOC is ever
    #   defined, this driver hangs -- not 'might'") with MAX_NR_HID_HOST_CONNECTIONS 1,
    #   so that _get() hands out a slot from a STATIC POOL and never reaches the Memory
    #   Manager. Its two onward calls are already in the traversal.
    # ⚠ CLASSIFIED ONLY AFTER READING BOTH, prompted by the count going 58 -> 60 when
    # 15.0's connect sweep landed. Both are pure reads of the STATIC link-key array in
    # bt_linkkey_db.c -- BT_LinkKeyCount loops gKeys[] counting a `used` flag, and
    # BT_LinkKeyGetByIndex delegates to BT_LinkKeyGetByIndexEx, which memcpys out of the
    # same array. No allocation, no File Manager, no blocking. That file is RAM-only by
    # design precisely so BTstack's link-key callbacks can run from a completion
    # (docs/M3-DESIGN.md 3b); the sweep timer is the same context.
    # ⚠ CLASSIFIED ONLY AFTER READING THE PORT AND THE BASE, because this project's
    # 58 -> 59 scar was SetPersistentTimer -- a TIMER call that was not legal in the
    # context it was made from, where the timer silently never started. So a timer symbol
    # gets read, not waved through.
    #   os9_set_timer          (src/btstack_run_loop_os9.c:137) is ONE assignment:
    #                          ts->timeout = hal_time_ms() + timeout + 1.
    #   btstack_run_loop_base_add_timer is a linked-list insert into a STATIC list head.
    # No allocation, no blocking, no File Manager. It is also the mechanism the battery
    # probe and the reconnect already use from this same context, and the battery probe
    # demonstrably fires on hardware.
    # ⚠ It ASSERTS if the same timer is added twice -- hence the gSweepArmed guard, which
    # bt_sweep_timeout clears before re-arming.
    'btstack_run_loop_set_timer': 'timeout arithmetic only; no allocation',
    'btstack_run_loop_set_timer_handler': 'stores a function pointer in the caller-owned '
                                          'timer source; no allocation',
    'BT_LinkKeyCount': 'pure loop over the static gKeys[] array',
    'BT_LinkKeyGetByIndex': 'memcpy out of the static gKeys[] array; no allocation',
    'hid_host_connect': 'static-pool slot + l2cap/sdp enqueue; no allocator because '
                        'HAVE_MALLOC is undefined (btstack_config.h)',
    # ⚠ CLASSIFIED ONLY AFTER TRACING APPLE'S OWN DRIVER, prompted by the PostToolUse
    # hook flagging the count going 58 -> 60 when the mouse path landed. The headline
    # "no forbidden primitive is reachable" does NOT cover an unclassified symbol --
    # that is precisely the 58 -> 59 SetPersistentTimer defect of 2026-09-03, where the
    # call was illegal in the context it was made from, the timer never started, and
    # scanning broke with a CLEAN audit.
    #
    # THE EVIDENCE, in usb-ddk/Examples/MouseModule -- Apple's shipping OS 9 USB mouse
    # driver -- is a call chain identical in shape to ours:
    #
    #   MouseCompletionProc          MouseModule.c:355, installed as pb.usbCompletion
    #                                => a USB completion routine, secondary interrupt level
    #     case kReadInterruptPipe    MouseModule.c:541
    #       (*pNotificationRoutine)  MouseModule.c:545
    #         USBMouseIn             HIDEmulation.c:81
    #           CursorDeviceMove     HIDEmulation.c:97
    #           CursorDeviceButtons  HIDEmulation.c:105
    #
    # ⇒ Both traps are called by Apple from a USB completion, which is the same level
    # BhRecordReport runs at. Neither allocates; they hand deltas and a button mask to
    # the Cursor Device Manager.
    #
    # ⚠ CursorDeviceNewDevice is NOT here and must not be: it ALLOCATES. It lives in
    # BT_MouseServiceAtTask, reached only from the defer response at task level, which
    # is why the mouse path drops reports until the device exists.
    # ⚠ CLASSIFIED v16.4, having been introduced unclassified by v15.5/v16.0. An
    # unclassified symbol is not a cleared one -- the audit simply ignores it and still
    # prints CLEAN, which is the FSpSetFInfo lesson. Each checked against its source:
    #
    #   gap_set_page_timeout (hci.c:10685) is NOT a bare store: two stores then
    #   hci_run(). That is the same shape as gap_disconnect and gap_remote_name_request
    #   below, both already vouched for at this level with "...+ hci_run; no alloc, no
    #   blocking" -- and hci_run is how BTstack is driven here, from the pump at
    #   secondary interrupt level. Reached via ConfigDone -> BT_StackStart.
    #
    #   The two hci_event_mode_change_get_* are static inlines doing
    #   little_endian_read_16 on the event buffer (btstack_event.h:677,695). Pure reads.
    #
    # ⚠ CursorDeviceNextDevice is deliberately absent: it is reachable ONLY at task
    # level (BT_MouseServiceAtTask), the audit confirms it appears nowhere below, and
    # listing it here would vouch for a level it is not used at.
    'gap_set_page_timeout': 'two stores into hci_stack then hci_run(); identical shape '
                            'to gap_disconnect/gap_remote_name_request already cleared '
                            'at this level. No allocation, no blocking.',
    'hci_event_mode_change_get_handle': 'static inline little_endian_read_16 on the '
                                        'event buffer; a pure read.',
    'hci_event_mode_change_get_interval': 'static inline little_endian_read_16 on the '
                                          'event buffer; a pure read.',
    'CursorDeviceMove': 'Cursor Device Manager trap; called from a USB completion '
                        '(secondary interrupt) by Apple\'s own OS 9 mouse driver -- '
                        'usb-ddk MouseModule HIDEmulation.c:97. No allocation.',
    'CursorDeviceButtons': 'same trap family and same Apple call site '
                           '(HIDEmulation.c:105); a button mask, no allocation.',
    #   hid_host.c:1286 -- hid_host_get_connection_for_hid_cid (a walk of the static
    #   connection list), a state switch that returns early for the three already-closing
    #   states, then l2cap_disconnect(cid) and return. No allocation, no File Manager, and
    #   it does NOT free the connection itself: hid_host_finalize_connection runs later,
    #   from the L2CAP close event. l2cap_disconnect is already reachable from the event
    #   handlers, so this adds no new primitive to the graph.
    'hid_host_disconnect': 'list walk + state guard + l2cap_disconnect; frees nothing '
                           'itself (finalize runs from the later close event)',
    #   hid_host.c:1341 -- the whole body is a list walk, two state guards, three field
    #   stores and l2cap_request_can_send_now_event. No allocation, no File Manager, and
    #   NOTHING IS SENT HERE: the actual GET_REPORT goes out later from the can-send-now
    #   callback, which is why it returns ERROR_CODE_SUCCESS whether or not the device
    #   ever answers. Contrast hid_host_send_set_report on the next line, which also
    #   stores a CALLER-OWNED report POINTER for that later send -- the lifetime trap
    #   documented at the Caps LED. get_report carries no buffer, so that trap does not
    #   apply here.
    'hid_host_send_get_report': 'list walk + state stores + request-can-send-now; sends '
                                'nothing itself and carries no buffer',
    'hci_event_connection_request_get_bd_addr': 'pure byte copy out of the event',
    'hci_event_connection_request_get_class_of_device': 'pure read of the event buffer',
    'hci_event_connection_request_get_link_type': 'pure read of the event buffer',
    # ⚠ v9.2's Authentication Complete ring, same hook prompt (58 -> 59). Read in
    # vendor/btstack/src/btstack_event.h:405 before classifying, not assumed from the
    # name -- the 2026-09-03 SetPersistentTimer delta was exactly this shape and the
    # call turned out to be illegal from the context it was made in.
    #   static inline uint16_t ..._get_connection_handle(const uint8_t *event)
    #   { return little_endian_read_16(event, 3); }
    # Two byte loads and a shift over a buffer the caller already holds. Its sibling
    # ..._get_status is the same shape and was classified on the line above at v6.6.
    # ⭐ Reading it also VERIFIED the field offsets this ring depends on: Authentication
    # Complete carries status at event[2] and the handle at event[3..4].
    'hci_event_authentication_complete_get_connection_handle':
        'static inline little_endian_read_16(event, 3); pure',
    # ⚠ v9.4's MODE CHANGE publisher, same hook prompt (58 -> 59). Read in
    # vendor/btstack/src/btstack_event.h:686 rather than assumed from the name:
    #   static inline uint8_t ..._get_mode(const uint8_t *event) { return event[5]; }
    # A single byte load out of a buffer the caller already holds -- the simplest
    # accessor in the family. ⭐ Reading it also confirmed the offset the publisher
    # depends on: Mode Change carries status at event[2], handle at [3..4], mode at [5].
    'hci_event_mode_change_get_mode': 'static inline return event[5]; pure',
    # ⚠ v9.5 hands the HID job to BTstack's own host. Hook fired 58 -> 60; both read in
    # vendor/btstack/src/classic/hid_host.c before classifying, not assumed:
    #   hid_host_init (1174)  two pointer assignments, a btstack_assert, and TWO
    #     l2cap_register_service calls -- the same primitive our own registration
    #     already makes from the same function (BT_StackStart), already classified
    #     safe at v8.6. No allocation, no File Manager, no trap.
    #   hid_host_register_packet_handler (1193)  a single pointer assignment.
    # ⭐ Neither introduces a new execution level: they are called from BT_StackStart,
    # beside code that has run on hardware for eleven versions.
    'hid_host_init': 'pointer stores + l2cap_register_service; same context as ours',
    'hid_host_register_packet_handler': 'single pointer assignment; pure',
    # ⚠ v9.6 pulls the GLOBAL security lever to match Tiger. Count rose 58 -> 60 and
    # the hook did NOT fire, because the edit went through a script rather than the
    # Edit tool -- so the delta was caught by reading the audit output rather than by
    # the guard. Worth knowing: the hook covers Edit, not every path to a source file.
    # Both read at vendor/btstack/src/hci.c:5752-5760 before classifying:
    #   gap_set_security_level  one struct field store
    #   gap_get_security_level  one branch on gap_secure_connections_only_mode, then
    #                           one field read
    # No allocation, no trap, no File Manager, and both are called from BT_StackStart
    # beside code that has run on hardware since M2.
    'gap_set_security_level': 'single struct field store; pure',
    'gap_get_security_level': 'one branch and one field read; pure',
    # v10.0. READ BEFORE CLASSIFYING, because the hook that flagged this delta cites a
    # real defect of exactly this shape (58 -> 59 with SetPersistentTimer on
    # 2026-09-03: the call was illegal from its context, the timer never started, and
    # scanning broke silently).
    #   hci_set_master_slave_policy (hci.c:10474)  the ENTIRE body is
    #       hci_stack->master_slave_policy = policy;
    #   One struct field store. No allocation, no File Manager, no spin-wait, no timer
    #   install, no Toolbox call, no live-QH reprogramming -- nothing on FORBIDDEN and
    #   nothing that could reach it. Strictly safer than gap_set_security_level,
    #   hid_host_init, l2cap_init and hci_power_control, all of which already run from
    #   the same place: BT_StackStart, reached from ConfigDone on a USB completion.
    'hci_set_master_slave_policy': 'single struct field store; pure',
    #   hid_host_send_set_report (hid_host.c:1359)  looks up the connection, range-
    #   checks against l2cap_max_mtu(), stores five fields INCLUDING THE CALLER'S
    #   POINTER, and calls l2cap_request_can_send_now_event. No allocation, no
    #   blocking, no File Manager. It is called from BhRecordReport, which is
    #   BTstack's own packet handler -- the same context every other l2cap call in
    #   this driver already runs in.
    #   ⚠⚠ ITS POINTER SEMANTICS ARE THE HAZARD, NOT ITS EXECUTION LEVEL: it does
    #   NOT copy the report, so the buffer must outlive the call. See bt_btstack.c.
    'hid_host_send_set_report': 'field stores + l2cap_request_can_send_now; no alloc',
    #   hid_host_send_report (hid_host.c:1330) is the INTERRUPT-channel twin of the
    #   above and its body is the same shape: connection lookup, two state range
    #   checks, an l2cap_max_mtu() bound check, five field stores, and
    #   l2cap_request_can_send_now_event on the interrupt CID. No allocation, no
    #   blocking, no File Manager, no trap. Same call site as its twin -- BhRecordReport,
    #   inside BTstack's own packet handler -- so it runs at the level every other l2cap
    #   call in this driver already runs at. Read, not assumed, after the hook flagged
    #   58 -> 59.
    #   ⚠⚠ IT CARRIES THE SAME POINTER HAZARD, which is the thing to remember about
    #   this pair: `connection->report = report` is stored and dereferenced LATER, so
    #   the caller's buffer must outlive the call. bt_btstack.c keeps it static.
    'hid_host_send_report': 'field stores + l2cap_request_can_send_now; no alloc',
    #   gap_disconnect (hci.c) -- reachable as
    #     bt_pump_timer_sih -> BT_ServiceMailbox -> BT_DisconnectByAddr -> gap_disconnect
    #   so it runs at SECONDARY INTERRUPT LEVEL, not task level. Read the body before
    #   accepting that: a connection lookup, a switch on conn->state, one field store,
    #   and hci_run(). No allocation, no File Manager, no blocking, no trap. hci_run is
    #   BTstack's dispatcher and this driver already calls it from every USB completion,
    #   so this adds no new context, only a new caller of one it already uses.
    'gap_disconnect': 'conn lookup + state store + hci_run; no alloc, no blocking',
    #   hci_connection_for_bd_addr_and_type (hci.c:423) is a READ-ONLY WALK of
    #   hci_stack->connections: iterator init, a type compare, a 6-byte memcmp, return
    #   the node or NULL. No allocation, no blocking, no trap, and it MUTATES NOTHING.
    #   Reached from BT_ServiceMailbox (secondary interrupt level) and from
    #   BT_SampleLiveConns in the publish path, which is the same level every other
    #   BTstack call in this driver already runs at.
    #   ⚠ Bounded: the loop runs over the connection list, which on this card is one or
    #   two entries, and BT_SampleLiveConns calls it at most kLiveSlots + kPagedSlots
    #   times. Chosen over maintaining our own shadow of the connection list precisely
    #   because the shadow -- one 'most recent peer' global -- is what produced the bug.
    'hci_connection_for_bd_addr_and_type': 'read-only list walk; no alloc, no mutation',
    #   ---- v13.0, the remote-name trio. All three read at interrupt level, from the
    #   same hci_event_handler every other event accessor in this driver runs in.
    #   gap_remote_name_request (hci.c): one state test, a 6-byte memcpy into
    #     hci_stack, two field stores, a state store and hci_run(). No allocation, no
    #     blocking. ⚠ It returns COMMAND_DISALLOWED if a name request is already in
    #     flight -- which is why the caller asks once per address and treats a refusal
    #     as 'ask again next connection' rather than retrying in place.
    'gap_remote_name_request': 'state test + memcpy into hci_stack + hci_run; no alloc',
    #   The two accessors are pure pointer arithmetic over the event buffer:
    #   ..._get_bd_addr is reverse_bytes(&event[3], 6) and ..._get_remote_name simply
    #   returns &event[9]. ⚠⚠ THE SECOND RETURNS A POINTER INTO THE EVENT BUFFER AND
    #   THE NAME IS NOT GUARANTEED NUL-TERMINATED -- the copy in bt_btstack.c is bounded
    #   to kNameChars-1 and terminates itself for exactly that reason.
    'hci_event_remote_name_request_complete_get_status': 'returns event[2]; pure',
    'hci_event_remote_name_request_complete_get_bd_addr': 'reverse_bytes over the event; pure',
    'hci_event_remote_name_request_complete_get_remote_name': 'returns &event[9]; pure pointer',
    #   hci_event_role_change_get_status (btstack_event.h:640)  `return event[2];`
    #   hci_event_role_change_get_role   (btstack_event.h:658)  `return event[9];`
    #   Both are `static inline` single byte reads out of the caller's own packet
    #   buffer. No call of any kind, so nothing on FORBIDDEN is reachable through
    #   them, and they are legal wherever the packet itself is -- which is the event
    #   handler, i.e. exactly where every other hci_event_*_get_* accessor we already
    #   classify is called from.
    'hci_event_role_change_get_status': 'static inline; returns event[2]; pure',
    'hci_event_role_change_get_role': 'static inline; returns event[9]; pure',
    # ---------------------------------------------------------------------------
    # M5's injector. ⚠⚠ THESE THREE ARE THE ONLY TOOLBOX CALLS THIS PROJECT MAKES
    # FROM INTERRUPT LEVEL, so they get the longest justification in this file.
    #
    # THE EVIDENCE IS APPLE'S OWN OS 9 USB KEYBOARD DRIVER, which calls all three from
    # a USB COMPLETION ROUTINE. The chain, read rather than assumed:
    #     KeyboardModule.c:266  KeyboardCompletionProc(USBPB *pb)   <- the completion
    #     KeyboardModule.c:378    case kReadInterruptPipe: NotifyRegisteredHIDUser(...)
    #     KBDHIDEmulation.c:168     -> (*pSHIMInterruptRoutine)(...)
    #     KeyIn.c:644               -> USBDemoKeyIn -> PostUSBKeyToMac
    #     KeyIn.c:452                  -> PostADBKeyToMac: KeyTranslate + PostEvent
    # ⭐ KeyboardCompletionProc is the ONLY function defined between KeyboardModule.c
    # :260 and :400, so :378 is unambiguously inside it. That was checked, not assumed.
    #
    #   PostEvent      posts to the system event queue. Interrupt-safe by design --
    #                  it is how ADB and serial drivers have always delivered input --
    #                  and Apple calls it from the completion above. Two independent
    #                  grounds, which is the bar this file should hold to.
    #   KeyTranslate   reads the KCHR data through a raw LOCKED pointer and updates a
    #                  caller-owned state long. No allocation, and NO Resource Manager:
    #                  bt_inject.c acquires and locks the handle once at TASK level in
    #                  BT_InjectInit, exactly as Apple does in InitUSBKeyboard
    #                  (KeyIn.c:355), so the interrupt path never touches a handle.
    #   TickCount      reads the Ticks low-memory global. Trivial, and Apple calls it
    #                  in the same function to stamp the repeat globals.
    #
    # ⚠ GetResource / HLock / ResError are deliberately NOT here. They are reachable
    # only from BT_InjectInit, which is called from ProbeInitialize at TASK level. If
    # they ever show up in this audit's output it means that separation has broken, and
    # that is a finding, not something to silence by adding them to this list.
    'PostEvent': 'system event queue; interrupt-safe; Apple posts from a USB completion',
    'KeyTranslate': 'reads a locked KCHR pointer; no allocation, no Resource Manager',
    'TickCount': 'reads the Ticks low-memory global',
    'reverse_bytes': 'pure byte copy',
    'little_endian_read_24': 'pure',
    # ⚠ v7.0's security instrument, same hook prompt (59 -> 62), each read in
    # btstack_event.h before being classified. One-line static inlines over a buffer
    # the caller already holds:
    #   ..._security_level_get_handle          little_endian_read_16(event, 2)
    #   ..._security_level_get_security_level  event[4]
    #   ..._security_level_get_status          event[5]
    'gap_event_security_level_get_handle': 'pure read of the event buffer',
    'gap_event_security_level_get_security_level': 'pure read of the event buffer',
    'gap_event_security_level_get_status': 'pure read of the event buffer',
    # ⚠ The two PREDICATES the instrument samples, read in hci.c rather than assumed.
    # gap_security_level walks to the connection and returns a value derived from its
    # authentication_flags and encryption_key_size -- pointer chase plus comparisons,
    # no allocation, nothing that can block. hci_remote_features_available is a single
    # bonding_flags test. Both are strictly lighter than the hci_send_cmd this same
    # handler already calls.
    'gap_security_level': 'connection lookup + flag comparisons; no allocation',
    'hci_remote_features_available': 'single bonding_flags test on the connection',
    # ⚠⚠ v7.2's initiator pairing, and this one was worth the audit's insistence.
    # gap_dedicated_bonding CREATES A CONNECTION OBJECT, so "does it allocate" is a
    # real question and not a formality. btstack_memory.c has THREE variants of
    # btstack_memory_hci_connection_get, chosen by config:
    #     pool size > 0, no HAVE_MALLOC  -> btstack_memory_pool_get
    #     HAVE_MALLOC                    -> malloc()   <- FORBIDDEN below task level
    #     pool size 0                    -> returns NULL
    # This build compiles the FIRST: btstack_config.h leaves HAVE_MALLOC undefined
    # ("NEVER", with its own warning that defining it hangs the driver) and sets
    # MAX_NR_HCI_CONNECTIONS 2. btstack_memory_pool_get is a linked-list pop -- read
    # it -- so there is no allocator on this path. Everything after that in
    # gap_dedicated_bonding is flag writes plus hci_run.
    # ⇒ Safe here ONLY because of that config. If HAVE_MALLOC is ever defined this
    # entry becomes a lie, which is the second reason btstack_config.h says never.
    'gap_dedicated_bonding':
        'static pool (no HAVE_MALLOC), flag writes, hci_run; no allocator',
    # ⚠ v7.7's Apple-matched Create_Connection parameters, same hook prompt
    # (59 -> 61). Both are ONE assignment into hci_stack and nothing else -- read in
    # hci.c, not assumed:
    #   hci_enable_acl_packet_types  hci_stack->enabled_packet_types_acl = types
    #   gap_set_allow_role_switch    hci_stack->allow_role_switch = allow ? 1 : 0
    # ⚠ Both are called from BT_StackStart, which ConfigDone reaches at interrupt
    # level, so they ARE on an interrupt path -- and a single store to an already
    # allocated struct is safe there. They must stay that shape: if a future BTstack
    # makes either of them do work, this entry becomes a lie.
    'hci_enable_acl_packet_types': 'one store into hci_stack; no allocation',
    'gap_set_allow_role_switch': 'one store into hci_stack; no allocation',
}

# C keywords and control-flow that regex-match a call but are not calls.
NOT_CALLS = {
    'if', 'for', 'while', 'switch', 'return', 'sizeof', 'do', 'else',
    'defined', 'case', 'break', 'continue', 'goto', 'static', 'const',
    'unsigned', 'signed', 'struct', 'union', 'enum', 'typedef', 'void',
}


# Matches a whole cpp line-marker LINE INCLUDING its surrounding newlines, so the
# substitution JOINS the lines either side of it rather than leaving a gap.
LINE_MARKER = re.compile(r'\n[ \t]*#[ \t]*\d+[ \t]+"[^"]*"[^\n]*\n')


def strip_line_markers(src):
    """Remove cpp line markers, e.g.  # 125 "foo.c" 3 4

    Needed only when auditing PREPROCESSED output, which is the trustworthy mode:
    it eliminates dead #ifdef branches and expands macros, both of which the
    source-text mode gets wrong. But cpp splits a definition across a marker --

        _Bool
        # 125 "btstack_linked_list.c"
            btstack_linked_list_add_tail(...){

    -- and the definition regex needs the return type and the name contiguous. So
    without this, 27 BTstack functions that ARE defined were reported as
    unclassified externals. Verified against the actual bytes, not guessed.

    The marker is replaced by a single SPACE, not by nothing: removing it but
    leaving the newlines still leaves `_Bool` and the function name on separate
    lines, and the definition regex cannot span a newline. Joining them is what
    actually fixes it.

    Joining cannot invent definitions. The regex only matches from a line start
    through a character class that excludes '(' and ';', so splicing
    `foo(); bar(){` onto one line still fails to match `bar` as a definition.

    Only numbered markers are touched, so real #include and #define lines in
    ordinary source are left exactly as they were.
    """
    return LINE_MARKER.sub(' ', src)


def strip_comments_and_strings(src):
    """Remove // and /* */ comments and "..." / '...' literals.

    NOT optional. A function name inside a comment creates a phantom edge, and
    an audit that cries wolf is one that gets ignored. Pascal-string literals
    ("\\pOS9BTProbe: ...") are especially dangerous here because they contain
    words that look like identifiers.
    """
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
        elif c in '"\'':
            quote = c
            i += 1
            while i < n and src[i] != quote:
                if src[i] == '\\':
                    i += 1
                i += 1
            i += 1
            out.append(' ')
        else:
            out.append(c)
            i += 1
    return ''.join(out)


# ⚠ THE ANCHOR IS `^ OR AFTER ; OR }`, AND THE ALTERNATIVES ARE NOT OPTIONAL.
#
# It was `^` alone, and that interacted with strip_line_markers to hide a large
# class of definitions. cpp emits a marker immediately before a function whenever
# it resumes after an include or jumps a line, which is extremely common:
#
#     }                                        <- end of the previous function
#     # 6138 "vendor/btstack/src/hci.c"        <- marker
#     int hci_power_control(HCI_POWER_MODE power_mode){
#
# strip_line_markers replaces the marker AND its two newlines with one space --
# it has to, that is what fixes the split-return-type case -- so the text becomes
# `} int hci_power_control(...){` and `int` is no longer at a line start. The
# definition was then invisible.
#
# The consequence was not cosmetic. An unparsed definition is not in `bodies`, so
# the BFS treats the function as a LEAF and never walks its body: the traversal
# stopped at every such function, and the FORBIDDEN check can only see what the
# traversal reaches. Measured on the vendored BTstack corpus: 4 of the audit's own
# declared roots reported "ENTRY POINT NOT FOUND", which the script itself calls a
# blind entry that proves nothing.
#
# Accepting `;` and `}` as anchors admits exactly what a joined marker leaves
# behind. It cannot invent a definition out of a call, because the pattern still
# requires a return type AND a name AND a '{' after the argument list: `} foo(a);`
# has no return type and ends in ';', and neither half matches. Control-flow
# keywords that do fit the shape (`} else if (x) {`) capture a name in NOT_CALLS
# and are discarded by the caller.
FUNC_DEF = re.compile(
    r'(?:^|[;}])[ \t]*[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{',
    re.M)
CALL = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')


def main():
    missing = [f for f in FILES if not os.path.exists(f)]
    if missing:
        print('ERROR: source not found: %s' % ', '.join(missing))
        return 2

    bodies = {}
    bodies_src = []
    for path in FILES:
        with open(path, 'r', errors='replace') as fh:
            src = strip_comments_and_strings(strip_line_markers(fh.read()))
        bodies_src.append(src)
        for m in FUNC_DEF.finditer(src):
            name = m.group(1)
            if name in NOT_CALLS:
                continue
            # Body = from the opening brace to its match.
            start = src.index('{', m.end() - 1)
            depth, j = 0, start
            while j < len(src):
                if src[j] == '{':
                    depth += 1
                elif src[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            bodies[name] = src[start:j]

    edges = {}
    for name, body in bodies.items():
        callees = set()
        for m in CALL.finditer(body):
            callee = m.group(1)
            if callee not in NOT_CALLS and callee != name:
                callees.add(callee)
        edges[name] = callees

    print('=' * 74)
    print('  BELOW-TASK-LEVEL AUDIT -- OS 9 Bluetooth driver')
    print('=' * 74)
    print('sources : %s' % ', '.join(FILES))
    print('defined : %d functions' % len(bodies))

    entries = ROOTS_OVERRIDE if ROOTS_OVERRIDE is not None else ENTRIES
    unknown_entries = [e for e in entries if e not in bodies]
    for e in unknown_entries:
        print('  !! ENTRY POINT NOT FOUND: %s '
              '-- a blind entry proves nothing' % e)
    print('roots   : %s' % ', '.join(e for e in entries if e in bodies))
    print()

    # BFS, remembering how we got there so a hit can be explained.
    parent, order = {}, []
    queue = collections.deque()
    for e in entries:
        if e in bodies:
            parent[e] = None
            queue.append(e)
    while queue:
        fn = queue.popleft()
        order.append(fn)
        for callee in sorted(edges.get(fn, ())):
            if callee not in parent:
                parent[callee] = fn
                queue.append(callee)

    def path_to(sym):
        chain, cur = [], sym
        while cur is not None:
            chain.append(cur)
            cur = parent.get(cur)
        return ' -> '.join(reversed(chain))

    reached_internal = [f for f in order if f in bodies]
    externals = sorted(s for s in parent if s not in bodies)

    print('-- OUR functions reachable below task level (%d) --'
          % len(reached_internal))
    for fn in sorted(reached_internal):
        print('     %s' % fn)
    print()

    violations = [s for s in externals if s in FORBIDDEN]
    safe = [s for s in externals if s in KNOWN_SAFE]

    # A third category, and it exists because the definition regex is a heuristic
    # rather than a C parser. On PREPROCESSED input especially, a definition can be
    # laid out in a way the regex will not match, and the symbol then looks like an
    # unknown external when it is really defined right here in the corpus. Verified
    # case: btstack_linked_list_add_tail, where cpp emitted the return type, a line
    # marker, and the name on three separate lines.
    #
    # Reporting these separately is the honest option. Calling them unclassified
    # overstates the risk and buries the few symbols that genuinely are unknown;
    # adding them to KNOWN_SAFE would be a lie, because they are not safe
    # primitives, they are ordinary internal functions.
    # ⚠ THIS TEST MUST DEMAND A DEFINITION, NOT MERELY A LINE-START CALL.
    #
    # It used to be  ^[ \t]*SYM[ \t]*\(  which also matches an ordinary call
    # written as its own statement:
    #
    #         btstack_memory_init();          <-- line start, then '('
    #         Microseconds(&us);
    #
    # Every such external was filed as "defined in the corpus", and because this
    # bucket is deliberately excluded from `unclassified` it does not affect the
    # exit code -- so the audit printed CLEAN while silently excusing real
    # externals. Measured: 10 of them across the driver-side file set, including
    # `Microseconds`, `hci_init` and `hci_power_control`.
    #
    # That is the precise failure this script was written to prevent, and the scar
    # is already in the docstring: an audit that cannot see a primitive is worse
    # than no audit, because it is believed. (The FORBIDDEN check was never
    # affected -- it is applied to `externals` before this split -- so no hazard on
    # the blocklist was ever missed. What was defeated is the completeness half,
    # i.e. exactly the PBStatusSync / NMRemove class of bug.)
    #
    # A definition's argument list is followed by '{'; a call's is followed by ';'.
    # Requiring the brace is what separates them, and it still admits the
    # cpp-split layout this bucket exists for, because strip_line_markers has
    # already joined those lines by then.
    corpus = '\n'.join(bodies_src)
    def defined_in_corpus(sym):
        return re.search(r'^[ \t]*' + re.escape(sym) + r'[ \t]*\([^;{]*\)[ \t\r\n]*\{',
                         corpus, re.M) is not None

    rest = [s for s in externals if s not in FORBIDDEN and s not in KNOWN_SAFE]
    unparsed = [s for s in rest if defined_in_corpus(s)]
    unclassified = [s for s in rest if s not in unparsed]

    print('-- external primitives reachable below task level (%d) --'
          % len(externals))
    for s in safe:
        print('  ok   %-32s %s' % (s, KNOWN_SAFE[s]))
    print()

    if unparsed:
        print('-- defined in the corpus, but the regex did not parse the definition --')
        print('   (not externals at all; the scanner is a heuristic, not a C parser)')
        for s_ in unparsed:
            print('  --   %s' % s_)
        print()

    if unclassified:
        print('-- UNCLASSIFIED: a human must look at each of these once --')
        for s in unclassified:
            print('  ??   %-32s via %s' % (s, path_to(s)))
        print()

    if violations:
        print('!! FORBIDDEN PRIMITIVES REACHABLE BELOW TASK LEVEL !!')
        for s in violations:
            print('  XX   %-32s %s' % (s, FORBIDDEN[s]))
            print('       via %s' % path_to(s))
        print()
        print('RESULT: FAIL -- %d forbidden primitive(s) reachable.'
              % len(violations))
        return 1

    if unclassified or unknown_entries:
        print('RESULT: INCOMPLETE -- no forbidden primitive is reachable, but '
              'the audit\n        cannot vouch for %d unclassified symbol(s)'
              '%s.' % (len(unclassified),
                       ' and %d missing entry point(s)' % len(unknown_entries)
                       if unknown_entries else ''))
        return 1

    print('RESULT: CLEAN -- nothing task-level-only is reachable from '
          'interrupt level,\n        and every external symbol on those paths '
          'is classified.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
