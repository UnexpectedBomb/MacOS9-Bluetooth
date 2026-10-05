# OS 9 Bluetooth

A native **Mac OS 9** Bluetooth HID host stack. It pairs and drives Bluetooth keyboards and
mice on classic Mac OS 9, a capability Apple never shipped.

It runs the whole stack on the Mac: HCI, L2CAP, SDP and the HID host role, over a USB
Bluetooth controller, with an OS 9 control panel and Control Strip module on top.

## Status

**Working on a Power Mac G4 MDD (FW800) with the internal Apple Bluetooth module (A1044).**
Keyboard and mouse run at the same time, with battery levels for both.

| | |
|---|---|
| Keyboard | Apple Wireless Keyboard **A1016**: typing, modifiers, media keys and eject |
| Mouse | Apple Wireless Mouse **A1015**: cursor and button |
| Battery | both devices, in the control panel and the Control Strip |
| Naming | the device's own name, or one you choose (Rename) |
| Coexistence | runs alongside the USB 2.0 (EHCI) extension |
| Boot | the paired keyboard still reaches the Open Firmware boot picker |

⚠ **Scope is deliberate: the A1044 module, the A1016 keyboard and the A1015 mouse.** Those
are the devices this has been measured against. Other Bluetooth keyboards and mice may well
work (the HID paths are generic), but they are untested.

🚧 **USB Bluetooth dongles: the transport works and pairing reaches the PIN exchange, but
does not complete.** A generic dongle (`0A12:0001`, "BT DONGLE10") binds on a Power Mac G4
MDD FW400, reaches `HCI_STATE_WORKING`, runs an inquiry that finds and correctly classifies
an A1016, pages it, and gets as far as `legacy PIN requests 1 / ANSWERED 1`. Authentication
then dies because the USB Expert removes the device mid-pairing
(`kNotifyDriverBeingRemoved`), which resets the controller.

That removal is the single open question, and it is a **USB** question rather than a Bluetooth
one: the stack above it is doing its job. Paused 2026-10-04. **No device has been driven
through a dongle**, so the A1044 remains the only configuration measured end to end.

## Components

| file | goes in | what it does |
|---|---|---|
| `USB Bluetooth Support` | Extensions | the driver: USB transport, HCI, L2CAP, HID host, input injection |
| `USBBluetoothSwitch` | Extensions | takes the A1044 out of HID-proxy mode so the stack can own it |
| `Bluetooth` | Control Panels | scan, pair, delete, disconnect, rename; per-device details |
| `Bluetooth Strip` | Control Strip Modules | battery gauge, device switching, quick access |
| `BTCheck` | anywhere | **strictly optional.** A diagnostic that writes a log rather than making you read a window. The stack does not need it and never calls it: install it only if something is wrong and you want to see why, or if you are reporting a problem. |

⚠ Keep the extension named **`USB Bluetooth Support`**. The Mac OS USB Expert scans only
Extensions files whose name begins "USB", so renaming it stops the driver loading, with no
error and no other symptom.

## Known issues

- ⚠ **While a paired Bluetooth keyboard is switched on, a wired USB keyboard is ignored at
  the Open Firmware boot picker.** Your Bluetooth keyboard works there (that is the point,
  and it is tested), but the wired one beside it will not respond, including its Return key.
  Switch the Bluetooth keyboard off and the wired one works again immediately. Nothing of
  ours runs at Open Firmware time; pairing makes the Bluetooth card present a second USB
  keyboard, and the firmware appears to use only one. Measured both directions on a G4 MDD.
- After a failed pairing, the **first boot afterwards still needs a manual pairing click** in
  the control panel.
- The driver's retry-on-failed-open path has never executed on hardware, so it is written but
  unproven.
- `BTCheck`'s bus dump walks only one USB controller, so it under-reports what is on the bus.

## Building

Requires [Retro68](https://github.com/autc04/Retro68) with the PowerPC toolchain.

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build
```

The control panel, Control Strip module and BTCheck build the same way from `cpanel/`, `csm/`
and `bt-check/`. Several invariants are checked at build time and will fail the build rather
than ship quietly: the counter-block layout shared by four binaries, every BTstack run-loop
timer being added to the run loop, and the driver's PEF export hashing.

Off-target unit tests (report decoding, UTF-8 conversion, the HID identity rules) run with the
host compiler:

```
tests/run-tests.sh
```

## Credits and licensing

**[BTstack](https://github.com/bluekitchen/btstack)**, © 2009 BlueKitchen GmbH, provides the
Bluetooth host stack above the transport. It is used under its own licence, which is included
with the source. ⚠ That licence permits redistribution, use and modification **solely for
personal benefit and not for any commercial purpose or monetary gain**. That restriction
carries over to this project and to anything built from it. Commercial licensing is BlueKitchen's
to grant, not ours; see their contact details in the licence.

**"Bluetooth Control Center"**, © 2003 Benjamin S. Ralston, is the only known prior OS 9
Bluetooth effort, and reading it is what settled how to fill Apple's USB parameter blocks and
control-transfer arguments. That work is gratefully credited. Its source is **not** redistributed
here: it carries a copyright notice with no licence grant, so it is reference material only.

Apple's *Mac OS USB DDK API Reference* (Rev 26, 1999) is the reference for the USB class-driver
side.

Everything else in this repository is original work, released under the **MIT licence**
(see `LICENSE`).

⚠ MIT covers this project's own source. It does **not** relicense BTstack: because the
driver links BTstack, BlueKitchen's non-commercial clause applies to any binary built
from here regardless of the MIT grant.
