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
