# OS 9 Bluetooth

A native **Mac OS 9** Bluetooth HID host stack. It pairs and drives Bluetooth keyboards and
mice on classic Mac OS 9, a capability Apple never shipped.

It runs the whole stack on the Mac: HCI, L2CAP, SDP and the HID host role, over a USB
Bluetooth controller, with an OS 9 control panel and Control Strip module on top. Pairing
happens in OS 9 itself; Mac OS X is never needed.

## Status

**Working on a Power Mac G4 MDD (FW800), with either the internal Apple Bluetooth module
(A1044) or a D-Link DBT-120 USB adapter.** Keyboard and mouse run at the same time, with
battery levels for both.

| | |
|---|---|
| Keyboard | Apple Wireless Keyboard **A1016**: typing, modifiers, media keys and eject |
| Mouse | Apple Wireless Mouse **A1015**: cursor and button |
| Adapters | the internal **A1044** module, or a **D-Link DBT-120** (one at a time) |
| Pairing | from OS 9 alone, with either adapter |
| Battery | both devices, in the control panel and the Control Strip |
| Naming | the device's own name, or one you choose (Rename) |
| Wake | optional: a Bluetooth keyboard or mouse can wake the Mac from sleep (see below) |
| Boot | with the internal module, the paired keyboard still reaches the Open Firmware boot picker |

⚠ **Scope is deliberate: the A1044 module, the DBT-120, the A1016 keyboard and the A1015
mouse.** Those are the devices this has been measured against. Other Bluetooth keyboards and
mice may well work (the HID paths are generic), but they are untested.

## Bluetooth adapters

**The internal Apple Bluetooth module (A1044).** Install both extensions: `USBBluetoothSwitch`
takes the module out of the mode it starts up in, so the stack can drive it. Pairings are
also kept in the module itself, so a paired keyboard works from the moment the Mac starts,
even at the Open Firmware boot picker.

**A D-Link DBT-120 USB adapter.** Install `USB Bluetooth Support` alone; the switcher is for
the internal module. The unit tested here reports itself as `0A12:0001`, a Bluetooth 1.1 CSR
adapter. A paired keyboard works once Mac OS is running; the boot picker cannot use it.

**One adapter at a time.** On a Mac that has the internal module, use a DBT-120 only with
`USBBluetoothSwitch` taken out of the Extensions folder, so the internal module stays with
Mac OS. With both active, devices keep flipping between connected and disconnected.
Pairings belong to the adapter they were made with, so pair your devices again after changing
adapters.

**Other USB adapters are untested.** One modern adapter did not work when it was last tried,
before this release: a generic Bluetooth 5 adapter that also reports `0A12:0001`
("BT DONGLE10") paired as far as the PIN exchange, and then the Mac OS USB Expert removed it
mid-pairing.

## Waking the Mac with Bluetooth

The control panel's **Allow Bluetooth devices to wake this computer** checkbox (off until you
check it) lets a paired keyboard or mouse wake the Mac from sleep, as Mac OS X's Bluetooth
preferences do.

It needs a Mac whose USB ports can wake it from sleep, as a wired USB keyboard does. On the
Power Mac G4 MDD FW800, where it was tested, that requires an additional extension called
Sleep Assistant. It was tested with the internal module; with a DBT-120 it is untested.

## Components

| file | version | goes in | what it does |
|---|---|---|---|
| `USB Bluetooth Support` | 18.6 | Extensions | the driver: USB transport, HCI, L2CAP, HID host, input injection |
| `USBBluetoothSwitch` | 1.7 | Extensions | takes the internal A1044 module out of HID-proxy mode so the stack can own it. Not needed with a DBT-120 |
| `Bluetooth` | 19.8 | Control Panels | scan, pair, delete, disconnect, rename; per-device details; the wake option |
| `Bluetooth Strip` | 2.7 | Control Strip Modules | battery gauge, device switching, quick access |
| `BTCheck` | 99.110 | anywhere | **strictly optional.** A diagnostic that writes a log rather than making you read a window. The stack does not need it and never calls it: install it only if something is wrong and you want to see why, or if you are reporting a problem. |

Every file's Get Info names the release it came from (`OS 9 Bluetooth 18.6`), and Extensions
Manager and Apple System Profiler show the same in their Package column.

⚠ Keep the extension named **`USB Bluetooth Support`**. The Mac OS USB Expert scans only
Extensions files whose name begins "USB", so renaming it stops the driver loading, with no
error and no other symptom.

## Known issues

- ⚠ **While a paired Bluetooth keyboard is switched on, a wired USB keyboard is ignored at
  the Open Firmware boot picker** (internal module). Your Bluetooth keyboard works there
  (that is the point, and it is tested), but the wired one beside it will not respond,
  including its Return key. Switch the Bluetooth keyboard off and the wired one works again
  immediately. Nothing of ours runs at Open Firmware time; pairing makes the Bluetooth card
  present a second USB keyboard, and the firmware appears to use only one. Measured both
  directions on a G4 MDD.
- After a failed pairing with the internal module, the **first boot afterwards still needs a
  manual pairing click** in the control panel.
- **A device still paired with another adapter or Mac does not enter pairing mode on a quick
  power cycle.** Take its batteries out for a while (a quick pull may not be enough), then
  switch it on: its light blinks and a scan finds it.
- **USB 2.0 coexistence is not re-tested for this release.** Release 17.6 ran alongside the
  USB 2.0 (EHCI) extension on a G4 MDD FW800, with a ROM that leaves the Bluetooth module's
  USB port to Mac OS.

## Building

Requires [Retro68](https://github.com/autc04/Retro68) with the PowerPC toolchain.

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build
```

The control panel, Control Strip module and BTCheck build the same way from `cpanel/`, `csm/`
and `bt-check/`. Several invariants are checked at build time and will fail the build rather
than ship quietly: the counter-block layout shared by four binaries, every BTstack run-loop
timer being added to the run loop, the version stamps in every 'vers' resource, and the
driver's PEF export hashing.

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
to grant, not ours; see their contact details in the licence. This project's changes to
BTstack are listed in `vendor/btstack/CLAUDE-OS9-PATCHES.md`.

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
