# Local patches to vendored BTstack

This is a **vendored copy** of BTstack, not a submodule. The files below carry local
patches that are **not** upstream. Every patch is marked in-source with the greppable
token `claude-os9 PATCH`, so after any re-vendor:

```bash
grep -rn 'claude-os9 PATCH' vendor/btstack/src/
```

should return exactly the hits listed here. If it returns fewer, a re-vendor clobbered a
patch and it must be re-applied.

---

## src/hci.c — send `Read_Buffer_Size` on pre-1.2 controllers (DBT-120)

**Why.** BTstack gates `Read_Buffer_Size` (init substate `HCI_INIT_READ_BUFFER_SIZE`)
behind `hci_command_supported(SUPPORTED_HCI_COMMAND_READ_BUFFER_SIZE)`. That bitmap is
filled only by a successful `Read_Local_Supported_Commands` (0x1002) — a command that did
not exist before **Bluetooth 1.2**. A **1.1** controller (our CSR **DBT-120**, `HCI
version 1`) rejects 0x1002, so the bitmap stays empty, the gate is false,
`Read_Buffer_Size` is **skipped**, `hci_stack->acl_packets_total_num` stays **0**, and
`can_send_acl_classic` is **never** true — so no ACL data ever leaves the host. HCI-command
work (pairing: PIN, encryption, authentication, link-key store) all succeeds; the HID L2CAP
channel never opens because L2CAP config + HID reports ride ACL. The A1044 (BT **1.2**)
dodged this: it supports 0x1002, so the gate passes.

`Read_Buffer_Size` has been a **mandatory core command since BT 1.1**, and this port is
Classic-only (`ENABLE_BLE` undefined in `src/btstack_config.h`), so there is no LE-only
controller the gate needs to protect. Sending it unconditionally is correct here.

**Patch 1** — `HCI_INIT_READ_BUFFER_SIZE` case: remove the `hci_command_supported(...)`
`if`/fall-through; always set substate `HCI_INIT_W4_READ_BUFFER_SIZE`, `hci_send_cmd(&hci_read_buffer_size)`, `break`.

**Patch 2** — `HCI_OPCODE_HCI_READ_BUFFER_SIZE` command-complete handler: after the existing
`acl_packets_total_num` assignment, floor it to ≥ 1:
`if (hci_stack->acl_packets_total_num == 0) hci_stack->acl_packets_total_num = 1;`
Insurance against a counterfeit CSR reporting 0 ACL buffers (its self-reported limits are
untrustworthy — it claims a 679-byte ACL payload; see `btstack_config.h`). The card returns
`Number_Of_Completed_Packets` per send (Tiger-confirmed), so a serialized send-one/wait-one
path replenishes from a floor of 1. A genuine non-zero count (the A1044's) is left untouched.

**Found & fixed** 2026-11-02, from a clean-room DBT-120 BTCheck capture
(`free classic ACL slots 0`, `can_send_acl_classic NO`, keyboard's inbound PSM 0x11 Conn
Request captured with 0 packets out). Context: the `project_os9_bt_patha_continuous_and_btstack`
memory.

---

## src/classic/hid_host.c: no SDP query on an incoming connection (capsled1, release 18.7)

**What upstream does.** When a device that paged us has opened both HID channels and was
accepted in REPORT mode, `hid_host` emits `CONNECTION_OPENED`, then sets
`HID_HOST_W2_SEND_SDP_QUERY` and queries the device's SDP record for its HID descriptor. The
connection returns to `HID_HOST_CONNECTION_ESTABLISHED` only when that query completes; only
then does it emit `DESCRIPTOR_AVAILABLE` and send `SET_PROTOCOL`.

**Why it is patched.** `sdp_client.c` has no timeout, so a query that never completes leaves
the connection below ESTABLISHED for as long as the link lives. Interrupt-channel reports
still arrive (typing works), but `hid_host_send_report` and `hid_host_send_get_report` refuse
with `COMMAND_DISALLOWED` (0x0C). Measured on release 18.6, 2026-11-17: the A1016's Caps Lock
LED writes and its battery query were both refused 0x0C for the connection's whole 199 s,
with `descriptor available 0` and no SET_PROTOCOL handshake. Intermittent: every other
November session completed the query.

**Patch 3**, the incoming REPORT-mode branch of the interrupt channel's `CHANNEL_OPENED`
handling: instead of the SDP query, do what the SDP success path does for an incoming
connection, minus the query and the descriptor event. The connection stays ESTABLISHED (set
just above), `set_protocol = true`, `requested_protocol_mode = HID_PROTOCOL_MODE_REPORT`, and
`l2cap_request_can_send_now_event(control_cid)`, so `SET_PROTOCOL(REPORT)` (0x71) goes out at
once. That is Tiger's order on the same Mac, module and keyboard (`docs/TIGER-HCI-TRACE.md`:
the channels open, then `71` on the control channel, then the handshake; no SDP query on a
reconnect).

**What it costs.** No HID descriptor and no SDP sniff parameters for incoming connections.
Nothing in the driver reads either: the keyboard and mouse decoders are fixed parsers, and
`HID_SUBEVENT_DESCRIPTOR_AVAILABLE` was only ever counted, so BTCheck's `descriptor
available` now reads 0 for incoming connections by design. Outgoing connections
(`hid_host_connect`, used after pairing) still query SDP first: they need it for the PSMs.

**Found & fixed** 2026-11-17; verified on the MDD the same day (capsled1: every LED write
accepted on a first connection and a reconnect, the lamp on and off, battery queries all
answered, the handshake with `descriptor available 0`).
