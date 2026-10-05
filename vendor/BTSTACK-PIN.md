# BTstack — pinned revision

    repository  https://github.com/bluekitchen/btstack
    tag         v1.8.2          (latest release as of 2026-08-31)
    commit      075a0780f0fad7ff67d58ac19f46e8953656a752

Pinned, not tracked. M2 is a port onto an API surface, and a moving upstream would
make every failure ambiguous between "our glue is wrong" and "upstream changed".
Re-pin deliberately, in its own commit, never as a side effect.

Reproduce the survey tree with:

    git clone --depth 1 --branch v1.8.2 https://github.com/bluekitchen/btstack.git
    git rev-parse HEAD    # must print 075a0780f0fa...

## ⚠ LICENSE: BSD 3-clause PLUS a non-commercial clause

`LICENSE` is the familiar BSD 3-clause text with a **fourth condition**:

> 4. Any redistribution, use, or modification is done solely for personal benefit
>    and not for any commercial purpose or for monetary gain.

Consequences, and they are not only ours:

* **Fine for this project.** A personal hobby port is squarely "personal benefit".
* **Redistribution is permitted** by clauses 1 to 3, so vendoring the source into a
  repo is allowed provided the copyright notice and conditions travel with it.
* ⚠ **The clause propagates.** Anything built on this cannot be relicensed
  permissively. If the MDD FW800 enablement pack ever wants a clean
  do-what-you-like story, a BTstack-derived Bluetooth component cannot provide it,
  and the pack's licensing has to say so plainly rather than by omission.
* ⚠ GitHub reports the repo as `NOASSERTION` precisely because clause 4 makes it
  non-standard. Do not let a tool's "BSD-3-Clause" guess end up in our metadata.

⇒ Settle this **before** the Bluetooth repo goes public, together with the existing
`vendor/bt-control-center/` question. Neither is urgent while the repo is private.

## Size, and why we will not vendor the whole thing

    whole clone       280 MB
    src               8.1 MB   (44 .c files, the core)
    src/classic       2.5 MB   (l2cap is in src/, sdp_* and hid_* are here)
    src/ble           1.5 MB   not needed: this is Classic HID, not LE
    platform/embedded 168 KB   the reference run loop — see the design doc
    3rd-party          37 MB   not needed

Vendor the subset the port actually compiles, listed explicitly in the build rather
than by wildcard, so an accidental new dependency shows up as a link error instead
of silently pulling in more of the stack.

## What the survey established

**Static allocation is fully supported and needs no patching.** `btstack_memory.c`
switches on `HAVE_MALLOC`: leave it undefined and define the `MAX_NR_*` limits, and
every pool becomes a plain `static` array. That is a requirement here, not a
preference — `scripts/level-audit.py` forbids allocators on these paths.

**The HCI transport interface is small and maps onto the seam we already have.**
`hci_transport_t` needs: `init`, `open`, `close`, `register_packet_handler`,
`can_send_packet_now`, `send_packet`, and three extensions that are UART-only or
SCO-only and can be NULL. `send_packet` dispatches on packet type onto
`BT_SendHCICommand` (control) and `BT_SendACL` (bulk), both of which already exist
and are hardware-validated. Events and ACL come back up through the registered
handler, called from our two USL completions.

**The run loop is the architectural problem.** See `docs/M2-DESIGN.md`.
