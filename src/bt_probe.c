/*
 *  bt_probe.c  --  OS9-Bluetooth  |  USB transport (M0) + HCI hand-off (M1)
 *
 *  A classic Mac OS 9 USB class driver that binds any Bluetooth HCI controller
 *  (USB device class 0xE0/0x01/0x01 -- external dongle OR a Mini's internal
 *  module; matched generically, no VID/PID hardcoded) and:
 *
 *    M0.0  build + Expert match        -- driver loads, ValidateHW/Initialize log.
 *    M0.1  pipe discovery              -- open interface 0, find the interrupt-IN pipe.
 *    M0.2  transport                   -- send commands (control) / read events (interrupt).
 *    M1    HCI command/event layer     -- hci.c drives bring-up + inquiry; see HCI_Start().
 *
 *  This file is the OS-9/USB half: it owns the device, discovers pipes, and
 *  exposes BT_SendHCICommand()/BT_Log() to the (device-agnostic) HCI layer.
 *  Output goes to the USB Expert log (Apple "USB Prober"). The USL is
 *  asynchronous: each call completes into a callback that advances the next step.
 *
 *  References: Apple "Mac OS USB DDK API Reference" Rev 26 (1999); prior art
 *  "Bluetooth Control Center" (c) 2003 B. S. Ralston (USBPB field access +
 *  control-transfer params mirror that working code).
 */

/* !! stdbool BEFORE USB.h, and it is required, not tidiness.
 * The build defines TYPE_BOOL=1 because BTstack needs MacTypes.h to skip its own
 * `enum { false = 0, true = 1 }`. But USB.h then uses `true` itself (USB.h:1969,
 * `on = true`) and would fail to compile with nothing defining it. stdbool.h
 * supplies true/false as macros, so both headers are satisfied and every
 * translation unit agrees that bool is the C99 type. */
#include <stdbool.h>
#include <USB.h>
/* ⚠ REQUIRED, not decorative. Without it CallSecondaryInterruptHandler2 is an implicit
 * declaration -- it still links, because DriverServicesLib is already on the line, but
 * the compiler then guesses the prototype. That is precisely the kind of silent
 * mismatch that produces a crash at interrupt level with nothing to read afterwards. */
#include <DriverServices.h>
/* ⚠ v17.4: Microseconds is declared in Timer.h, NOT in OSUtils.h or DriverServices.h.
 * Without this it is implicitly declared int, and the UnsignedWide* argument is passed
 * to an unprototyped function -- src/btstack_run_loop_os9.c carries the same note. */
#include <Timer.h>
#include "bt_hci_m1.h"
#include "bt_pump.h"          /* BT_Pump / BT_DeliverPacket -- the BTstack seam */
#include "bt_btstack.h"       /* BT_StackInit / BT_StackState                   */
#include "bt_defer.h"         /* the interrupt->task trampoline (TASK-level init) */
#include "bt_keyfile.h"       /* link key persistence   (TASK level only)        */
#include "bt_inject.h"        /* M5: key injection; Init is TASK level only     */

/* USB-IF Bluetooth HCI transport class triplet. */
enum {
    kBTClass    = 0xE0,   /* Wireless Controller             */
    kBTSubClass = 0x01,   /* RF Controller                   */
    kBTProto    = 0x01    /* Bluetooth Programming Interface */
};

/* Expert-log status level for our messages. The 2003 prior art uses 4 or 5 for
 * exactly this and calls it "Status level for USB Expert Log"; USB Prober's
 * Commands > Status Level defaults to 5, so 5 is always displayed. */
enum { kBTStatusLevel = 5 };

#define kEventBufSize 260   /* holds a full HCI event (2 hdr + up to 255 params) */

/* ---- Dispatch-table procs (forward) ----------------------------------- */
static OSStatus ProbeValidateHW (USBDeviceRef device, USBDeviceDescriptor *desc);
static OSStatus ProbeInitialize (USBDeviceRef device, USBDeviceDescriptorPtr pDesc, UInt32 busPower);
static OSStatus ProbeInitializeInterface (UInt32 interfaceNum,
                                         USBInterfaceDescriptorPtr pInterface,
                                         USBDeviceDescriptorPtr pDevice,
                                         USBInterfaceRef interfaceRef);
static OSStatus ProbeFinalize   (USBDeviceRef device, USBDeviceDescriptorPtr pDesc);

static OSStatus ProbeNotify     (UInt32 notification, void *pointer, UInt32 refCon);

/* ---- Exported symbol #1: the description the Expert matches on ---------- *
 *
 * ⚠⚠ THIS IS A BYTE-EXACT LAYOUT, DELIBERATELY *NOT* USB.h's
 * USBDriverDescription. Do not "simplify" it back to the header's struct.
 *
 * USB.h declares USBInterfaceInfo as five UInt8 fields. Retro68's GCC therefore
 * packs it into 5 bytes, so our nameInfoStr landed at offset 21. The USB Expert
 * was compiled with CodeWarrior's 2-byte member alignment, which pads that
 * member to 6 bytes, so the Expert reads nameInfoStr at offset 22 -- one byte
 * later than we wrote it. Every field from there on was misread.
 *
 * PROVED, not guessed. USB Prober's Bus Devices dump reported our driver as:
 *     Driver Name:...... S9BTProbe        (not OS9BTProbe)
 *     Driver Class:..... 1  (Audio)       (we said 0xE0)
 *     Driver SubClass:.. 0                (we said 0x01)
 *     Driver Version:... 96.8             (we said 0.6)
 * and reading our own bytes with nameInfoStr at 22 reproduces all four exactly:
 * the Pascal length comes from offset 22 = 'O' = 79 so the name prints from
 * offset 23 with trailing NULs, class comes from offset 54 which held our
 * SUBCLASS byte 0x01 = "Audio", subclass from 55 = 0, and the version from
 * 56 = 0x60 0x80 = "96.8".
 *
 * The consequence was not cosmetic. With a garbage class the Expert bound this
 * driver to a bus-powered HUB (device 105, VID/PID 0000/0000, class 9) instead
 * of the dongle, and every USBFindNextInterface for class 0xE0 then correctly
 * returned kUSBNotFound, because a hub has no Bluetooth interface. That single
 * missing pad byte accounts for the whole M0.1 failure.
 */
typedef struct {
    OSType  sig;                    /*  0  'usbd'                              */
    UInt32  descVersion;            /*  4  kInitialUSBDriverDescriptor         */
    /* USBDeviceInfo */
    UInt16  vendor;                 /*  8  0 = any                             */
    UInt16  product;                /* 10  0 = any                             */
    UInt16  release;                /* 12                                      */
    UInt16  devProtocol;            /* 14                                      */
    /* USBInterfaceInfo, PADDED TO 6 BYTES -- this is the whole fix */
    UInt8   configValue;            /* 16                                      */
    UInt8   interfaceNum;           /* 17                                      */
    UInt8   ifClass;                /* 18  0xE0                                */
    UInt8   ifSubClass;             /* 19  0x01                                */
    UInt8   ifProtocol;             /* 20  0x01                                */
    UInt8   pad;                    /* 21  <-- the byte GCC omitted            */
    /* USBDriverType */
    UInt8   name[32];               /* 22  Str31, Pascal                       */
    UInt8   driverClass;            /* 54  0xE0                                */
    UInt8   driverSubClass;         /* 55  0x01                                */
    UInt8   verMajor;               /* 56                                      */
    UInt8   verMinorBug;            /* 57                                      */
    UInt8   verStage;               /* 58                                      */
    UInt8   verNonRel;              /* 59                                      */
    UInt32  loadingOptions;         /* 60  kUSBDoNotMatchInterface             */
} BTDriverDesc;                     /* 64 bytes total                          */

/* Compile-time proof of the layout the Expert actually reads. If any of these
 * ever fails the build, the descriptor has drifted and the Expert will misparse
 * it exactly as it did before v0.7 -- silently, and by binding to the wrong
 * device rather than by failing. */
#define BT_ASSERT(name, cond) typedef char name[(cond) ? 1 : -1]
BT_ASSERT(bt_desc_is_64,      sizeof(BTDriverDesc) == 64);
BT_ASSERT(bt_name_at_22,      __builtin_offsetof(BTDriverDesc, name) == 22);
BT_ASSERT(bt_class_at_54,     __builtin_offsetof(BTDriverDesc, driverClass) == 54);
BT_ASSERT(bt_subclass_at_55,  __builtin_offsetof(BTDriverDesc, driverSubClass) == 55);
BT_ASSERT(bt_version_at_56,   __builtin_offsetof(BTDriverDesc, verMajor) == 56);
BT_ASSERT(bt_options_at_60,   __builtin_offsetof(BTDriverDesc, loadingOptions) == 60);

/* ---- The A1044's three known identities ------------------------------------------
 * Defined here rather than beside the mode-switch code because the descriptor array
 * below references them.
 *   1000  HID-proxy, what the card boots as. Interface 0 is 03/01/01 (HID/Boot/
 *         Keyboard) and no 0xE0 interface exists -- run 31.
 *   8202  what it becomes after our CSR switch -- run 33. PROVED to be the card and
 *         not this machine's internal USB modem by run 34's control: with the
 *         extension removed and no switch sent, 8202 is absent from the bus.
 *   8204  the WORKING identity, read from Tiger's own System Profiler: "Built-in
 *         Bluetooth 2.0+EDR HCI", full speed. We have never reached it. */
#define kA1044Vendor   0x05AC
#define kA1044Product  0x1000
#define kA1044Switched 0x8202
/* ★★ 8204 IS A DIFFERENT AND BETTER STATE THAN 8202, and the distinction decides how
 * ProbeInitialize treats each.
 *
 * 8202 is UNIDENTIFIED: it appears in no Apple Bluetooth plist (§9a), run 33 left the
 * card there, and its interfaces have never been described. The branch for it walks and
 * STOPS, because bringing a stack up on an unknown personality is the guess this
 * project keeps refusing to make.
 *
 * 8204 is IDENTIFIED: §9a records CSRUSBBluetoothHCIController matching 33284 = 0x8204,
 * so Apple's own HCI controller driver claims it. Bringing a stack up on it is not a
 * guess, it is what Apple does. The 2026-09-06 run put the card here and Apple System
 * Profiler showed it healthy with no driver bound.
 *
 * ⇒ So there is deliberately NO "walk and stop" branch for 8204. It falls through to the
 * normal configuration chain, whose exact-triple search for 0xE0/01/01 either succeeds,
 * giving a working transport, or fails and lets ConfigStepAny record the real interface
 * class. Note the "not 0xE0/01/01" remark on the 8202 branch is about 8202 -- 8204's
 * interfaces are genuinely unmeasured. */
#define kA1044HCI      0x8204
#define kA1044Working  0x8204

/* ★★★ THE MODE SWITCH IS OFF BY DEFAULT, AND THAT IS A DELIBERATE PRODUCT DECISION.
 *
 * Today the switch reaches 8202 -- a personality NO Apple Bluetooth driver claims
 * (§9e) -- and occasionally leaves the card unable to enumerate until a Tiger boot
 * clears it (RELEASE-GATES.md, "what makes the card go missing"). So it buys nothing
 * and risks gate 1: on a Mac whose only keyboard is Bluetooth, a card stuck out of
 * HID-proxy is a firmware lockout.
 *
 * ⇒ Until 8202 -> 8204 is solved, this ships SAFE AND INERT on the internal card.
 * The dongle path is unaffected: rule 2 matches a standard 0xE0 interface and needs
 * no switch at all, so Phase 2 loses nothing.
 *
 * ⚠⚠ AND WITH THE SWITCH OFF, RULE 1 IS OMITTED FROM THE DESCRIPTOR ARRAY ENTIRELY.
 * A rule that binds 05AC:1000 and then does nothing would claim the HID-proxy card
 * and give OS 9's own USBHIDKeyboardModule nothing back -- exactly what
 * [[feedback_dont_break_coexisting_drivers]] forbids, and precisely the wrong thing
 * to do to a card whose proxy mode is what makes boot-time keyboards work. Leaving
 * the card completely untouched is the only behaviour that can be *guaranteed* not to
 * break gate 1.
 *
 * Nothing diagnostic is lost: BTCheck's bus dump sees the card with no driver bound
 * at all, which is how runs 34 and 37 were read. */
/* ⚠⚠ TURNED ON FOR THE MODE-SWITCH RUN, 2026-09-06, DELIBERATELY AND TEMPORARILY.
 *
 * ★ What this run exists to measure: THE STALL CLEAR, which has never executed. §9c
 * established that Apple's driver expects the request to return a pipe stall and clears
 * it, that OS 9's equivalent is -6979, and that leaving the DEFAULT CONTROL PIPE stalled
 * is what made the card non-enumerable in runs 35 and 40. The clear went in afterwards
 * and no run has exercised it. One variable, one attempt.
 *
 * ⚠ kSwMaxTries STAYS AT 1. Apple sends the request seven times and that is probably
 * what walks 8202 to 8204, but raising it is gated by an explicit written warning whose
 * stated cost is a card that stops enumerating. Measuring first is that warning being
 * obeyed, not ignored.
 *
 * ⚠⚠ THE COST, ACCEPTED BY THE USER WITH THE FACTS IN HAND: rule 1 claims the A1044 at
 * EVERY boot while this is 1, so the HID-proxy keyboard that currently works under OS 9
 * stops working for as long as the extension is installed. Recovery is dragging the
 * extension out and rebooting. If the card itself parks, recovery is boot Tiger then
 * boot back (§8a). A WIRED keyboard is mandatory for this run.
 *
 * ⏭ SET THIS BACK TO 0 once the run is read, unless the result says otherwise. */
/* ⚠⚠⚠ BACK TO 0 ON 2026-09-06, AND NOT AS A RETREAT. Turning it off is what puts rule
 * 1b FIRST in the descriptor array, and that is the experiment.
 *
 * ★ THE HYPOTHESIS: the Expert may read only the FIRST 'usbd' record. The array's own
 * header comment calls the multi-record layout "INFERRED, not documented" and says in
 * as many words: "If it turns out only the FIRST record is ever read, Phase 1 -- the
 * A1044 -- still works, because its rule [is first]". The order was always a hedge
 * against exactly this, and the evidence now favours the hedge being needed:
 *
 *   rule 1  at position 1  ->  MATCHED the card at 1000, twice
 *   rule 2a at position 1  ->  MATCHED the dongle (when the switch was off)
 *   rule 1b at position 2  ->  never matched, across two runs
 *   rule 2b at any position -> `interface-init calls 0` in EVERY log ever taken
 *
 * Nothing beyond the first record has ever demonstrably matched anything.
 *
 * ⚠⚠ "THE CARD IS ALREADY AT 8204, because that state survives reboots (measured
 * 2026-09-06)" -- THAT CLAIM IS FALSIFIED. It survives SOMETIMES:
 *
 *     v6.4 -> switched to 8204
 *     v6.3 -> found 8204   SURVIVED
 *     (CardBus work)       -> found 1000   REVERTED
 *     v6.4 -> switched to 8204
 *     v6.5 -> found 8204   SURVIVED   (the Max_Num_Keys run)
 *     v6.6 -> found 1000   REVERTED
 *
 * Two for two either way, and the mechanism is unknown -- a cold power-off would do
 * it, so would time, so would a USB bus reset returning the CSR firmware to its
 * HID-proxy default. Not measured, so not asserted.
 *
 * ⇒ CONSEQUENCE FOR EVERY RUN PLAN: a switch-OFF build binds NOTHING when the card
 * has reverted, and the run is VOID rather than negative. Three of the last five runs
 * were lost this way. Never plan a measurement run that assumes the mode persisted --
 * pair it with the switch-ON boot immediately before, back to back.
 *
 * ⏭ THE REAL FIX is two separate 'ndrv' extensions, each with its own single-record
 * descriptor: a switcher matching 05AC:1000 whose only job is to send the switch and
 * get out of the way, and this driver matching 05AC:8204. The card re-enumerates
 * LIVE after the switch -- that is ordinary USB hot-plug and the Expert handles it --
 * so both would run in ONE boot with no dance and no dependence on persistence.
 * USBExpertInstallDeviceDriver does NOT solve this: it needs the ref, hub ref and
 * port of the device to claim, and after a switch the old device is gone and the new
 * one's ref was never ours. So this build sends NO switch and still gets rule 1b in
 * front of the card it was written for. If it binds, the multi-record inference is
 * wrong and every rule after the first has been dead weight. If it does not bind, the
 * problem is in the rule itself and the array is exonerated.
 *
 * ⚠ Side effect worth stating: with the switch off nothing will ever move a card OUT of
 * proxy mode, so on a machine whose card is at 1000 this build is inert and the
 * HID-proxy keyboard keeps working. That is the correct default and the reason this
 * gate exists. */
/* ⚠⚠ 1 FOR THE SWITCH-ONLY BUILD. This build's ONLY job is to move the card from
 * HID-proxy to 8204 and then get out of the way. It CANNOT also bind the result,
 * because only the first descriptor record is read (proved by v6.2) and turning this on
 * puts rule 1 (05AC:1000) in front of rule 1b (05AC:8204).
 *
 * ⇒ Pair it with the switch-OFF build, which puts rule 1b first:
 *     install THIS  -> boot -> card moves to 8204, nothing binds it
 *     install THAT  -> boot -> rule 1b is record 1 and binds it, stack comes up
 *
 * ⏭ The single-build fix is USBExpertInstallDeviceDriver: bind the proxy card, switch
 * it, then ask the Expert to attach us to the re-enumerated device by reference rather
 * than by descriptor match. Not built, and its semantics are unverified. */
/* ⚠ 0 for v6.5, the SAME half of the pair as v6.3 -- rule 1b is record 1 and binds the
 * card at 8204. v6.5 changes only what the block RECORDS, never what it binds, because
 * a build that answers a question must not also change the conditions the question was
 * asked under. The card stays at 8204 across a warm restart (measured: the v6.3 boot
 * found it there after v6.4 switched it), so no re-switch is needed between runs. */
#define kSendModeSwitch 0

/* Rule order, v16.6 onward: 2a (device-level 0xE0) FIRST, then 2b (interface-level
 * 0xE0), then 1b (05AC:8204) as a backstop. Rule 1 (the HID-proxy A1044) is only
 * compiled in when the switch is enabled. Rule 3 is GONE -- it was bound to the user's
 * modem; see the note where it was. */
/* ⚠ RULE 1b (05AC:8204, the switched card) IS ALWAYS COUNTED, unlike rule 1. It cannot
 * claim the HID-proxy card, so it is safe to carry unconditionally -- see its comment. */
#if kSendModeSwitch
#define kBTDescRules 4      /* 1 (proxy) + 2a + 2b + 1b (switched) */
#else
#define kBTDescRules 3      /* 2a + 2b + 1b (switched); rule 1 omitted -- see above */
#endif

/* ⚠⚠ AN ARRAY OF MATCH RULES. THE ARRAY IS REAL -- VERIFIED, NOT INFERRED.
 *
 * Apple's own USBMassStorageClassDriver carries SIX contiguous 64-byte 'usbd' records
 * in its data section, immediately followed by a word that is NOT 'usbd' -- it is
 * kClassDriverPluginVersion (0x00001100), i.e. the dispatch table. Dumped 2026-10-03
 * from ../usb2-ehci/re-work/pef1.data.bin, and every byte is as claimed:
 *
 *     0x0278 + n*0x40, n = 0..5   75 73 62 64 00 00 00 00 ff ff ff ff 00 00 00 00
 *                                 ...ifClass 0x08, ifSubClass 0x01..0x06...
 *                                 ...driverClass/SubClass 08/01..06, opts 0x08
 *     0x03F8                      00 00 11 00       <- dispatch table, terminator
 *
 * Six live rules, one per mass-storage subclass, same driver name on all six, and a
 * natural terminator. A shipping Apple driver does not carry five dead records, so the
 * Expert walks them until the signature stops matching. OUR OWN BUILT PEF HAS EXACTLY
 * THE SAME SHAPE -- three 'usbd' records at 64-byte spacing followed by 00 00 11 00.
 *
 * ⚠ TWO THINGS THAT DUMP ALSO SETTLES, BOTH OF WHICH v16.5 GOT WRONG:
 *
 *   - 0xFFFF IS THE WILDCARD. All six of Apple's records spell "any vendor, any
 *     product" as ff ff ff ff. The Expert's own trace agrees -- see rule 2a.
 *   - ORDER IS A HEDGE, NOT A FIX. Since the walk is real, position should not matter.
 *     2a leads anyway because it is the record PROVEN to bind this dongle (run47), and
 *     because it alone covers BOTH supported devices, so a first-record-only Expert
 *     would still work. That costs nothing and removes a variable.
 *
 * ★ WHY ONE GENERIC RECORD COVERS BOTH DEVICES. The switched A1044 and the CSR dongle
 * are the same shape to the Expert -- both declare class 0xE0/0x01/0x01 at DEVICE level
 * and both carry a 0xE0/0x01/0x01 interface:
 *     A1044 @ 8204   BOUND DEVICE devclass 0xE0, exact-triple find err 0 (four runs)
 *     CSR dongle     Device Class 224, SubClass 1, Protocol 1; Interface #0 224/1/1
 *                    (logs/USBProber_BusDevices_run7_*.txt, and run47 bound it)
 * So rule 2a is not "the dongle rule" -- it is the rule for a standard HCI controller,
 * which is what both of them are.
 */
BTDriverDesc TheUSBDriverDescription[kBTDescRules] =
{
#if kSendModeSwitch
  /* ---- RULE 1: the A1044 internal module, matched at DEVICE level by VID/PID ----
   *
   * ⚠⚠ COMPILED OUT WHEN kSendModeSwitch IS 0, WHICH IS THE DEFAULT. Binding
   * 05AC:1000 only makes sense if we are going to switch it; a rule that claims the
   * HID-proxy card and then does nothing takes the HID interfaces away from OS 9's
   * own USBHIDKeyboardModule and hands back nothing. See the note at kSendModeSwitch.
   *
   *
   * Run 26 measured the card's device descriptor as class 00/00/00, so there is no
   * class to match on. We match its VID/PID instead and bind it as a DEVICE, which
   * gets us something valuable for free: ProbeInitialize's existing exact-triple
   * search for 0xE0/0x01/0x01 will fail, and the wildcard retry (ConfigStepAny, in
   * place since v0.5) then RECORDS THE CARD'S REAL INTERFACE CLASS into the counter
   * block. That is exactly how we learned the hub's interface was class 0x09.
   * So this rule is both the fix and the measurement, using only proven machinery.
   *
   * ⚠⚠ VENDOR *AND* PRODUCT ARE PINNED, AND THAT IS A SAFETY REQUIREMENT, NOT TIDINESS.
   * Run 30's bus dump shows THREE other devices with device class 00/00/00:
   * 05AC:0204 (an Apple internal keyboard) and 413C:301A (Dell). A rule matching
   * device class 0 alone would bind the user's KEYBOARD and try to configure it.
   * kUSBDoNotMatchGenericDevice additionally forces the vendor comparison. */
  {
    kTheUSBDriverDescriptionSignature,
    kInitialUSBDriverDescriptor,
    0x05AC, 0x1000, 0, 0,                       /* Apple internal Bluetooth ONLY */
    0, 0, 0x00, 0x00, 0x00, 0,                  /* device class 0 + the pad      */
    { 10, 'O','S','9','B','T','P','r','o','b','e' },
    0x00,                                       /* driverClass matches device class 0 */
    0x00,
    0x17, 0x60, finalStage, 0,                     /* 17.6                           */
    kUSBDoNotMatchGenericDevice | kUSBDoNotMatchInterface
  },
#endif /* kSendModeSwitch -- rule 1 */

  /* ---- RULE 2a: any standard-HCI controller, matched at DEVICE level ------------
   *
   * ★★ ADDED AFTER MEASURING THE ACTUAL DONGLE, and it closes a gap that would have
   * bitten the moment the dongle enumerated again.
   *
   * ioreg on a modern Mac reports our dongle as:
   *     idVendor 0x0A12  idProduct 0x0001
   *     bDeviceClass 0xE0  bDeviceSubClass 0x01  bDeviceProtocol 0x01
   * i.e. it declares Bluetooth HCI at the DEVICE level -- and it is exactly the
   * personality Apple's own CSRUSBBluetoothHCIController claims (VID 2578, PID 1).
   *
   * ⚠⚠ Rule 2b below matches with kUSBInterfaceMatchOnly, which asks the Expert to
   * consider this driver for INTERFACES ONLY. A device that declares its class at the
   * device level is offered at device level, so an interface-only rule can miss it
   * entirely -- the mirror image of run 26's lesson, where a device class of 0x00 put
   * the class on the interfaces and a device-level match could not see the A1044.
   * Both shapes exist in the wild, so BOTH rules exist here.
   *
   * ⚠ Every dongle run that ever worked (through run 25) predates v2.0's move to
   * interface-only matching, so the interface path has NEVER been confirmed on this
   * hardware. This rule restores the shape that demonstrably worked.
   *
   * Vendor and product are ANY, deliberately: unlike the A1044's rules there is no
   * safety reason to pin them, because class 0xE0/01/01 at device level IS
   * "a Bluetooth HCI controller" and nothing else legitimately claims it. */
  {
    kTheUSBDriverDescriptionSignature,
    kInitialUSBDriverDescriptor,
    /* ⚠⚠⚠ kUSBAnyVendor (0xFFFF), NOT 0. v16.5 HAD THIS BACKWARDS AND SHIPPED IT.
     *
     * v16.5's comment here claimed that 0 is the wildcard and 0xFFFF a literal vendor
     * ID of 65535, on the strength of Apple's usb-ddk examples -- MouseModule and
     * CompositeClassDriver both write a literal 0 under "vendor = not device specific".
     * That reading is wrong, and three independent measurements say so:
     *
     *   1. THE EXPERT'S OWN TRACE. logs/USBExpert_run1b_renamed_loads_but_no_match.log
     *      shows the Expert announcing our dongle as
     *          class=224, subclass=1, protocol=1, vendor=0xa12, product=0x1
     *      and checking it against matchers that spell "any" as 65535:
     *          checking ... class=3, subclass=1, protocol=2, vendor=0xffff, product=0xffff
     *      That is Apple's own HID boot-mouse matcher. 0xFFFF IS the sentinel.
     *
     *   2. A SHIPPING APPLE DRIVER. USBMassStorageClassDriver carries six contiguous
     *      64-byte 'usbd' records (../usb2-ehci/re-work/pef1.data.bin at 0x0278 + n*0x40,
     *      terminated by 0x00001100 = the dispatch table). Every one of the six begins
     *          75 73 62 64 00 00 00 00 ff ff ff ff 00 00 00 00
     *      i.e. vendor AND product = 0xFFFF, matching every vendor's mass storage.
     *
     *   3. OUR OWN DONGLE, ON THIS OS. run47/run49 bound THIS dongle with THIS field set
     *      to kUSBAnyVendor -- logs/run47_DONGLE_BOUND_stack_WORKING_2026-09-02_v350.log
     *      reads "BOUND DEVICE VID:PID 0A12:0001 devclass 0xE0", three pipes open, 19 HCI
     *      commands, 287 ACL reads. 0xFFFF demonstrably matches 0x0A12.
     *
     * The examples are not wrong so much as not binding: with the generic-device path
     * taken (kUSBDoNotMatchGenericDevice clear) vendor is not the discriminator anyway.
     * But 0xFFFF is what the Expert and Apple's own shipped driver use, and it is what
     * is PROVEN on this dongle, so that is what we write.
     *
     * ⚠ devProtocol goes back to 0 as well. v16.5 set it to kBTProto to mirror
     * MouseModule; the shape proven in run47 had 0, and this is not the build to vary a
     * second field in. */
    kUSBAnyVendor, kUSBAnyProduct, 0, 0,        /* ANY vendor, ANY product       */
    0, 0, kBTClass, kBTSubClass, kBTProto, 0,   /* interface 0xE0/0x01/0x01 + pad   */
    { 10, 'O','S','9','B','T','P','r','o','b','e' },
    kBTClass,                                   /* driverClass MATCHES device class */
    kBTSubClass,
    0x17, 0x60, finalStage, 0,                     /* 17.6                           */
    kUSBDoNotMatchInterface
  },

  /* ---- RULE 2b: any standard-HCI controller, matched at INTERFACE level ----------
   * The CSR dongle and any other standard controller. Its interface 0 is
   * 0xE0/0x01/0x01 -- proven by our own interface search finding it in every dongle
   * run since M0.1 -- so interface matching should catch it. ⚠ Still unverified,
   * because run 30's dongle never enumerated; Phase 2 must confirm. */
  {
    kTheUSBDriverDescriptionSignature,
    kInitialUSBDriverDescriptor,
    /* ⚠ Same correction as 2a, in the same direction: 0xFFFF is the Expert's "any",
     * and 0 is a literal vendor 0. See the three measurements quoted in rule 2a. */
    kUSBAnyVendor, kUSBAnyProduct, 0, 0,        /* ANY vendor, ANY product       */
    0, 0, kBTClass, kBTSubClass, kBTProto, 0,   /* interface 0xE0/0x01/0x01         */
    { 10, 'O','S','9','B','T','P','r','o','b','e' },
    kBTClass,
    kBTSubClass,
    0x17, 0x60, finalStage, 0,                     /* 17.6                           */
    kUSBInterfaceMatchOnly
  },

  /* ---- RULE 1b: the A1044 AFTER THE SWITCH, at 05AC:8204 ------------------------
   *
   * ★★★ ADDED 2026-09-06, and it is what the successful switch run earned. That run put
   * the card at 8204 -- the state CSRUSBBluetoothHCIController matches (§9a) and the one
   * RELEASE-GATES named as the open problem -- and Apple System Profiler showed it
   * healthy and enumerating with "Driver name: Not available". Nothing claimed it, which
   * is exactly the "Software needed for the USB device 'Unnamed Device' is not
   * available" alert the user saw.
   *
   * ⚠⚠ NOT GATED BY kSendModeSwitch, DELIBERATELY, BECAUSE THIS RULE IS SAFE IN A WAY
   * RULE 1 IS NOT. A card at 8204 has ALREADY been switched by somebody; it is not the
   * HID-proxy state. So this rule can never claim the proxy card, can never take the
   * wireless keyboard away, and costs nothing on a machine where no switch has happened.
   * Rule 1 has exactly the opposite property, which is why it stays behind the gate.
   *
   * ⚠⚠ MOVED TO LAST IN v16.6, AND IT IS NOW A BACKSTOP RATHER THAN THE PRIMARY RULE.
   * It used to sit FIRST, ahead of 2a, from v6.0 onward. Rule 2a is now first because
   * the switched A1044 and the CSR dongle turn out to be THE SAME SHAPE to the Expert,
   * so one generic record claims both -- see the measurement at the head of the array.
   * This rule stays only because it costs one record and covers the case where a card
   * at 8204 somehow does NOT present device class 0xE0.
   *
   * ⚠ A CORRECTION THIS BUILD CARRIES. The comment here used to read "WHY DEVICE-LEVEL,
   * when ASP reports Class 0 and the class is therefore on the interfaces", and called
   * the 8204 interface descriptors "never measured". Both statements are false, and have
   * been since 2026-09-15. The counter block records pDesc->deviceClass straight from the
   * Expert, and four separate runs -- BTCheck v99.36, v99.47, v99.63 and v99.72 -- agree:
   *       BOUND DEVICE  VID:PID 05AC:8204  devclass 0xE0
   *       exact-triple find err     0x00000000
   * So the switched card declares 0xE0 at DEVICE level AND carries a 0xE0/01/01
   * interface. Device-level matching is right here for the measured reason, not the
   * guessed one -- and a comment that outlived its measurement is exactly the failure
   * CLAUDE.md names. */
  {
    kTheUSBDriverDescriptionSignature,
    kInitialUSBDriverDescriptor,
    0x05AC, 0x8204, 0, 0,                       /* the SWITCHED A1044 only       */
    0, 0, 0x00, 0x00, 0x00, 0,                  /* device class 0 + the pad      */
    { 10, 'O','S','9','B','T','P','r','o','b','e' },
    0x00,                                       /* driverClass matches device class 0 */
    0x00,
    0x17, 0x60, finalStage, 0,                     /* 17.6                           */
    kUSBDoNotMatchGenericDevice | kUSBDoNotMatchInterface
  },

  /* ---- RULE 3 IS GONE. IT WAS BOUND TO THE USER'S MODEM. -------------------------
   *
   * ⚠⚠⚠ THE WORST ERROR IN THIS PROJECT SO FAR, AND IT WAS A REASONING ERROR ON MY
   * PART, NOT A CODING ONE. Recorded in full because the shape of the mistake is
   * more useful than the fix.
   *
   * Rule 3 pinned 05AC:8202 in the belief that the CSR mode switch produced it. The
   * USB ID databases list 05AC:8202 as an "HCF V.90 Data/Fax Modem", class 0x02 is
   * Communications, and this machine HAS an internal USB modem -- its own System
   * Profiler reports InternalUSBModem.kext with spmodem_interfacetype USB. I raised
   * that hazard at run 39, then talked myself out of it on the strength of run 34's
   * control, in which 8202 was absent with the extension removed.
   *
   * ⚠ RUN 34 WAS NOT A CONTROL. Tabulating 8202 across every run with a bus dump
   * shows it appearing and disappearing INDEPENDENTLY of anything we did:
   *
   *     run 26   8202 present   no switch code existed at all
   *     run 29   8202 present   v2.1, no switch code
   *     run 30   8202 present   v2.1, no switch code
   *     run 38   8202 present   v2.6, switch compiled OUT -- verified zero live
   *                             CsrModeSwitch call sites through the preprocessor
   *
   * ★ Run 38 is decisive: 05AC:1000 and 05AC:8202 were on the bus SIMULTANEOUSLY in a
   * build that could not send a switch. They are two different devices. The switch
   * never produced 8202 -- what it actually did was make 1000 disappear, and 8202's
   * comings and goings were the modem enumerating intermittently, which I read as
   * causation because I only ever compared two runs at a time.
   *
   * ⇒ The lesson: a control is only a control if the variable it holds fixed is
   * actually the only one moving. Against an intermittent device, comparing a pair of
   * runs proves nothing, and the whole table has to be laid out before any causal
   * claim is made. Four earlier theories about this card died of the same disease.
   *
   * ⇒ And the consequence: v2.8 through v3.4 shipped a rule that would bind the
   * user's internal modem, which is exactly what
   * [[feedback_dont_break_coexisting_drivers]] forbids. Removed. Nothing replaces it:
   * there is no known device we need to bind at 8202, because 8202 is not ours. */
};

/* The array must be exactly two contiguous 64-byte records, or a walking Expert would
 * read garbage as a third rule. */
/* ⚠ 64 bytes per rule, and the Expert reads this array BY SIZE -- so a miscount here
 * is the same class of bug as the missing pad byte at offset 21. Tied to kBTDescRules
 * rather than a literal, so switching the gate cannot leave the array and its declared
 * length disagreeing: 2 rules with the switch off, 3 with it on. */
BT_ASSERT(bt_desc_array_matches_rules,
          sizeof(TheUSBDriverDescription) == kBTDescRules * 64);

/* ---- Exported symbol #2: the dispatch table the Expert calls ----------- */
USBClassDriverPluginDispatchTable TheClassDriverPluginDispatchTable =
{
    kClassDriverPluginVersion,
    ProbeValidateHW,
    ProbeInitialize,
    ProbeInitializeInterface,   /* ⚠ was nil -- see run 26 and the descriptor above */
    ProbeFinalize,
    ProbeNotify
};

/* ---- Transport state --------------------------------------------------- */
static USBDeviceRef gDevice;                 /* the matched device            */
static USBPipeRef   gIntPipe;                /* interrupt-IN (HCI events)     */
static UInt8        gIfaceNum;               /* Bluetooth interface number    */
static UInt32       gBusPower;               /* mA the Expert says are available */
/* ★★★★★ v17.3: TWO INTERRUPT READS OUTSTANDING, NOT ONE -- AND THE LOG NAMED IT.
 *
 * 17.2 existed to separate two explanations of the dongle teardown. It answered:
 *
 *     first stall at (ms)  444824     last stall at (ms) 480165
 *     Finalize  at (ms)    480172     stalls spanned 35341 ms
 *                                     last stall -> Finalize 7 ms
 *
 * 35 seconds of stalls, so they are not an artifact of removal -- and the removal lands
 * 7 ms after the last one. The stalls accumulate, and the teardown comes on the back of
 * one of them. The counts then give the mechanism outright:
 *
 *     run        unsolicited events   stalls   teardowns   armed - completions
 *     16.6 idle         0                0         0              1
 *     17.1             32               32         1             33
 *     17.2             93               98         3            100
 *
 * ⇒ ⚠⚠⚠ ALL OF THE ABOVE IS AN EPITAPH. THE CONCLUSION WAS WRONG, THE FIX WAS INERT,
 * AND v17.6 REVERTED IT. Kept because the reasoning is instructive and because the next
 * person will otherwise re-derive it from the same numbers and reach the same place.
 *
 * WHAT WENT WRONG. The 35-second span above is a MEASUREMENT ARTIFACT OF MY OWN RULE.
 * kWStallFirstMs is written once and never rewritten, kWStallLastMs tracks the latest --
 * so across a session with THREE teardowns the span runs from the first burst to the
 * last and reads as "spread". 17.5 finally produced a session with exactly ONE teardown:
 *
 *     first stall 371003   last stall 371096   Finalize 371102
 *     stalls spanned 93 ms        last stall -> Finalize 6 ms
 *     UNSOLICITED events 9        stalls 32
 *
 * 32 stalls in 93 MILLISECONDS, against 9 unsolicited events. And the per-teardown count
 * is the same in every run ever recorded -- 98/3, 66/2, 32/1, all ~32.
 *
 * ⇒ THE STALLS ARE A SYMPTOM. The device is removed for some other reason and this
 * re-arm loop spins ~32 times in ~93 ms until Finalize arrives. That single fact
 * explains every dead end at once: why depth 2 was inert, why handler time was
 * irrelevant (140 us vs 150 us), why the 5 s timeout did not fit the arithmetic, and
 * why "32 per teardown" was so suspiciously round -- it is just how many spins fit in
 * the removal window.
 *
 * ⇒ AND THE UNSOLICITED-EVENT CORRELATION WAS COINCIDENCE. 1.05 and 1.03 looked
 * compelling across two runs; 9 events against 32 stalls in the third killed it. Both
 * quantities scale with how much activity a session had, which is not a mechanism.
 *
 * ★ THE LESSON, WHICH IS THE REASON THIS COMMENT SURVIVES THE CODE IT DESCRIBED: three
 * mechanisms were inferred from a ratio and all three were wrong, while the ratio itself
 * was right every time. A correlation says WHICH things move together. It never says
 * WHY. Measure the step in between before building on it.
 *
 * ⇒ So the interrupt path is back to ONE read, exactly as it was through v17.2 and
 * exactly as the A1044 has always run it. The open question is not in here: it is why
 * the USB Expert removes this dongle. See docs/DONGLE-STATUS.md. */
static USBPB        gCfgPB, gCmdPB, gIntPB;  /* config / command / event PBs  */
static UInt8        gEvent[kEventBufSize];   /* interrupt-IN event buffer     */

/* ---- M0.3: the ACL data path ------------------------------------------- *
 * L2CAP rides on ACL, and ACL rides on the two BULK endpoints, which M0.1
 * discovered on this dongle as 0x02 OUT and 0x82 IN at 64 bytes each. The
 * interrupt pipe carries only HCI events.
 *
 * ⚠ The read is capped at the BUFFER size, never at the controller's advertised
 * ACL length. This controller reports 679 bytes, which is an odd figure and it is
 * a counterfeit CSR, so its self-reported limits are not to be trusted as bounds
 * on what it might actually send. A static buffer plus a hard cap means a lying
 * controller cannot overrun us. */
#define kACLBufSize 2048
static USBInterfaceRef gIfaceRef;            /* the interface, NOT a pipe     */

/* Each USBPB carries ONE transaction at a time, so "can send" means the previous
 * completion has run. BTstack asks before every send and queueing into a PB that
 * is still in flight would corrupt it. Set before the USL call, cleared in the
 * completion -- including on an immediate error, or the pipe would wedge shut. */
static volatile int gCmdBusy, gAclOutBusy;
static USBPipeRef   gBulkIn, gBulkOut;
/* v9.7: the descriptor fields the Expert returned for each pipe we opened. */
static unsigned long gPipeInt, gPipeOut, gPipeInB;
static USBPB        gAclInPB, gAclOutPB;
static UInt8        gAclIn[kACLBufSize];

/* ======================================================================= *
 *  OBSERVABILITY OF LAST RESORT -- the counter block
 * ======================================================================= *
 *
 *  The USB Expert's log will not carry our messages. Established over runs 2,
 *  3 and 4: the driver demonstrably loads and its Initialize runs to completion,
 *  Prober's Status Level is at its maximum of 5, four separate probes were
 *  emitted through BOTH USBExpertStatus and USBExpertStatusLevel including two
 *  whose leading byte was a space so they could survive a length check, and not
 *  one appeared, while 45 "Driver -" lines from Apple's own drivers appear in
 *  the same log. The mechanism works; it will not work for us, and we have no
 *  explanation.
 *
 *  So the driver records its own progress here instead, and BTCheck finds this
 *  block by scanning the System heap for the magic -- exactly how FWFixCheck
 *  reads the FireWire hook's block and how the EHCI counters are read.
 *
 *  ⚠ PLAIN ALIGNED STORES ONLY. Everything after Initialize runs in a USL
 *  completion at secondary interrupt level, so no allocation, no File Manager,
 *  nothing task-level-only may appear here. That is why this is a static array
 *  and not a log file: scripts/level-audit.py enforces it, and this driver has
 *  no task-level context to drain a ring buffer from anyway.
 *
 *  ⚠ The magic is present in the FILE too, with the counters zeroed, so a copy
 *  of the extension sitting in a disk cache buffer can match the scan. Treat a
 *  block whose counters are all zero as "found the file, not the live driver" --
 *  the same caveat FWFixCheck carries. kWInit > 0 is the proof of life.
 */
enum {
    kWMagic = 0, kWBuild,
    kWValidate, kWInit, kWFinal, kWNotify, kWNotifyCode,
    kWCfgSteps, kWStageMax, kWCfgStatus, kWImmErr, kWCfgDone,
    kWArmInt, kWIntComp, kWIntStatus, kWActCount,
    kWCmdComp, kWCmdStatus, kWCmdSent, kWLastOpcode,
    kWLastEvent, kWSayCalls, kWIface,
    kWEvt0, kWEvt1,                  /* first 8 bytes of the last HCI event */
    kWBusPower, kWFindErr1, kWFindErr2, kWIfaceFound, kWFindAnyTried, kWSpare5,
    /* word 31 onward: the HCI layer's parsed values, written via BT_Record().
     * kWHciBase must stay at 32 so words 0..30 keep the meanings BTCheck
     * already decodes, and kWHciBase + kRecCount must stay below kWEnd. */
    kWWasEnd31,                      /* was 'ENDS' up to v0.8; now free        */
    /* M0.3 ACL path, words 31 and 48..55. Word 31 was the old terminator slot;
     * the rest sit above the HCI section, which ends at kWHciBase + kRecCount. */
    kWPipes = 31,                    /* nibbles: int<<8 | bulkOut<<4 | bulkIn   */
    kWHciBase = 32,
    kWAclArmed = 48, kWAclInComp, kWAclInStatus, kWAclInCount,
    kWAclTotal, kWAclHdr, kWAclSent, kWAclOutComp, kWAclOutStatus, kWImmErrCode,
    /* M2: the BTstack half. Mirrored out of the glue's globals by BT_StackPoll()
     * so BTCheck sees them without needing to know BTstack's symbols. */
    kWStkState = 64, kWStkPump, kWStkReenter, kWStkTx, kWStkRx, kWStkRefused,
    kWStkTxDone,
    /* M2b: L2CAP / inquiry / SDP. Mirrored out of bt_btstack.c's globals by the
     * same BT_StackPoll(). Words 71..89; 90..94 stay free.
     *
     * ⚠ EVERY STATUS WORD HERE IS STORED AS 0x100 | status, NEVER as the bare
     * byte. That is the kRecInqComplete lesson from M1 made structural: a bare 0
     * cannot be told apart from "this event never arrived", which is exactly the
     * ambiguity that made one M1 row useless. A zero word now means NOT RECEIVED
     * and 0x100 means received-with-status-0. */
    kWL2Init = 71,      /* 1 = l2cap_init done, 2 = + sdp_client_init done       */
    kWInqState,         /* 0 none, 1 started, 2 complete                         */
    kWInqStartRc,       /* gap_inquiry_start() return, as 0x100 | rc             */
    kWInqResults,       /* count of GAP_EVENT_INQUIRY_RESULT                     */
    kWInqComplStatus,   /* 0x100 | status                                        */
    kWTgtAddrHi,        /* chosen peer BD_ADDR, first 3 bytes                    */
    kWTgtAddrLo,        /*                       last 3 bytes                    */
    kWTgtCoD,           /* its 24-bit class of device                            */
    kWTgtPick,          /* (responder index << 8) | why we picked it             */
    kWHciConnStatus,    /* HCI_EVENT_CONNECTION_COMPLETE, 0x100 | status         */
    kWHciConnHandle,    /* the ACL connection handle                             */
    kWSdpIssued,        /* 1 = sdp_client_query_uuid16 actually called           */
    kWSdpQueryRc,       /* its return, as 0x100 | rc                             */
    kWSdpAttrBytes,     /* count of SDP_EVENT_QUERY_ATTRIBUTE_BYTE               */
    kWSdpRecords,       /* distinct SDP record ids seen                          */
    kWSdpComplete,      /* 1 = SDP_EVENT_QUERY_COMPLETE arrived                  */
    kWSdpStatus,        /* its status, as 0x100 | status                          */
    kWL2capEvts,        /* count of L2CAP_EVENT_* seen by our observer           */
    kWLastUnkEvt,       /* last event code we did not recognise -- a debug aid   */
    /* ⚠ RUN 16 COULD NOT DISTINGUISH TWO CAUSES, AND THESE FOUR FIX THAT.
     *
     * Run 16 sent Inquiry, got Command Status 0, and then the block showed
     * `int completions 17` for ever: no Inquiry Result, no Inquiry Complete. Two
     * explanations fit that identically, and the block could not separate them:
     *   (a) the reader is armed and WAITING, and the controller went silent;
     *   (b) our re-arm silently failed, so we STOPPED LISTENING.
     * The re-arm's USBIntRead return was not checked, which is what made (b)
     * invisible -- a real latent defect, not just missing instrumentation.
     *
     * kWIntArmed vs kWIntComp is now the discriminator: armed exactly one more
     * time than completed, with no arm error, means the pipe is waiting and the
     * silence is the controller's.
     *
     * kWUnsolEvents matters because EVERY event this driver has ever received, in
     * every run, was a direct response to a command we had just sent. An
     * unsolicited event has never once been observed, so "the transport delivers
     * unsolicited events" is an untested assumption, not a known good. */
    kWIntArmed = 90,    /* interrupt-IN reads ISSUED (arms), including re-arms   */
    kWIntArmErr,        /* last re-arm immediate error, as 0x100 | (err & 0xFF)  */
    kWUnsolEvents,      /* events whose code is NOT 0x0E/0x0F -- i.e. unsolicited*/
    kWScanMode,         /* 1 = connectable asked, 3 = + discoverable asked       */
    /* M2c: PAIRING. Run 17 got ACL up and 1495 bytes of L2CAP traffic, then the
     * link died with Disconnection Complete reason 0x22 = LMP RESPONSE TIMEOUT,
     * right after we sent IO_Capability_Request_Reply (0x042B). Cause found in
     * BTstack: `ssp_auto_accept` defaults to 0 (hci.c:5579), so on
     * HCI_EVENT_USER_CONFIRMATION_REQUEST it does NOTHING and waits for the app to
     * answer. We never answered, so the peer timed out. Ours, not the dongle's.
     *
     * ⚠ Run 17 could only see that reason by hand-decoding `last event bytes`. The
     * disconnect reason is far too important to leave implicit. */
    kWDiscReason = 94,  /* Disconnection Complete reason, as 0x100 | reason       */
    kWDiscHandle,       /* the handle that dropped                               */
    kWDiscCount,        /* how many disconnections                              */
    kWIoCapReqs,        /* HCI_EVENT_IO_CAPABILITY_REQUEST seen                  */
    kWUserConfReqs,     /* HCI_EVENT_USER_CONFIRMATION_REQUEST seen              */
    kWPinReqs,          /* HCI_EVENT_PIN_CODE_REQUEST -- legacy pairing instead   */
    kWAuthComplete,     /* HCI_EVENT_AUTHENTICATION_COMPLETE, 0x100 | status      */
    kWSspAuto,          /* 1 = gap_ssp_set_auto_accept(1) was called             */
    kWLinkKeyReqs,      /* HCI_EVENT_LINK_KEY_REQUEST seen                       */
    /* M2d: the SDP retry that closes M2, and the pairing events SSP ACTUALLY emits.
     *
     * ⚠ kWAuthComplete above watches HCI_EVENT_AUTHENTICATION_COMPLETE (0x06), which
     * belongs to the LEGACY Authentication_Requested flow. SSP does not use it, so it
     * read NOT RECEIVED on run 18 -- a successful pairing -- and looked like a
     * failure. SSP completes with SIMPLE_PAIRING_COMPLETE (0x36) and
     * ENCRYPTION_CHANGE (0x08). Both are recorded here. See docs/M3-DESIGN.md §8.
     *
     * The block does NOT grow for these: words 103..126 were already free below the
     * terminator at 127, so an older BTCheck still finds the block. It just cannot
     * display these rows. */
    kWSimplePairing = 103, /* SIMPLE_PAIRING_COMPLETE, as 0x100 | status          */
    kWEncryptChange,       /* ENCRYPTION_CHANGE, as 0x100 | status                */
    kWEncryptOn,           /* its encryption_enabled byte -- nonzero = encrypted  */
    kWSdpRetries,          /* SDP queries re-issued after encryption came up      */
    kWSdpRetryRc,          /* the retry's return, as 0x100 | rc                   */
    kWSdpNotReady,         /* retries skipped because sdp_client was still busy   */
    kWSdpEventsAny,        /* EVERY event delivered to our SDP handler, any code */
    kWSdpLastEvt,          /* first SDP event code we did not recognise          */
    /* M2e: counters that a LATER FAILURE CANNOT ERASE, plus pool-exhaustion.
     *
     * ⚠ Run 20 was a completely successful run that DISPLAYED TWO FAILURES, because
     * `kWHciConnStatus` and `kWSdpStatus` are last-wins: the successful ACL connection
     * and the 909-byte SDP query were overwritten by a later doomed retry. Recording
     * a COUNT of successes alongside the last status means success is permanent in the
     * block and only the most recent failure is transient -- which is the right way
     * round. Same family as the four instrumentation misses: state what was observed.
     *
     * kWAllocFails pays the debt btstack_config.h wrote down before M2 even started:
     * "pool exhaustion must be instrumented in the counter block, not discovered as a
     * mystery." Run 20 was a mystery that had to be decoded by hand. */
    kWConnOks = 111,       /* ACL connections that completed with status 0        */
    kWSdpOks,              /* SDP queries that completed with status 0            */
    kWAllocFails,          /* any status seen == 0x56 BTSTACK_MEMORY_ALLOC_FAILED */
    /* M3.1: the RAM link key store. `link key requests` read 0 in runs 18-21 because
     * there was no store to answer from; these rows are how we will see it answering. */
    kWLkGets = 114,        /* get_link_key calls -- i.e. peers asking              */
    kWLkHits,              /* those that FOUND a key: the reconnect-without-repair  */
    kWLkPuts,              /* put_link_key calls -- pairings that produced a key    */
    kWLkDeletes,           /* delete_link_key calls                                */
    kWLkStored,            /* keys currently held                                   */
    kWLkEvicted,           /* store was full and the oldest was dropped             */
    kWLkDirty,             /* 1 = a task-level flush is owed (consumed at M3 §6.4)  */
    kWSecReqs = 121,       /* gap_request_security_level calls -- the run 22 fix   */
    kWLkAddrHi,            /* the most recently stored key's BD_ADDR, high 3 bytes */
    kWLkAddrLo,            /*                                        low 3 bytes  */
    kWLkType,              /* its link_key_type_t, as 0x100 | type                 */
    /* M3.2: PIPE STALL RECOVERY. Run 23 died here and the v1.6 instrumentation is
     * what caught it: `interrupt reads armed 22` vs `completions 21` with
     * `last re-arm error` nonzero.
     *
     * Both readers completed with -6911 kUSBNotRespondingErr ("Pipe stall, No device,
     * device hung") and the re-arm was then refused with -6979 kUSBPipeStalledError,
     * whose own comment in MacErrors.h reads "Pipe has stalled, ERROR NEEDS TO BE
     * CLEARED". We never cleared it -- IntCompletion re-armed blindly -- so one USB
     * glitch killed the event stream permanently.
     *
     * ⚠ kWIntArmErr stores only 0x100 | (err & 0xFF), which lost the high bytes and
     * forced the error to be brute-forced from its low byte. The full-width words
     * below exist so that never has to be done again. */
    kWIntStallClears = 125,  /* USBClearPipeStallByReference on the interrupt pipe  */
    kWAclStallClears,        /* ... and on the bulk-IN pipe                        */
    /* ⚠ BLOCK GREW 128 -> 160 WORDS, TERMINATOR 127 -> 159. BTCheck MUST move in the
     * SAME commit: it scans for 'ENDS' at kWEnd, so an older BTCheck will not find the
     * block AT ALL. This has now bitten four times; do not let it bite a fifth. */
    kWIntArmErrFull = 128,   /* the re-arm error, FULL 32 bits, not a low byte      */
    kWStallClearRc,          /* last USBClearPipeStallByReference return, full width */
    kWIntStatusFull,         /* last interrupt-IN completion status, full width      */
    kWAclStatusFull,         /* last bulk-IN completion status, full width           */
    /* M3.3/M3.4: the trampoline and the file. ★ kWDeferRuns is THE proof that task
     * level was reached from an interrupt-level trigger; kWKfLoaded is THE proof that
     * a bond survived a reboot. */
    kWDeferReqs = 132, kWDeferRuns, kWDeferInstallErrs, kWDeferDropped,
    kWKfLoads, kWKfLoaded, kWKfFlushes, kWKfWritten, kWKfErr, kWKfRejected,
    /* M3.5: what the inquiry SAW, by major device class. ⚠ The responder table holds
     * 4 and real runs see 12-15 answers, so a keyboard answering late was counted and
     * then discarded -- invisible in the readout. These tallies count EVERY responder,
     * so "did a keyboard answer?" is answerable in a crowded room, which with no GUI
     * is the only way to know. */
    kWInqPeriph = 142, kWInqPhones, kWInqComputers, kWInqOther,
    kWInqPeriphAddrHi, kWInqPeriphAddrLo, kWInqPeriphCoD,
    kWRespStored, kWRespDisplaced,
    /* M3.6 / PHASE 1: the INTERFACE init path. ⚠ Run 26 measured the A1044's DEVICE
     * class as 00/00/00, so a device-level 0xE0 match can never see it. kWIfaceTriplet
     * is the datum the USB bus dump could not reach -- the interface descriptor is
     * handed to us directly on this path. */
    kWIfaceInit = 151,     /* ProbeInitializeInterface calls -- 0 = still device-only */
    kWIfaceInitNum,        /* which interface number                                 */
    kWIfaceTriplet,        /* class<<16 | subclass<<8 | protocol                     */
    kWIfaceDevClass,       /* device class of whatever THIS instance bound            */
    kWIfaceVIDPID,         /* vendor<<16 | product -- set on BOTH init paths          */
    /* ---- M0.0 / PHASE 1: the CSR HID-proxy -> HCI mode switch ------------------
     * See docs/M0-MODE-SWITCH.md. BlueZ's hid2hci.rules maps 05ac:1000 to
     * --method=csr, and usb_switch_csr() sends bmRequestType 0x40, bRequest 0,
     * wValue 0 (enum mode { HCI = 0 }), wIndex 0, no data.
     *
     * ⚠⚠ THE VERDICT IS INVERTED FROM EVERY EXPECTATION. BlueZ treats a TIMEOUT as
     * SUCCESS and a clean completion as failure (EALREADY): the card changes
     * personality mid-request and never finishes the handshake. Reading these words
     * the ordinary way -- "nonzero status = broken" -- would report the working case
     * as a failure and cost a hardware cycle. kWSwVerdict states the answer outright
     * so no one has to remember the inversion at analysis time. */
    kWSwTried = 156,       /* mode-switch attempts made                              */
    kWSwImmErr,            /* immediate return from USBDeviceRequest, full width     */
    kWSwStatus,            /* completion usbStatus, full width                       */
    kWSwVerdict,           /* 0 none, 1 SWITCHED (timeout), 2 IGNORED, 3 other error */
    /* ---- M0.0b: enumerate EVERY interface, not just the first ------------------
     * Run 34 (extension removed) showed the unclaimed A1044 yielding THREE bus
     * entries, while the Apple keyboard yields three and the Dell mouse two -- both
     * device-class-0x00 composites -- and every hub yields one. OS 9 enumerates
     * class-0x00 devices PER INTERFACE, so the card has three interfaces and run 31
     * reported only the first of them.
     *
     * kWIfaceFound records a single wildcard hit. These slots record the whole set,
     * by calling USBFindNextInterface repeatedly on the same PB -- it is a find-NEXT,
     * so re-issuing continues the walk. Same encoding as kWIfaceFound:
     * class<<24 | subclass<<16 | protocol<<8 | interface index. */
    kWIfScanCount = 160,   /* how many interfaces the walk recorded                 */
    kWIfScan0, kWIfScan1, kWIfScan2, kWIfScan3,
    kWIfScanErr,           /* the status that ended the walk, full width            */
    /* ---- M0.0c: the stall handling Apple's own driver does, and we did not ------
     * CSRHIDTransitionDriver tests the request's result against kIOUSBPipeStalled
     * (0xE000404F), clears the stall on pipe zero, and retries -- seven sends in all.
     * We sent once and filed a stall under "other error", leaving the DEFAULT CONTROL
     * PIPE STALLED, which is a device that can no longer be enumerated. See
     * docs/M0-MODE-SWITCH.md §9 and [[reference_os9_usb_pipe_stall_must_be_cleared]]. */
    kWSwStalls = 166,      /* stalls seen on the switch request                     */
    kWSwClearRc,           /* last USBClearPipeStallByReference return, full width  */
    kWSwDeferred,          /* retries handed back to the task-level trampoline      */
    /* ---- M3.7: legacy PIN pairing ---------------------------------------------
     * ⚠ gPinReqs has been counted since M2c and the request was NEVER ANSWERED, so
     * legacy pairing could not complete. These words make the answer visible: how
     * many we answered, what gap_pin_code_response returned, and who asked.
     * kWPinAnswered lagging kWPinReqs is the signature of the old bug returning. */
    kWPinAnswered = 169, kWPinRespRc, kWPinAddrHi, kWPinAddrLo,
    /* ---- M0.4: one instance per device -----------------------------------------
     * ⚠ Run 47 bound the dongle FOUR TIMES. Four instances, all via the device path,
     * four BTstacks and four sets of pipes armed on one radio -- only one can own the
     * endpoints, so the rest are wasted at best and fighting the first at worst. It
     * also made the counters unreadable: four blocks, and no way to say which
     * instance a number came from.
     *
     * These words live in the SHARED block and are the interlock. The device refs of
     * everything we have bound, so a second instance can recognise a device that is
     * already claimed and stand down. */
    kWBoundCount = 173, kWBound0, kWBound1, kWBound2, kWBound3,
    kWDupDeclined,        /* duplicate binds refused                                */
    /* ---- M0.5: stop recovering a pipe whose device has gone ---------------------
     * Run 48: 212 interrupt-pipe and 691 bulk-IN "stall clears" across five
     * bind/unbind cycles, with last clear rc and re-arm err both -6997
     * kUSBUnknownPipeErr -- roughly 180 futile recoveries per session against a pipe
     * ref the USL no longer recognised. This counts the point at which we stopped. */
    kWDeadPipeStops,
    /* ---- M0.6: empty reads, which are NOT stalls -------------------------------
     * Run 50: 1708 ACL completions with 1703 "stall clears", last status -6911 and
     * last byte count 0. A bulk-IN pipe with nothing to deliver completes with
     * kUSBNotRespondingErr and zero bytes, and -6911 is in the stall family -- so
     * every idle poll was met with USBClearPipeStallByReference. These count the
     * empty reads so the volume is visible without being mistaken for faults. */
    kWAclEmptyReads, kWIntEmptyReads,
    /* Audio/Video responders (major class 0x04), previously lumped into kWInqOther. */
    kWInqAudio,
    /* ---- M0.7: the ACL reader parks while no link exists ------------------------
     * kWAclIdleParked counts re-arms declined because there was no ACL link. It
     * should be LARGE and the stall-clear counts should collapse; if instead the
     * clears stay high, the link count is being held nonzero by something. */
    kWAclIdleParked, kWAclLinkUps, kWAclLinkDowns,
    /* !! The block grew from 64 to 96 words at M2. kWImmErrCode had reached 57 and
     * the BTstack counters would not fit below the old terminator at word 63. 96
     * words is 384 bytes and leaves room for the L2CAP counters M2 needs next.
     * BTCheck MUST be updated in the SAME commit: it has already been broken twice
     * by disagreeing with this block, once on the version tag and once on the
     * stage names. */
    /* ⚠ THE BLOCK GREW 96 -> 128 WORDS AT M2c, and the terminator moved with it.
     * BTCheck MUST be updated in the SAME commit -- it has now been broken three
     * times by disagreeing with this block (the version tag, the stage names, and
     * the 64->96 growth). It scans for 'ENDS' at kWEnd, so an old BTCheck simply
     * will not find the block at all. 128 words is 512 bytes. */
    /* ⚠ THE BLOCK GREW 160 -> 192 WORDS AT M0.0, terminator 159 -> 191, because the
     * four mode-switch words above needed 156..159 and 159 was the old terminator.
     * BTCheck moves in THIS SAME COMMIT (it scans for 'ENDS' at kWEnd, so an older
     * BTCheck finds no block at all). That is the fifth time this coupling has had
     * to be honoured; the version gate is what makes the mismatch loud instead of
     * silent. 192 words is 768 bytes. */
    /* ======================= M6: THE SCAN CHANNEL ==============================
     * The control panel's command area and the published responder table.
     * docs/SCAN-DESIGN.md; the protocol is docs/M3-DESIGN.md §5c.
     *
     * ⚠⚠ THE BLOCK GREW 192 -> 272 WORDS, terminator 191 -> 271. BTCheck moves in the
     * SAME COMMIT -- it scans for 'ENDS' at kWEnd, so an older BTCheck finds no block
     * at all. That is the SIXTH time this coupling has had to be honoured. Words 0-190
     * are untouched on purpose, so every existing reader keeps working.
     *
     * ⚠ kWCmdSeq IS THE ONLY TRIGGER AND IS WRITTEN LAST. The panel fills the command
     * and its arguments, then bumps the sequence. A single command word would let the
     * driver act on a half-written argument list. The driver copies sequence into ack
     * once it has consumed the command, so "not acknowledged" is a comparison the
     * panel can make rather than a timeout it has to guess at. */
    kWCmd = 192,          /* what the panel wants done (kCmdXxx below)             */
    kWCmdArg0, kWCmdArg1, /* command arguments, written BEFORE the sequence         */
    kWCmdSeq,             /* ★ written LAST by the panel -- the only trigger        */
    kWCmdAck,             /* copied from kWCmdSeq by the driver once consumed       */
    kWCmdResult,          /* 0x100 | status of the last command                     */
    kWScanState,          /* 0 idle, 1 running, 2 complete -- drives chasing arrows */
    kWScanCount,          /* responders published in the table below                */

    /* The responder table, 16 entries of 4 words, at 200..263. Four words because the
     * 2003 panel's row record is the shape that makes a later connection work:
     * address, class of device, and the page-scan/clock parameters that arrive in the
     * inquiry result and are throwaway unless kept (docs/SCAN-DESIGN.md §2). */
    kWScanBase = 200,
    kWScanEnd  = 263,     /* kWScanBase + 16*4 - 1                                  */

    /* ★ M7: the addresses we hold link keys for, so the panel can say "Paired" per row
     * and can list a bonded device that answers no inquiry. 8 entries x 2 words. */
    kWKeyCount = 264,
    kWKeyBase  = 265,
    kWKeyEnd   = 280,     /* kWKeyBase + 8*2 - 1                                    */
    /* ⚠ Bit N = the Nth published key was handed to the controller, the only evidence
     * a bond is mutual. Without it the panel called a key on disk "Paired". */
    kWKeyHandedMask = 281,
    /* M8: 1 = discoverable and connectable, 0 = neither. Published so the panel draws
     * the radio's ACTUAL state on launch instead of assuming its own default. */
    kWRadioOn = 282,
    /* Distinct from the -1000-N range so the panel can tell "already scanning" from a
     * genuine refusal without decoding an error code. ⚠ Mirrored in cpanel/bt_panel.c. */
    /* ⚠ A HISTOGRAM, because "last notification" cannot show a SEQUENCE. Run 53 had 12
     * notifications and we could only see the final one. Which codes arrive, and how
     * often, is the question the churn investigation actually needs. Words 283-286 were
     * the last four free before kWEnd, so this fits with no block growth. */
    kWNotifyRemoved = 283,   /* 0x0B kNotifyDriverBeingRemoved                  */
    kWNotifySleep   = 284,   /* 0x01/0x02 sleep request or demand               */
    kWNotifyOther   = 285,   /* anything else, including expert terminating     */
    /* ⚠ The last free word before kWEnd. gap_inquiry_stop()'s result, so a refused
     * start and the recovery attempted after it are both visible in the log. */
    kWInqStopRc     = 286,

    kBTAlreadyScanning = -1100,
    /* ⚠ "That address held no key" -- distinct from success AND from a failure, so the
     * panel can say so instead of claiming a deletion it never made. */
    kBTNoSuchBond      = -1102,

    /* ★ M4 step 1: the incoming HID control channel. docs/M4-DESIGN.md §10 step 1.
     * ⚠ The block grows 288 -> 304, which moves 'ENDS' again. Eighth time; the
     * top-level CMakeLists now fails the build if either reader disagrees, so this is
     * mechanical rather than a landmine. 299..302 left spare for step 2's interrupt
     * channel so THAT does not have to move it a ninth time. */
    kWHidListenRc   = 288,
    kWHidIncoming   = 289,
    kWHidAccepted   = 290,
    kWHidDeclined   = 291,
    kWHidOpened     = 292,
    kWHidClosed     = 293,
    kWHidOpenStatus = 294,
    kWHidLastPsm    = 295,
    kWHidCtrlCid    = 296,
    kWHidPeerHi     = 297,
    kWHidPeerLo     = 298,

    /* ★★ THE MAILBOX SERVICER'S OWN COUNTERS, in the words that were already spare, so
     * kWEnd does NOT move: the block layout is unchanged, the CMake guard still passes
     * with all three readers at 303, and BTCheck v61 keeps finding the block. */
    kWTimerRuns     = 299,           /* firings of the driver-owned pump timer     */
    kWMbxServiced   = 300,           /* commands the timer has consumed            */
    kWMbxStale      = 301,           /* servicer ran with nothing to do            */

    /* ★★★ WHY THE TIMER DID NOT START, which v5.5 had no way to say. It recorded
     * neither SetPersistentTimer's return code nor which of the handler's four guards
     * fired, so a failed CALL and a bailing HANDLER were indistinguishable -- and the
     * whole run could only conclude "35 pump calls, so it never ran".
     *
     * TWO ATTEMPT SITES, RECORDED SEPARATELY, because the leading hypothesis is that
     * the call is not legal from ConfigDone's INTERRUPT context. Task level tries
     * first; ConfigDone only tries if that did not take. Comparing the two words says
     * which context works instead of leaving it to be inferred. */
    kWTimerRcTask   = 302,           /* 0x100 | rc from the task-level attempt      */
    kWTimerRcIrq    = 303,           /* 0x100 | rc from the ConfigDone attempt      */
    kWTimerBail     = 304,           /* bitmask of guards that turned the handler back */

    /* ★★★ DOES THIS CONTROLLER HAVE A LINK-KEY STORE OF ITS OWN?
     *
     * The A1044 in HID-proxy mode runs the Bluetooth stack on-chip and reconnects a
     * paired keyboard with no host involved -- proven 2026-09-06, when an A1016 paired
     * under Tiger drove the Open Firmware boot picker. So the CARD holds a link key.
     *
     * The question that decides how OS 9 could ever pair one is whether a host can PUT
     * a key there, and it turns out not to need a vendor command at all: the Bluetooth
     * spec defines Read/Write/Delete_Stored_Link_Key (OGF 0x03, OCF 0x0D/0x11/0x12)
     * for exactly this. BTstack ships only the Delete wrapper, which is itself evidence
     * the concept is real; the other two we define ourselves.
     *
     * Read_Stored_Link_Key's command complete returns Max_Num_Keys, and that number IS
     * the answer: greater than zero means the controller has a store and Write will
     * work. Asking costs one harmless read at bring-up and needs no mode switch, so it
     * can be measured on the dongle before the A1044 is touched.
     *
     * ⚠ Both words fit in what was already spare, so kWEnd stays 307 and the block
     * layout is unchanged. */
    kWStoredKeyRc   = 305,           /* 0x100 | status of Read_Stored_Link_Key       */
    kWStoredKeyCap  = 306,           /* (Max_Num_Keys << 16) | Num_Keys_Read         */

    /* ★★★ v6.5 -- WHY THE PROBE NEVER SENT, and WHO PAGED US.
     *
     * The v6.3 run bound the A1044 at 8204, reached HCI_STATE_WORKING and fired the
     * pump timer 3193 times with a bail mask of 0, yet Read_Stored_Link_Key never
     * went out. Our own gate was provably open (kWCmdSent 19 == kWCmdComp 19, so
     * gCmdBusy was 0), so the refusal happened inside BTstack -- where the block had
     * no counter whatsoever. That is the instrumentation gap these six words close.
     *
     * ⚠ The block grows 307 -> 313, which moves 'ENDS' for the NINTH time. BTCheck
     * scans for the terminator AT kWEnd, so an older BTCheck finds no block at all
     * rather than misreading this one -- which is why BTCheck moves in this SAME
     * commit, exactly as it has the previous eight times. */
    kWSendRc        = 307,           /* 0x100 | hci_send_cmd's return code           */
    kWGateFirst     = 308,           /* (hci state << 8) | gate bits, FIRST sample   */
    kWGateLast      = 309,           /* ... and the LAST, so a brief state is visible*/
    kWAclSlots      = 310,           /* free classic ACL slots BTstack believes it has*/
    kWConnPeerHi    = 311,           /* BD_ADDR of whatever completed a connection,  */
    kWConnPeerLo    = 312,           /*   top 3 bytes then bottom 3                  */

    /* ★★★ v6.6 -- NAME THE BONDED DEVICES, AND FIND WHERE THE 8 LINKS DIED.
     *
     * v6.5 answered the store question (Max_Num_Keys 16, Num_Keys_Read 2) and then
     * showed the real defect: a peer connected EIGHT times, sent one 12-byte L2CAP
     * signalling packet each time, and timed out each time at reason 8. kWAclSent
     * stayed 0 throughout -- we never answered once. security requests made 8,
     * link key requests 0, no Auth Complete, no Encryption Change.
     *
     * Two things the block could not say, and both are needed:
     *
     * 1. WHO. The peer was 00:0A:95:xx:xx:xx, an Apple OUI, and the obvious guess is
     *    the A1016 -- but the Tiger notes never recorded the keyboard's address, so
     *    the comparison cannot be made. The controller ALREADY told us both bonded
     *    addresses: Read_Stored_Link_Key with Read_All_Flag = 1 returns them in
     *    HCI_Return_Link_Keys (0x15) events, which we counted as unsolicited and
     *    threw away. Parsing them names both devices with no Tiger boot.
     *
     *    ⚠⚠ ADDRESSES ONLY. That event carries the 16-byte LINK KEYS beside each
     *    address. They are secrets, they are of no diagnostic use, and a heap block
     *    that BTCheck dumps to a log file on a shared volume is the last place they
     *    should live. Read the address, step over the key.
     *
     * 2. WHERE IT DIES. The command SEQUENCE, not just the last opcode. With no
     *    debugger, an eight-deep ring of sent opcodes is the only way to see the
     *    order, and the order is the whole question: whether Authentication_Requested
     *    (0x0411) is ever sent, and what the controller answers.
     *
     * ⭐ The v6.5 log already carries a strong hint that must be checked rather than
     * assumed. Its last event bytes were 0F 04 00 00 1B 04: a Command Status for
     * Read_Remote_Supported_Features (0x041B) with Num_HCI_Command_Packets = 0. hci.c
     * sets num_cmd_packets from that byte, so at that instant BTstack had NO command
     * credit left. kWLastCmdStatus records it explicitly instead of leaving it to be
     * hand-decoded out of a raw byte dump.
     *
     * ⚠ The block grows 313 -> 341, the TENTH move of 'ENDS'. BTCheck and the panel
     * move in this same commit; the CMake guard enforces it. */
    kWRlkEvents     = 313,           /* HCI_Return_Link_Keys events seen             */
    kWRlkKeys       = 314,           /* Num_Keys summed across them                  */
    kWRlkA0Hi       = 315,           /* the bonded addresses, top 3 bytes then low 3  */
    kWRlkA0Lo       = 316,
    kWRlkA1Hi       = 317,
    kWRlkA1Lo       = 318,
    kWRlkA2Hi       = 319,
    kWRlkA2Lo       = 320,
    kWRlkA3Hi       = 321,
    kWRlkA3Lo       = 322,
    kWRlkSlots      = 4,             /* how many of the 16 we have room to name       */

    kWConnReqs      = 323,           /* HCI_EVENT_CONNECTION_REQUEST count            */
    kWConnReqHi     = 324,           /* who paged, at PAGE time rather than after     */
    kWConnReqLo     = 325,
    kWConnReqCoD    = 326,           /* its class of device -- keyboard? mouse?       */
    kWConnReqType   = 327,           /* 0x100 | link type (1 = ACL)                   */

    /* ⚠ The COUNT only. The last one's status is already published at kWAuthComplete
     * and a second copy would just be another number that has to agree with a first. */
    kWAuthComps     = 328,           /* Authentication_Complete count                 */
    kWLkNotifs      = 330,           /* Link_Key_Notification count (a NEW pairing)   */
    kWLastCmdStatus = 331,           /* (opcode << 16) | (credits << 8) | status      */

    kWCmdRing       = 332,           /* the last 8 opcodes SENT, oldest to newest     */
    kWCmdRingEnd    = 339,
    kWCmdRingIdx    = 340,           /* where the next one goes                       */

    /* ★★★ v6.7 -- WHAT THE CONTROLLER SENT BACK, which is the one thing never recorded.
     *
     * v6.6's opcode ring showed 0x0409 Accept_Connection_Request then 0x041B
     * Read_Remote_Supported_Features, three times over, and then nothing at all.
     * Authentication_Requested is never sent because hci.c:8096 gates it on
     * BONDING_RECEIVED_REMOTE_FEATURES, and hci.c sets that flag ONLY from the
     * HCI_EVENT_READ_REMOTE_SUPPORTED_FEATURES_COMPLETE (0x0B) handler. The controller
     * answered 0x041B with Command Status status 0, credit 0 -- accepted, in progress
     * -- and then never appeared to complete it.
     *
     * "Never appeared to" was inference from an ABSENCE: the block had no record of a
     * single event code, so a missing completion and a mis-parsed one looked identical.
     * That is not a conclusion, it is a gap.
     *
     * Each slot is (code << 16) | (param_len << 8) | byte2. Byte 2 is the status field
     * of almost every HCI event, so a slot carries the answer and not merely the fact.
     *
     * ⚠ Filled in the TRANSPORT (hci_transport_os9.c), not in the BTstack handler:
     * that handler also sees events BTstack synthesises itself, and the question is
     * strictly what the controller said. See the note there.
     *
     * ⚠ The block grows 341 -> 362, the ELEVENTH move of 'ENDS'. BTCheck and the panel
     * move in this same commit and the CMake guard enforces it. */
    kWEvtRing       = 341,           /* 16 slots, oldest-to-newest by kWEvtRingIdx    */
    kWEvtRingEnd    = 356,
    kWEvtRingIdx    = 357,           /* where the NEXT one goes = the OLDEST slot     */
    kWEvtTotal      = 358,           /* every event the transport delivered           */
    kWRrsfCount     = 359,           /* ⭐ event 0x0B arrivals -- 0 answers the question */
    kWRrsfStatus    = 360,           /* 0x100 | its status (0 IS a valid status)       */
    kWRrsfHandle    = 361,           /* and for which connection handle                */

    /* ★★★ v6.8 -- THE LEVEL_0 PROBE, and the data path that makes it mean anything.
     *
     * v6.7 found a three-link deadlock: L2CAP holds an incoming connection pending
     * LEVEL_2 security, security waits on authentication, authentication waits on a
     * Read_Remote_Supported_Features completion the A1044 does not deliver while the
     * link is alive. We cannot make the card complete that command; we can stop L2CAP
     * waiting on it. At LEVEL_0 the connection is reported and answered immediately.
     *
     * ⚠⚠ kWHidLevel IS NOT OPTIONAL. A run whose security level is ambiguous cannot
     * be interpreted at all, and this project has already lost a run to being unable
     * to tell two builds apart. Every other row here is meaningless without it.
     *
     * ⚠ And the raw first payload is captured beside the decoder's verdict. The
     * decoder has 32 passing host tests and zero hardware mileage; if it rejects a
     * real report, the bytes are the only way to find out why. A verdict without the
     * evidence behind it would just be a new thing to take on trust. */
    kWHidLevel      = 362,           /* ⚠ 0 or 2 -- WHICH BUILD THIS WAS            */
    kWHidDataPkts   = 363,           /* L2CAP data packets on the HID channel        */
    kWHidLastDataLen= 364,
    kWHidFirstLen   = 365,           /* bytes captured (capped)                      */
    kWHidFirstFull  = 366,           /* ... of this many actually received           */
    kWHidFirstData  = 367,           /* 4 words = 16 bytes, big-endian per word      */
    kWHidFirstDataEnd = 370,
    kWHidDecodeRc   = 371,           /* 0x100 | BT_DecodeBootKeyboard's return       */
    kWHidDecodeOk   = 372,           /* reports it accepted                          */
    kWHidRollovers  = 373,
    kWHidLastMods   = 374,
    kWHidLastNKeys  = 375,
    kWHidLastKey0   = 376,           /* ⭐ a real HID usage ID would be a first      */
    /* ⚠⚠ FABRICATED EVENTS, COUNTED. Printed by BTCheck right beside the real
     * 0x0B arrivals so no future reader can mistake one for the other. An
     * injected event that is not counted is a lie with no audit trail. */
    kWSynthRrsf     = 377,

    /* ★★★ v7.0 -- WHICH BRANCH OF gap_request_security_level RAN.
     *
     * Authentication_Requested has never once been sent, across five runs and four
     * different theories of mine, every one of them wrong. It is gated on
     * BONDING_SEND_AUTHENTICATE_REQUEST, which hci.c sets in exactly one reachable
     * place: gap_request_security_level's final else-branch (hci.c:9177). That
     * function has three outcomes and only one is observable from outside:
     *
     *   requested <= current  -> emits GAP_EVENT_SECURITY_LEVEL(current, SUCCESS)
     *                            and returns. Event ARRIVES; no auth requested.
     *   authentication_active -> bumps the level, emits NOTHING.
     *   otherwise             -> sets the flag; the event arrives LATER with the
     *                            achieved level.
     *
     * ⇒ The event's presence identifies the first case. Its ABSENCE narrows to two,
     * and the sampled predicates are what separate those. ALL OF IT IS RECORDED AS
     * RAW READINGS -- the verdict belongs to BTCheck's decision table and to whoever
     * reads it, not to a comment written before the measurement.
     *
     * ⚠ The handler ring is NOT a duplicate of the transport's. That one sees only
     * real controller packets, by design, so it cannot see anything BTstack
     * synthesises -- GAP_EVENT_SECURITY_LEVEL (0xD8) and the whole L2CAP_EVENT_*
     * family among them. "L2CAP events seen 1" was the symptom of that blindness:
     * one counter, no codes, no way to know which event it was.
     *
     * ⚠ The block grows 378 -> 401, the THIRTEENTH move of 'ENDS'. BTCheck and the
     * panel move in this same commit and the CMake guard enforces it. */
    kWSecEvtCount   = 378,           /* GAP_EVENT_SECURITY_LEVEL arrivals            */
    kWSecEvtLevel   = 379,           /* 0x100 | the level it reported                */
    kWSecEvtStatus  = 380,           /* 0x100 | its status                           */
    kWSecEvtHandle  = 381,
    kWSecLvlFirst   = 382,           /* 0x100 | gap_security_level, while link UP    */
    kWSecLvlLast    = 383,
    kWRemFeatFirst  = 384,           /* 0x100 | hci_remote_features_available        */
    kWRemFeatLast   = 385,
    kWLiveSamples   = 386,           /* how many samples were taken on a live link   */
    kWHandlerRing   = 387,           /* 16 slots: every code reaching OUR handler    */
    kWHandlerRingEnd = 402,
    kWHandlerIdx    = 403,
    kWHandlerTotal  = 404,
    /* v7.1: the handle the sampler actually SAW, from both files. A count of zero
     * could not distinguish never-set from cleared-early from sampled-too-soon. */
    kWSampHandle    = 405,
    kWSampHciHandle = 406,
    kWSampHandleMax = 407,
    kWSampHciMax    = 408,
    kWTimerAtEnd    = 409,           /* timer firings, beside the samples        */

    /* ★★★★ v7.2 -- INITIATOR PAIRING, AND THE WRITE THAT MAKES IT USEFUL.
     *
     * The goal is a device paired FROM OS 9 that the card then proxies unaided. These
     * rows are the whole of it:
     *   kWPairAsked/Rc     did gap_dedicated_bonding get called, and accept
     *   kWBondDone/Status  ⭐ GAP_EVENT_DEDICATED_BONDING_COMPLETED -- the verdict
     *   kWWroteKeyTried/Rc did Write_Stored_Link_Key go out
     *   kWWroteKeyDone     ⭐⭐ (Num_Keys_Written << 8) | Status from its completion.
     *                      Status 0 with 1 key written is the goal step LANDING in
     *                      the controller's own store.
     *
     * ⚠ The block grows 410 -> 419, the FOURTEENTH move of 'ENDS'. BTCheck and the
     * panel move in this same commit and the CMake guard enforces it. */
    kWPairAsked     = 410,
    kWPairRc        = 411,
    kWPairAddrHi    = 412,
    kWPairAddrLo    = 413,
    kWBondDone      = 414,
    kWBondStatus    = 415,
    kWWroteKeyTried = 416,
    kWWroteKeyRc    = 417,
    kWWroteKeyDone  = 418,

    /* v7.3: Create_Connection's own parameter bytes. See the capture site -- 0x12
     * means a parameter was rejected and nothing recorded which one. 4 words = 16
     * bytes, covering all 13 parameters. */
    kWCreateConn    = 419,
    kWCreateConnEnd = 422,
    kWCreateConnLen = 423,           /* the command's total length, 0 = never sent   */
    /* v7.4: the FIRST Command Status carrying a non-zero status, WITH its opcode.
     * v7.3 recorded two 0x12 rejections and could not name the command. */
    kWFirstBadCmd   = 424,           /* (opcode << 16) | (credits << 8) | status     */
    kWBadCmdCount   = 425,
    /* v7.5: ALL the failing command statuses, not just the first. v7.4's single slot
     * was filled by a harmless 0x0C52/0x01 bring-up refusal and hid the two 0x12s. */
    kWBadCmdRing    = 426,
    kWBadCmdRingEnd = 429,
    /* v7.6: which pairing path ran, and the link state that chose it. */
    kWPairPath      = 430,           /* 1 = authenticate existing, 2 = create      */
    kWLinkWasUp     = 431,           /* 0x100 | (a link was open at decision time) */
    /* v7.8: arm-and-wait pairing, and the controller's own command bitmap. */
    kWArmedFired    = 432,           /* the arm fired on an inbound connection      */
    kWArmedRc       = 433,           /* 0x100 | Authentication_Requested's send rc  */
    kWSuppCmds0     = 434,           /* octets 0..3 of Read_Local_Supported_Commands*/
    kWSuppCmds1     = 435,           /* octets 4..7                                 */
    kWSuppCmdsGot   = 436,           /* 0 = the bitmap was never captured           */
    /* v7.9: the card's own Bluetooth version. Decides which devices can EVER pair. */
    kWLocalVer      = 437,           /* 0x1000000 | HCI<<16 | LMP<<8 | status       */
    kWLocalMfr      = 438,
    kWCreateConnPatched = 439,       /* Create_Connections whose packet type we fixed */
    /* v8.1: Delete_Stored_Link_Key -- proves the card's store is WRITABLE at all. */
    kWDelStoredTried = 440,
    kWDelStoredRc    = 441,          /* 0x100 | send rc                              */
    kWDelStoredDone  = 442,          /* 0x100 | (Num_Keys_Deleted << 8) | status     */
    kWDelStoredHi    = 443,
    kWDelStoredLo    = 444,
    /* ---- v8.2: proving the A1016 can pair in OS 9 with no help from Tiger --------
     * ⚠ THE BLOCK GROWS HERE, so BTCheck and the control panel BOTH move in this same
     * commit. They scan for 'ENDS' at kWEnd; a reader with the old 445 finds no block
     * at all and reports nothing. That exact mismatch has already cost this project a
     * boot -- v6.4's block ended at 307 while BTCheck v73 scanned at 313. */
    kWStoredKeyCapFirst = 445,       /* the BRING-UP read, kept across a re-read     */
    kWRlkPasses         = 446,       /* Read_Stored_Link_Key sends ACCEPTED          */
    kWPagedCount        = 447,       /* distinct addresses that PAGED us             */
    kWPagedA0Hi         = 448,       /* 4 x (hi, lo, class-of-device) = 12 words     */
    kWPagedEnd          = 459,
    /* ---- v8.6: the report-protocol experiment (docs/M4-DESIGN.md) --------------
     * ⚠ THE BLOCK GROWS AGAIN, so BTCheck moves in this same commit. */
    kWHidIntrListenRc   = 460,       /* 0x100 | l2cap_register_service(PSM 0x13)   */
    kWHidIntrCid        = 461,
    kWHidIntrOpened     = 462,
    kWHidSetProtoTried  = 463,       /* SET_PROTOCOL(Report) sends attempted       */
    kWHidSetProtoRc     = 464,       /* 0x100 | l2cap_send rc; 0xFE = armed only   */
    kWHidHandshake      = 465,       /* 0x100 | HIDP handshake header byte         */
    kWHidCtrlDataPkts   = 466,       /* packets on the CONTROL channel             */
    kWHidIntrDataPkts   = 467,       /* packets on the INTERRUPT channel           */
    /* ---- v8.7: what the peer SAID, and HOW LONG the link lived ------------------
     * ⚠ THE BLOCK GROWS AGAIN, so BTCheck and the panel move in this same commit. */
    kWAclCapCount       = 468,       /* ACL packets captured, 0..kAclCapSlots      */
    kWAclCap0           = 469,       /* 4 slots x 16 bytes = 4 words each           */
    kWAclCapEnd         = 484,
    kWLinkUpMs          = 485,       /* hal_time_ms at the FIRST Connection Complete */
    kWLinkDownMs        = 486,       /* ... and at the FIRST Disconnection Complete  */
    kWLinkLifeMs        = 487,       /* ⭐ the headline: how long the link lived     */
    kWRrsfSentMs        = 488,       /* when 0x041B went out                         */
    kWRrsfDoneMs        = 489,       /* when its completion arrived                  */
    /* ---- v8.8: authenticate an inbound link ourselves --------------------------
     * ⚠ THE BLOCK GROWS AGAIN, so BTCheck and the panel move in this same commit. */
    kWAutoAuthTried     = 490,       /* inbound links we authenticated unprompted    */
    kWAutoAuthRc        = 491,       /* 0x100 | hci_send_cmd rc                      */
    kWAutoAuthHandle    = 492,
    /* ⚠⚠ WHICH BUILD THIS WAS, for exactly the reason kWHidLevel exists.
     *
     * Without this word, "authed 0 while something paged us" has two completely
     * different meanings -- our address test failed, or the gate is simply OFF -- and
     * BTCheck's first draft printed the accusatory one unconditionally. In the
     * SHIPPING build kHidAutoAuth is 0, so every normal run would have been reported
     * as "that is OUR bug". This project has already split three readouts that called
     * the shipping state a failure; publishing the gate is what stops the fourth. */
    kWAutoAuthGate      = 493,       /* ⚠ kHidAutoAuth as compiled: 0 or 1           */
    /* ---- v8.9: IS THIS BLOCK FRESH? ⚠ THE BLOCK GROWS, readers move with it ------
     * ⚠⚠ READ THESE BEFORE ANY OTHER WORD. Until v8.9 the block was mirrored only
     * from the packet handler, so on a quiet radio it froze at bring-up and every
     * reader presented those values as current. The v8.8 run reported `timer firings
     * 3` and a reserved packet buffer after 136 s of uptime and both were stale
     * snapshots, not measurements. These two words are what makes that detectable:
     * publishes should climb about 10 per second, and the ms stamp should be close to
     * the newest timestamp anywhere else in the block. */
    kWPublishRuns       = 494,       /* timer-driven refreshes of this block          */
    kWPublishMs         = 495,       /* hal_time_ms at the last refresh               */
    /* ---- v9.0: WERE WE EVEN REACHABLE? ⚠ THE BLOCK GROWS, readers move with it ----
     * The controller's own Scan_Enable, read back with OCF 0x19 instead of inferred
     * from the variable we set when we asked for it. Bit 1 is page scan. "Nothing
     * paged us" and "we were not listening" were indistinguishable before these. */
    kWScanEnaRc         = 496,       /* 0x100 | hci_send_cmd rc of the last read      */
    kWScanEnaSends      = 497,       /* reads successfully sent                       */
    kWScanEnaReads      = 498,       /* Command Completes that came back              */
    kWScanEnaFirst      = 499,       /* 0x100 | Scan_Enable, FIRST read               */
    kWScanEnaLast       = 500,       /* 0x100 | Scan_Enable, LAST read                */
    /* ---- v9.1: WHAT WE SEND. ⚠ THE BLOCK GROWS, readers move with it -------------
     * The inbound capture has existed since v8.7 and its mirror never did, so five
     * driver versions analysed the peer's silence without once examining our own
     * Connection Response. `ACL packets sent 6` with 6 completions says our bytes
     * reach the wire, so the peer receives something from us and stops. Same slot
     * geometry as the inbound capture; BTCheck decodes both with one helper. */
    kWAclTxCapCount     = 501,       /* outbound ACL packets captured, 0..kAclCapSlots */
    kWAclTxCap0         = 502,       /* 4 slots x 24 bytes = 6 words each              */
    kWAclTxCapEnd       = 525,       /* ⚠ 24 not 16: RESULT is at byte 16              */
    kWAclTxLen0         = 526,       /* FULL length per slot, before truncation        */
    kWAclTxLenEnd       = 529,
    /* ---- v9.2: EVERY Authentication Complete. ⚠ THE BLOCK GROWS AGAIN ------------
     * kWAuthComplete is last-value-wins, so `Auth Completes 2` could not say whether
     * the FIRST succeeded -- the difference between auth working and encryption being
     * lost downstream, and auth never running at all. */
    kWAuthRingCount     = 530,
    kWAuthRing0         = 531,       /* 0x1000000 | (status << 16) | handle           */
    kWAuthRingEnd       = 534,
    kWAuthRingMs0       = 535,       /* hal_time_ms of each                           */
    kWAuthRingMsEnd     = 538,
    /* ---- v9.3: the supervision stretch. ⚠ THE BLOCK GROWS AGAIN ------------------ */
    kWSupTimeoutTried   = 539,
    kWSupTimeoutRc      = 540,       /* 0x100 | hci_send_cmd rc                       */
    kWSupTimeoutHandle  = 541,
    kWSupGate           = 542,       /* ⚠ kHidLongSupervision as compiled             */
    /* ---- v9.4: the link policy, the queue, and MODE CHANGE. ⚠ BLOCK GROWS -------- */
    kWPolicyGate        = 543,       /* ⚠ kHidLinkPolicy as compiled                  */
    kWPolicyReadRc      = 544,
    kWPolicyReadSends   = 545,
    kWPolicyReads       = 546,       /* Command Completes that came back              */
    kWPolicyReadBack    = 547,       /* 0x10000 | status<<8 | settings; bit2 = SNIFF  */
    kWPolicyWriteRc     = 548,
    kWPolicyWriteSends  = 549,
    kWLinkCmdEnq        = 550,
    kWLinkCmdSent       = 551,
    kWLinkCmdRefused    = 552,
    kWLinkCmdDropped    = 553,
    kWLinkCmdDepth      = 554,       /* still queued at sample time                   */
    kWModeChanges       = 555,
    kWModeLast          = 556,       /* 0x100 | mode; 2 = SNIFF                       */
    kWModeLastMs        = 557,
    /* ---- v9.5: BTstack's own HID host, finally driving. ⚠ BLOCK GROWS ----------- */
    kWBhGate            = 558,       /* ⚠ kHidUseBtstackHost as compiled            */
    kWBhInit            = 559,       /* hid_host_init actually ran                  */
    kWBhIncoming        = 560,
    kWBhCid             = 561,
    kWBhAcceptRc        = 562,       /* 0x100 | hid_host_accept_connection rc        */
    kWBhOpened          = 563,
    kWBhOpenedStatus    = 564,       /* 0x100 | status                               */
    kWBhOpenedMs        = 565,
    kWBhClosed          = 566,
    kWBhClosedMs        = 567,
    kWBhSetProtoRsp     = 568,       /* ⭐ 0x100 | HIDP handshake                    */
    kWBhDescAvail       = 569,
    kWBhReports         = 570,       /* ⭐⭐ real HID reports                        */
    kWBhOtherEvts       = 571,
    /* v9.6: ⚠ the level hid_host ACTUALLY registered with, not the gate's intent. */
    kWGapLevel          = 572,       /* 0x100 | gap_get_security_level()             */
    /* ---- v9.7: THE TRANSPORT. ⚠ BLOCK GROWS. The 0x13 question, measured --------
     * Tiger gets one Number_Of_Completed_Packets per ACL send on this exact card
     * (docs/TIGER-HCI-TRACE.md). We appear to get none, which would mean our ACL
     * packets never leave the controller and the peer never hears us. ⚠ The previous
     * reading came from a 16-entry event ring; these are real counters. */
    kWNumCompEvts       = 573,       /* ⭐⭐ 0x13 events, COUNTED                     */
    kWNumCompTotal      = 574,       /* packets those events acknowledged            */
    kWNumCompHandle     = 575,
    kWNumCompMs         = 576,
    kWBufSizeRc         = 577,       /* 0x10000 | status<<8, Read_Buffer_Size        */
    kWAclBufLen         = 578,       /* controller's ACL packet length               */
    kWAclBufNum         = 579,       /* controller's ACL buffer COUNT                */
    kWPipeInt           = 580,       /* ⚠ classType<<24|subclass<<16|proto<<8|other  */
    kWPipeOut           = 581,
    kWPipeInB           = 582,

    /* ---- v9.9: THE RAW PIPE REFERENCES. ⚠ BLOCK GROWS -------------------------
     * ⚠⚠ THE THREE WORDS I SHOULD HAVE PUBLISHED IN v9.7 AND ARGUED MYSELF OUT OF.
     *
     * v9.7 published four USBPB DESCRIPTOR fields per pipe and they turned out to
     * carry nothing about the endpoint: bulk-OUT and bulk-IN came back byte-identical
     * (0x02000103), which is equally consistent with "the fields just echo our inputs"
     * and with "both finds returned the SAME PIPE". I took the first reading and
     * declared the pipe question closed. The disambiguator was always these three
     * refs, and they cost three words.
     *
     * ⇒ THE ARGUMENT THAT WAS WRONG, written out so it is not repeated: "we receive
     * real inbound ACL, which is impossible on an OUT endpoint, therefore the
     * direction filter works, therefore gBulkOut is the OUT endpoint." The two finds
     * are NOT symmetric -- USB.h has kUSBIn = 1 but ⭐ kUSBOut = 0. If the USL reads
     * 0 as "unspecified" the IN find still filters correctly while the OUT find
     * degenerates to "first bulk pipe, any direction". Working inbound therefore
     * proves gBulkIn and says NOTHING about gBulkOut.
     *
     * ⇒ gBulkOut == gBulkIn is the whole hypothesis, and it explains every number:
     * a write issued on an IN pipe completes with status 0 and moves nothing outward,
     * so the controller has nothing to transmit and never emits 0x13. It also
     * explains the read counts (3 armed, 1 completed here; 7 armed, 3 completed in
     * v9.7) as contention between reads and "writes" on one pipe. */
    /* ---- v10.0: THE ROLE, AND WHAT THE USL ACTUALLY MOVED. ⚠ BLOCK GROWS ---------
     * ⭐ ROLE_CHANGE HAS NEVER BEEN HANDLED, in ten builds. Tiger accepts with role
     * 0x00 and gets a Role_Change 215 ms later; BTstack defaults to remain-slave
     * (hci.c:5551) and we never looked. A slave may only transmit when the master
     * polls it, and 0x13 is emitted on TRANSMISSION -- so a queued packet on an
     * unpolled slave link is exactly "accepted, never acknowledged".
     *
     * ⭐⭐ AND usbActCount ON THE ACL WRITE, the most fundamental unmeasured fact
     * about the send path. We have recorded usbStatus for ten builds and never the
     * byte count. It partitions the problem with no ambiguity:
     *   act == req (20)  the USL moved every byte to the endpoint, so the fault is
     *                    at or beyond the controller -- and role/packet-type are the
     *                    live candidates
     *   act == 0 or short  the USL completed having moved nothing, and the fault is
     *                    in our own USL usage, upstream of the controller entirely */
    kWRefInt            = 583,       /* ⭐ raw USBPipeRef, interrupt-IN               */
    kWRefBulkOut        = 584,       /* ⭐⭐ raw USBPipeRef, bulk-OUT                 */
    kWRefBulkIn         = 585,       /* ⭐ raw USBPipeRef, bulk-IN                    */
    kWPipeOrder         = 586,       /* 1 = v9.9 IN-then-chained-OUT, 0 = old re-seed */
    kWPipeChainErr      = 587,       /* immediate err from the CHAINED out-find, if any */

    kWRoleChanges       = 588,       /* HCI_EVENT_ROLE_CHANGE arrivals                */
    kWRoleStatus        = 589,       /* 0x100 | status of the last one                */
    kWRoleNew           = 590,       /* 0x100 | new role: 0 = MASTER, 1 = slave       */
    kWRoleMs            = 591,       /* when, so it can be placed against link-up     */
    kWAclOutAct         = 592,       /* ⭐⭐ usbActCount of the last ACL write         */
    kWAclOutReq         = 593,       /* what we asked it to send, to compare against  */

    /* ---- M5 step 1: THE CONSUMER PAGE. ⚠ BLOCK GROWS ------------------------------
     * ⭐ v10.0 got the reports arriving; this is what is IN them. And the ring exists
     * to settle the one thing the decoder deliberately refuses to guess: the
     * descriptor marks Eject and Mute RELATIVE and the volume keys ABSOLUTE, and
     * whether the A1016 actually CLEARS the relative bits is unmeasured. Tiger's
     * capture shows four presses and no releases, which is a fact about what was
     * extracted from it, not about the wire.
     *
     * ⚠ TRANSITIONS ONLY. The keyboard sends idle reports continuously, so a ring of
     * every value would fill with zeros before a key was pressed -- the instrument
     * would measure its own noise, which is exactly the trap that cost v8.9 and v9.4 a
     * run each. Each slot is 0x100 | byte8 so an unfilled slot differs from a real 0.
     *
     * ⇒ READ IT AS A SEQUENCE. 00 -> 04 -> 00 means volume up cleared on release;
     * 00 -> 01 -> (no zero) means Eject latched and the injection policy must treat a
     * set relative bit as a one-shot rather than a held state. */
    kWHidConsRpts       = 594,       /* reports that carried byte 8 at all            */
    kWHidConsSeen       = 595,       /* ⭐ OR of every nonzero nibble seen            */
    kWHidConsPadBits    = 596,       /* ⚠ reports with bits 4..7 set (descriptor wrong)*/
    kWHidConsRingCount  = 597,
    kWHidConsRingLost   = 598,       /* transitions past the end of the ring          */
    kWHidConsRing       = 599,       /* .. 610, TWELVE slots -- see kHidConsumerRing  */
    kWHidConsRingEnd    = 610,
    kWHidKeyRpts        = 611,       /* reports that actually HELD a key              */

    /* ---- M5 step 2c: INJECTION. ⚠ BLOCK GROWS -------------------------------------
     * ⚠⚠ EVERY ONE OF THESE EXISTS BECAUSE bt_inject.c CANNOT BE TESTED OFF-TARGET.
     * The decode, the keycode table and the press/release diff have 169 host
     * assertions behind them; this file has none and cannot have any. The log is the
     * only way to see inside it, so each failure mode gets its own counter rather than
     * sharing one -- "no keystroke appeared" has at least five distinct causes and one
     * boolean cannot separate them. */
    kWInjInitRuns       = 612,
    kWInjKchrOk         = 613,       /* 1 once KCHR is held                           */
    kWInjKchrErr        = 614,       /* 0x100 | ResError(), if it failed              */
    kWInjCalls          = 615,       /* BT_KeyEvents calls = decoded reports          */
    kWInjEventsSeen     = 616,       /* ⭐ press/release events the DIFF produced      */
    kWInjEvents         = 617,       /* events handed to the injector                 */
    kWInjPosted         = 618,       /* ⭐⭐ PostEvent calls actually made             */
    kWInjNoVk           = 619,       /* usages with no Mac virtual keycode            */
    kWInjNoKchr         = 620,       /* dropped for want of KCHR                      */
    kWInjNoChar         = 621,       /* KeyTranslate produced nothing                 */
    kWInjLastUsage      = 622,       /* 0x100 | last usage                            */
    kWInjLastVk         = 623,       /* 0x100 | last virtual keycode                  */
    kWInjPostErr        = 624,       /* 0x100 | last nonzero PostEvent result         */

    /* ---- M6: THE MEDIA KEYS. ⚠ BLOCK GROWS ----------------------------------------
     * The volume API is MEASURED to work on this machine (three beeps audibly rising,
     * 2026-09-15), so these say whether OUR path reaches it -- a different question. */
    kWMedPresses        = 625,       /* rising edges seen                             */
    kWMedRuns           = 626,       /* task-level services performed                 */
    kWMedVolUp          = 627,
    kWMedVolDn          = 628,
    kWMedMute           = 629,
    kWMedEject          = 630,       /* ⚠ COUNTED, NOT ACTED ON -- see bt_inject.c    */
    kWMedGetErr         = 631,       /* 0x100 | GetDefaultOutputVolume                */
    kWMedSetErr         = 632,       /* 0x100 | SetDefaultOutputVolume                */
    kWMedLastVol        = 633,       /* 0x1000000 | the last value written            */
    kWMedMuted          = 634,
    kWMedDropped        = 635,       /* presses arriving with one already queued      */

    /* ---- v11.2: the switcher's self-healing marker, from OUR side. ⚠ BLOCK GROWS -----
     * ⚠⚠ v11.1 SHIPPED WITHOUT THESE AND THE LOG COULD NOT ANSWER THE ONE QUESTION THE
     * FEATURE TURNS ON: did the driver CLEAR the marker? If it did not, the next boot
     * declines and the card goes back to Mac OS -- safe, but silently permanent, and
     * indistinguishable in a log from "the feature was never built". The only way to
     * find out was to reboot and see, which is a diagnostic costing a boot. */
    kWMarkRuns          = 636,       /* BT_ClearSwitchMarker calls that did work       */
    kWMarkCleared       = 637,       /* ⭐ 1 = the marker is gone, next boot will switch */
    kWMarkErr           = 638,       /* 0x100 | the File Manager error, if any          */

    /* ---- v11.3: the HARDWARE volume write. ⚠ BLOCK GROWS ---------------------------
     * Reading 'hvol' has always worked; WRITING it has never been tried, and the
     * asymmetry is the whole bug -- see bt_inject.c. */
    kWMedHwSetErr       = 639,       /* 0x100 | SetSoundOutputInfo(siHardwareVolume)   */
    kWMedHwReadBack     = 640,       /* 0x1000000 | hvol read straight back after      */
    kWMedDevFound       = 641,       /* 1 = an 'sdev' output device was located        */
    /* v11.4: Caps Lock. The toggle is pure and host-tested; the LED is a write BACK
     * to the keyboard and only hardware can confirm it. */
    kWLedSends          = 642,       /* LED reports sent (state changes only)          */
    kWLedRc             = 643,       /* 0x100 | hid_host_send_set_report result        */
    kWCapsOn            = 644,       /* our caps toggle, as the driver believes it     */
    /* v11.6: ⚠ BLOCK GROWS BY ONE. The KEYBOARD's answer to the LED write, which is a
     * different fact from kWLedRc above -- that word is only what the submit call
     * returned, and it returns SUCCESS without a byte having left the machine. All
     * three readers move to 646 in THIS SAME COMMIT; they find the block by matching
     * 'ENDS' at kWEnd, so a reader left at 645 reports NO BLOCK FOUND, which reads as
     * "the driver never loaded" and gets diagnosed as an install fault. That is the
     * mistake the comment at CMakeLists.txt:363 says has bitten SEVEN times. */
    kWLedRsp            = 645,       /* 0x100 | HIDP SET_REPORT handshake from the kbd */
    /* v11.6: ⭐ THE DISCRIMINATOR THE VOLUME QUESTION HAS BEEN MISSING.
     *
     * The driver reads 0xE16A9C back from siHardwareVolume while BTCheck, an APP, reads
     * 01000100 from the same selector on the same machine minutes apart. Four cheaper
     * explanations are now dead, each checked rather than argued:
     *   - wrong selector?      siHardwareVolume IS FOUR_CHAR_CODE('hvol'), Sound.h:344.
     *   - wrong variable type? both sides pass &(long), identically.
     *   - lookup differs?      both are FindNextComponent(0, {'sdev',0,0,0,0}).
     *   - write refused?       no: err 0 = success, on BOTH calls.
     * ⇒ The remaining two possibilities need DIFFERENT fixes, and nothing we have
     * distinguishes them:
     *   (a) the driver got a DIFFERENT Component than the app did -- a lookup problem,
     *       fixable in this driver.
     *   (b) the driver got the SAME Component and the Component Manager cannot dispatch
     *       it usefully from a driver fragment -- not fixable here at all, and the thing
     *       that turns this into a design question about context.
     * Publishing the Component token itself separates them in ONE run: BTCheck prints
     * its own alongside. Same number = (b). Different = (a). One word to stop guessing,
     * and each run costs a reboot. */
    kWMedDevId          = 646,       /* the Component token the DRIVER got, verbatim    */
    /* v11.7: ⚠ BLOCK GROWS BY SIX. Apple's own volume route, and the Caps Lock
     * persistence instrument. All three readers move to 653 in THIS SAME COMMIT. */
    kWMedTrapOk         = 647,       /* 1 = TrapAvailable(0xABEC), Apple's own gate     */
    kWMedTrapCalls      = 648,       /* taps issued through Apple's trap                */
    kWMedTrapRc         = 649,       /* 0x100 | the trap's pascal short result          */
    kWInjMapClobber     = 650,       /* ⭐ keystrokes where low mem had lost our KeyMap  */
    kWInjMapLastSys     = 651,       /* 0x1000000 | system KeyMap byte 7 on entry       */
    kWInjMapLastOur     = 652,       /* 0x1000000 | our shadow's byte 7 on entry        */
    /* v11.9: eject. ⚠ press and release MUST balance at rest -- an unmatched press
     * is a key left down, which for eject means the tray cycling. */
    kWEjectPress        = 653,       /* holds started                                   */
    kWEjectRelease      = 654,       /* holds ended                                     */
    kWEjectWhich        = 655,       /* 0x100 | the eject code actually sent             */
    /* v12.0: the PAIRING SAFETY NET. Capture the controller's existing keys so the
     * A1016's Tiger-made bond can be put back if re-pairing stalls. */
    kWRlkCaptured       = 656,       /* keys copied from the card into our DB           */
    kWLkCaptured        = 657,       /* DB-side count of the same                       */
    kWRestoreTried      = 658,       /* restore-to-controller commands issued           */
    /* v12.3: Delete now drops the row and the link as well as the card's key. */
    kWLkArchived        = 659,       /* live entries demoted to archived                */
    kWDiscReqs          = 660,       /* delete-time disconnects requested               */
    kWDiscRc            = 661,       /* 0x100 | gap_disconnect result                   */
    /* v12.5: ⭐ IS A LINK UP RIGHT NOW, to the peer named by kWConnPeerHi/Lo?
     * Published explicitly rather than having the panel test kWHciConnHandle, whose
     * word number lives in an UNNUMBERED enum run -- counting entries by hand to get
     * a cross-binary constant is exactly the drift the CMake guards exist to stop.
     * The panel enables Disconnect on this and nothing else. */
    kWLinkUp            = 662,
    kWPagedForgot       = 663,       /* pager records dropped by a Delete  */
    /* v12.8: the blocked ("Disconnected") device. The panel MUST be able to show
     * this or a blocked device just looks broken -- see the note in bt_btstack.c. */
    kWBlockHi           = 664,
    kWBlockLo           = 665,
    kWBlockActive       = 666,
    kWBlockDrops        = 667,       /* reconnections refused                       */
    /* v12.9: the LIVE connections, by address. The panel enables Disconnect from
     * these rather than from the single most-recent peer, which is not a list. */
    kWLiveCount         = 668,
    kWLiveA0Hi          = 669, kWLiveA0Lo = 670,
    kWLiveA1Hi          = 671, kWLiveA1Lo = 672,
    /* v13.0: the devices' OWN names. Two slots of {hi, lo, 24 chars as 6 words}.
     * ⚠ 8 words per slot, so kWNameBase + 2*8 - 1 is the last one -- check this
     * arithmetic against kWEnd if a slot is ever added. */
    kWNameReqs          = 673,
    kWNameOks           = 674,
    kWNameCount         = 675,
    kWNameBase          = 676,       /* .. 691 : 2 slots x 8 words              */
    /* v13.1: the regression that broke scanning, and its recovery. */
    kWNameDeferred      = 692,       /* name asks skipped because a scan was live  */
    kWInqRetries        = 693,       /* refused starts we cancelled and retried    */
    kWInqRecovered      = 694,       /* of those, retries that then SUCCEEDED      */

    /* ★★★★★★ v13.3: THE OUTGOING HID CONNECT. One call we have never made, and it is
     * the suspected cause of TWO things this project has been treating as properties of
     * the hardware: the power-cycle step after pairing, and the Connect button being
     * unable to connect. See BT_TryOutgoingHid. These three words are how we find out. */
    kWHidOutGate        = 695,       /* the compile-time gate, so a log names its build */
    kWHidOutTried       = 696,       /* hid_host_connect calls made                   */
    kWHidOutRc          = 697,       /* 0x100 | its return code                       */
    kWHidOutCid         = 698,       /* the hid_cid it handed back                    */
    /* v13.4: the fresh-bond latch, so a log can tell "never armed" (no new pairing
     * happened) from "armed and the call was refused". Without it a 0 in kWHidOutTried
     * has two meanings and neither can be acted on. */
    kWJustBonded        = 699,       /* Link Key Notifications that armed the connect */
    /* v13.7: key-file opens that FAILED. Separates "no file" from "file with 0
     * records", which had produced identical rows for three reboots running. */
    kWKfNoFile          = 700,
    /* ★★★ v13.8: THE HALF-OPEN WATCHDOG, and kWBhIncomingMs is the measurement this
     * project never had. There was no accept-at timestamp, so the healthy accept->open
     * gap was not recoverable from ANY banked log -- which is why the 10 s timeout is a
     * safety factor over the one latency we did measure (271 ms, Auth Complete to open)
     * rather than a number taken from this event. The next run fixes that. */
    kWBhIncomingMs      = 701,       /* hal_time_ms() when we accepted                */
    /* ⚠ v13.9 REUSES 702/703 WITH NEW MEANINGS, which is safe ONLY because BTCheck
     * refuses a driver whose tag is not exactly kExpectedDriverTag -- 13.9 moves that to
     * 13.9's tag, so no older reader can misread these. Do not reuse a word without that
     * gate in place. 13.8's watchdog is gone (it raced L2CAP's identical 10 s RTX
     * timer); what matters now is the FAILED OPEN that L2CAP's own timeout produces. */
    kWBhFailedOpens     = 702,       /* OPENED events carrying a NON-ZERO status       */
    kWBhFailedStatus    = 703,       /* 0x100 | the last of them; 0x69 = L2CAP RTX     */
    kWBhReconnTried     = 704,       /* reconnects attempted after a teardown         */
    kWBhReconnRc        = 705,       /* 0x100 | hid_host_connect rc                   */

    /* ★★★ v13.9: "is a HID channel open RIGHT NOW", maintained by the driver instead of
     * inferred by each reader. Three defects came from inferring it three different ways
     * -- see the note at gBhLive in bt_btstack.c. Every reader uses this word now. */
    kWBhLive            = 706,       /* 1 = a HID channel is open                      */
    /* ★★★ v14.1: THE BATTERY PROBE. Does the A1016 answer a GET_REPORT at all? Tiger
     * reads Feature 71 (percent, 0..100) and Input 48 (state, 0..2) on this keyboard;
     * neither has ever been asked for by this stack. See the long note at the probe in
     * bt_btstack.c, including why "report 48 never arrives unsolicited" does NOT mean
     * unreadable, and why report IDs 68/69 must never be sent.
     * ⚠ The payload is RAW and unsampled on purpose -- the declaration is not the answer. */
    kWBatSends          = 707,       /* get_report calls made                          */
    kWBatResponses      = 708,       /* GET_REPORT_RESPONSE events seen                */
    kWBatPctRc          = 709,       /* 0x100 | send rc, Feature 71                    */
    kWBatPctHs          = 710,       /* 0x100 | handshake; 0x100 = SUCCESSFUL          */
    kWBatPctLen         = 711,       /* payload length as reported                     */
    kWBatPctVal         = 712,       /* first 4 payload bytes, big-endian              */
    kWBatStRc           = 713,       /* 0x100 | send rc, Input 48                      */
    kWBatStHs           = 714,
    kWBatStLen          = 715,
    kWBatStVal          = 716,
    /* ★★★ v14.2: the PUBLISH, which is what a CSM actually consumes. Without these a
     * successful read and a failed write look identical from a log, and the only
     * evidence would be a file on the target's disk that nobody can see from here --
     * the same "readable and invisible" gap that made 13.8's retry unmeasurable. */
    kWBatPublished      = 717,       /* ★ readings ACCEPTED (0..100, addressed)       */
    kWBatFlushes        = 718,       /* task-level flushes that found work            */
    kWBatFlushErr       = 719,       /* File Manager failures writing the file        */

    /* ★★★ THE MOUSE, v14.6, for the A1015. ⚠ THE UNCLAIMED ROWS ARE THE POINT: the
     * A1015's wire format has never been measured, and the keyboard cost a whole session
     * to exactly this shape of unknown -- a documented report in an undocumented framing,
     * every decoder branch refusing it, and a log that said only "reports accepted 0".
     * A length and four bytes here turn the next run into an answer. */
    kWMouseReports      = 720,       /* BT_InjectMouse calls (decoded mouse reports)   */
    kWMouseDecodeOk     = 721,       /* reports the MOUSE decoder accepted             */
    kWMouseMoves        = 722,       /* CursorDeviceMove calls (nonzero delta)         */
    kWMouseBtnChanges   = 723,       /* CursorDeviceButtons calls (mask CHANGED)       */
    kWMouseDropped      = 724,       /* reports dropped: no cursor device yet          */
    kWMouseWheelSeen    = 725,       /* wheel bytes seen -- counted, never delivered   */
    kWCurCreated        = 726,       /* CursorDeviceNewDevice successes (expect 0 or 1)*/
    kWCurNewErr         = 727,       /* CursorDeviceNewDevice failures                 */
    kWUnclaimedReports  = 728,       /* ⚠ reports NEITHER decoder accepted             */
    kWUnclaimedLen      = 729,       /* ⚠ length of the last such report               */
    kWUnclaimedB0       = 730,       /* ⚠ its first FOUR bytes, big-endian             */

    /* ★ the scan filter, v14.6 */
    kWShowAllDevices    = 731,       /* 1 = Preferences marker present, filter OFF     */
    kWInqFiltered       = 732,       /* responders hidden as not drivable              */

    /* ★ the low-battery warning, v14.8. gAlertsDropped being nonzero is a POLICY bug
     * upstream of the Notification Manager, not a Manager failure: it means the
     * threshold latch is posting more often than the user can dismiss. */
    kWBatWarnings       = 733,       /* warnings the policy decided to post            */
    kWAlertsPosted      = 734,       /* NMInstall calls that succeeded                 */
    kWAlertsDropped     = 735,       /* ⚠ posts refused: one already outstanding       */
    kWAlertErr          = 736,       /* 0x100 | last nonzero NMInstall result          */

    /* ★★★ TWO HID DEVICES, v15.0. kWHidDevFull is the one to read on a bad run: a
     * third device tried to connect and was dropped, which is out of scope but must not
     * be invisible. kWSweepConnects vs kWSweepSkipped separates "we asked and it did not
     * answer" from "we never asked because it was already connected". */
    kWHidDevOpens       = 737,       /* slots claimed at CONNECTION_OPENED             */
    kWHidDevCloses      = 738,
    kWHidDevFull        = 739,       /* ⚠ a connection dropped: no free slot           */
    /* ⚠⚠ RETIRED v16.1, NOT RENUMBERED. The reconnection sweep is deleted and these
     * read 0 forever. Reclaiming the indices would renumber every word above them in a
     * table FOUR binaries agree on, and a hand-edited count beside a hand-edited end is
     * exactly what scribbled 52 bytes past a System heap block in 14.6. A hole is free;
     * renumbering is not. */
    kWSweepRuns         = 740,       /* retired v16.1 */
    kWSweepConnects     = 741,       /* retired v16.1 */
    kWSweepSkipped      = 742,       /* retired v16.1 */
    kWSweepRc           = 743,       /* retired v16.1 */
    kWDev0Cid           = 744,       /* slot 0: cid, then addr hi/lo, then role bits   */
    kWDev0Hi            = 745,
    kWDev0Lo            = 746,
    kWDev0Role          = 747,       /* 1 = keyboard, 2 = mouse, 3 = both seen, 0 = ?  */
    kWDev1Cid           = 748,
    kWDev1Hi            = 749,
    kWDev1Lo            = 750,
    kWDev1Role          = 751,

    /* v15.1: keeping the A1015 report-ID reading under measurement */
    kWMouseDxMin        = 752,       /* signed, as decoded; see BTCheck            */
    kWMouseDxMax        = 753,
    kWMouseDyMin        = 754,
    kWMouseDyMax        = 755,
    kWMouseBtnMask      = 756,       /* OR of every button mask decoded            */
    kWMouseRidOther     = 757,       /* ⚠ 5-byte 0xA1 reports NOT carrying id 0x02 */

    kWCurMoveErr        = 758,       /* 0x10000 | OSErr from CursorDeviceMove    */
    kWCurBtnErr         = 759,       /* 0x10000 | OSErr from CursorDeviceButtons */

    /* v15.2: the battery scheduler -- did we ASK the mouse at all? */
    kWBatTargets        = 760,       /* devices the chain has been pointed at       */
    kWBatGiveUps        = 761,       /* probes abandoned because nothing came back  */

    /* v15.3: each device's OWN battery level, so the log can say which is which */
    kWDev0Pct           = 762,
    kWDev0BatHs         = 763,       /* 0x100 | handshake: names a REFUSAL          */
    kWDev1Pct           = 764,
    kWDev1BatHs         = 765,

    /* v15.5: the sniff probe. Tiger sniffs at 20 slots (12.5 ms); ours was never read */
    kWSniffKbd          = 766,       /* slots, 0.625 ms each; 0 = never sniffed     */
    kWSniffMse          = 767,
    kWSniffOther        = 768,       /* a link we could not attribute to a device   */
    kWMouseRatePeak     = 769,       /* most mouse reports in any 250 ms window     */

    /* v15.6: the calibration swipe -- total counts, for true cpi */
    kWMouseAbsDx        = 770,
    kWMouseAbsDy        = 771,

    kWSweepEnabled      = 772,       /* retired v16.1 */

    /* v16.2: the Cursor Device setup calls' verdicts. Appended, NOT dropped into the
     * retired sweep holes at 740..743 -- filling a hole is safe only if every reader is
     * updated in lockstep, and a mismatched BTCheck would print a cursor error code
     * under the label "sweep passes". Words are cheap; that confusion is not. */
    kWCurAccelErr       = 773,
    kWCurButtonsErr     = 774,
    kWCurUpiErr         = 775,       /* ⚠ nonzero = OS 9 refused the resolution */

    kWCurAccelUsed      = 776,       /* v16.4: Fixed acceleration we inherited      */
    kWCurDevPtr         = 777,       /* v16.4: our CursorDevicePtr, to name our row */

    /* ★★★★★ v17.2: WHEN DID THE STALLS HAPPEN, AND WHEN DID THE EXPERT PULL US.
     *
     * The dongle teardown is the last thing between here and a paired keyboard, and
     * two explanations fit every number we have. Both imply 32 stalls and one
     * Finalize, and they need OPPOSITE fixes:
     *
     *   A. THE STALLS CAUSE THE REMOVAL. Only one interrupt read is ever outstanding
     *      (a single gIntPB, re-armed inside its own completion), so between a
     *      completion and the re-arm nothing is posted. Inquiry results arrive fast,
     *      the endpoint halts, stalls accumulate, the Expert gives up.
     *      ⇒ Fix: keep two reads outstanding.
     *
     *   B. THE REMOVAL CAUSES THE STALLS. The device drops for some other reason and
     *      our single-PB re-arm loop spins until Finalize arrives, logging a stall
     *      each time round.
     *      ⇒ Fix: double-buffering changes nothing; look at why it drops.
     *
     * ⇒ THE DISCRIMINATOR IS TIME, and nothing we record has a clock on it. If the
     * stalls fall in a burst in the last moments before Finalize, it is B. If they are
     * spread across the session, it is A. One run answers it either way, which is the
     * whole reason to spend three words rather than ship a guess -- two speculative
     * ROMs on this project were both wrong and the probe settled it in one run. */
    kWStallFirstMs      = 778,       /* hal_time_ms at the FIRST stall clear        */
    kWStallLastMs       = 779,       /* ... and at the most recent one              */
    kWFinalizeMs        = 780,       /* ... and at the Expert's teardown            */

    /* v17.3: how many interrupt reads we keep posted. 1 = the A1044's unchanged flow,
     * 2 = the fix for the unsolicited-event stall window. Recorded so a run SAYS which
     * path it took, rather than leaving it to be inferred from the stall count it was
     * meant to change -- the same reason the Create_Connection patch counts itself. */
    kWIntDepth          = 781,

    /* ★★★★★ v17.4: HOW LONG DO WE SPEND INSIDE THE COMPLETION, AND FOR WHICH EVENTS.
     *
     * ⚠⚠ 17.3's FIX WAS INERT, WHICH FALSIFIES THE MECHANISM I INFERRED, NOT THE
     * CORRELATION I MEASURED. With two reads posted the ratio did not move:
     *     17.2 depth 1   stalls 98 / unsolicited 93 = 1.05
     *     17.3 depth 2   stalls 66 / unsolicited 64 = 1.03
     * and that ratio is far tighter than stalls-per-second (2.77 vs 3.27), so the link
     * to unsolicited events is real. What is dead is "the window between a completion
     * and the re-arm", because depth 2 closes that window and changed nothing.
     *
     * ⇒ I jumped from "one stall per unsolicited event" to "therefore the re-arm gap"
     * without ever testing that second step. The correlation said WHICH events, never
     * WHY. This measures the step I skipped.
     *
     * THE CANDIDATE: BT_DeliverPacket runs BTstack INSIDE the completion handler, at
     * interrupt level. A command reply is a short trip through hci.c. An unsolicited
     * event -- an Inquiry Result -- is a longer one, and can send commands of its own.
     * If that trip is long enough, the posted read(s) time out WHILE WE ARE IN IT, and
     * having two posted is no help because both are waiting on the same handler to
     * return. That fits every number including 17.3's.
     *
     * ⇒ Split the measurement by event type, because the whole claim is that the two
     * differ. If unsolicited ~= command, the candidate is dead and I will have ruled
     * out my own theory rather than shipped it. Microseconds, not ms: a trip of a few
     * hundred microseconds and one of fifty milliseconds must not both read "0". */
    kWDelivMaxCmdUs     = 782,       /* longest BT_DeliverPacket for 0x0E/0x0F      */
    kWDelivMaxUnsolUs   = 783,       /* ... and for everything else                 */
    kWDelivLastUs       = 784,       /* the most recent, whatever kind              */

    /* ★★★★★ v17.5: STOP THEORISING AND RECORD THE COMPLETION STREAM ITSELF.
     *
     * Three mechanisms have now been proposed and two KILLED BY MEASUREMENT:
     *   - the re-arm gap        -> dead: depth 2 closed it and the ratio did not move
     *   - time in the handler   -> dead: 140 us unsolicited vs 150 us command reply
     *   - OS 9's 5 s transfer timeout -> dead by arithmetic on data already held: 66
     *     empty reads in 20.2 s, where a 5 s timeout yields at most 8 with two reads
     *     posted. And the empty-read rate (3.27/s) matches the unsolicited event rate
     *     (3.17/s), so each event produces one IMMEDIATE empty completion.
     *
     * ⇒ Every theory so far has been me inferring a mechanism from a ratio. The ratio
     * has been right every time and the mechanism wrong every time. So record what the
     * completions ACTUALLY ARE, in order, and read the pattern instead of guessing it:
     * is the empty completion interleaved one-for-one after each event? What actCount
     * does the event itself carry? Does the pattern differ for 0x0E replies?
     *
     * One word per completion: status low byte << 24 | actCount. Eight slots is enough
     * to see the repeat, and small enough that two stores at interrupt level stay
     * irrelevant beside the 150 us already measured in the same handler. */
    kWCompRing0         = 785,       /* 8 slots, oldest-first by index below        */
    kWCompRingIdx       = 793,       /* next slot to write; ring is 785..792        */

    kWEnd = 794,                     /* 'ENDS' */

    /* ⚠⚠⚠ DERIVED, NEVER TYPED. THIS WAS A SYSTEM HEAP SCRIBBLER IN 14.6.
     *
     * kWCount is the ALLOCATION SIZE and kWEnd is the index of the terminator word, so
     * the count is structurally kWEnd + 1 -- always, by definition. It was a hand-typed
     * 721 beside a hand-typed kWEnd of 720, and the two agreed only by coincidence. 14.6
     * moved kWEnd to 733 for the mouse counters and left kWCount at 721, so EnsureBlock
     * allocated 721 words and then wrote words 721..733 -- THIRTEEN WORDS, 52 BYTES --
     * PAST THE END of a System heap block, over the next block's header.
     *
     * The symptom was not a crash report. The driver simply never came up: BTCheck said
     * "NO BLOCK FOUND", the switch marker was therefore never cleared, and the NEXT boot
     * saw a stale marker and correctly declined to switch -- so the user's keyboard came
     * up in proxy mode. Three layers of designed-in fallback between the defect and the
     * symptom, and none of them pointed at the allocator.
     *
     * ⚠ check-block-words.py compared kWEnd across the three binaries and passed,
     * because the drift was WITHIN this file. A cross-binary guard cannot see a
     * single-binary invariant; it now checks this one too. */
    kWCount = kWEnd + 1
};
/* ⚠ And asserted at COMPILE time as well as in the script, because this enum is where
 * someone will next add a counter and the failure mode is silent heap corruption. */
BT_ASSERT(bt_block_count_tracks_end, kWCount == kWEnd + 1);
BT_ASSERT(bt_block_end_inside,       kWEnd < kWCount);
BT_ASSERT(bt_hidcap_fits,      kWHidFirstData + 4 - 1 == kWHidFirstDataEnd);
BT_ASSERT(bt_hidcap_below_end, kWHidLastKey0 < kWEnd);
BT_ASSERT(bt_hring_fits,       kWHandlerRing + kHandlerRingLen - 1 == kWHandlerRingEnd);
BT_ASSERT(bt_hring_below_end,  kWHandlerTotal < kWEnd);
/* v8.2. ⚠ Tied to kPagedSlots so growing the ring cannot silently overrun 'ENDS' --
 * the block's stride bugs have all been of exactly this shape. */
BT_ASSERT(bt_paged_fits,       kWPagedA0Hi + kPagedSlots * 3 - 1 == kWPagedEnd);
/* v8.7. ⚠ Tied to BOTH constants so growing either the slot count or the bytes per
 * slot cannot silently overrun 'ENDS' -- every stride bug in this block has been of
 * exactly this shape. 4 bytes per word. */
BT_ASSERT(bt_aclcap_fits,      kWAclCap0 + (kAclCapSlots * kAclCapBytes) / 4 - 1
                               == kWAclCapEnd);
BT_ASSERT(bt_aclcap_below_end, kWRrsfDoneMs < kWEnd);
/* v8.8. ⚠ The LAST word before the terminator, so this assert is the one that fires
 * if a future group is appended without moving kWEnd -- which would write 'ENDS' over
 * a counter and leave every reader scanning for a block that is no longer there. */
BT_ASSERT(bt_autoauth_below_end, kWAutoAuthGate < kWEnd);
BT_ASSERT(bt_publish_below_end,  kWPublishMs < kWEnd);
BT_ASSERT(bt_scanena_below_end,  kWScanEnaLast < kWEnd);
/* v9.1. ⚠ Tied to BOTH constants, like the inbound capture, so growing either the
 * slot count or the bytes per slot cannot silently overrun the length array or
 * 'ENDS'. Every stride bug in this block has been exactly this shape. */
BT_ASSERT(bt_acltx_fits,      kWAclTxCap0 + (kAclCapSlots * kAclTxCapBytes) / 4 - 1
                              == kWAclTxCapEnd);
BT_ASSERT(bt_acltxlen_fits,   kWAclTxLen0 + kAclCapSlots - 1 == kWAclTxLenEnd);
BT_ASSERT(bt_acltx_below_end, kWAclTxLenEnd < kWEnd);
/* v9.2. ⚠ Tied to kAuthRingSlots so growing the ring cannot overrun its own ms
 * array or 'ENDS'. */
BT_ASSERT(bt_authring_fits,   kWAuthRing0 + kAuthRingSlots - 1 == kWAuthRingEnd);
BT_ASSERT(bt_authringms_fits, kWAuthRingMs0 + kAuthRingSlots - 1 == kWAuthRingMsEnd);
BT_ASSERT(bt_authring_below_end, kWAuthRingMsEnd < kWEnd);
BT_ASSERT(bt_suptimeout_below_end, kWSupGate < kWEnd);
BT_ASSERT(bt_policy_below_end,     kWModeLastMs < kWEnd);
BT_ASSERT(bt_bhost_below_end,      kWGapLevel < kWEnd);
BT_ASSERT(bt_transport_below_end, kWPipeInB < kWEnd);
/* v9.9: the raw refs are the new tail, so this is the assert that breaks first when
 * the block next grows without kWEnd moving. */
BT_ASSERT(bt_piperefs_below_end, kWPipeChainErr < kWEnd);
/* v10.0: the role/actCount words are the new tail. This is now the assert that breaks
 * first if the block grows again without kWEnd moving with it. */
BT_ASSERT(bt_role_below_end, kWAclOutReq < kWEnd);
/* M5: the consumer ring is the new tail. ⚠ Its LAST slot, not its first. */
BT_ASSERT(bt_consring_fits, kWHidConsRing + kHidConsumerRing - 1 == kWHidConsRingEnd);
BT_ASSERT(bt_consring_below_end, kWHidConsRingEnd < kWEnd);
BT_ASSERT(bt_keyrpts_below_end, kWHidKeyRpts < kWEnd);
BT_ASSERT(bt_inject_below_end, kWInjPostErr < kWEnd);
BT_ASSERT(bt_media_below_end, kWMedDropped < kWEnd);
BT_ASSERT(bt_mark_below_end, kWMarkErr < kWEnd);
BT_ASSERT(bt_hwvol_below_end, kWMedDevFound < kWEnd);
BT_ASSERT(bt_caps_below_end, kWCapsOn < kWEnd);
BT_ASSERT(bt_ledrsp_below_end, kWLedRsp < kWEnd);
BT_ASSERT(bt_devid_below_end,  kWMedDevId < kWEnd);
BT_ASSERT(bt_trap_below_end,   kWMedTrapRc < kWEnd);
BT_ASSERT(bt_map_below_end,    kWInjMapLastOur < kWEnd);
BT_ASSERT(bt_eject_below_end,  kWEjectWhich < kWEnd);
BT_ASSERT(bt_capture_below_end, kWRestoreTried < kWEnd);
BT_ASSERT(bt_disc_below_end,    kWDiscRc < kWEnd);
BT_ASSERT(bt_linkup_below_end,  kWLinkUp < kWEnd);
BT_ASSERT(bt_forgot_below_end,  kWPagedForgot < kWEnd);
BT_ASSERT(bt_block_below_end,   kWBlockDrops < kWEnd);
BT_ASSERT(bt_live_below_end,    kWLiveA1Lo < kWEnd);
BT_ASSERT(bt_name_below_end,    kWNameBase + 2 * 8 - 1 < kWEnd);
BT_ASSERT(bt_inqrec_below_end,  kWInqRecovered < kWEnd);
BT_ASSERT(bt_paged_below_end,  kWPagedEnd < kWEnd);
BT_ASSERT(bt_ccap_fits,        kWCreateConn + 4 - 1 == kWCreateConnEnd);
BT_ASSERT(bt_ccap_below_end,   kWCreateConnLen < kWEnd);
BT_ASSERT(bt_bcr_fits,         kWBadCmdRing + kBadCmdRingLen - 1 == kWBadCmdRingEnd);
BT_ASSERT(bt_evtring_fits,      kWEvtRing + kEvtRingLen - 1 == kWEvtRingEnd);
BT_ASSERT(bt_evtring_below_end, kWRrsfHandle < kWEnd);
BT_ASSERT(bt_ring_fits,     kWCmdRing + 8 - 1 == kWCmdRingEnd);
BT_ASSERT(bt_ring_below_end, kWCmdRingIdx < kWEnd);
BT_ASSERT(bt_hid_below_end, kWHidPeerLo < kWEnd);
BT_ASSERT(bt_keys_fit,        kWKeyBase + 8 * 2 - 1 == kWKeyEnd);
BT_ASSERT(bt_keys_below_end,  kWKeyEnd < kWEnd);
BT_ASSERT(bt_keys_after_scan, kWScanEnd < kWKeyCount);

/* Commands the control panel may leave in kWCmd. ⚠ Keep these stable: the panel is a
 * separately built binary, so a renumbering silently changes what a button does. */
enum {
    kCmdNone         = 0,
    kCmdStartInquiry = 1,
    kCmdSetRadio     = 2,     /* Arg0 = 1 on, 0 off                        */
    kCmdDeleteBond   = 3,     /* Arg0 = addr hi 3 bytes, Arg1 = addr lo    */
    /* ★★★★ M3.7. Arg0 = addr hi 3 bytes, Arg1 = addr lo -- the SAME encoding as
     * kCmdDeleteBond, deliberately, so the panel has one address convention and
     * not two. ⚠ Keep the NUMBER stable: the panel is a separately built binary
     * and a renumbering silently changes what a button does. */
    kCmdPairDevice   = 4,
    /* ★★★★ Delete a key from the CONTROLLER's own store, same address encoding as
     * the other two. ⚠ Keep the NUMBER stable -- the panel is built separately. */
    kCmdDelStoredKey = 5,
    /* ★★★★ v8.5: HAND THE CARD BACK TO HID-PROXY MODE, the last ship-blocker.
     *
     * Pairing today needs extension surgery: install the switcher, reboot, pair,
     * remove the switcher, reboot. Two reboots and moving files by hand, because the
     * switcher switches unconditionally at every Initialize and nothing ever switches
     * back. But the card returns to proxy on a power cycle by itself, so the mode is
     * VOLATILE -- there is no persistent state to undo, only a request to send.
     *
     * ⭐ And the request is documented prior art, not a guess: hid2hci.c's
     * usb_switch_csr() is bmRequestType 0x40 / bRequest 0 / wValue = mode, with
     * enum mode { HCI = 0, HID = 1 } (docs/M0-MODE-SWITCH.md §2). Our forward switch
     * is that exact request with wValue 0. This is the same one with wValue 1.
     *
     * ⚠ The doc's caveat -- "it can only be sent from a host that can see the card,
     * which today means Tiger" -- was written when nothing could bind 8204. The
     * two-extension architecture fixed that: in HCI mode THIS driver owns the device,
     * so this driver is the host that can see it.
     *
     * ⚠ Keep the NUMBER stable; the panel is a separately built binary. */
    kCmdSwitchToProxy = 6,
    /* ★★★★★★ v12.0: PUT A SAVED KEY BACK INTO THE CONTROLLER. Arg0/Arg1 encode the
     * address exactly as kCmdDeleteBond and kCmdDelStoredKey do.
     *
     * This is the undo for kCmdDelStoredKey, and it is the reason deleting the A1016's
     * Tiger-made bond is a reversible experiment rather than a one-way door. The key
     * comes from OUR database, which v12.0 seeds from the controller's own store at
     * startup -- so the thing being restored is the exact key that was there before.
     *
     * ⚠ It can only restore what was captured. If the database has no entry for the
     * address, this reports failure rather than writing anything, because writing a
     * WRONG key would be worse than writing none: the peer would be bonded to a secret
     * neither side agrees on, and the failure would look like a radio fault.
     *
     * ⚠ Keep the NUMBER stable; the panel is a separately built binary. */
    kCmdRestoreKey   = 7,
    /* ★★★★★★ v12.5: DISCONNECT, the button that has been on screen and DEAD.
     *
     * The control exists, is created, is HiliteControl'd to 255 in two places, and has
     * no hit handler at all. The model it was drawn for is the user's and is Tiger's:
     *   Disconnect -- end the link, KEEP the device visible as Available;
     *   Delete     -- do that AND remove it from the list entirely.
     * Everything this driver did until now conflated the two, which is why a delete
     * that worked perfectly still looked broken: the link stayed up and the row stayed
     * put. ⚠ Keep the NUMBER stable; the panel is built separately. */
    kCmdDisconnect   = 8,
    /* ★★★★ v12.8: release the block set by Disconnect. ⚠ Keep the NUMBER stable. */
    kCmdAllowDevice  = 9
};

/* Designated initialisers, so the magic and terminator are in the FILE as well
 * as in memory. They have to be: the file's bytes are what make the block
 * findable at all, and it is also why BTCheck treats an all-zero-counters hit as
 * a disk cache copy of the extension rather than the live driver. */
/* ★★★★★ THE BLOCK MUST OUTLIVE THE DRIVER. This was driver globals until v3.0, and
 * that made it structurally unreadable in exactly the case it existed for.
 *
 * Five consecutive runs -- 33, 35, 39, 40, 41, every one of them a mode-switch run --
 * reported NO BLOCK FOUND. The switch changes the card's identity, rule 1's device
 * disappears, the Expert unloads the driver, and the globals go with it. kWSwTried,
 * kWSwVerdict, kWSwStalls and kWSwClearRc have NEVER ONCE BEEN READ, and neither has
 * a single line of BTCheck's verdict decoding. Every conclusion about the switch since
 * run 33 came from the bus dump alone, which is how three wrong theories got offered
 * into a data vacuum.
 *
 * NewPtrSys puts it in the System heap, and it is DELIBERATELY NEVER FREED. A leak of
 * 768 bytes per boot is the correct trade for a diagnostic that survives the event it
 * is measuring. ProbeFinalize must not free it either.
 *
 * ⚠ ALLOCATION IS TASK LEVEL ONLY. NewPtrSys is on the audit's forbidden list, so
 * EnsureBlock is called from the Expert's task-level entry points and NEVER from
 * Bump/Note -- those run at interrupt level and simply do nothing while gCB is null. */
static unsigned long *gCB = NULL;

static void EnsureBlock(void)
{
    short i;

    if (gCB != NULL) return;

    /* ★★ FIND AN EXISTING BLOCK BEFORE MAKING ONE. This is what makes the
     * one-instance-per-device interlock possible at all.
     *
     * ⚠ Each fragment copy has its OWN globals -- see
     * [[reference_os9_two_fragment_copies_own_globals]] -- so a static guard in one
     * instance is invisible to the others. That is exactly why run 47 produced FOUR
     * blocks and four independent BTstacks: every instance allocated its own. Sharing
     * one block gives them somewhere to see each other, and as a bonus BTCheck reports
     * ONE block instead of four, so a counter finally belongs to a known instance.
     *
     * The tag is matched as well as the magic and terminator: a block laid out by a
     * different driver version would be the wrong shape, and blocks only die at
     * restart, so a stale one could otherwise be adopted with a mismatched layout. */
    {
        THz zone = SystemZone();
        unsigned long lo = (unsigned long)zone->heapData;
        unsigned long hi = (unsigned long)zone->bkLim;
        unsigned long *p, *lim = (unsigned long *)(hi - (kWCount * 4 + 16));

        for (p = (unsigned long *)((lo + 3) & ~3UL); p < lim; p++) {
            if (p[kWMagic] == 0x42545031UL &&
                p[kWBuild] == 0x76483630UL &&      /* 'vH60' -- THIS build only */
                p[kWEnd]   == 0x454E4453UL) {
                gCB = p;                           /* adopt it; do NOT re-zero */
                return;
            }
        }
    }

    gCB = (unsigned long *)NewPtrSys((Size)kWCount * sizeof(unsigned long));
    if (gCB == NULL) return;            /* counters silently absent; nothing else breaks */

    for (i = 0; i < kWCount; i++) gCB[i] = 0;
    gCB[kWMagic] = 0x42545031UL;   /* 'BTP1' */
    gCB[kWBuild] = 0x76483630UL;   /* 'vH60'. ⚠ MUST match the tag EnsureBlock scans
                                    * for above, or instances will never adopt each
                                    * other's block and the interlock silently fails.
                                    * BTCheck matches ONLY the leading 'v'
                                    * byte, so any tag works -- but it did once
                                    * demand 'v0' and broke on the 1.0 bump, so if
                                    * this format ever changes, change BTCheck in
                                    * the SAME commit. */
    gCB[kWEnd]   = 0x454E4453UL;   /* 'ENDS' */
}

/* The HCI layer's second destination for every value it parses, because BT_Log
 * goes to the Expert log and the Expert log carries nothing we post. A plain
 * bounded store: safe from the USL completion context this runs in. */
void BT_Record(UInt32 which, UInt32 value)
{
    if (which < (UInt32)kRecCount) gCB[kWHciBase + which] = value;
}

BT_ASSERT(bt_hci_slots_fit,  kWHciBase + kRecCount <= kWAclArmed);
BT_ASSERT(bt_acl_slots_fit,  kWImmErrCode < kWStkState);
BT_ASSERT(bt_stk_slots_fit,  kWStkTxDone < kWEnd);
/* ⚠ M6: the responder table is written by index arithmetic, so a layout mistake would
 * scribble over the terminator and make the whole block unfindable rather than merely
 * wrong. Sixteen entries of four words from kWScanBase must end before kWEnd. */
BT_ASSERT(bt_scan_table_fits,   kWScanBase + 16 * 4 - 1 == kWScanEnd);
BT_ASSERT(bt_scan_below_end,    kWScanEnd < kWEnd);
BT_ASSERT(bt_cmd_below_scan,    kWScanCount < kWScanBase);

/* ⚠ These run at INTERRUPT level. They must never allocate -- see EnsureBlock. A null
 * gCB means the block could not be created, and dropping a counter is the correct
 * response to that; faulting at interrupt level is not. */
static void Bump(short w)                  { if (gCB) gCB[w]++; }
static void Note(short w, unsigned long v) { if (gCB) gCB[w] = v; }

/* ★★★ LIKE Note(), BUT A FRESH INSTANCE'S ZERO MUST NOT ERASE THE EVIDENCE.
 *
 * ⚠⚠ WHY THIS EXISTS -- the 2026-10-04 dongle scan, which read as "the scan found
 * nothing" when the truth was "nobody can tell what the scan found".
 *
 * The counter block is SHARED and EnsureBlock deliberately adopts an existing one
 * without re-zeroing, so counters Bump()ed straight into it survive a teardown. The
 * mirrored words do not: they are republished every 100 ms from INSTANCE GLOBALS, and
 * a second fragment copy has its own globals, all zero. See
 * reference_os9_two_fragment_copies_own_globals.
 *
 * So when the Expert unloaded and reloaded the driver mid-session, the surviving block
 * still said "mailbox serviced command 1, result success" -- written straight into the
 * block -- while gap_inquiry_start's return and the responder count, mirrored from
 * globals, had been overwritten with the new instance's zeros. The log therefore read
 * "gap_inquiry_start rc NOT RECEIVED" and "inquiry responses 0" for an inquiry that had
 * demonstrably STARTED. Two rows of the same log flatly contradicting each other, and
 * the one that looked like an answer was the wrong one.
 *
 * ⇒ For forensic words, keep the last non-zero value. "Never happened" and "happened in
 * the instance before this one" then stop being the same reading.
 *
 * ⚠⚠ ONLY FOR WORDS NOTHING ACTS ON. Deliberately NOT used for:
 *   - kWInqState / kWScanState: the panel greys Set Up New Device on these, so a
 *     preserved 1 would latch the button off forever -- the exact latching bug
 *     BT_ScanStart's own comment records from run 58.
 *   - kWScanCount / kWScanBase: the panel builds the pairing list from these. After a
 *     teardown there genuinely is no scan result, and preserving the table would offer
 *     the user devices from a dead instance that may no longer be in range.
 * Those must keep clearing to zero, because clearing is the CORRECT answer for them. */
static void NoteKeep(short w, unsigned long v)
{
    if (!gCB) return;
    if (v == 0 && gCB[w] != 0) return;   /* keep what the previous instance proved */
    gCB[w] = v;
}

/* config state-machine steps (index held in gCfgPB.usbRefcon) */
enum {
    kFindIface = 0, kSetConfig, kNewIfaceRef, kSetIface,
    /* ⚠ v9.9 SWAPPED THE TWO BULK STATES. The order is now interrupt-IN, bulk-IN,
     * bulk-OUT, and the names track what each state ISSUES. See kWRefBulkOut. */
    kConfigIface, kFindIntPipe, kFindBulkIn, kFindBulkOut, kCfgDone
};

static void ConfigStep    (USBPB *pb);
static void ConfigStepAny (USBPB *pb);
/* M0.0b: declared here, above ConfigStepAny's definition, because the interface walk
 * calls the finisher from three different terminator paths. */
static void FinishIfaceScan(void);
static void ArmAclRead    (void);
static void AclInCompletion  (USBPB *pb);
static void AclOutCompletion (USBPB *pb);
static void ConfigDone    (void);
static void CmdCompletion (USBPB *pb);
static void IntCompletion (USBPB *pb);
/* Pipe stall recovery -- forward-declared because AclInCompletion sits above the
 * definitions and both readers need the same contract. See run 23. */
static Boolean isStallClass(OSStatus err);
static void    ClearStall(USBPipeRef pipe, short whichCounter);

/* ⚠ DEFINED HERE, not beside ClearStall, because both readers consult it and a
 * file-scope static cannot be forward-declared. Set by ProbeNotify on device removal
 * and cleared on every new bind; see the long note at ClearStall. */
static Boolean gDeviceGone = false;

/* ⚠ How many ACL links the controller currently reports. The ACL reader is only
 * re-armed while this is nonzero -- see the long note in AclInCompletion. Maintained
 * from HCI connection/disconnection complete, so it is the controller's view rather
 * than a guess of ours. */
static short gAclLinks = 0;

/* kUSBPending means the call was queued and will complete asynchronously. */
static Boolean immediateError(OSStatus err)
{
    return (err != kUSBPending) && (err != noErr);
}

/* Records the errno on an immediate failure and says whether to bail. v1.0 took
 * an immediate error in the config chain and all we knew was that it happened;
 * the status is what names the cause.
 *
 * ⚠ Deliberately a FUNCTION, not the macro this started as. scripts/level-audit.py
 * builds its call graph by scanning for calls, and a macro's body is invisible to
 * it: the audit reported TRY as an unclassified external and the calls inside it
 * would have been silently dropped from the graph. An audit that cannot see a
 * primitive is worse than no audit, because it is believed. */
static Boolean Failed(OSStatus err)
{
    if (!immediateError(err)) return false;
    Note(kWImmErrCode, (unsigned long)err);
    return true;
}

static void InitPB(USBPB *pb, USBDeviceRef ref, USBCompletion done)
{
    pb->pbVersion     = kUSBCurrentPBVersion;
    pb->pbLength      = sizeof(*pb);
    pb->usbReference  = ref;
    pb->usbCompletion = done;
    pb->usbStatus     = noErr;
}

/* ======================================================================= */
/*  Hooks the HCI layer (hci.c) calls into.                                 */
/* ======================================================================= */

/* Send an HCI command packet (wire-formatted by hci.c) on the control pipe.
 *
 * ⚠ SWAPPED TO THE BLUETOOTH-SPEC VALUES IN v0.8, on evidence. v0.7 used the 2003
 * prior art's values, kUSBOther as the recipient with BRequest = 0xE0, and the
 * dongle answered HCI_Reset (opcode 0x0C03) with usbStatus -6912,
 * kUSBEndpointStallErr, "Device didn't understand" -- a control-endpoint STALL,
 * which is exactly how a device rejects a request it does not recognise.
 *
 * The Bluetooth USB transport spec says an HCI command is
 *     bmRequestType = 0x20   (host-to-device, class, RECIPIENT = DEVICE)
 *     bRequest      = 0x00
 *     wValue = wIndex = 0, HCI command packet as the data stage
 * which is what this now sends. The handoff pre-committed this as the single
 * fallback for a stall at this exact point, so the run cost nothing to interpret.
 *
 * Keeping the prior art's values on the record rather than deleting them: they
 * were kUSBOut/kUSBClass/kUSBOther with BRequest = 0xE0. If a future controller
 * stalls on the spec values instead, that pair is the thing to try. */
OSStatus BT_SendHCICommand(const void *pkt, UInt32 len)
{
    InitPB(&gCmdPB, gDevice, CmdCompletion);
    gCmdPB.usb.cntl.BMRequestType = USBMakeBMRequestType(kUSBOut, kUSBClass, kUSBDevice);
    gCmdPB.usb.cntl.BRequest      = 0;          /* Bluetooth spec; prior art used 0xE0 */
    gCmdPB.usb.cntl.WValue        = 0;
    gCmdPB.usb.cntl.WIndex        = 0;
    gCmdPB.usbBuffer              = (void *)pkt;
    gCmdPB.usbReqCount            = len;
    gCmdBusy = 1;
    Bump(kWCmdSent);
    if (len >= 2) {
        unsigned long op = (unsigned long)(((const UInt8 *)pkt)[0] |
                           (((const UInt8 *)pkt)[1] << 8));
        Note(kWLastOpcode, op);
        /* ★ THE SEQUENCE, not just the last one. With no debugger the ORDER of
         * commands is the only way to see where a handshake stops, and "what was the
         * last opcode" has twice been the wrong question -- the last opcode in the
         * v6.5 run was Read_Remote_Supported_Features, which says nothing about
         * whether Authentication_Requested was ever attempted before it.
         *
         * ⚠ Writes straight into the block. gCB is non-NULL here: BT_SendHCICommand
         * only runs once the stack is up, which is long after EnsureBlock, and Note
         * itself is the block's guarded accessor.
         *
         * ⚠ The index is a read-modify-write and is NOT interrupt-safe. Sends happen
         * mostly at secondary interrupt level, which is serialized, but bring-up runs
         * hci_power_control from task level, so a torn update is possible in principle.
         * Deliberately left unlocked: the worst case is one overwritten ring slot in
         * purely diagnostic data, and taking a lock at interrupt level to protect a
         * debug counter would be the more dangerous choice. */
        if (gCB != NULL) {
            unsigned long i = gCB[kWCmdRingIdx] & 7UL;
            gCB[kWCmdRing + i] = op;
            gCB[kWCmdRingIdx]  = (i + 1UL) & 7UL;
            /* ★ WHEN the features read went out, so its outstanding time is readable
             * against the link lifetime. If the read was issued at t+50 ms and the link
             * died at t+20000, the peer had twenty seconds to answer and did not --
             * a very different finding from a read issued after the link was already
             * gone. FIRST send only; a retry would overwrite the interesting one. */
            if (op == 0x041BUL && gRrsfSentMs == 0) gRrsfSentMs = hal_time_ms();
        }
        /* ★★★★ AND THE FULL PARAMETER BYTES OF Create_Connection (0x0405).
         *
         * The first initiator attempt got ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS
         * (0x12) back from this exact command, and "invalid parameters" is useless
         * without knowing WHICH parameter. The opcode ring holds opcodes only, so the
         * bytes we actually sent were unrecoverable.
         *
         * Create_Connection's parameters are BD_ADDR(6), Packet_Type(2),
         * Page_Scan_Repetition_Mode(1), Reserved(1), Clock_Offset(2),
         * Allow_Role_Switch(1) -- 13 bytes, captured verbatim from offset 3 (past
         * opcode and length). Packet_Type is the leading suspect, because BTstack
         * derives it from the remote's supported features and this build FABRICATES
         * those as all-zero; a mask that comes out empty is exactly what a controller
         * would reject. ⚠ That is a hypothesis and these bytes are what settles it --
         * do not act on it before reading them.
         *
         * ⚠ FIRST ONE WINS. A retry with different parameters would overwrite the
         * bytes that caused the rejection, which is the interesting set. */
        /* ★★★★ REWRITE Create_Connection's PACKET TYPE TO APPLE'S VALUE.
         *
         * v7.9 measured what this card actually is: HCI version 2, LMP version 2,
         * manufacturer 0x000A -- a Cambridge Silicon Radio part at BLUETOOTH 1.2. No
         * SSP, no BLE, and NO EDR (EDR needs LMP 3).
         *
         * And its supported-commands octet 0 reads 0xFF, so it DOES claim
         * Create_Connection. The command is implemented; the parameters are being
         * refused.
         *
         * ⭐ BTstack's hci_usable_acl_packet_types ends in `^ 0x3306`, which
         * unconditionally SETS the 2-DH1/3-DH1/2-DH3/3-DH3/2-DH5/3-DH5 "shall not be
         * used" bits. Those bits are EDR, introduced with Bluetooth 2.0 -- on a 1.2
         * controller those positions are not EDR flags at all. Setting them is what
         * "Invalid HCI Command Parameters" is for, and 0x12 is what we get, with
         * BTstack's 0xFF1E and with the 0x330E that hci_enable_acl_packet_types
         * produced.
         *
         * ⚠⚠ AND THE FILTER CANNOT FIX IT. That XOR happens AFTER the enabled-types
         * mask is applied, so no value passed to hci_enable_acl_packet_types can clear
         * those bits -- they would have to be already set in (usable & enabled), and
         * `usable` is derived from local features which on a 1.2 part do not include
         * EDR. BTstack structurally cannot emit a 1.2-legal mask through its public
         * API. Patching vendored BTstack was rejected earlier and stays rejected; this
         * rewrites OUR outgoing bytes instead, where it is visible.
         *
         * ⚠ THE VALUE IS APPLE'S LITERAL 0x0800, not a mask I derived. Apple's own OS 9
         * stack sends exactly that to this class of card
         * (vendor/bt-control-center/USB Bluetooth/HCI/HCI_Events.c:59). Using their
         * measured byte rather than my reading of which 1.2 bits are reserved means
         * this does not rest on spec recall -- which is the thing that has been wrong
         * six times running on this bug.
         *
         * ⚠ Little-endian on the wire: 0x0800 is bytes 00 08 at offsets 9,10 (past the
         * 3-byte header and the 6-byte address). The capture below records what
         * actually leaves, so the rewrite is checkable rather than assumed. */
        /* ⚠⚠⚠ AND IT IS A 1.2 WORKAROUND, SO IT MUST ONLY RUN ON A 1.2 CONTROLLER.
         *
         * Read the justification above and notice that EVERY clause of it says so:
         * "on a 1.2 controller those positions are not EDR flags at all", "`usable` is
         * derived from local features which on a 1.2 part do not include EDR",
         * "BTstack structurally cannot emit a 1.2-legal mask". All true of the A1044,
         * which reports LMP version 2. None of it is true of the dongle, which reports
         * LMP version 12 -- Bluetooth 5.3, with real EDR and real local features.
         *
         * ⇒ On a modern controller BTstack's 0x330E is already the CORRECT mask: DM1
         * permitted, every EDR type forbidden. The patch replaces it with 0x0800, which
         * CLEARS the mandatory DM1 bit (0x0008) and leaves every EDR "shall not be
         * used" bit clear -- i.e. it forbids the one packet type that must always be
         * available and re-permits EDR to a Bluetooth 1.1 keyboard that has none.
         *
         * ⚠ THIS IS THE THIRD TIME IN ONE SESSION, and the pattern is worth naming
         * rather than fixing a third time in silence: "never page the device", the
         * 5.12 s page timeout and this packet-type rewrite were all derived for the
         * A1044 and all applied unconditionally to a part two decades newer. Each was
         * correct where it was measured. A workaround carries the controller it was
         * measured on, and it has to be asked for ID.
         *
         * ⚠ The A1044 keeps today's bytes exactly: LMP 2 fails this gate, so the patch
         * still runs there and the Create_Connection capture below will show 0x0800 on
         * that machine as it always has. */
        if (op == 0x0405 && len >= 16 && ((gLocalVer >> 8) & 0xFFUL) < 4) {
            UInt8 *pt = (UInt8 *)pkt;
            pt[9]  = 0x00;
            pt[10] = 0x08;
            if (gCB != NULL) Bump(kWCreateConnPatched);
        }

        if (gCB != NULL && op == 0x0405 && len >= 16
            && gCB[kWCreateConnLen] == 0) {
            unsigned long w, k;
            for (w = 0; w < 4; w++) {
                unsigned long v = 0;
                for (k = 0; k < 4; k++) {
                    unsigned long idx = 3 + w * 4 + k;
                    v = (v << 8) | (unsigned long)
                        (idx < len ? ((const UInt8 *)pkt)[idx] : 0);
                }
                gCB[kWCreateConn + w] = v;
            }
            gCB[kWCreateConnLen] = len;
        }
    }
    /* ⚠⚠ NEVER WEDGE THE COMMAND PATH SHUT. gCmdBusy is cleared by CmdCompletion,
     * and a synchronous refusal from USBDeviceRequest means CmdCompletion will NEVER
     * RUN -- so without this unwind the flag latches at 1 and BT_CanSendCommand
     * answers "no" for the rest of the session. Every HCI command after it is refused
     * and the stack goes permanently mute.
     *
     * This is [[feedback_guards_must_not_latch]] a third time, and the correct shape
     * was already sitting twenty lines below in BT_SendACL, which has always done
     * exactly this. Found while tracing the v6.3 run.
     *
     * ⚠ It is NOT what happened in that run -- kWCmdSent 19 equalled kWCmdComp 19,
     * so all nineteen completed and gCmdBusy was 0. Fixed because it is wrong, not
     * because it is the current suspect; do not let this crowd out the real cause. */
    {
        OSStatus err = USBDeviceRequest(&gCmdPB);
        if (immediateError(err)) gCmdBusy = 0;
        return err;
    }
}

/* Every status message goes through here, so there is exactly one place to
 * change when the Expert's logging API is the thing under suspicion.
 *
 * WHY NOT plain USBExpertStatus: on 2026-08-30 the driver demonstrably loaded and
 * ran (the Expert logged "calling driver initialize routine" then "driver
 * initialization completed 0") and NOT ONE of our USBExpertStatus messages
 * reached the log, with Prober's Status Level already at its maximum of 5. So
 * filtering is ruled out and the un-levelled 1.0-era call is the suspect.
 * The 2003 prior art wraps exactly this decision: below USB 1.2 it calls
 * USBExpertStatus, at 1.2 and above USBExpertStatusLevel, and it posts at level
 * 4 or 5. This machine reports USB 1.5.9, so the prior art would take the
 * levelled branch and we never did.
 *
 * ⚠ USBExpertStatusLevel is USBServicesLib 1.2 and later. That is every OS 9.2.2
 * machine, but it does narrow the import set: on a pre-1.2 system the fragment
 * would fail to load rather than merely log nothing. Acceptable for a probe. */
static void Say(USBDeviceRef ref, const void *pstr, UInt32 value)
{
    USBExpertStatusLevel(kBTStatusLevel, ref, (StringPtr)pstr, value);
}

/* Copy the BTstack glue's counters into the block. Called after every delivery, so
 * BTCheck sees live values without BTCheck or the block needing to know anything
 * about BTstack's own symbols. */
/* ========================= M6b: THE CONTROL PANEL CALLS IN =====================
 *
 * ⚠⚠ THIS REPLACES A COMMAND CHANNEL THAT COULD NOT WORK, and the reason it could
 * not work is worth keeping.
 *
 * docs/SCAN-DESIGN.md §5 asserted that an application and an 'ndrv' "cannot call each
 * other", so the panel left commands in the shared block and BT_StackPoll picked them
 * up. But BT_StackPoll runs ONLY on a USB completion, and after bring-up the
 * interrupt-IN read settles into a proper blocking wait -- so on an idle radio nothing
 * ever calls it. Run 47 measured the consequence exactly: seq 9, ack 2. The user
 * pressed the button seven times and the driver never noticed; the published inquiry
 * data was byte-identical to the previous run because no new inquiry ever ran.
 *
 * ★ The premise was simply false, and the refutation was in the prior art already in
 * our own vendor/ tree. BTCC/BluetoothInterface.c gets the driver's CFM connection
 * from USBGetNextDeviceByClass, resolves "ManagerSendCommand" out of the PEF container
 * with FindSymbol, and CALLS IT. An app and an 'ndrv' call each other routinely.
 *
 * ⚠⚠ AND THE RACE THAT CREATES, which is the whole reason for the two-function shape
 * below. The panel's call arrives at TASK level. USB completions arrive at SECONDARY
 * INTERRUPT level and can preempt it, so touching BTstack directly from the exported
 * function would let two contexts walk BTstack's lists at once. A hand-rolled flag
 * cannot fix that: task level cannot safely spin against an interrupt, and dropping a
 * completion to win a lock would lose an HCI packet.
 *
 * ★★ CallSecondaryInterruptHandler2 closes it with an OS guarantee instead of a
 * guess. It runs the handler AT secondary interrupt level -- the same level as the USB
 * completions -- and the OS serializes secondary interrupt handlers against one
 * another. The two contexts become mutually exclusive by construction: no lock, no
 * atomic, no window, and no timer. The call blocks until the handler returns, so the
 * panel gets a real result rather than a promise. */

static OSStatus bt_scan_start_sih(void *p1, void *p2)
{
    /* (void) rather than #pragma unused: the audit strips comments but not pragmas,
     * so "#pragma unused (p1, p2)" reads as a call to a function named "unused" and
     * puts a phantom edge in the call graph. Harmless here, but phantom edges are how
     * an audit's output stops being trustworthy. */
    (void)p1; (void)p2;

    /* ⚠ ONE FRAGMENT COPY PER INSTANCE, AND ONLY ONE OF THEM IS BOUND.
     *
     * An INIT-installed driver exists as more than one fragment copy with SEPARATE
     * globals; the shared counter block is adopted by whichever instance runs, but
     * BTstack's state is per-copy. FindSymbol in the panel resolves into one specific
     * copy, and if that copy never bound a device its BTstack was never started --
     * calling into it would run the stack uninitialised. gDevice is this copy's own
     * marker, so an unbound copy refuses and the panel moves to the next connection. */
    if (gDevice == kNoDeviceRef) return kUSBDeviceBusy;
    if (gCB == NULL)             return kUSBDeviceBusy;

    /* ⚠⚠ TRANSLATE. DO NOT LEAK THE COUNTER ENCODING ACROSS THIS BOUNDARY.
     *
     * BT_ScanStart returns 0x100 | gap_inquiry_start(), because every *Rc word in the
     * counter block is stored that way so a zero means "never received" rather than
     * "succeeded" -- see bt_pump.h. That convention is right for the block and WRONG
     * for a function an application calls: v4.2 returned it verbatim, so a completely
     * successful start came back as 256, the panel compared it against noErr, decided
     * the call had failed, showed no chasing arrows and left the list alone. The
     * inquiry had actually run. The button looked dead while working perfectly.
     *
     * ★ BTstack's ERROR_CODE_SUCCESS is 0, so the low byte is the real verdict. */
    {
        unsigned long r = BT_ScanStart();
        unsigned long e = r & 0xFFUL;
        if (e == 0) return noErr;
        /* ★★ 0x0C COMMAND DISALLOWED IS NOT A FAILURE, IT IS A RACE.
         *
         * gap_inquiry_start refuses while an inquiry is already active, and ours run
         * for 8 x 1.28s -- about ten seconds, including the one bring-up performs. A
         * click inside that window returned -1012 and the panel said "start failed",
         * which is both alarming and wrong: a scan was running, which is exactly what
         * the user asked for. Given its own code the panel can say so and show the
         * arrows instead of reporting a fault. */
        if (e == 12) return (OSStatus)kBTAlreadyScanning;
        return (OSStatus)(-1000L - (long)e);
    }
}

/* EXPORTED (src/bt_probe.exp). Called by the control panel at task level.
 *
 * ⚠⚠ NO LONGER THE NORMAL PATH. Kept ONLY as the bisect probe, reached by a modifier
 * click, because it is the thing proven to cause the churn: run of 2026-09-03 showed
 * 3 teardowns across 11 presses of this, against 0 across 5 presses of an identical
 * hop with an empty handler and 0 across 119 s idle. Normal operation now goes through
 * the mailbox (see BT_ServiceMailbox), which the driver's own timer services.
 *
 * ★ It stays reachable on purpose rather than being deleted outright: if the timer
 * turns out not to service the mailbox on real hardware, this is how the user can
 * still start a scan and how we would find out why. It is NOT a fallback and nothing
 * calls it automatically -- two automatic ways to start an inquiry is exactly the
 * trap the retired command path warned about, where the one that silently does
 * nothing is the one blamed last. */
OSStatus BTScanStart(void);
OSStatus BTScanStart(void)
{
    return CallSecondaryInterruptHandler2(bt_scan_start_sih, NULL, NULL, NULL);
}

/* ================= THE MAILBOX, AND THE SERVICER IT NEVER HAD =================
 *
 * ⚠⚠⚠ THIS PATH WAS REMOVED ONCE AND ITS REMOVAL NOTE IS WHY IT IS BACK. It used to
 * be serviced from StackPoll, i.e. only ever on a USB completion, and after bring-up
 * the interrupt-IN read settles into a blocking wait -- so on an idle radio it never
 * ran. Run 47 recorded the failure exactly: seq 9, ack 2, no new inquiry, published
 * table byte-identical to the run before. The mailbox was not the wrong idea; it had
 * no reliable servicer.
 *
 * ★★★ SetPersistentTimer IS THAT SERVICER, and it is the whole fix. It repeats on its
 * own (unlike SetInterruptTimer, which is one-shot), and its handler is a
 * SecondaryInterruptHandler2 owned BY THE DRIVER -- not one an application entered.
 * That distinction is the point:
 *
 *   The driver issues HCI commands from its own completion context constantly and has
 *   NEVER churned: 28 commands, a full pairing, an SDP browse of 1956 events, L2CAP
 *   config. What churns is a command issued from a hop an APPLICATION started. So the
 *   work moves back into the driver's own context rather than into the app's, which is
 *   also why the prior art's shape (call at task level in the app's context) was not
 *   simply copyable -- it would race BT_Pump inside a completion, and gInPump does not
 *   guard a direct call.
 *
 * ⚠ SINGLE WRITER PER WORD, which is what makes this race-free without a lock. The
 * panel writes kWCmd/Arg0/Arg1 and then kWCmdSeq LAST; only the panel ever writes
 * those. The driver writes kWCmdAck and kWCmdResult; only the driver ever writes
 * those. A sequence number rather than a flag means a request cannot be lost to a
 * clear that races a set. OS 9 runs on one CPU, so there is no store-ordering concern
 * across the pair. */
static TimerID       gPumpTimer = NULL;
static volatile int  gTimerLive = 0;
static unsigned long gTimerRuns = 0;
static unsigned long gMbxServiced = 0;
static unsigned long gMbxStale = 0;
/* ⚠ A BITMASK, NOT A COUNT, and OR-ed rather than assigned: the handler can be turned
 * back for different reasons at different moments in one instance's life, and knowing
 * WHICH reasons ever applied is the diagnostic. A count would say "it bailed a lot"
 * and answer nothing. */
static unsigned long gTimerBail = 0;
static unsigned long gTimerRcTask = 0;
static unsigned long gTimerRcIrq = 0;

/* ⚠ Forward declaration: the definition sits with the switch machinery several
 * hundred lines below, where gSwitchBusy and the verdict enum are in scope. It is
 * ARM-ONLY and interrupt-safe; the USB request itself is issued at task level from
 * BT_DeferredSwitchIfPending. Read the note there before changing either. */
static Boolean BT_HandBackToProxy(void);

static void BT_ServiceMailbox(void)
{
    unsigned long seq, cmd, a0, a1;

    if (gCB == NULL) return;
    seq = gCB[kWCmdSeq];
    if (seq == gCB[kWCmdAck]) { gMbxStale++; return; }

    cmd = gCB[kWCmd];
    a0  = gCB[kWCmdArg0];
    a1  = gCB[kWCmdArg1];

    /* ⚠ ACK BEFORE DOING THE WORK, not after. If the work faults or the driver is
     * torn down mid-command, an un-acked sequence would be retried on every timer
     * firing forever -- a latching failure, and this project has been bitten by two
     * of those. One request, one attempt, and the result word says what happened. */
    gCB[kWCmdAck] = seq;
    gMbxServiced++;

    switch (cmd) {
    case kCmdStartInquiry:
        gCB[kWCmdResult] = 0x100UL | (BT_ScanStart() & 0xFFUL);
        break;
    case kCmdSetRadio:
        gCB[kWCmdResult] = 0x100UL | (BT_SetRadio(a0 != 0) & 0xFFUL);
        break;
    case kCmdDeleteBond:
        /* ⚠ THE DISCONNECT BELONGS ON THIS PATH TOO, and leaving it off the first time
         * was an oversight rather than a decision. v12.3 put it on the card-side delete
         * because that is where the keyboard test went -- but which branch a row takes
         * depends on whether the CARD happens to hold a key for it, and the user's
         * requirement does not: "it should immediately stop communicating with Mac OS."
         * A device we forget while its link stays up keeps typing either way, and the
         * delete looks broken either way.
         *
         * ⚠ Order matters: disconnect BEFORE forgetting the key. BT_DisconnectByAddr
         * checks the live peer against this address, and it reads gConnPeerHi/Lo rather
         * than anything the delete touches -- but sequencing it first keeps that true
         * even if the lookup ever changes to consult the database. */
        (void)BT_DisconnectByAddr(a0, a1);
        gCB[kWCmdResult] = 0x100UL | (BT_DeleteBondByAddr(a0, a1) & 0xFFUL);
        break;
    /* ★★★★ Pair with the selected device, as the INITIATOR. The five runs before
     * this one drove the responder case, which BTstack correctly does not
     * authenticate from. See BT_PairDevice. */
    case kCmdPairDevice:
        /* ⚠⚠ RELEASE ANY BLOCK FIRST, AND THIS IS A REPORTED BUG NOT A PRECAUTION.
         *
         * The user clicked Disconnect on the phone, then tried to pair it again, and
         * the PHONE said "couldn't connect due to incorrect PIN or password" -- after
         * flashing the PIN prompt for a split second. Nothing was wrong with the PIN.
         * We were REFUSING the device at Connection Complete because Disconnect had
         * blocked it, and the phone reported our refusal in the only vocabulary it has.
         *
         * ⇒ You cannot meaningfully pair a device you are simultaneously refusing, so
         * asking to pair one is an unambiguous statement that you want it back. The
         * block is the user's own earlier instruction and this is the user overriding
         * it -- no confirmation, because they just expressed the newer intent. */
        if (BT_IsBlocked(a0, a1)) BT_SetBlocked(0, 0, 0);
        gCB[kWCmdResult] = 0x100UL | (BT_PairDevice(a0, a1) & 0xFFUL);
        break;
    /* ★★★★ ⚠ DESTRUCTIVE AND PERSISTENT. Removes a pairing from the card's own
     * firmware store, which Tiger shares. The panel confirms the address with the
     * user before this ever arrives. See BT_DeleteStoredKey. */
    /* ★★★★ v8.5: give the card back to OS 9's own HID driver. See kCmdSwitchToProxy.
     *
     * ⚠⚠ THIS ENDS THE SESSION FOR US, BY DESIGN. The card changes USB personality,
     * our device reference dies, and the Expert sends kNotifyDriverBeingRemoved
     * followed by Finalize -- the same teardown a reboot produces, which is a path
     * this driver has exercised on every run. Nothing after this point in the session
     * can pair anything, and that is the intended outcome: the keyboard comes back.
     *
     * ⚠ REFUSE RATHER THAN STOMP if a request is already in flight. gSwitchPB is one
     * static parameter block still owned by the USL, and overwriting it mid-flight is
     * the bug the forward path's guard exists to prevent. Recorded, not silent.
     *
     * ⚠ The attempt budget is reset first. kSwMaxTries bounds an automatic
     * re-enumerate loop on the forward path; this is one deliberate user action and
     * must not be refused because earlier forward attempts used the budget up. */
    /* ★★★★★★ THE CASE THAT WAS NEVER HERE.
     *
     * kCmdDelStoredKey has existed since v8.2, the panel has sent it since v8.2, the
     * driver carries a four-line comment describing it -- and there was no `case` for
     * it in this switch. Every card-side delete the user ever performed fell through to
     * `default:` and was answered "unknown command". The card's own store has therefore
     * NEVER been written to by this product, which is why Num_Keys_Read sat at 2 across
     * a delete, a restart and a second delete.
     *
     * ⚠⚠ THE COMMENT IS WHAT MADE IT INVISIBLE. Reading this switch, the eye finds a
     * paragraph about deleting from the card's firmware store sitting immediately above
     * a `case`, and reads the pair as one thing. The paragraph documents
     * kCmdDelStoredKey; the case beneath it is kCmdSwitchToProxy. A comment is not a
     * case, and this cost three hardware runs -- two of them spent diagnosing a delete
     * that was never dispatched.
     *
     * ⇒ AND THE MAILBOX COULD NOT REPORT IT: the driver dutifully wrote
     * 0x100 | 0xFF = "unknown command" into kWCmdResult every single time, and BTCheck
     * printed the mailbox's seq and ack but never the command or the result. The
     * instrument had the answer and did not show it. BTCheck now prints both. */
    case kCmdAllowDevice:
        /* ⭐ The undo for Disconnect. Clears the block so the device may reconnect --
         * it does NOT reach out to it, because on this card we cannot: Create_Connection
         * is rejected 0x12. The device supplies the link; we stop refusing it. */
        BT_SetBlocked(0, 0, 0);
        gCB[kWCmdResult] = 0x100UL | 0UL;
        break;

    case kCmdDisconnect:
        /* ⚠ Link only. No key is touched, by design -- that is the whole distinction
         * from Delete, and conflating them is what this command exists to undo. */
        /* ⭐ BLOCK FIRST, THEN DROP. If the link were dropped first the peer could
         * re-page and be accepted in the window before the block was set -- which is
         * precisely the one-second reconnection that made this button useless. */
        BT_SetBlocked(a0, a1, 1);
        gCB[kWCmdResult] = 0x100UL | (BT_DisconnectByAddr(a0, a1) ? 0UL : 0xFDUL);
        break;

    case kCmdDelStoredKey: {
        /* ★★★★★★ v12.3: DELETE NOW MEANS WHAT THE USER EXPECTS IT TO MEAN.
         *
         * The user's words, and they are right: "this is the problem with not having
         * devices immediately dropped from the list once deleted: we have a hard time
         * knowing what their actual state is." A deleted keyboard kept reading
         * "Paired", so the control panel could not be used to tell what had happened --
         * during the very sequence where that was the only thing we needed to know.
         *
         * Three things, in this order:
         *  1. remove the key from the CARD's firmware store (what it always did);
         *  2. demote OUR copy to archived, so the row disappears from the list while
         *     the key survives for Restore. This is the half that was missing;
         *  3. drop the live ACL link, because NO KEY DELETION EVER DISCONNECTS
         *     ANYTHING. A bonded device keeps working until the link goes down, which
         *     is why the keyboard kept typing after every delete and made the whole
         *     operation look like it had failed. */
        int rc = BT_DeleteStoredKey(a0, a1);
        (void)BT_LinkKeyArchive(a0, a1);
        (void)BT_DisconnectByAddr(a0, a1);
        /* ⭐ AND FORGET THE PAGE. Without this the row survives every other half of
         * the delete and reads "Available" -- honest, and not what Delete means. */
        (void)BT_ForgetPager(a0, a1);
        /* ⚠ AND RELEASE ANY BLOCK ON IT. Deleting a device the user had also
         * Disconnected must not leave an invisible refusal behind for an address that
         * no longer appears anywhere in the panel -- that is an unfindable state. */
        if (BT_IsBlocked(a0, a1)) BT_SetBlocked(0, 0, 0);
        gCB[kWCmdResult] = 0x100UL | ((unsigned long)rc & 0xFFUL);
        break;
    }

    case kCmdRestoreKey: {
        /* ⚠⚠ THIS COMMENT USED TO SAY "Task level (the mailbox is serviced from the
         * defer trampoline)". THAT WAS FALSE, and the static audit is what caught it:
         *     bt_pump_timer_sih -> BT_ServiceMailbox -> ...
         * bt_pump_timer_sih is a SECONDARY INTERRUPT HANDLER, so every case in this
         * switch runs below task level. CLAUDE.md says it exactly: "a comment saying
         * 'this runs at task level' is a claim about the call graph, and it expires
         * silently when callers change." Mine was wrong the day I wrote it.
         *
         * ⇒ What is actually safe here, checked rather than assumed: the database
         * lookup is memcmp/memcpy over a fixed array; BT_WriteKeyToController queues
         * one async HCI command; and the promote-to-live path asks for task level via
         * BT_DeferRequest rather than reaching for it. No allocation, no File Manager,
         * no blocking anywhere on this path. */
        unsigned char addr[6], key[16], type;
        int found = BT_LinkKeyFindByAddr(gCB[kWCmdArg0], gCB[kWCmdArg1],
                                         addr, key, &type);
        if (!found) {
            gCB[kWCmdResult] = 0x100UL | 0xFCUL;   /* nothing captured for that peer */
        } else {
            BT_WriteKeyToController(addr, key);
            /* ⭐ AND ASK THE CARD AGAIN. Delete_Stored_Link_Key's completion triggers a
             * re-read (the v8.2 mechanism) and the WRITE path never did -- so after a
             * restore the log still showed the post-delete count and looked as though
             * nothing had gone back. The write reported keys written 1 and the key
             * count said 0, in the same log, which is the sort of contradiction that
             * costs a run to unpick. */
            BT_TryStoredKeyProbe();
            gRestoreTried++;
            gCB[kWCmdResult] = 0x100UL | 0UL;
        }
        break;
    }

    case kCmdSwitchToProxy:
        /* ⚠ Through a helper, not inline. gSwitchBusy, the verdict enum and
         * CsrModeSwitch are all declared several hundred lines BELOW this dispatch,
         * and hoisting them up here to satisfy one case would drag the whole switch
         * apparatus out of the section it documents. */
        gCB[kWCmdResult] = 0x100UL | (BT_HandBackToProxy() ? 0UL : 0xFDUL);
        break;
    default:
        gCB[kWCmdResult] = 0x100UL | 0xFFUL;    /* unknown command, and say so */
        break;
    }
}

/* ⚠⚠ ROOTED IN scripts/level-audit.py IN THIS COMMIT. Every SecondaryInterruptHandler2
 * gets rooted where it is created; "it only pumps" is exactly the exception that
 * erodes the habit. This one reaches BTstack and the USB transport, so it is the most
 * important root in the file, not the least. */
static OSStatus bt_pump_timer_sih(void *p1, void *p2)
{
    (void)p1; (void)p2;

    /* ⚠⚠⚠ FOUR GUARDS, AND EVERY ONE OF THEM MATTERS. A persistent timer keeps firing
     * until cancelled, and this driver is unloaded and rebound several times a session,
     * so "the handler runs against torn-down state" is not hypothetical here -- it is
     * the documented shape of the freeze candidate in ProbeFinalize. CancelTimer can
     * also race a firing handler, which is why gTimerLive exists in addition to it. */
    if (!gTimerLive)             { gTimerBail |= 1; return noErr; }
    if (gDeviceGone)             { gTimerBail |= 2; return noErr; }
    if (gDevice == kNoDeviceRef) { gTimerBail |= 4; return noErr; }
    if (gCB == NULL)             { gTimerBail |= 8; return noErr; }

    gTimerRuns++;
    BT_ServiceMailbox();
    /* ⚠ Retried from here rather than sent once from an event, because a refused send
     * must not latch. See BT_TryStoredKeyProbe: the first version latched before the
     * send and consequently never fired at all. */
    BT_TryStoredKeyProbe();
    /* ⚠ v9.0. Self-throttled to every 50th firing -- see BT_TryScanEnableProbe. It
     * answers the one question the v8.8 run reduced to and could not answer: were we
     * page-scanning at all, or did the keyboard simply not try? */
    BT_TryScanEnableProbe();
    /* ⚠ v9.4: ONE per-link command per tick. See BT_LinkCmdEnqueue -- v9.3 fired two
     * commands from Connection Complete against one command credit and the auth send
     * came back 0x0C COMMAND_DISALLOWED. Four commands per link cannot be a burst. */
    BT_LinkCmdPump();
    /* ⚠ AFTER the probe and unconditional. v6.5 sampled these from inside the probe,
     * which stopped the moment the probe succeeded and left BTCheck reporting the
     * gates as they were at that one healthy instant. */
    BT_SampleSendGates();
    /* ★ Pump unconditionally, not only when a command arrived. This is the second
     * defect the timer fixes: BTstack's timers only ever advanced when a USB
     * completion happened, so every timeout in the stack was unreliable. M4's
     * SET_PROTOCOL handshake needs them to work. */
    BT_Pump();
    /* ★★★★★ AND MIRROR THE RESULT INTO THE BLOCK, which only a USB completion did.
     *
     * ⚠⚠ LAST, and unconditionally. Everything above this line updates the glue's
     * globals; BT_StackPoll is the only thing that copies them into the block, and it
     * was reached only from the packet handler. On an idle radio there are no packets,
     * so the block froze at bring-up and BTCheck printed bring-up values under
     * headings that say "last" -- see the long note at BT_PublishFromTimer. Publishing
     * here costs one pass over 16 scan slots and 8 keys every 100 ms and makes a quiet
     * run readable, which is the run we most need to read. */
    BT_PublishFromTimer();
    return noErr;
}

/* 100 ms: far below anything a user notices on a button, far above the cost of a
 * secondary interrupt, and fine resolution for BTstack timeouts measured in seconds. */
#define kPumpTimerMs 100

/* atTask: which attempt site this is, so the two return codes stay distinguishable. */
static void BT_StartPumpTimerAt(int atTask)
{
    OSStatus rc;
    if (gPumpTimer != NULL) return;
    gTimerLive = 1;
    rc = SetPersistentTimer((Duration)(kPumpTimerMs * durationMillisecond),
                            bt_pump_timer_sih, NULL, &gPumpTimer);
    /* ⚠ 0x100 | rc, the block's convention throughout: a plain 0 would be
     * indistinguishable from "never attempted", which is the exact ambiguity that
     * made v5.5's run unreadable. */
    if (atTask) gTimerRcTask = 0x100UL | ((unsigned long)rc & 0xFFUL);
    else        gTimerRcIrq  = 0x100UL | ((unsigned long)rc & 0xFFUL);
    if (rc != noErr) {
        /* ⚠ FAIL OPEN AND VISIBLY. Without the timer the mailbox has no servicer and
         * scanning would silently do nothing -- run 47 again. Clearing gTimerLive and
         * leaving gPumpTimer NULL means the counters stay at zero, which is exactly
         * what BTCheck and the panel will show. */
        gTimerLive = 0;
        gPumpTimer = NULL;
    }
}

/* ★★ TASK LEVEL FIRST. This is the change v5.5 needed: ProbeInitialize runs at task
 * level and has already called EnsureBlock, so the timer can be armed from a context
 * whose legality is not in question. The handler's own guards make it a harmless
 * no-op until ConfigDone has actually built the stack, so arming early costs nothing.
 *
 * ⚠ ConfigDone still tries, but only if this did not take -- gPumpTimer being non-NULL
 * makes the second call return immediately. Two words record the two outcomes, so the
 * run says WHICH context worked rather than leaving it to be inferred from success. */
static void BT_StartPumpTimerTask(void) { BT_StartPumpTimerAt(1); }
static void BT_StartPumpTimerIrq(void)  { BT_StartPumpTimerAt(0); }

static void BT_StopPumpTimer(void)
{
    /* ⚠ FLAG FIRST, THEN CANCEL. CancelTimer does not promise that a handler already
     * in flight has finished, so the flag is what actually makes the handler a no-op.
     * Same ordering reason as gDeviceGone before the pipe aborts. */
    gTimerLive = 0;
    if (gPumpTimer != NULL) {
        (void)CancelTimer(gPumpTimer, NULL);
        gPumpTimer = NULL;
    }
}

/* ⚠⚠ A DELIBERATELY EMPTY ENTRY POINT, PURELY TO BISECT THE TEARDOWN.
 *
 * Established by the panel's action log, one press per run:
 *     W3>3  walk only (exact triple)        -> no teardown, walk INNOCENT
 *     S3>3  walk + FindSymbol, no call      -> no teardown, FindSymbol INNOCENT
 *     N     walk + FindSymbol + call        -> teardown, every time
 *
 * So the CALL is what unloads the driver. That still spans two very different causes:
 *
 *   (a) an application calling into a driver's fragment at all, or
 *   (b) what our handler DOES once inside -- CallSecondaryInterruptHandler2, entering
 *       BTstack from an app-initiated context, or starting the inquiry.
 *
 * This function distinguishes them. It touches nothing: no secondary interrupt, no
 * BTstack, no globals, no USB. If calling THIS still cycles the driver, the cause is
 * (a) and the whole direct-call architecture is wrong for this OS. If it does not, the
 * cause is inside our handler and is ours to fix.
 *
 * ⚠ Remove once the question is answered. It is an instrument, not a feature. */
OSStatus BTNoop(void);
OSStatus BTNoop(void)
{
    return noErr;
}

/* ★★ RESULT: `acts P1>1`. Calling into the fragment from an app is INNOCENT, so the
 * cause is inside our own handler. Two candidates remain:
 *
 *   (a) CallSecondaryInterruptHandler2 itself, invoked from an application context
 *   (b) what runs INSIDE it -- BTstack, and the HCI Inquiry it puts on the wire
 *
 * BTNoopSIH has BTScanStart's exact shape -- app calls it at task level, it hops to
 * secondary interrupt level -- but the handler touches NOTHING. No BTstack, no USB, no
 * globals. If this cycles the driver the fault is (a) and the hop is the problem; if it
 * does not, the fault is (b) and the problem is issuing USB work from a secondary
 * interrupt handler that an application started.
 *
 * ⚠ INSTRUMENT, not a feature. Goes out with BTNoop once answered. */
static OSStatus bt_empty_sih(void *p1, void *p2)
{
    (void)p1; (void)p2;
    return noErr;
}

OSStatus BTNoopSIH(void);
OSStatus BTNoopSIH(void)
{
    return CallSecondaryInterruptHandler2(bt_empty_sih, NULL, NULL, NULL);
}

/* ★ THE ON/OFF CONTROL. Same shape as BTScanStart and for the same reason: the panel
 * calls at task level, the work happens at secondary interrupt level where it is
 * serialized against the USB completions. p1 carries the requested state. */
static OSStatus bt_set_radio_sih(void *p1, void *p2)
{
    (void)p2;
    if (gDevice == kNoDeviceRef) return kUSBDeviceBusy;
    if (gCB == NULL)             return kUSBDeviceBusy;
    (void)BT_SetRadio(p1 != NULL);
    return noErr;
}

/* ★ FORGET A BOND. Same secondary-interrupt shape as the other two entry points.
 *
 * ⚠ The address travels as two pointer-sized values rather than through a shared
 * buffer. A SecondaryInterruptHandler2 takes exactly two void* arguments, which is
 * enough for a 6-byte BD_ADDR split hi/lo -- and passing the address of a local would
 * hand the handler a pointer into a stack frame it may outlive. */
static OSStatus bt_delete_bond_sih(void *p1, void *p2)
{
    if (gDevice == kNoDeviceRef) return kUSBDeviceBusy;
    if (gCB == NULL)             return kUSBDeviceBusy;
    /* ⚠ 0 means "no key for that address", which is NOT success. The panel reports the
     * difference rather than claiming to have deleted something it never held. */
    if (!BT_DeleteBondByAddr((unsigned long)p1, (unsigned long)p2))
        return (OSStatus)kBTNoSuchBond;
    return noErr;
}

OSStatus BTDeleteBond(long hi, long lo);
OSStatus BTDeleteBond(long hi, long lo)
{
    return CallSecondaryInterruptHandler2(bt_delete_bond_sih, NULL,
                                          (void *)hi, (void *)lo);
}

OSStatus BTSetRadio(long on);
OSStatus BTSetRadio(long on)
{
    /* ⚠ The flag travels as the POINTER value, which is what a
     * SecondaryInterruptHandler2 takes. Passing the address of a local would be a
     * pointer into a stack frame the handler may outlive. */
    return CallSecondaryInterruptHandler2(bt_set_radio_sih, NULL,
                                          on ? (void *)1 : (void *)0, NULL);
}

void BT_StackPoll(unsigned long state)
{
    Note(kWStkState,   state);
    Note(kWStkPump,    gPumpCalls);
    Note(kWStkReenter, gPumpReentered);
    Note(kWStkTx,      gTxCmd + gTxAcl);
    Note(kWStkRx,      gRxEvent + gRxAcl);
    Note(kWStkRefused, gTxRefused);
    Note(kWStkTxDone,  gTxDone);

    /* M2b. Same mirror, same interrupt, same cost: Note() is a plain store into
     * the block. Kept in one function so there is exactly one place where the
     * BTstack side's view and BTCheck's view can drift apart. */
    Note(kWL2Init,         gL2Init);
    Note(kWInqState,       gInqState);     /* ⚠ NOT NoteKeep: the panel gates on it */
    NoteKeep(kWInqStartRc, gInqStartRc);   /* forensic: survives a teardown         */
    NoteKeep(kWInqStopRc,  gInqStopRc);
    /* M4 step 1 */
    Note(kWHidListenRc,   gHidListenRc);
    Note(kWHidIncoming,   gHidIncoming);
    Note(kWHidAccepted,   gHidAccepted);
    Note(kWHidDeclined,   gHidDeclined);
    Note(kWHidOpened,     gHidOpened);
    Note(kWHidClosed,     gHidClosed);
    Note(kWHidOpenStatus, gHidOpenStatus);
    Note(kWHidLastPsm,    gHidLastPsm);
    Note(kWHidCtrlCid,    gHidCtrlCid);
    Note(kWHidPeerHi,     gHidPeerHi);
    Note(kWHidPeerLo,     gHidPeerLo);
    NoteKeep(kWInqResults,     gInqResults);      /* forensic: "did ANY device answer" */
    NoteKeep(kWInqComplStatus, gInqComplStatus);
    Note(kWTgtAddrHi,      gTgtAddrHi);
    Note(kWTgtAddrLo,      gTgtAddrLo);
    Note(kWTgtCoD,         gTgtCoD);
    Note(kWTgtPick,        gTgtPick);
    NoteKeep(kWHciConnStatus,  gHciConnStatus);
    NoteKeep(kWHciConnHandle,  gHciConnHandle);
    Note(kWSdpIssued,      gSdpIssued);
    Note(kWSdpQueryRc,     gSdpQueryRc);
    Note(kWSdpAttrBytes,   gSdpAttrBytes);
    Note(kWSdpRecords,     gSdpRecords);
    Note(kWSdpComplete,    gSdpComplete);
    Note(kWSdpStatus,      gSdpStatus);
    Note(kWL2capEvts,      gL2capEvts);
    Note(kWLastUnkEvt,     gLastUnkEvt);
    Note(kWStoredKeyRc,    gStoredKeyRc);
    Note(kWStoredKeyCap,   gStoredKeyCap);
    Note(kWSendRc,         gSendRc);
    Note(kWGateFirst,      gGateFirst);
    Note(kWGateLast,       gGateLast);
    Note(kWAclSlots,       gAclSlots);
    Note(kWConnPeerHi,     gConnPeerHi);
    Note(kWConnPeerLo,     gConnPeerLo);
    Note(kWRlkEvents,      gRlkEvents);
    Note(kWRlkKeys,        gRlkKeys);
    {   /* The four address slots, published as a run rather than by hand: a
         * hand-written list of eight Note() calls is how kWords drifted. */
        int k;
        for (k = 0; k < kWRlkSlots; k++) {
            Note(kWRlkA0Hi + k * 2,     gRlkAddr[k][0]);
            Note(kWRlkA0Hi + k * 2 + 1, gRlkAddr[k][1]);
        }
    }
    Note(kWConnReqs,       gConnReqs);
    Note(kWConnReqHi,      gConnReqHi);
    Note(kWConnReqLo,      gConnReqLo);
    Note(kWConnReqCoD,     gConnReqCoD);
    Note(kWConnReqType,    gConnReqType);
    Note(kWAuthComps,      gAuthComps);
    Note(kWLkNotifs,       gLkNotifs);
    Note(kWLastCmdStatus,  gLastCmdStatus);
    {   /* The event ring, copied as a run. ⚠ Bounded by kEvtRingLen, the SAME macro
         * the ring itself is declared with (bt_pump.h) -- not a literal 16. */
        int k;
        for (k = 0; k < kEvtRingLen; k++) Note(kWEvtRing + k, gEvtRing[k]);
    }
    Note(kWEvtRingIdx,     gEvtRingIdx);
    Note(kWEvtTotal,       gEvtTotal);
    Note(kWRrsfCount,      gRrsfCount);
    Note(kWRrsfStatus,     gRrsfStatus);
    Note(kWRrsfHandle,     gRrsfHandle);
    Note(kWHidLevel,       gHidLevel);
    Note(kWHidDataPkts,    gHidDataPkts);
    Note(kWHidLastDataLen, gHidLastDataLen);
    Note(kWHidFirstLen,    gHidFirstDataLen);
    Note(kWHidFirstFull,   gHidFirstDataFull);
    {   /* The captured payload, 4 bytes per word big-endian so BTCheck can print it
         * as a byte string in arrival order. ⚠ Bounded by kHidCapBytes, the same macro
         * the buffer is declared with -- not a literal 16. */
        int k;
        for (k = 0; k < kHidCapBytes / 4; k++)
            Note(kWHidFirstData + k,
                 ((unsigned long)gHidFirstData[k * 4    ] << 24) |
                 ((unsigned long)gHidFirstData[k * 4 + 1] << 16) |
                 ((unsigned long)gHidFirstData[k * 4 + 2] <<  8) |
                  (unsigned long)gHidFirstData[k * 4 + 3]);
    }
    Note(kWHidDecodeRc,    gHidDecodeRc);
    Note(kWHidDecodeOk,    gHidDecodeOk);
    Note(kWHidRollovers,   gHidRollovers);
    Note(kWHidLastMods,    gHidLastMods);
    Note(kWHidLastNKeys,   gHidLastNKeys);
    Note(kWHidLastKey0,    gHidLastKey0);
    Note(kWSynthRrsf,      gSynthRrsf);
    Note(kWSecEvtCount,    gSecEvtCount);
    Note(kWSecEvtLevel,    gSecEvtLevel);
    Note(kWSecEvtStatus,   gSecEvtStatus);
    Note(kWSecEvtHandle,   gSecEvtHandle);
    Note(kWSecLvlFirst,    gSecLvlFirst);
    Note(kWSecLvlLast,     gSecLvlLast);
    Note(kWRemFeatFirst,   gRemFeatFirst);
    Note(kWRemFeatLast,    gRemFeatLast);
    Note(kWLiveSamples,    gLiveSamples);
    {   /* ⚠ Bounded by kHandlerRingLen, the same macro the ring is declared with. */
        int k;
        for (k = 0; k < kHandlerRingLen; k++)
            Note(kWHandlerRing + k, gHandlerEvtRing[k]);
    }
    Note(kWHandlerIdx,     gHandlerEvtIdx);
    Note(kWHandlerTotal,   gHandlerEvtTotal);
    Note(kWSampHandle,     gSampledHandle);
    Note(kWSampHciHandle,  gSampledHciHandle);
    NoteKeep(kWSampHandleMax,  gSampledHandleMax);
    NoteKeep(kWSampHciMax,     gSampledHciMax);
    Note(kWTimerAtEnd,     gTimerRuns);
    /* ⚠⚠ NoteKeep, AND 16.7 SHOULD HAVE DONE THIS IN THE SAME BREATH AS THE INQUIRY
     * WORDS. It fixed "did a scan run" and left "did a PAIR run" mirrored from instance
     * globals -- so on 2026-10-04 the log said "Pair requested 0" on a dongle that had
     * torn down TWICE, and that row could not distinguish "the user never clicked Pair"
     * from "the click landed on the instance before this one". Those need opposite next
     * steps, which is the whole reason NoteKeep exists.
     *
     * ★ The mailbox rows stayed trustworthy through all of it because MailboxSend writes
     * gBlock[kWCmd] STRAIGHT INTO THE SHARED BLOCK from the panel -- nothing mirrors
     * them. That is the property these words now borrow.
     *
     * ⚠ Forensic only, and none of them is read by cpanel/bt_panel.c -- verified, same
     * as the inquiry set. The panel's own pairing UI gates on its selected row and on
     * MailboxCaughtUp, never on these. */
    NoteKeep(kWPairAsked,      gPairAsked);
    NoteKeep(kWPairRc,         gPairRc);
    NoteKeep(kWPairAddrHi,     gPairAddrHi);
    NoteKeep(kWPairAddrLo,     gPairAddrLo);
    NoteKeep(kWBondDone,       gBondDone);
    NoteKeep(kWBondStatus,     gBondStatus);
    NoteKeep(kWWroteKeyTried,  gWroteKeyTried);
    NoteKeep(kWWroteKeyRc,     gWroteKeyRc);
    NoteKeep(kWWroteKeyDone,   gWroteKeyDone);
    Note(kWFirstBadCmd,    gFirstBadCmdStatus);
    Note(kWBadCmdCount,    gBadCmdCount);
    {   int k;
        for (k = 0; k < kBadCmdRingLen; k++)
            Note(kWBadCmdRing + k, gBadCmdRing[k]);
    }
    NoteKeep(kWPairPath,       gPairPath);
    Note(kWLinkWasUp,      gLinkWasUp);
    Note(kWArmedFired,     gArmedFired);
    Note(kWArmedRc,        gArmedRc);
    Note(kWSuppCmds0,      gSuppCmds[0]);
    Note(kWSuppCmds1,      gSuppCmds[1]);
    Note(kWSuppCmdsGot,    gSuppCmdsGot);
    Note(kWLocalVer,       gLocalVer);
    Note(kWLocalMfr,       gLocalMfr);
    Note(kWDelStoredTried, gDelStoredTried);
    Note(kWDelStoredRc,    gDelStoredRc);
    Note(kWDelStoredDone,  gDelStoredDone);
    Note(kWDelStoredHi,    gDelStoredHi);
    Note(kWDelStoredLo,    gDelStoredLo);
    /* v8.2 */
    Note(kWStoredKeyCapFirst, gStoredKeyCapFirst);
    Note(kWRlkPasses,         gRlkPasses);
    Note(kWPagedCount,        gPagedCount);
    /* v8.6 */
    Note(kWHidIntrListenRc,  gHidIntrListenRc);
    Note(kWHidIntrCid,       gHidIntrCid);
    Note(kWHidIntrOpened,    gHidIntrOpened);
    Note(kWHidSetProtoTried, gHidSetProtoTried);
    Note(kWHidSetProtoRc,    gHidSetProtoRc);
    Note(kWHidHandshake,     gHidHandshake);
    Note(kWHidCtrlDataPkts,  gHidCtrlDataPkts);
    /* v8.7 */
    Note(kWAclCapCount, gAclCapCount);
    {
        int ci, cw;
        for (ci = 0; ci < kAclCapSlots; ci++)
            for (cw = 0; cw < kAclCapBytes / 4; cw++)
                Note(kWAclCap0 + ci * (kAclCapBytes / 4) + cw,
                     ((unsigned long)gAclCap[ci][cw * 4 + 0] << 24)
                   | ((unsigned long)gAclCap[ci][cw * 4 + 1] << 16)
                   | ((unsigned long)gAclCap[ci][cw * 4 + 2] << 8)
                   |  (unsigned long)gAclCap[ci][cw * 4 + 3]);
    }
    /* v9.1: and the OUTBOUND mirror, same geometry so one BTCheck decoder reads both.
     * ⚠ The full length is published per slot: a 16-byte capture of a longer packet
     * must not be read as a short packet, which is how a truncated Configuration
     * Request would masquerade as a malformed one. */
    Note(kWAclTxCapCount, gAclTxCapCount);
    {
        int ci, cw;
        for (ci = 0; ci < kAclCapSlots; ci++) {
            Note(kWAclTxLen0 + ci, gAclTxCapLen[ci]);
            for (cw = 0; cw < kAclTxCapBytes / 4; cw++)
                Note(kWAclTxCap0 + ci * (kAclTxCapBytes / 4) + cw,
                     ((unsigned long)gAclTxCap[ci][cw * 4 + 0] << 24)
                   | ((unsigned long)gAclTxCap[ci][cw * 4 + 1] << 16)
                   | ((unsigned long)gAclTxCap[ci][cw * 4 + 2] << 8)
                   |  (unsigned long)gAclTxCap[ci][cw * 4 + 3]);
        }
    }
    Note(kWLinkUpMs,    gLinkUpMs);
    Note(kWLinkDownMs,  gLinkDownMs);
    Note(kWLinkLifeMs,  gLinkLifeMs);
    Note(kWRrsfSentMs,  gRrsfSentMs);
    Note(kWRrsfDoneMs,  gRrsfDoneMs);
    /* v8.8 */
    Note(kWAutoAuthTried,  gAutoAuthTried);
    Note(kWAutoAuthRc,     gAutoAuthRc);
    Note(kWAutoAuthHandle, gAutoAuthHandle);
    /* ⚠ The COMPILED gate, not a variable. It is a compile-time constant on purpose:
     * a runtime copy could disagree with the code the gate actually guards, which is
     * the whole failure mode kProbeGateMarker exists to catch in the artifact. */
    Note(kWAutoAuthGate,   (unsigned long)kHidAutoAuth);
    Note(kWHidIntrDataPkts,  gHidIntrDataPkts);
    {
        int pi;
        for (pi = 0; pi < kPagedSlots; pi++) {
            Note(kWPagedA0Hi + pi * 3 + 0, gPagedAddr[pi][0]);
            Note(kWPagedA0Hi + pi * 3 + 1, gPagedAddr[pi][1]);
            Note(kWPagedA0Hi + pi * 3 + 2, gPagedAddr[pi][2]);
        }
    }
    Note(kWScanMode,       gScanMode);     /* live state: must still clear */
    /* ⚠⚠⚠ THE WHOLE SECURITY AND CONNECTION GROUP, SWEPT AT ONCE -- AND IT SHOULD HAVE
     * BEEN DONE THIS WAY TWO BUILDS AGO.
     *
     * 16.7 made the inquiry words survive a teardown. 16.8 made the pairing words
     * survive. Both times I converted the group I happened to be reading and left the
     * rest, and both times the NEXT run's answer landed in a group I had not swept.
     * 17.0's log is the third instance: "legacy PIN requests 0 / of those ANSWERED 0"
     * printed on a session with Initialize 2, Finalize 1 -- so it cannot distinguish
     * "the controller never asked us for a PIN" from "it asked the instance before this
     * one". That is the single row the run existed to produce.
     *
     * ⇒ The rule, instead of another list: A WORD THAT ANSWERS "DID X EVER HAPPEN" IS
     * FORENSIC AND MUST BE STICKY. A word the panel ACTS on is live state and must
     * still clear. Only four are live state -- kWInqState, kWScanState, kWScanCount,
     * kWScanBase -- plus kWScanMode here, which reports the radio's current mode.
     * Verified: none of the words below appears in cpanel/bt_panel.c. */
    NoteKeep(kWDiscReason,     gDiscReason);
    NoteKeep(kWDiscHandle,     gDiscHandle);
    NoteKeep(kWDiscCount,      gDiscCount);
    NoteKeep(kWIoCapReqs,      gIoCapReqs);
    NoteKeep(kWUserConfReqs,   gUserConfReqs);
    NoteKeep(kWPinReqs,        gPinReqs);
    NoteKeep(kWPinAnswered,    gPinAnswered);
    NoteKeep(kWPinRespRc,      gPinRespRc);
    NoteKeep(kWPinAddrHi,      gPinAddrHi);
    NoteKeep(kWPinAddrLo,      gPinAddrLo);
    NoteKeep(kWAuthComplete,   gAuthComplete);
    Note(kWSspAuto,        gSspAuto);      /* live config, re-asserted each bring-up */
    NoteKeep(kWLinkKeyReqs,    gLinkKeyReqs);
    NoteKeep(kWSimplePairing,  gSimplePairing);
    NoteKeep(kWEncryptChange,  gEncryptChange);
    NoteKeep(kWEncryptOn,      gEncryptOn);
    NoteKeep(kWSdpRetries,     gSdpRetries);
    NoteKeep(kWSdpRetryRc,     gSdpRetryRc);
    NoteKeep(kWSdpNotReady,    gSdpNotReady);
    NoteKeep(kWSdpEventsAny,   gSdpEventsAny);
    NoteKeep(kWSdpLastEvt,     gSdpLastEvt);
    NoteKeep(kWConnOks,        gConnOks);
    NoteKeep(kWSdpOks,         gSdpOks);
    NoteKeep(kWAllocFails,     gAllocFails);
    Note(kWLkGets,         gLkGets);
    Note(kWLkHits,         gLkHits);
    Note(kWLkPuts,         gLkPuts);
    Note(kWLkDeletes,      gLkDeletes);
    Note(kWLkStored,       gLkStored);
    Note(kWLkEvicted,      gLkEvicted);
    Note(kWLkDirty,        gLkDirty);
    Note(kWSecReqs,        gSecReqs);
    Note(kWLkAddrHi,       gLkLastAddrHi);
    Note(kWLkAddrLo,       gLkLastAddrLo);
    Note(kWLkType,         gLkLastType);
    Note(kWDeferReqs,      gDeferReqs);
    Note(kWDeferRuns,      gDeferRuns);
    Note(kWDeferInstallErrs, gDeferInstallErrs);
    Note(kWDeferDropped,   gDeferDropped);
    Note(kWKfLoads,        gKfLoads);
    Note(kWKfLoaded,       gKfLoaded);
    Note(kWKfFlushes,      gKfFlushes);
    Note(kWKfWritten,      gKfWritten);
    Note(kWKfErr,          gKfErr);
    Note(kWKfRejected,     gKfRejected);
    /* ⚠ NoteKeep, not Note: these are the "what did the scan actually SEE" tallies, and
     * a teardown must not silently turn "a keyboard answered" into "nothing answered".
     * They are read by BTCheck only -- the panel builds its list from kWScanCount/
     * kWScanBase, which deliberately DO still clear. See NoteKeep's comment. */
    NoteKeep(kWInqPeriph,      gInqPeriph);
    NoteKeep(kWInqPhones,      gInqPhones);
    NoteKeep(kWInqComputers,   gInqComputers);
    NoteKeep(kWInqOther,       gInqOther);
    NoteKeep(kWInqAudio,       gInqAudio);
    NoteKeep(kWInqPeriphAddrHi, gInqPeriphAddrHi);
    NoteKeep(kWInqPeriphAddrLo, gInqPeriphAddrLo);
    NoteKeep(kWInqPeriphCoD,   gInqPeriphCoD);
    NoteKeep(kWRespStored,     gRespStored);
    Note(kWRespDisplaced,  gRespDisplaced);

    /* ======================= M6: THE SCAN CHANNEL ==============================
     * Publish the responder table, then service any command the panel has left.
     * docs/SCAN-DESIGN.md; protocol in docs/M3-DESIGN.md §5c.
     *
     * ⚠⚠ THE COMMAND-SERVICING HALF IS GONE, DELIBERATELY, AND IS NOT COMING BACK.
     *
     * It read kWCmdSeq against kWCmdAck here and started the inquiry when they
     * differed. That could only ever run on a USB completion, and after bring-up the
     * interrupt-IN read settles into a blocking wait -- so on an idle radio it never
     * ran at all. Run 47: seq 9, ack 2, no new inquiry, published table byte-identical
     * to the run before it.
     *
     * ★ The panel now calls BTScanStart directly (see M6b above). Leaving this path in
     * as a fallback would mean TWO ways to start an inquiry, one of which silently does
     * nothing on an idle radio -- which is worse than one way that works, because it is
     * the one that would get blamed last. Publishing stays; commanding is gone. */
    if (gCB != NULL) {
        /* ⚠ FIRST, because every word below is only as trustworthy as these two. See
         * the note at BT_PublishFromTimer: publishing used to happen only when a USB
         * completion arrived, so a quiet run left the whole block frozen at bring-up
         * with nothing in it to say so. Now the freshness is IN the block. */
        Note(kWPublishRuns, gPublishRuns);
        Note(kWPublishMs,   gPublishMs);
        Note(kWScanEnaRc,    gScanEnaRc);
        Note(kWScanEnaSends, gScanEnaSends);
        Note(kWScanEnaReads, gScanEnaReads);
        Note(kWScanEnaFirst, gScanEnaFirst);
        Note(kWScanEnaLast,  gScanEnaLast);
        Note(kWSupTimeoutTried,  gSuperTimeoutTried);
        Note(kWSupTimeoutRc,     gSuperTimeoutRc);
        Note(kWSupTimeoutHandle, gSuperTimeoutHandle);
        Note(kWSupGate,          (unsigned long)kHidLongSupervision);
        Note(kWPolicyGate,       (unsigned long)kHidLinkPolicy);
        Note(kWPolicyReadRc,     gPolicyReadRc);
        Note(kWPolicyReadSends,  gPolicyReadSends);
        Note(kWPolicyReads,      gPolicyReads);
        Note(kWPolicyReadBack,   gPolicyReadBack);
        Note(kWPolicyWriteRc,    gPolicyWriteRc);
        Note(kWPolicyWriteSends, gPolicyWriteSends);
        Note(kWLinkCmdEnq,       gLinkCmdEnq);
        Note(kWLinkCmdSent,      gLinkCmdSent);
        Note(kWLinkCmdRefused,   gLinkCmdRefused);
        Note(kWLinkCmdDropped,   gLinkCmdDropped);
        Note(kWLinkCmdDepth,     gLinkCmdCount);
        Note(kWModeChanges,      gModeChanges);
        Note(kWModeLast,         gModeLast);
        Note(kWModeLastMs,       gModeLastMs);
        Note(kWRoleChanges,      gRoleChanges);
        Note(kWRoleStatus,       gRoleStatus);
        Note(kWRoleNew,          gRoleNew);
        Note(kWRoleMs,           gRoleMs);
        Note(kWHidConsRpts,      gHidConsumerReports);
        Note(kWHidConsSeen,      gHidConsumerSeen);
        Note(kWHidConsPadBits,   gHidConsumerPadBits);
        Note(kWHidConsRingCount, gHidConsumerRingCount);
        Note(kWHidConsRingLost,  gHidConsumerRingLost);
        Note(kWHidKeyRpts,       gHidKeyReports);
        Note(kWInjInitRuns,      gInjInitRuns);
        Note(kWInjKchrOk,        gInjKchrOk);
        Note(kWInjKchrErr,       gInjKchrErr);
        Note(kWInjCalls,         gInjCalls);
        Note(kWInjEventsSeen,    gInjEventsSeen);
        Note(kWInjEvents,        gInjEvents);
        Note(kWInjPosted,        gInjPosted);
        Note(kWInjNoVk,          gInjNoVk);
        Note(kWInjNoKchr,        gInjNoKchr);
        Note(kWInjNoChar,        gInjNoChar);
        Note(kWInjLastUsage,     gInjLastUsage);
        Note(kWInjLastVk,        gInjLastVk);
        Note(kWInjPostErr,       gInjPostErr);
        Note(kWMedPresses,       gMedPresses);
        Note(kWMedRuns,          gMedRuns);
        Note(kWMedVolUp,         gMedVolUp);
        Note(kWMedVolDn,         gMedVolDn);
        Note(kWMedMute,          gMedMute);
        Note(kWMedEject,         gMedEject);
        Note(kWMedGetErr,        gMedGetErr);
        Note(kWMedSetErr,        gMedSetErr);
        Note(kWMedLastVol,       gMedLastVol);
        Note(kWMedMuted,         gMedMuted);
        Note(kWMedDropped,       gMedDropped);
        Note(kWMarkRuns,         gMarkRuns);
        Note(kWMarkCleared,      gMarkCleared);
        Note(kWMarkErr,          gMarkErr);
        Note(kWMedHwSetErr,      gMedHwSetErr);
        Note(kWMedHwReadBack,    gMedHwReadBack);
        Note(kWMedDevFound,      gMedDevFound);
        Note(kWLedSends,         gLedSends);
        Note(kWLedRc,            gLedRc);
        Note(kWCapsOn,           gCapsOnMirror);
        Note(kWLedRsp,           gLedRsp);
        Note(kWMedDevId,         gMedDevId);
        Note(kWMedTrapOk,        gMedTrapOk);
        Note(kWMedTrapCalls,     gMedTrapCalls);
        Note(kWMedTrapRc,        gMedTrapRc);
        Note(kWInjMapClobber,    gInjMapClobber);
        Note(kWInjMapLastSys,    gInjMapLastSys);
        Note(kWInjMapLastOur,    gInjMapLastOur);
        Note(kWEjectPress,       gEjectPress);
        Note(kWEjectRelease,     gEjectRelease);
        Note(kWEjectWhich,       gEjectWhich);
        Note(kWRlkCaptured,      gRlkCaptured);
        Note(kWLkCaptured,       gLkCaptured);
        Note(kWRestoreTried,     gRestoreTried);
        Note(kWLkArchived,       (unsigned long)BT_LinkKeyArchivedCount());
        Note(kWDiscReqs,         gDiscReqs);
        Note(kWDiscRc,           gDiscRc);
        Note(kWLinkUp,           (gHciConnHandle != 0) ? 1UL : 0UL);
        Note(kWPagedForgot,      gPagedForgot);
        Note(kWBlockHi,          gBlockHi);
        Note(kWBlockLo,          gBlockLo);
        Note(kWBlockActive,      gBlockActive);
        Note(kWBlockDrops,       gBlockDrops);
        BT_SampleLiveConns();
        Note(kWLiveCount,        gLiveCount);
        Note(kWLiveA0Hi,         gLiveAddr[0][0]);
        Note(kWLiveA0Lo,         gLiveAddr[0][1]);
        Note(kWLiveA1Hi,         gLiveAddr[1][0]);
        Note(kWLiveA1Lo,         gLiveAddr[1][1]);
        Note(kWNameReqs,         gNameReqs);
        Note(kWNameOks,          gNameOks);
        Note(kWNameCount,        gNameCount);
        Note(kWNameDeferred,     gNameDeferred);
        Note(kWInqRetries,       gInqRetries);
        Note(kWInqRecovered,     gInqRecovered);
        {
            /* ⚠ Packed BIG-ENDIAN, four chars per word, because that is how the panel
             * will unpack them and PowerPC makes it the natural order. A name shorter
             * than 24 characters is NUL-padded by the copy in bt_btstack.c, so trailing
             * zero bytes are the terminator rather than garbage. */
            unsigned int si, wi;
            for (si = 0; si < kNameSlots; si++) {
                unsigned int base = (unsigned int)kWNameBase + si * 8;
                Note(base + 0, gNameAddr[si][0]);
                Note(base + 1, gNameAddr[si][1]);
                for (wi = 0; wi < 6; wi++) {
                    unsigned long w = 0;
                    unsigned int k;
                    for (k = 0; k < 4; k++)
                        w = (w << 8) | (unsigned long)gNameText[si][wi * 4 + k];
                    Note(base + 2 + wi, w);
                }
            }
        }
        {   int ci;
            for (ci = 0; ci < kHidConsumerRing; ci++)
                Note(kWHidConsRing + ci, gHidConsumerRing[ci]);
        }
        Note(kWBhGate,         (unsigned long)kHidUseBtstackHost);
        Note(kWBhInit,         gBtstackHostInit);
        Note(kWBhIncoming,     gBhIncoming);
        Note(kWBhCid,          gBhCid);
        Note(kWBhAcceptRc,     gBhAcceptRc);
        Note(kWBhOpened,       gBhOpened);
        Note(kWBhOpenedStatus, gBhOpenedStatus);
        Note(kWBhOpenedMs,     gBhOpenedMs);
        Note(kWBhClosed,       gBhClosed);
        Note(kWBhClosedMs,     gBhClosedMs);
        Note(kWBhSetProtoRsp,  gBhSetProtoRsp);
        Note(kWBhDescAvail,    gBhDescAvail);
        Note(kWBhReports,      gBhReports);
        Note(kWBhIncomingMs,   gBhIncomingMs);
        Note(kWBhFailedOpens,  gBhFailedOpens);
        Note(kWBhFailedStatus, gBhFailedStatus);
        Note(kWBhLive,         gBhLive);
        Note(kWBatSends,       gBatSends);
        Note(kWBatResponses,   gBatResponses);
        Note(kWBatPctRc,       gBatPctRc);
        Note(kWBatPctHs,       gBatPctHs);
        Note(kWBatPctLen,      gBatPctLen);
        Note(kWBatPctVal,      gBatPctVal);
        Note(kWBatStRc,        gBatStRc);
        Note(kWBatStHs,        gBatStHs);
        Note(kWBatStLen,       gBatStLen);
        Note(kWBatStVal,       gBatStVal);
        Note(kWBatPublished,   gBatPublished);
        Note(kWBatFlushes,     gBatFlushes);
        Note(kWBatFlushErr,    gBatFlushErr);
        /* ★ the mouse, and the scan filter */
        Note(kWMouseReports,     gMouseReports);
        Note(kWMouseDecodeOk,    gMouseDecodeOk);
        Note(kWMouseMoves,       gMouseMoves);
        Note(kWMouseBtnChanges,  gMouseBtnChanges);
        Note(kWMouseDropped,     gMouseDropped);
        Note(kWMouseWheelSeen,   gMouseWheelSeen);
        Note(kWCurCreated,       gCurCreated);
        Note(kWCurNewErr,        gCurNewErr);
        Note(kWUnclaimedReports, gUnclaimedReports);
        Note(kWUnclaimedLen,     gUnclaimedLen);
        Note(kWUnclaimedB0,      gUnclaimedB0);
        Note(kWShowAllDevices,   gShowAllDevices);
        Note(kWInqFiltered,      gInqFiltered);
        Note(kWBatWarnings,      gBatWarnings);
        Note(kWAlertsPosted,     gAlertsPosted);
        Note(kWAlertsDropped,    gAlertsDropped);
        Note(kWAlertErr,         gAlertErr);
        /* ★ two-device HID */
        /* v16.1: the sweep is gone; its five words stay retired in place. */
        Note(kWHidDevOpens,      gHidDevOpens);
        Note(kWHidDevCloses,     gHidDevCloses);
        Note(kWHidDevFull,       gHidDevFull);
        {
            unsigned long d[12];
            BT_HidDevSnapshot12(d);
            Note(kWDev0Cid, d[0]);  Note(kWDev0Hi,  d[1]);
            Note(kWDev0Lo,  d[2]);  Note(kWDev0Role, d[3]);
            Note(kWDev0Pct, d[4]);  Note(kWDev0BatHs, d[5]);
            Note(kWDev1Cid, d[6]);  Note(kWDev1Hi,  d[7]);
            Note(kWDev1Lo,  d[8]);  Note(kWDev1Role, d[9]);
            Note(kWDev1Pct, d[10]); Note(kWDev1BatHs, d[11]);
        }
        Note(kWBatTargets,     gBatTargets);
        Note(kWBatGiveUps,     gBatGiveUps);
        Note(kWCurAccelUsed,   (unsigned long)gCurAccelUsed);
        Note(kWCurDevPtr,      (unsigned long)gCurDevPtr);
        Note(kWCurAccelErr,    (unsigned long)gCurAccelErr);
        Note(kWCurButtonsErr,  (unsigned long)gCurButtonsErr);
        Note(kWCurUpiErr,      (unsigned long)gCurUpiErr);
        Note(kWCurMoveErr,     (unsigned long)gCurMoveErr);
        Note(kWCurBtnErr,      (unsigned long)gCurBtnErr);
        Note(kWMouseAbsDx,     gMouseAbsDx);
        Note(kWMouseAbsDy,     gMouseAbsDy);
        Note(kWSniffKbd,       gSniffKbdSlots);
        Note(kWSniffMse,       gSniffMseSlots);
        Note(kWSniffOther,     gSniffOtherSlots);
        Note(kWMouseRatePeak,  gMouseRatePeak);
        Note(kWMouseDxMin,     (unsigned long)(long)gMouseDxMin);
        Note(kWMouseDxMax,     (unsigned long)(long)gMouseDxMax);
        Note(kWMouseDyMin,     (unsigned long)(long)gMouseDyMin);
        Note(kWMouseDyMax,     (unsigned long)(long)gMouseDyMax);
        Note(kWMouseBtnMask,   gMouseBtnMask);
        Note(kWMouseRidOther,  gMouseRidOther);
        Note(kWBhReconnTried,  gBhReconnTried);
        Note(kWBhReconnRc,     gBhReconnRc);
        Note(kWHidOutGate,     (unsigned long)kHidOutgoingConnect);
        Note(kWHidOutTried,    gHidOutTried);
        Note(kWHidOutRc,       gHidOutRc);
        Note(kWHidOutCid,      gHidOutCid);
        Note(kWJustBonded,     gJustBonded);
        Note(kWKfNoFile,       gKfNoFile);
        Note(kWBhOtherEvts,    gBhOtherEvts);
        Note(kWGapLevel,       gGapLevel);
        Note(kWNumCompEvts,    gNumCompEvts);
        Note(kWNumCompTotal,   gNumCompTotal);
        Note(kWNumCompHandle,  gNumCompHandle);
        Note(kWNumCompMs,      gNumCompMs);
        Note(kWBufSizeRc,      gBufSizeRc);
        Note(kWAclBufLen,      gAclBufLen);
        Note(kWAclBufNum,      gAclBufNum);
        Note(kWPipeInt,        gPipeInt);
        Note(kWPipeOut,        gPipeOut);
        Note(kWPipeInB,        gPipeInB);
        Note(kWAuthRingCount, gAuthRingCount);
        {
            int qi;
            for (qi = 0; qi < kAuthRingSlots; qi++) {
                Note(kWAuthRing0   + qi, gAuthRing[qi]);
                Note(kWAuthRingMs0 + qi, gAuthRingMs[qi]);
            }
        }
        Note(kWScanState, gInqState);
        Note(kWScanCount, (unsigned long)BT_ScanPublish(gCB, kWScanBase, 16));
        Note(kWKeyCount,  (unsigned long)BT_KeyPublish(gCB, kWKeyBase, 8));
        Note(kWKeyHandedMask, gLkHandedMask);   /* ⚠ AFTER publish: it sets the mask */
        Note(kWRadioOn, gRadioOn);
        /* The servicer's own counters, so a mailbox that is never picked up is
         * VISIBLE rather than silent. Run 47's failure (seq 9, ack 2) was only
         * diagnosable because the two sequences were both in the block; these say
         * whether the timer is running at all. */
        Note(kWTimerRuns,   gTimerRuns);
        Note(kWMbxServiced, gMbxServiced);
        Note(kWMbxStale,    gMbxStale);
        Note(kWTimerRcTask, gTimerRcTask);
        Note(kWTimerRcIrq,  gTimerRcIrq);
        Note(kWTimerBail,   gTimerBail);
    }
}

/* Was a send accepted? kUSBPending means queued, noErr means done; anything else
 * is a synchronous refusal. Exported so hci_transport_os9.c does not have to know
 * USL error semantics -- it had a hand-written `err == 0 || err == 1` in it, which
 * is exactly the sort of guessed constant that goes wrong quietly. */
int BT_SendWasAccepted(OSStatus err) { return !immediateError(err); }

/* Asked by BTstack's hci_transport can_send_packet_now, once per send. */
int BT_CanSendCommand(void) { return (gDevice  != 0) && !gCmdBusy; }
int BT_CanSendACL(void)     { return (gBulkOut != 0) && !gAclOutBusy; }

void BT_Log(const void *pstr, UInt32 value)
{
    Bump(kWSayCalls);
    Say(gDevice, pstr, value);
}

/* ======================================================================= */
/*  M0.1 -- open interface 0 and find the interrupt-IN pipe.                 */
/*  Driven by completions: usbCompletion == ConfigStep, so each USL call    */
/*  re-enters here with usbRefcon advanced to the next step.                */
/* ======================================================================= */
static void ConfigStep(USBPB *pb)
{
    Bump(kWCfgSteps);
    Note(kWCfgStatus, (unsigned long)pb->usbStatus);
    if ((unsigned long)pb->usbRefcon > gCB[kWStageMax]) Note(kWStageMax, (unsigned long)pb->usbRefcon);
    if (pb->usbStatus != noErr) {
        /* ---- one diagnostic retry, only after the interface search ----------
         * v0.5 passed the Expert's busPower into usbReqCount and STILL got
         * kUSBNotFound (-6987) at this exact point.
         *
         * ⚠ POWER IS FULLY EXONERATED, two independent ways, so do not revisit it.
         * The block reads busPower = 250, and USB.h defines kUSB500mAAvailable =
         * 250: the units are 2 mA, so 250 IS the full 500 mA of a root port, and
         * the dongle has always been on a rear on-board port. Separately, the USL
         * has a distinct error for insufficient power, kUSBDevicePowerProblem,
         * which the 2003 prior art traps on its own; we got kUSBNotFound instead.
         * Passing busPower through is still correct, but it was not the fix.
         *
         * The next question is not answerable by reasoning:
         * does this dongle actually present an interface of class 0xE0/0x01/0x01
         * at all? Its DEVICE descriptor says 0xE0/0x01/0x01 (the Expert reported
         * it), but the device and interface descriptors are different things and
         * a cheap CSR clone need not match.
         *
         * So ask for ANY interface, with 0/0/0 as the wildcard the prior art
         * uses for its pipe searches, and record what comes back. That turns the
         * next run into a measurement of the hardware rather than another guess
         * about our code. */
        if (pb->usbRefcon == kSetConfig && gCB[kWFindAnyTried] == 0) {
            Note(kWFindErr1, (unsigned long)pb->usbStatus);
            Note(kWFindAnyTried, 1);
            InitPB(&gCfgPB, gDevice, ConfigStepAny);
            gCfgPB.usbClassType    = 0;      /* 0 = any, as in the pipe search */
            gCfgPB.usbSubclass     = 0;
            gCfgPB.usbProtocol     = 0;
            gCfgPB.usb.cntl.WValue = 0;
            gCfgPB.usb.cntl.WIndex = 0;
            gCfgPB.usbReqCount     = gBusPower;
            gCfgPB.usbBuffer       = nil;
            gCfgPB.usbFlags        = 0;
            (void)USBFindNextInterface(&gCfgPB);
            return;
        }
        Say(gDevice, "\pOS9BTProbe: M0.1 step failed (status in value)", pb->usbStatus);
        return;
    }

    switch (pb->usbRefcon++)
    {
        case kFindIface:                 /* locate the Bluetooth interface */
            pb->usbClassType    = kBTClass;
            pb->usbSubclass     = kBTSubClass;
            pb->usbProtocol     = kBTProto;
            pb->usb.cntl.WValue = 0;
            pb->usb.cntl.WIndex = 0;
            /* ⚠ usbReqCount is the AVAILABLE BUS POWER in mA, not a byte count.
             * USBFindNextInterface filters on it, so a 0 here means "no power to
             * offer" and an interface that draws 500 mA -- which this dongle does,
             * per Apple System Profiler -- can never be satisfied. It then returns
             * kUSBNotFound (-6987), which is exactly what BTCheck read out of the
             * counter block: stage 1, last usbStatus -6987, M0.1 dead.
             * The 2003 prior art passes PowerAvail here; we passed 0 and had
             * discarded the Expert's busPower argument with #pragma unused. */
            pb->usbReqCount     = gBusPower;
            pb->usbBuffer       = nil;
            pb->usbFlags        = 0;
            if (Failed(USBFindNextInterface(pb))) goto fail;
            break;

        case kSetConfig:                 /* select the configuration found */
            gIfaceNum       = pb->usb.cntl.WIndex;   /* remember interface # */
            pb->usbReqCount = 0;
            if (Failed(USBSetConfiguration(pb))) goto fail;
            break;

        case kNewIfaceRef:               /* device ref -> interface ref */
            pb->usbOther = 0;
            if (Failed(USBNewInterfaceRef(pb))) goto fail;
            break;

        case kSetIface:                  /* SET_INTERFACE (standard request) */
            gIfaceRef = pb->usbReference;   /* NewInterfaceRef's result         */
            pb->usb.cntl.BMRequestType = USBMakeBMRequestType(kUSBNone, kUSBStandard, kUSBInterface);
            pb->usb.cntl.BRequest      = kUSBRqSetInterface;
            pb->usb.cntl.WValue        = 0;
            pb->usb.cntl.WIndex        = gIfaceNum;
            pb->usbReqCount            = 0;
            pb->usbBuffer              = nil;
            if (Failed(USBDeviceRequest(pb))) goto fail;
            break;

        case kConfigIface:               /* open the interface's pipes */
            if (Failed(USBConfigureInterface(pb))) goto fail;
            break;

        /* ⚠⚠ THIS COMMENT WAS WRONG FOR NINE BUILDS AND IS KEPT AS A WARNING. It read:
         *
         *   "⚠ EVERY pipe search must be re-seeded with the INTERFACE reference.
         *    USBFindNextPipe overwrites usbReference with the PIPE it found, so the
         *    next search would otherwise be asking the USL to look for pipes inside a
         *    pipe. That is what broke v1.0 [...] The 2003 prior art chains its three
         *    searches on one PB without re-seeding, so it is NOT a safe model here.
         *    Its L2CAP was a stub and its pipe discovery may never have fully worked
         *    either. Do not copy it."
         *
         * ⇒ APPLE'S OWN SHIPPING SAMPLE DOES EXACTLY WHAT THAT PARAGRAPH FORBIDS.
         * usb-ddk/Examples/USBEnetSample/USBEnetDriver.c:488-506 saves the bulk-OUT
         * ref and then issues the bulk-IN find with that PIPE reference still in
         * usbReference. USBFindNextPipe is a find-NEXT: the PB carries the walk
         * cursor, exactly as USBFindNextInterface does. Re-seeding the interface ref
         * RESTARTS the walk every time, so two consecutive finds can return the same
         * pipe -- and the whole DDK, with working drivers for this era, was sitting in
         * usb-ddk/Examples the entire time. I dismissed the prior art on a guess about
         * its quality instead of reading Apple's.
         * [[feedback_consult_prior_art_constantly]]
         *
         * ⚠ The v1.0 failure the old text cites was an INTERRUPT-to-BULK chain, which
         * on this card crosses nothing Apple demonstrates; Apple's proven chain is
         * bulk-to-bulk within one interface, which is the only one we now rely on.
         * The interrupt find below is still seeded from the interface, deliberately. */
        case kFindIntPipe:               /* find the interrupt-IN pipe */
            pb->usbReference = gIfaceRef;
            pb->usbFlags     = kUSBIn;
            pb->usbClassType = kUSBInterrupt;
            pb->usbSubclass  = 0;
            if (Failed(USBFindNextPipe(pb))) goto fail;
            break;

        case kFindBulkIn:                /* the interrupt-IN pipe just came back */
            /* ⚠ v9.7: CAPTURE WHAT THE EXPERT ACTUALLY GAVE US. Every run has assumed
             * "interrupt-IN, bulk-OUT, bulk-IN" because that is what we asked for and
             * the find succeeded. It has never been checked. Publishing all four
             * descriptor fields sidesteps which one carries the endpoint address --
             * the log will show what the Expert filled in, and a Bluetooth device's
             * endpoints are conventionally 0x81 int-IN, 0x02 bulk-OUT, 0x82 bulk-IN,
             * so a wrong pipe will be obvious rather than argued about.
             * ⚠ IT DID NOT: all four fields are our own inputs plus leftovers, and
             * bulk-OUT and bulk-IN came back byte-identical. See kWRefBulkOut. */
            gPipeInt = ((unsigned long)pb->usbClassType << 24)
                     | ((unsigned long)pb->usbSubclass  << 16)
                     | ((unsigned long)pb->usbProtocol  << 8)
                     |  (unsigned long)pb->usbOther;
            gIntPipe         = pb->usbReference;
            Note(kWRefInt, (unsigned long)gIntPipe);
            Note(kWPipes, gCB[kWPipes] | 0x100);
            /* ★★★★★★ v9.9: FIND BULK-IN FIRST, THEN CHAIN BULK-OUT OFF IT.
             *
             * ⚠⚠ THE ORDER IS THE FIX, and it is robust under BOTH hypotheses:
             *
             *   IF the direction filter honours kUSBOut (== 0), the old order was
             *   already right and this order is equally right -- kUSBIn finds the IN
             *   pipe, kUSBOut finds the OUT pipe, done.
             *
             *   IF the USL reads usbFlags 0 as "unspecified", the OLD order asked for
             *   "first bulk pipe, any direction" FIRST and could hand us the IN pipe as
             *   gBulkOut -- which is exactly the shape of every measurement we have.
             *   This order asks with kUSBIn (== 1, demonstrably honoured, since inbound
             *   ACL genuinely arrives) FIRST, then continues the walk. The next bulk
             *   pipe after the IN one can only be the OUT one.
             *
             * ⇒ So we no longer depend on kUSBOut filtering at all. That is the point:
             * the previous code needed an unverified property of the USL to be true,
             * and this does not.
             *
             * ⚠ AND THE CHAIN IS APPLE'S OWN IDIOM, not the 2003 prior art's. The
             * comment above this block called chaining unsafe and said "Do not copy
             * it" -- but usb-ddk/Examples/USBEnetSample/USBEnetDriver.c:488-506 does
             * EXACTLY this: it saves the bulk-OUT ref and issues the bulk-IN find with
             * that PIPE reference still sitting in usbReference. Apple shipped it.
             * USBFindNextPipe is a find-NEXT and the PB carries the walk cursor, the
             * same way USBFindNextInterface does (see kIfScanMax below). Re-seeding
             * the interface ref RESTARTS the walk, which is the bug this fixes. */
            pb->usbFlags     = kUSBIn;
            pb->usbClassType = kUSBBulk;
            pb->usbSubclass  = 0;        /* 0 = the first one, as Apple's sample does */
            if (Failed(USBFindNextPipe(pb))) goto fail;
            break;

        case kFindBulkOut:               /* the bulk-IN pipe just came back */
            gPipeInB = ((unsigned long)pb->usbClassType << 24)
                     | ((unsigned long)pb->usbSubclass  << 16)
                     | ((unsigned long)pb->usbProtocol  << 8)
                     |  (unsigned long)pb->usbOther;
            gBulkIn          = pb->usbReference;
            Note(kWRefBulkIn, (unsigned long)gBulkIn);
            Note(kWPipes, gCB[kWPipes] | 0x001);
            Note(kWPipeOrder, 1);        /* this build used the v9.9 order */
            /* ⚠ DELIBERATELY NOT RE-SEEDED. usbReference still holds the bulk-IN pipe,
             * so the walk continues from there instead of restarting at the interface. */
            pb->usbFlags     = kUSBOut;
            pb->usbClassType = kUSBBulk;
            pb->usbSubclass  = 0;
            if (Failed(USBFindNextPipe(pb))) {
                /* ⚠⚠ FALL BACK RATHER THAN LOSE THE RUN. v1.0's note records a chained
                 * search taking an immediate error on this card. If that happens the
                 * old re-seeded form is still better than no bulk-OUT at all: it is
                 * what the last nine builds shipped, and a driver with no send pipe
                 * cannot even reach the question we are asking. The error is recorded
                 * so a fallback run is never mistaken for a chained one. */
                Note(kWPipeChainErr, gCB[kWImmErrCode]);   /* Failed() just stashed it */
                Note(kWPipeOrder, 2);    /* 2 = chain refused, fell back to re-seed */
                pb->usbReference = gIfaceRef;
                pb->usbFlags     = kUSBOut;
                pb->usbClassType = kUSBBulk;
                pb->usbSubclass  = 0;
                if (Failed(USBFindNextPipe(pb))) goto fail;
            }
            break;

        case kCfgDone:
            gPipeOut = ((unsigned long)pb->usbClassType << 24)
                     | ((unsigned long)pb->usbSubclass  << 16)
                     | ((unsigned long)pb->usbProtocol  << 8)
                     |  (unsigned long)pb->usbOther;
            gBulkOut = pb->usbReference;
            Note(kWRefBulkOut, (unsigned long)gBulkOut);
            Note(kWPipes, gCB[kWPipes] | 0x010);
            Say(gDevice, "\pOS9BTProbe: M0.1 OK - interrupt-IN and both bulk pipes open", 0);
            ConfigDone();
            break;
    }
    return;

fail:
    Bump(kWImmErr);
    Say(gDevice, "\pOS9BTProbe: M0.1 USL call returned an immediate error", 0);
}

/* ======================================================================= */
/*  Transport ready: arm the event reader, then hand off to the HCI layer.  */
/* ======================================================================= */
/* Completion for the wildcard interface search above. Records what the device
 * really presents and stops. Deliberately does NOT continue configuration: this
 * build is measuring, not fixing. */
/* ⚠ Run 34's finding: this used to record ONE interface and stop, which is why the
 * card looked like a single 03/01/01 HID interface for three runs. USBFindNextInterface
 * is a find-NEXT, so re-issuing it on the same PB continues the walk. Bounded at
 * kIfScanMax and counted -- an unbounded walk against a device that kept returning the
 * same interface would spin at interrupt level with no way out. */
#define kIfScanMax 4

static void ConfigStepAny(USBPB *pb)
{
    unsigned long slot;

    Note(kWFindErr2, (unsigned long)pb->usbStatus);

    if (pb->usbStatus != noErr) {
        /* End of the walk. kUSBNotFound here is the NORMAL terminator, not a fault. */
        Note(kWIfScanErr, (unsigned long)pb->usbStatus);
        FinishIfaceScan();
        return;
    }

    /* Keep kWIfaceFound meaning exactly what it always meant -- the FIRST hit -- so
     * every earlier run's readout stays comparable. */
    if (gCB[kWIfScanCount] == 0)
        Note(kWIfaceFound, ((unsigned long)pb->usbClassType << 24) |
                           ((unsigned long)pb->usbSubclass  << 16) |
                           ((unsigned long)pb->usbProtocol  <<  8) |
                            (unsigned long)pb->usb.cntl.WIndex);

    slot = gCB[kWIfScanCount];
    if (slot < kIfScanMax)
        Note(kWIfScan0 + slot, ((unsigned long)pb->usbClassType << 24) |
                               ((unsigned long)pb->usbSubclass  << 16) |
                               ((unsigned long)pb->usbProtocol  <<  8) |
                                (unsigned long)pb->usb.cntl.WIndex);
    Bump(kWIfScanCount);

    if (gCB[kWIfScanCount] >= kIfScanMax) { FinishIfaceScan(); return; }

    /* Continue the walk. usbClassType/Subclass/Protocol stay 0/0/0 (any) and WIndex
     * carries the position, which the USL advances for us. */
    pb->usbClassType = 0;
    pb->usbSubclass  = 0;
    pb->usbProtocol  = 0;
    pb->usbReqCount  = gBusPower;
    pb->usbBuffer    = nil;
    pb->usbFlags     = 0;
    if (immediateError(USBFindNextInterface(pb))) {
        Note(kWIfScanErr, 0xFFFFFFFFUL);
        FinishIfaceScan();
    }
}

static void ConfigDone(void)
{
    Bump(kWCfgDone);
    /* Arm the interrupt-IN reader first so we can't miss any event.
     *
     * ⚠ v17.6: ONE read, for both devices. v17.3 armed two on anything that was not
     * the A1044; 17.5 showed the stalls it targeted are a symptom of the removal, not a
     * cause of it, so the second read bought nothing and this is back to the shape that
     * ran through v17.2. kWIntDepth is still published -- as a constant 1 -- so a log
     * SAYS which shape produced it rather than leaving the reader to date the build.
     * The epitaph at gIntPB has the measurements. */
    Note(kWIntDepth, 1UL);
    InitPB(&gIntPB, gIntPipe, IntCompletion);
    gIntPB.usbBuffer   = gEvent;
    gIntPB.usbReqCount = kEventBufSize;
    Bump(kWArmInt);
    Bump(kWIntArmed);          /* counted here too, so kWIntArmed is TOTAL arms */
    if (immediateError(USBIntRead(&gIntPB)))
        Say(gDevice, "\pOS9BTProbe: could not arm interrupt read", 0);

    /* Arm the ACL reader too. Nothing sends ACL until L2CAP exists at M2, but a
     * controller may push data at us the moment it is configured, and an unarmed
     * bulk-IN pipe would drop it silently. Armed early, re-armed on completion. */
    ArmAclRead();

    /* ⚠ M2 SWITCHOVER. This used to call HCI_Start(), our own bring-up sequence
     * from M1 (Reset, Read_Local_Version, Read_BD_ADDR, ..., Inquiry). BTstack's
     * hci.c does that job and assumes it owns the controller, so the two cannot
     * both run and ours is retired here rather than left racing.
     *
     * src/hci.c is still compiled and its parser still works; it is simply no
     * longer started. Keeping it means M1's hardware-validated code is one line
     * away if BTstack bring-up has to be bisected against it. */
    BT_StackStart();

    /* ★★ START THE PUMP TIMER LAST, once the stack exists and both readers are armed.
     * Earlier would mean the handler could fire into a half-configured instance; this
     * is the first moment everything it touches is real.
     *
     * ⚠ ConfigDone runs at INTERRUPT level (see the note in ProbeFinalize about the
     * initial arm), and SetPersistentTimer is documented as callable from interrupt
     * level -- it is DriverServicesLib, the same library as
     * CallSecondaryInterruptHandler2, which this file already calls from here. */
    BT_StartPumpTimerIrq();
}

/* ======================================================================= */
/*  M0.3 -- ACL data over the bulk pipes. The L2CAP layer at M2 sits on this. */
/* ======================================================================= */

/* Re-arm the bulk-IN reader. Bounded by the buffer, never by anything the
 * controller claims. */
static void ArmAclRead(void)
{
    OSStatus err;

    if (gBulkIn == 0 || gDeviceGone) return;
    InitPB(&gAclInPB, gBulkIn, AclInCompletion);
    gAclInPB.usbBuffer   = gAclIn;
    gAclInPB.usbReqCount = kACLBufSize;
    Bump(kWAclArmed);
    err = USBBulkRead(&gAclInPB);
    if (immediateError(err)) {
        Bump(kWImmErr);
        Say(gDevice, "\pOS9BTProbe: could not arm the ACL bulk read", 0);
        /* ★ THE CLEAR LIVES HERE NOW, on a REFUSED RE-ARM -- which is exactly r23's
         * case: both readers completed with -6911 and the re-arm was then refused with
         * -6979 kUSBPipeStalledError, and nothing ever cleared it, so one glitch
         * killed the stream permanently.
         *
         * ⚠ ONE clear and ONE retry, never a loop. A refused re-arm produces no
         * completion, so there is nothing to spin on -- but a loop here would be a
         * loop against a device that may be gone. */
        if (isStallClass(err)) {
            ClearStall(gBulkIn, kWAclStallClears);
            gAclInPB.usbStatus = noErr;
            Bump(kWAclArmed);
            if (immediateError(USBBulkRead(&gAclInPB)))
                Note(kWAclStatusFull, (unsigned long)err);
        }
    }
}

static void AclInCompletion(USBPB *pb)
{
    Bump(kWAclInComp);
    Note(kWAclInStatus, (unsigned long)pb->usbStatus);
    Note(kWAclInCount,  (unsigned long)pb->usbActCount);

    if (pb->usbStatus == noErr && pb->usbActCount >= 4) {
        Note(kWAclTotal, gCB[kWAclTotal] + pb->usbActCount);
        BT_DeliverPacket(0x02 /* HCI_ACL_DATA_PACKET */, gAclIn,
                         (unsigned short)pb->usbActCount);
        /* HCI ACL header: handle+flags (LE 16), then length (LE 16). */
        Note(kWAclHdr, (unsigned long)(gAclIn[0] | (gAclIn[1] << 8) |
                                      ((UInt32)gAclIn[2] << 16) |
                                      ((UInt32)gAclIn[3] << 24)));
    }

    /* Re-arm unless we are being torn down, exactly as the event reader does. */
    /* ⚠ SAME STALL CONTRACT AS THE INTERRUPT PIPE. Run 23 showed BOTH readers
     * completing with kUSBNotRespondingErr, so the bulk-IN pipe needs the clear
     * exactly as much -- and it carries every L2CAP byte, so a dead bulk-IN pipe
     * means a dead keyboard even if events still flow. */
    Note(kWAclStatusFull, (unsigned long)pb->usbStatus);

    /* ★★★★ AN EMPTY BULK-IN READ IS NOT A STALL. This is the whole of run 50.
     *
     *     ACL read completions 1708      bulk-IN stalls cleared 1703
     *     last ACL read status -6911     last ACL byte count 0
     *     Initialize calls 14            Finalize calls 13
     *
     * The bulk-IN pipe completes with kUSBNotRespondingErr and ZERO BYTES whenever it
     * has nothing to deliver, which on an idle radio is essentially always. -6911 is
     * in the stall family, so every empty read was met with
     * USBClearPipeStallByReference -- 1703 clears and 1721 re-arms in one session, a
     * hot loop hammering an entirely healthy dongle.
     *
     * ⭐ AND THAT EXPLAINS THE CYCLING, INVERSELY TO EXPECTATION:
     *     run 49, phone connected, real ACL traffic:  411 reads,  397 clears,  3 cycles
     *     run 50, no phone, pipe always idle:        1708 reads, 1703 clears, 14 cycles
     * More idle means more empty reads means more thrash means more devices dropping
     * off the bus. Asking for a quiet radio to get clean numbers made it worse, which
     * is why the measurement was worth taking.
     *
     * ⚠ r23's LESSON IS PRESERVED, AND THIS IS THE PRECISE CORRECTION TO IT. r23 saw
     * both readers complete with -6911 AND THE RE-ARM THEN REFUSED with -6979. The
     * clear belongs on the RE-ARM REFUSAL, which is where the interrupt path has
     * always put it -- never on a completion status, which cannot distinguish "stalled"
     * from "nothing to read". ArmAclRead now carries it. */
    if (pb->usbStatus == kUSBNotRespondingErr && pb->usbActCount == 0)
        Bump(kWAclEmptyReads);

    /* ⚠ gDeviceGone was added to the interrupt reader in v3.7 and NOT here -- one of
     * two readers fixed, which with 13 removals a session left this one re-arming a
     * dead pipe every time. */
    if (pb->usbStatus != kUSBAbortedError && !gDeviceGone) {
        /* ★★★ DO NOT RE-ARM WHEN NO ACL LINK EXISTS. This is the treadmill fix.
         *
         * Run 51 measured the cost precisely: 286 empty reads, 286 REFUSED re-arms,
         * 285 successful stall clears, 573 arms -- every packet slot on an idle pipe
         * costs arm, refusal, clear, arm, and all of it at interrupt level. Run 51
         * also proved the clears are REQUIRED rather than spurious, so the only way
         * to stop paying for them is to stop polling a pipe that cannot have data.
         *
         * With no ACL link there is by definition nothing to receive, so the poll is
         * pure waste. The trigger to start again is HCI_EVENT_CONNECTION_COMPLETE,
         * which arrives on the INTERRUPT pipe -- and that pipe is never disarmed, so
         * the wake-up path cannot be lost.
         *
         * ⚠ DELIBERATELY FAIL-SAFE, because this touches the pipe that carries every
         * L2CAP byte and run 49 proved that path works. The initial arm at ConfigDone
         * is UNCHANGED, so behaviour with a link present is byte-for-byte what it was;
         * the only change is that an idle pipe is allowed to go quiet instead of being
         * re-armed forever. If the link count is ever wrong in the direction of "too
         * many", the old behaviour returns rather than data being dropped. */
        if (gAclLinks > 0 || gCB == NULL)
            ArmAclRead();
        else
            Bump(kWAclIdleParked);
    }
}

/* ★ Called from the BTstack glue on connection and disconnection complete. The ACL
 * reader is armed on the way up and allowed to lapse on the way down. */
void BT_AclLinkUp(void)
{
    gAclLinks++;
    Bump(kWAclLinkUps);
    /* Arm immediately: L2CAP signalling follows the connection event closely, and an
     * unarmed bulk-IN pipe would drop the first packet silently. */
    ArmAclRead();
}

void BT_AclLinkDown(void)
{
    /* ⚠ Never below zero. A disconnection complete for a link we never counted --
     * BTstack can emit one for a failed connection attempt -- must not wrap the count
     * into a huge positive and re-enable the treadmill for the rest of the boot. */
    if (gAclLinks > 0) gAclLinks--;
    Bump(kWAclLinkDowns);
}

static void AclOutCompletion(USBPB *pb)
{
    gAclOutBusy = 0;             /* clear BEFORE notifying, so hci_run can send */
    Bump(kWAclOutComp);
    Note(kWAclOutStatus, (unsigned long)pb->usbStatus);
    /* ⭐⭐ v10.0: THE BYTE COUNT, WHICH TEN BUILDS RECORDED A STATUS WITHOUT. A status
     * of 0 says the USL completed the request; it does NOT say it moved any bytes.
     * Compare against kWAclOutReq: equal means the bytes reached the endpoint and the
     * fault is at or beyond the controller; 0 or short means we never got them out of
     * the host and every controller-side theory has been chasing a phantom. */
    Note(kWAclOutAct, (unsigned long)pb->usbActCount);
    BT_NotifyPacketSent();
}

/* Send one HCI ACL data packet. The M2 L2CAP layer calls this; nothing does yet,
 * which is why it is exported rather than static. */
OSStatus BT_SendACL(const void *pkt, UInt32 len)
{
    if (gBulkOut == 0) return kUSBNotFound;
    if (len == 0 || len > kACLBufSize) return paramErr;
    InitPB(&gAclOutPB, gBulkOut, AclOutCompletion);
    gAclOutPB.usbBuffer   = (void *)pkt;
    gAclOutPB.usbReqCount = len;
    gAclOutBusy = 1;
    Bump(kWAclSent);
    Note(kWAclOutReq, len);      /* v10.0: the oracle for kWAclOutAct */
    {
        OSStatus err = USBBulkWrite(&gAclOutPB);
        if (immediateError(err)) gAclOutBusy = 0;   /* never wedge the pipe shut */
        return err;
    }
}

static void CmdCompletion(USBPB *pb)
{
    gCmdBusy = 0;                /* clear BEFORE notifying, so hci_run can send */
    Bump(kWCmdComp);
    Note(kWCmdStatus, (unsigned long)pb->usbStatus);
    BT_NotifyPacketSent();
    if (pb->usbStatus != noErr)
        Say(gDevice, "\pOS9BTProbe: HCI command control-transfer error (status in value)", pb->usbStatus);
}

/* ---- pipe stall recovery ------------------------------------------------- *
 * ⚠ RUN 23 IS WHY THIS EXISTS, and it was a permanent death, not a hiccup.
 *
 * Both readers completed with kUSBNotRespondingErr (-6911, "Pipe stall, No device,
 * device hung") and the blind re-arm was then refused with kUSBPipeStalledError
 * (-6979). MacErrors.h spells out the contract in the constant's own comment:
 * "Pipe has stalled, ERROR NEEDS TO BE CLEARED". We never cleared it, so the event
 * stream stopped for good and only a reboot brought it back. For a keyboard driver
 * that is the difference between a glitch and a dead keyboard.
 *
 * The stall family is a contiguous run in MacErrors.h -- kUSBUnderRunErr (-6907)
 * through kUSBCRCErr (-6915), every one of them documented as a pipe stall or a
 * hung device -- plus kUSBPipeStalledError (-6979) itself. kUSBAbortedError is
 * deliberately NOT in it: that one means we are being torn down and must stop. */
/* ⚠⚠⚠ THE WINDOW USED TO START AT -6907 AND THAT WAS WRONG.
 *
 * Run 49 recorded 411 ACL read completions and 397 "stall clears" -- very nearly one
 * clear per read, on a pipe that was demonstrably healthy: it carried a full SDP
 * browse of 977 bytes in 14 records and completed a pairing. A pipe that is genuinely
 * stalled does not deliver 411 completions.
 *
 * The cause is the top two entries of the old range, and MacErrors.h says it plainly:
 *
 *     kUSBUnderRunErr  -6907   "Less data than buffer"
 *     kUSBOverRunErr   -6908   "Packet too large or more data than buffer"
 *
 * ⭐ An under-run is the NORMAL outcome of a bulk-IN read whose buffer is bigger than
 * the packet -- 1537 bytes across 411 reads is under four bytes each, so nearly every
 * read was short. We were calling USBClearPipeStallByReference on a working pipe
 * hundreds of times per session.
 *
 * Everything the header actually describes as a stall stays in, so r23's case is
 * untouched: -6909 through -6915 all say "Pipe stall" or "Device didn't understand",
 * and -6911 kUSBNotRespondingErr -- the status r23's dead readers returned -- is still
 * treated as a stall. Only "less data than I asked for" and "more data than I asked
 * for" stop being faults, because they never were. */
static Boolean isStallClass(OSStatus err)
{
    if (err == kUSBAbortedError) return false;
    if (err == kUSBUnderRunErr || err == kUSBOverRunErr) return false;
    if (err <= -6909 && err >= -6915) return true;      /* the real pipe-stall family */
    if (err == kUSBPipeStalledError) return true;
    return false;
}

/* Clear a stall and report it. USBClearPipeStallByReference takes only the pipe ref
 * -- no parameter block, no completion -- and the audit already classifies it as an
 * interrupt-safe async USL call. */
/* ⚠⚠⚠ A RECOVERY PATH MUST GIVE UP WHEN THE THING IT IS RECOVERING IS GONE.
 *
 * Run 48 measured 212 interrupt-pipe stall clears and 691 bulk-IN stall clears across
 * five bind/unbind cycles, and the telling values were:
 *
 *     last clear rc      0xFFFFE4AB = -6997 kUSBUnknownPipeErr  "pipe ref not recognised"
 *     re-arm err (full)  0xFFFFE4AB = -6997
 *     last int status    0xFFFFE4BA = -6982 kUSBAbortedError    "pipe aborted"
 *
 * So we were clearing stalls and re-arming reads on a pipe that NO LONGER EXISTS --
 * roughly 180 futile attempts per session, after the device had already gone away.
 *
 * ⭐ This is the exact inverse of the rule that got the stall handling written in the
 * first place. r23 taught that a stalled pipe which is never cleared is permanent
 * death, so we clear. But a pipe whose DEVICE has been removed cannot be cleared, and
 * retrying is not persistence, it is a spin. Both halves are needed: recover a live
 * pipe, and stop the moment the pipe is dead.
 *
 * gDeviceGone is set by ProbeNotify on kNotifyRemoveDevice, which run 48 shows firing
 * five times (last notification 0x0000000B). Its DEFINITION sits further up, above
 * the ACL reader, because both readers consult it and a file-scope static cannot be
 * forward-declared. */

static Boolean pipeIsDead(OSStatus err)
{
    return err == kUSBUnknownPipeErr ||     /* -6997 the ref is not recognised */
           err == kUSBUnknownDeviceErr ||   /* -6998 nor is the device         */
           err == kUSBNoDeviceErr;          /* -6990 there is no device        */
}

static void ClearStall(USBPipeRef pipe, short whichCounter)
{
    OSStatus rc;
    if (pipe == 0 || gDeviceGone) return;
    Bump(whichCounter);
    /* ★ v17.2: timestamp the FIRST and the LATEST stall. The spread between them, read
     * against kWFinalizeMs, is what separates "the stalls caused the removal" from
     * "the removal caused the stalls" -- see the note at kWStallFirstMs.
     *
     * ⚠⚠ FIRST-WINS, AND NoteKeep IS THE WRONG TOOL FOR IT. NoteKeep overwrites on any
     * non-zero value -- it only refuses to let a ZERO erase a non-zero -- so using it
     * here would have stamped the LATEST stall into the word labelled FIRST, silently
     * collapsing every spread to zero and making case A look like case B. The word has
     * to be written once and then left alone, which is a plain guarded store.
     *
     * ★ Writing it into the shared block directly also means it survives a teardown,
     * which is the property that matters: the window has to span instances.
     *
     * hal_time_ms is Microseconds(), already cleared for this level in
     * btstack_config.h. */
    {
        unsigned long now = (unsigned long)hal_time_ms();
        if (gCB != NULL && gCB[kWStallFirstMs] == 0) Note(kWStallFirstMs, now);
        Note(kWStallLastMs, now);
    }
    rc = USBClearPipeStallByReference(pipe);
    Note(kWStallClearRc, (unsigned long)rc);
    /* ⚠ Latch on a DEAD pipe, and only on a dead pipe. Any other failure is left
     * retryable, because that is the r23 lesson and it still holds. */
    if (pipeIsDead(rc)) {
        gDeviceGone = true;
        Bump(kWDeadPipeStops);
    }
}

static void IntCompletion(USBPB *pb)
{
    /* ⚠ v17.6: one reader again, so there is one buffer. `evt` stays as a local rather
     * than reverting to gEvent everywhere: it reads the same, and if a future build
     * ever does need more than one read posted, the only thing that has to change is
     * this line. The slot arithmetic and its clamp are gone with the experiment. */
    UInt8 *evt = gEvent;

    Bump(kWIntComp);
    Note(kWIntStatus, (unsigned long)pb->usbStatus);
    Note(kWIntStatusFull, (unsigned long)pb->usbStatus);
    Note(kWActCount, (unsigned long)pb->usbActCount);

    /* ★ v17.5: record EVERY completion, in order. See kWCompRing0 -- three inferred
     * mechanisms have been wrong, so this stops inferring and keeps the evidence. */
    if (gCB != NULL) {
        unsigned long i = gCB[kWCompRingIdx] & 7UL;
        gCB[kWCompRing0 + i] = (((unsigned long)pb->usbStatus & 0xFFUL) << 24)
                             |  ((unsigned long)pb->usbActCount & 0xFFFFUL);
        gCB[kWCompRingIdx] = (i + 1UL) & 7UL;
    }
    if (pb->usbStatus == noErr && pb->usbActCount >= 2) {
        Note(kWLastEvent, (unsigned long)(evt[0] | (evt[1] << 8)));
        Note(kWEvt0, (unsigned long)((evt[0] << 24) | (evt[1] << 16) |
                                     (evt[2] << 8)  |  evt[3]));
        Note(kWEvt1, (unsigned long)((evt[4] << 24) | (evt[5] << 16) |
                                     (evt[6] << 8)  |  evt[7]));
    }
    /* Up into BTstack. HCI_HandleEvent (our M1 parser) is deliberately no longer
     * called: BTstack's hci.c owns the controller now. */
    /* ★ v17.4: time the trip into BTstack, split by event kind. See kWDelivMaxCmdUs.
     *
     * ⚠ us.lo only. Microseconds() is 64-bit and wraps the low word about every 71
     * minutes; a delta across that wrap would read as a huge bogus maximum, so it is
     * discarded rather than recorded. One lost sample per 71 minutes against a
     * guaranteed-wrong outlier is the right trade for a maximum. */
    if (pb->usbStatus == noErr && pb->usbActCount >= 2) {
        UnsignedWide t0, t1;
        Microseconds(&t0);
        BT_DeliverPacket(0x04 /* HCI_EVENT_PACKET */, evt,
                         (unsigned short)pb->usbActCount);
        Microseconds(&t1);
        if (t1.lo >= t0.lo) {                       /* no wrap in between */
            unsigned long dt = t1.lo - t0.lo;
            short isCmd = (evt[0] == 0x0E || evt[0] == 0x0F);
            short w = isCmd ? kWDelivMaxCmdUs : kWDelivMaxUnsolUs;
            Note(kWDelivLastUs, dt);
            if (gCB != NULL && dt > gCB[w]) Note(w, dt);
        }
    }

    /* Count events the controller sent US, unprompted. An HCI event is a command
     * response only if it is Command Complete (0x0E) or Command Status (0x0F);
     * anything else -- Inquiry Result, Inquiry Complete, Connection Request,
     * Disconnection Complete -- is the controller talking on its own initiative.
     *
     * ⚠ This has been ZERO in every run to date, which is why run 16's silence
     * after the Inquiry was not diagnosable: we have never proved this transport
     * can deliver an unsolicited event at all. */
    if (pb->usbStatus == noErr && pb->usbActCount >= 2
        && evt[0] != 0x0E && evt[0] != 0x0F)
        Bump(kWUnsolEvents);

    /* Re-arm the reader unless we're being torn down.
     *
     * ⚠ THE RETURN VALUE IS CHECKED NOW, and it must be. It was discarded, so a
     * re-arm that failed immediately would end the event stream permanently and
     * silently -- the block would look exactly like a controller that had gone
     * quiet, which is precisely the ambiguity that made run 16 undiagnosable.
     * Counting arms and recording the error separates the two for good. */
    /* ⚠⚠ AND NOT IF THE DEVICE IS GONE. The abort check alone was not enough: run 48
     * shows the re-arm failing with kUSBUnknownPipeErr, which means completions were
     * arriving with statuses OTHER than kUSBAbortedError after the device had been
     * removed, and each one re-armed a dead pipe. gDeviceGone closes that. */
    if (pb->usbStatus != kUSBAbortedError && !gDeviceGone) {
        OSStatus err;

        /* ⚠⚠ THE COMPLETION-STATUS CLEAR IS GONE, for the same reason as the ACL
         * reader's: a completion status cannot distinguish "the pipe is stalled" from
         * "there was nothing to read". Run 50 recorded 529 interrupt-pipe clears
         * against 817 completions on a working controller.
         *
         * The clear now happens only where r23's evidence actually put it -- on a
         * RE-ARM THAT IS REFUSED, below. That path was already here and already
         * correct; the redundant one above it was doing the damage. */
        if (pb->usbStatus == kUSBNotRespondingErr && pb->usbActCount == 0)
            Bump(kWIntEmptyReads);

        gIntPB.usbBuffer   = evt;
        gIntPB.usbReqCount = kEventBufSize;
        gIntPB.usbStatus   = noErr;
        Bump(kWIntArmed);
        err = USBIntRead(&gIntPB);

        /* ⚠ ONE clear-and-retry, never a loop. Each re-arm is asynchronous, so a
         * failed one simply means no completion arrives -- there is nothing to spin
         * on. Retrying once here recovers the case where the stall was still latched
         * when we first asked, without ever becoming a latching guard.
         *
         * ⚠ HONEST LIMIT: if this retry also fails there is no further completion to
         * recover from, so the reader stays dead until the extension reloads. Closing
         * that needs a timer-driven re-arm, which is the Time Manager pump already on
         * the M3 roadmap (docs/M3-DESIGN.md §4). The counters below make the state
         * visible instead of silent, which is the part that was missing. */
        if (immediateError(err)) {
            Note(kWIntArmErr, 0x100UL | ((unsigned long)err & 0xFFUL));
            Note(kWIntArmErrFull, (unsigned long)err);
            if (isStallClass(err)) {
                ClearStall(gIntPipe, kWIntStallClears);
                gIntPB.usbStatus = noErr;
                Bump(kWIntArmed);
                err = USBIntRead(&gIntPB);
                if (immediateError(err)) {
                    Note(kWIntArmErr, 0x100UL | ((unsigned long)err & 0xFFUL));
                    Note(kWIntArmErrFull, (unsigned long)err);
                }
            }
        }
    }
}

/* ======================================================================= */
/*  USB Expert dispatch-table procs                                         */
/* ======================================================================= */
static OSStatus ProbeValidateHW(USBDeviceRef device, USBDeviceDescriptor *desc)
{
#pragma unused (desc)
    EnsureBlock();          /* task level -- the Expert's first call into us */
    Bump(kWValidate);
    /* ⚠⚠⚠ v10.4 CALLED BT_InjectInit() HERE AND THE DRIVER DID NOT LOAD AT ALL.
     *
     * The v10.4 run: card switched to 05AC:8204, switcher VERDICT *** SWITCHED ***,
     * and NO BLOCK FOUND. EnsureBlock() is the FIRST statement in this function, so a
     * missing block means ValidateHW did not complete -- the fragment never came up.
     *
     * ⇒ AND THE PRIOR ART SAYS WHY I SHOULD NOT HAVE PUT IT HERE. Apple calls
     * InitUSBKeyboard() from exactly ONE place: USBHIDControlDevice's
     * kHIDEnableDemoMode case (KBDHIDEmulation.c:70), in response to a control request
     * that arrives LONG after load. It is never called during driver load, and Apple's
     * driver therefore never touches the Resource Manager on the load path. I read
     * InitUSBKeyboard, transcribed what it does, and did not check WHEN it is called.
     *
     * ⚠ THIS IS A THEORY, NOT A CONFIRMED CAUSE. Removing this one call is a
     * single-variable experiment and the existing counters are the discriminator: a
     * block that appears at all means ValidateHW now completes, and kWValidate /
     * kWInit then say how far bring-up got.
     *
     * ⇒ KCHR is now acquired LAZILY AT TASK LEVEL through the defer trampoline, on
     * first need -- see BT_InjectKeyEvent and BT_DeferredSwitchIfPending. That is
     * on-demand like Apple's, and off the load path entirely. */
    Say(device, "\pOS9BTProbe: ValidateHW - matched a Bluetooth HCI device", 0);
    return noErr;
}

/* ======================= M0.0  THE CSR MODE SWITCH ==========================
 * Full derivation in docs/M0-MODE-SWITCH.md. Nothing here is guessed: BlueZ's
 * tools/hid2hci.rules maps idVendor 0a12|0458|05ac + idProduct 1000 to
 * --method=csr, and 05ac:1000 is this card. The rule groups it with CSR's own
 * VID (0a12, which is also our dongle's), so the A1044 is a CSR BlueCore behind
 * an Apple ID.
 *
 * Run 32 proved the switch does not survive a restart, which matches BlueZ's and
 * Windows hid2hci's independent statements that it must be re-run every boot. So
 * this is not a one-time provisioning step -- it is part of every bring-up.
 */
/* ⚠⚠⚠ v2.6 SENDS NO MODE SWITCH. THIS IS DELIBERATE AND IS THE WHOLE POINT OF THE BUILD.
 *
 * v2.5 walked the interfaces and then switched, and the card left the USB bus and did not
 * come back -- not through a power cycle (run 36). Booting Tiger cleared it and the card
 * returned to normal HID-proxy (run 37), so it is a recoverable hang rather than damage.
 * But WHICH of v2.5's three changes caused it was never isolated, because v2.5 changed
 * three things at once: it added the walk, it moved the switch from task level to interrupt
 * level, and it added rule 3.
 *
 * This build changes ONE thing: it walks and stops. The measurement we actually wanted --
 * what interfaces 1 and 2 of the card are -- never needed a switch to obtain it.
 *
 * The switch code below is KEPT, not deleted, so the researched request (docs/M0-MODE-SWITCH.md)
 * survives with its derivation. kSendModeSwitch is the single gate. Turning it back on is a
 * deliberate act that should come with its own run plan and the Tiger recovery step written
 * into it. */
/* ⚠⚠ kSendModeSwitch IS DEFINED ONCE, NEAR THE TOP, BESIDE THE kA1044* IDENTITIES.
 *
 * It used to be defined HERE as well, and the two definitions disagreed: the top one
 * said 0 and this one said 1, so this later definition silently won for everything
 * below it. A gate that exists in two places is not a gate. Moved and deduplicated;
 * do not reintroduce a second definition.
 *
 * History kept, since the reasoning still matters: v2.7 turned the switch on because
 * run 38 had measured the proxy personality completely (two HID interfaces, keyboard
 * and mouse, nothing else), so there was no longer any reason to walk the card first
 * -- and walking-then-switching was one of the sequences that hung it. The switch is
 * sent the way v2.4 sent it, immediately from ProbeInitialize at task level, which
 * produced 8202 with the card healthy in run 33.
 *
 * ⚠ The recovery, from run 37, belongs in every run plan that touches the card's
 * mode: if the card goes missing from the OS 9 bus, BOOT TIGER AND BOOT BACK. */

/* ⚠ Re-entry guard for the SCAN's parameter block. gSwitchPB got one of these in v2.4 and
 * gCfgPB did not, which is the same defect class: a second StartIfaceScan while a walk is in
 * flight would re-initialise a PB the USL still owns. Run 31 showed Initialize being called
 * once, so this is hardening rather than a diagnosed cause -- but "not currently reachable"
 * is not a reason to leave the sharp edge in. */
static Boolean gScanBusy = false;

static void    ConfigStepAny(USBPB *pb);
static Boolean CsrModeSwitch(USBDeviceRef device, UInt16 mode);
static void    FinishIfaceScan(void);
static Boolean immediateError(OSStatus err);

/* Walk every interface of `device`, recording each into the kWIfScan slots. */
static void StartIfaceScan(USBDeviceRef device)
{
    if (gScanBusy) { Note(kWIfScanErr, 0xFFFFFFFDUL); return; }
    gScanBusy = true;

    Note(kWIfScanCount, 0);
    InitPB(&gCfgPB, device, ConfigStepAny);
    gCfgPB.usbClassType    = 0;      /* 0/0/0 = any, the prior art's wildcard */
    gCfgPB.usbSubclass     = 0;
    gCfgPB.usbProtocol     = 0;
    gCfgPB.usb.cntl.WValue = 0;
    gCfgPB.usb.cntl.WIndex = 0;
    gCfgPB.usbReqCount     = gBusPower;
    gCfgPB.usbBuffer       = nil;
    gCfgPB.usbFlags        = 0;
    if (immediateError(USBFindNextInterface(&gCfgPB))) {
        Note(kWIfScanErr, 0xFFFFFFFEUL);
        /* ⚠ Fail open: the completion will never run, so the switch would never be
         * sent and this instance would sit on the card doing nothing at all. */
        FinishIfaceScan();
    }
}

static void FinishIfaceScan(void)
{
    /* ⚠ Released FIRST, on every path, so a walk that ended in an error cannot strand the
     * scanner for the rest of the boot. [[reference_os9_recovery_paths_fail_open]] */
    gScanBusy = false;

    /* ⚠⚠ THE WALK NEVER SWITCHES ANYTHING. v2.5 chained a switch onto this terminator,
     * which put CsrModeSwitch at INTERRUPT level and is one of the two changes that could
     * have hung the card. The chain is gone: the switch is sent from ProbeInitialize at
     * task level, exactly as in v2.4, and this function now only releases the guard.
     * ⇒ CsrModeSwitch is task-level-only again, so the static audit no longer has to
     * reason about it at all. */
}

enum { kSwNone = 0, kSwSwitched = 1, kSwIgnored = 2, kSwOtherErr = 3, kSwStalled = 4,
       /* ★★★ v8.5: the REVERSE trip, HCI -> HID-proxy. Distinct codes so the log can
        * never confuse the two directions -- the four kWSw* words are shared, and a
        * reader seeing "SWITCHED" would otherwise have no way to tell which way. */
       kSwToProxyOk      = 5,   /* timed out, i.e. the card DID change personality  */
       kSwToProxyIgnored = 6,   /* completed cleanly, i.e. the card refused          */
       kSwToProxyErr     = 7,   /* anything else                                     */
       kSwToProxyBusy    = 8 }; /* asked while a request was already in flight       */

/* ⚠⚠⚠ BACK TO ONE. Apple sends this request seven times (§9c) and copying that is
 * what broke run 43.
 *
 * The evidence, and it fits every data point we have:
 *
 *   ONE switch and nothing else     -> card healthy at 8202   runs 33, 39, 42
 *   walk the card, then switch      -> card ABSENT             run 35
 *   switch, then walk the card      -> card ABSENT             run 40
 *   switch SEVEN times              -> card ABSENT             run 43
 *
 * Run 42's counters read `switch attempts 1, retries deferred 0` and the card was
 * fine; v3.1 raised the cap to 7 and the card stopped enumerating. That is a clean
 * A/B with one variable.
 *
 * ⇒ The empirical rule is simply: ANY EXTRA USB TRAFFIC TO THIS CARD AROUND THE
 * SWITCH leaves it unable to enumerate. Not a mechanism -- a measured regularity,
 * which is more than any of the six mechanisms proposed for this card managed.
 *
 * Why Apple can send seven and we cannot: its transition driver wins the device with
 * a probe score of 60000 (§9e), holds it open, and issues the requests synchronously
 * inside start(). We chain ours through asynchronous completions at a device that has
 * already begun going away. Same request, different conditions -- and §7's lesson was
 * exactly this, that prior art's safety margins belong to the prior art's conditions.
 *
 * ⚠ DO NOT RAISE THIS WITHOUT A RUN THAT MEASURES IT. The cost of being wrong is a
 * card that stops enumerating, which is gate 1's failure mode
 * (docs/RELEASE-GATES.md) and a firmware lockout for anyone whose only keyboard is
 * Bluetooth. */
/* ★★★ RAISED 1 -> 7 ON 2026-09-06, AND THE WARNING ABOVE IS BEING OBEYED RATHER THAN
 * OVERRIDDEN. It said "DO NOT RAISE THIS WITHOUT A RUN THAT MEASURES IT". That run
 * happened, and here is what it measured:
 *
 *   ONE attempt moves the card from 05AC:1000 to 05AC:8204 and leaves it there. Apple
 *   System Profiler sees the device and reports "Driver name: Not available", but
 *   USBGetNextDeviceByClass -- the enumeration drivers are MATCHED against -- cannot
 *   see it at all. It listed the card as three entries at 1000 and lists nothing after
 *   the switch, across two separate runs. So no descriptor rule can bind it: v6.0's
 *   rule 1b for 05AC:8204 was not wrong, it was unreachable.
 *
 * ⇒ One request half-transitions the card. Apple sends SEVEN (§9c), and the state we
 * were parking in is the cost of stopping at one, not evidence that continuing is
 * dangerous. The failure the old cap guarded against has ALREADY HAPPENED at one
 * attempt; completing the sequence is the way out of it rather than deeper in.
 *
 * ⚠ THE RETRY CHAIN ITSELF NEEDED NO CHANGE. SwitchCompletion already ends in
 * "if (st != noErr && gCB[kWSwTried] < kSwMaxTries) CsrModeSwitch(gDevice);" -- chained
 * from the completion, back to back, deliberately not deferred, exactly matching
 * Apple's unrolled sends. It covers the timeout path our stack reports where Apple's
 * reports a stall. The cap was the only thing holding it at one, which is why this is a
 * one-line change and not a rewrite.
 *
 * ⚠ A clean noErr still stops early: that means the card ignored the request, and
 * resending an ignored request is pointless. */
#define kSwMaxTries 7

/* ⚠ Set at task level, cleared at task level, read by the trampoline. volatile
 * because the interrupt-level completion sets it again for each retry. */
static volatile Boolean gSwitchPending = false;

static USBPB   gSwitchPB;
static Boolean gSwitchBusy = false;   /* gSwitchPB is in flight; see CsrModeSwitch */
/* ★★★ v8.5: WHICH DIRECTION THE IN-FLIGHT REQUEST IS GOING.
 *
 * The completion has to interpret its own status differently per direction -- not the
 * meaning of the status, which is the same (timeout = the card changed personality),
 * but which verdict code to record. Read at interrupt level from the completion,
 * written at task level before the send, hence volatile. */
static volatile UInt16 gSwitchMode = 0;    /* 0 = to HCI, 1 = to HID-proxy */

/* ⚠⚠ DO NOT ROUTE THIS THROUGH Failed(). Failed() treats any nonzero usbStatus as
 * fatal, and here the FAILING status IS THE SUCCESS CASE. See the enum comment at
 * kWSwTried. */
static void SwitchCompletion(USBPB *pb)
{
    OSStatus st = pb->usbStatus;

    gSwitchBusy = false;              /* released FIRST -- see the fail-open rule */
    Note(kWSwStatus, (unsigned long)st);

    /* ★★★ THE STALL PATH -- the whole reason v2.9 exists.
     *
     * Apple's CSRHIDTransitionDriver tests the request result against
     * kIOUSBPipeStalled and, on getting it, clears the stall on pipe zero and
     * re-sends. Up to v2.8 we filed -6979 under "other error" and did nothing, which
     * left the DEFAULT CONTROL PIPE STALLED. A device whose control pipe is stalled
     * cannot be enumerated -- which is exactly the card vanishing from the bus in
     * runs 35 and 40, and why walking made it worse, since USBFindNextInterface
     * drives that same pipe.
     *
     * ⚠ This project already had the rule, from r23:
     * [[reference_os9_usb_pipe_stall_must_be_cleared]] -- a stalled pipe that is
     * never cleared is PERMANENT DEATH. v2.2 applied it to the interrupt and bulk-IN
     * pipes and never to the control pipe.
     *
     * The device reference IS the default control pipe's reference in this API, which
     * is why gSwitchPB.usbReference is the same gDevice we clear here. */
    if (st == kUSBPipeStalledError) {
        Bump(kWSwStalls);
        Note(kWSwVerdict, kSwStalled);
        Note(kWSwClearRc, (unsigned long)USBClearPipeStallByReference(gDevice));

        /* Retry -- but hand it back to TASK LEVEL through the trampoline rather than
         * re-entering from this completion. BT_DeferRequest is built to be called
         * from interrupt level and coalesces, so this is the sanctioned path.
         *
         * ⚠ v8.5: THE STALL CLEAR ABOVE IS RIGHT IN BOTH DIRECTIONS -- a control pipe
         * left stalled is permanent death [[reference_os9_usb_pipe_stall_must_be_cleared]]
         * -- but the RETRY is forward-only. On the way back to proxy there is nothing
         * to retry against: if the card took the request its personality has already
         * changed and our device reference is stale. */
        if (gSwitchMode == 0 && gCB[kWSwTried] < kSwMaxTries) {
            gSwitchPending = true;
            Bump(kWSwDeferred);
            BT_DeferRequest();
        }
        return;
    }

    /* ★★★ v8.5: THE REVERSE TRIP IS SCORED SEPARATELY AND STOPS HERE.
     *
     * Same status inversion -- a timeout means the card changed personality, which is
     * success -- but recorded under its own codes so no reader can mistake the
     * direction, and it does NOT fall through to the seven-send retry below.
     *
     * ⚠ NO RETRY LOOP ON THE WAY BACK, deliberately. The forward burst exists because
     * Apple's CSRHIDTransitionDriver sends seven and the card at 1000 re-enumerates
     * still at 1000 if it does not take. The reverse is different in kind: it is a
     * single user-initiated action, and once the card has gone to proxy our driver's
     * device reference is dead -- so a second send would be issued against a reference
     * the USL no longer owns. One request, scored, done. */
    if (gSwitchMode == 1) {
        if (st == noErr)
            Note(kWSwVerdict, kSwToProxyIgnored);
        else if (st == kUSBNotRespondingErr || st == kUSBTimedOut)
            Note(kWSwVerdict, kSwToProxyOk);
        else
            Note(kWSwVerdict, kSwToProxyErr);
        return;
    }

    if (st == noErr) {
        /* Completed cleanly => the card did NOT switch. BlueZ calls this EALREADY.
         * Either it was already in HCI mode, or it is not a switchable part. */
        Note(kWSwVerdict, kSwIgnored);
    } else if (st == kUSBNotRespondingErr || st == kUSBTimedOut) {
        /* -6911 "Pipe stall, No device, device hung" and -6971 "Transaction timed
         * out" are the two ways this SDK reports the card going away mid-request.
         * (kUSBTransactionTimeout does not exist in this SDK -- checked.) */
        /* The device changed personality mid-request and never completed the
         * handshake -- BlueZ's success signature, and what run 42 actually measured
         * (0xFFFFE501 = -6911, with pipe stalls seen 0). Note that Apple's driver
         * instead expects kIOUSBPipeStalled: the two USB stacks report the same
         * physical event differently, which is why v2.9's stall path is correct
         * against Apple's code and inert against ours. */
        Note(kWSwVerdict, kSwSwitched);
    } else {
        Note(kWSwVerdict, kSwOtherErr);
    }

    /* ★★★ v3.1: KEEP SENDING. This is the one measured difference from Apple left.
     *
     * Run 42 was the first run whose counters could be read, and it showed
     * `switch attempts 1`: we sent once, got -6911, called it SWITCHED and stopped.
     * Apple's CSRHIDTransitionDriver sends the request SEVEN times, unrolled and
     * unconditional -- it does not stop when one send reports the device going away
     * (docs/M0-MODE-SWITCH.md §9c). And the card we produce lands on 8202, which is
     * a personality no Apple Bluetooth driver claims, rather than the 8204 Tiger
     * drives.
     *
     * ⚠ Chained from the completion rather than deferred, deliberately. Apple's seven
     * sends are sequential and back-to-back; routing each through the Notification
     * Manager would spread them over event-loop turns and stop resembling the thing
     * we are trying to reproduce. Interrupt level is no longer a concern here: run 42
     * retired the task-versus-interrupt theory, and this is one call.
     *
     * ⚠ A clean noErr means the card ignored the request, so retrying it is pointless
     * -- that is the only case that stops early. */
#if kSendModeSwitch
    /* ⚠ Gated alongside every other send. With the switch off nothing can reach this
     * completion anyway -- rule 1 is compiled out and BT_DeferredSwitchIfPending is
     * gated -- but "unreachable in practice" is the sort of claim this project has
     * been caught out by before. Gating it makes "no switch can be sent" structural
     * rather than argued: with the gate at 0 the preprocessor leaves ZERO live call
     * sites, which is checkable in one command. */
    if (st != noErr && gCB[kWSwTried] < kSwMaxTries) {
        Bump(kWSwDeferred);
        /* ⚠⚠ HARDCODED 0, AND NOT gSwitchMode, DELIBERATELY.
         *
         * This call sits in a COMPLETION, i.e. interrupt level. It is the forward
         * seven-send burst, and the note above justifies interrupt level for THAT
         * specific case only. The reverse trip must never be issued from here --
         * CsrModeSwitch at interrupt level is the documented v2.5 card-hang candidate.
         *
         * Mode 1 cannot reach this line today: the reverse branch at the top of this
         * function returns before it, and the stall retry is gated on gSwitchMode == 0.
         * Passing gSwitchMode would have been correct-by-accident and would have turned
         * a future deletion of that early return into an interrupt-level reverse
         * request. A literal 0 makes this call forward-only by construction. */
        CsrModeSwitch(gDevice, 0);
    }
#endif
}

/* Returns true if this device is the HID-proxy A1044 and a switch was started, in
 * which case the caller must NOT go on to bring up the stack on this instance. */
static Boolean CsrModeSwitch(USBDeviceRef device, UInt16 mode)
{
    OSStatus err;

    /* ⚠⚠ BOUND THE ATTEMPTS, AND GUARD THE SHARED PB. Found by the pre-staging
     * falsification pass, not by a hardware run.
     *
     * If the switch does NOT take, the card re-enumerates still as 05ac:1000, rule 1
     * matches again, and we switch again -- an unbounded switch/re-enumerate loop
     * with the USB bus in the middle of it. Worse, gSwitchPB is ONE static parameter
     * block: a second call while the first is in flight would overwrite a PB the USL
     * still owns.
     *
     * The bound is deliberately small and COUNTED rather than silent. This is not the
     * latching guard [[feedback_guards_must_not_latch]] warns about -- it does not
     * suppress a retry that could succeed; it stops a loop that by construction
     * cannot, and kWSwTried shows exactly how many attempts happened. */
    /* ⚠ WAS 3, NOW kSwMaxTries (7). The 3 was invented in v2.4 as a loop stopper when
     * we had no idea how many attempts were reasonable. We now know: Apple's
     * CSRHIDTransitionDriver sends this request SEVEN times, unrolled
     * (docs/M0-MODE-SWITCH.md §9c). Leaving the old 3 here would have silently capped
     * the retry sequence below what the card is documented to need, and the cap would
     * have looked like "the switch just does not work". */
    if (gCB[kWSwTried] >= kSwMaxTries || gSwitchBusy) {
        Note(kWSwVerdict, kSwOtherErr);
        return true;
    }
    gSwitchBusy = true;
    /* ⚠ SET BEFORE THE SEND, because the completion can run before USBDeviceRequest
     * has even returned and it reads this to choose its verdict code. */
    gSwitchMode = mode;

    Bump(kWSwTried);
    Note(kWSwVerdict, kSwNone);

    InitPB(&gSwitchPB, device, SwitchCompletion);
    gSwitchPB.usb.cntl.BMRequestType =
        USBMakeBMRequestType(kUSBOut, kUSBVendor, kUSBDevice);   /* 0x40 */
    gSwitchPB.usb.cntl.BRequest = 0;
    gSwitchPB.usb.cntl.WValue   = mode;  /* enum mode { HCI = 0, HID = 1 } */
    gSwitchPB.usb.cntl.WIndex   = 0;
    gSwitchPB.usbBuffer         = nil;
    gSwitchPB.usbReqCount       = 0;

    err = USBDeviceRequest(&gSwitchPB);
    Note(kWSwImmErr, (unsigned long)err);

    /* ⚠ An immediate error here is a real failure -- it means the request was never
     * issued, which is different from the request being issued and timing out. Only
     * the COMPLETION can report the success case. */
    if (err != noErr && err != kUSBPending) {
        /* ⚠ The completion will NEVER run, so release the flag here or the switch is
         * stranded for the rest of the boot. An early return that keeps its
         * re-entrancy flag is the exact shape of the bug that once stranded the
         * keyboard: [[reference_os9_recovery_paths_fail_open]]. */
        gSwitchBusy = false;
        Note(kWSwVerdict, kSwOtherErr);
    }

    return true;
}

/* ★ TASK LEVEL. Called from bt_defer_resp, which is the Notification Manager
 * trampoline's task-level half. This is how the switch gets its settle time without
 * blocking the USB Expert.
 *
 * Apple's driver simply does IOSleep(1000) inside start(). We cannot: ProbeInitialize
 * is what the Expert calls to load us, and blocking it for a second stalls driver
 * loading for every other device on the bus. Deferring instead gives the card AT
 * LEAST as much settle time -- the notification is not serviced until something runs
 * an event loop -- and costs the Expert nothing. It also puts the retries at task
 * level, which is where Apple's are. */
void BT_DeferredSwitchIfPending(void)
{
    /* ★★★★★ v10.5: M5's KCHR LANDS HERE, AND IT MUST BE BEFORE THE EARLY RETURN.
     *
     * ⚠⚠ THE EARLY RETURN BELOW IS WHY. Once the mode switch has been done
     * gSwitchPending is false forever, so anything placed after that line would never
     * run again -- and KCHR is needed long AFTER the switch, on the first keystroke.
     * Putting it first is not tidiness; it is the difference between working and
     * silently never running. [[feedback_guards_must_not_latch]]
     *
     * ⚠ This is the ONLY task-level code this driver runs after load. GetResource is
     * the Resource Manager and cannot be called from the USB completions that drive
     * everything else, and v10.4 proved it cannot go on the load path either: calling
     * it from ValidateHW stopped the fragment loading at all. Apple has the same
     * constraint and resolves it the same way -- on demand, never at load
     * (KBDHIDEmulation.c:70).
     *
     * ⚠ Free to call every time: it returns immediately once the resource is held, and
     * BT_InjectKeyEvent requests a defer when it finds KCHR missing, so the first
     * keystroke or two may be dropped and counted rather than typed. That is the
     * accepted cost of keeping the Resource Manager off both the load path and the
     * interrupt path. */
    BT_InjectInit();
    /* ⚠ M6: the volume change runs HERE, at task level, for the same reason KCHR is
     * acquired here -- the Sound Manager is not documented interrupt-safe and the
     * Consumer bits arrive in a USB completion. Apple defers too: PatchSystemTask /
     * KeyboardSystemTaskPatch sit beside its own DoSoundUpButton. Free when nothing
     * is queued -- it takes the pending mask, finds it zero, and returns.
     * ⚠⚠ AND IT MUST BE BEFORE THE EARLY RETURN BELOW, like BT_InjectInit: once the
     * mode switch is done gSwitchPending is false forever, and a media key pressed
     * afterwards is the ONLY case that matters. */
    BT_MediaServiceAtTask();
    /* ★★★ v11.1: tell the switcher we came up. BEFORE the early return, like the
     * two above -- gSwitchPending is false forever after the first switch, and the
     * marker must be cleared on EVERY boot we reach this point, not just that one.
     * Cheap after the first success: it returns immediately once cleared. */
    BT_ClearSwitchMarker();

    if (!gSwitchPending) return;
    gSwitchPending = false;
    /* ★★★ v8.5: THE REVERSE TRIP LANDS HERE TOO, AND THIS IS THE WHOLE POINT OF THE
     * TRAMPOLINE.
     *
     * ⚠⚠ CsrModeSwitch IS TASK-LEVEL-ONLY, and this file says why in as many words:
     * "v2.5 chained a switch onto this terminator, which put CsrModeSwitch at
     * INTERRUPT level and is one of the two changes that COULD HAVE HUNG THE CARD."
     *
     * kCmdSwitchToProxy arrives on the mailbox, and BT_ServiceMailbox is called from
     * bt_pump_timer_sih -- a SecondaryInterruptHandler2, i.e. BELOW TASK LEVEL. My
     * first version called CsrModeSwitch straight from that dispatch, which would have
     * reintroduced the v2.5 hang candidate exactly. The static audit's unclassified
     * count rising by one is what caught it.
     *
     * ⇒ BT_HandBackToProxy only ARMS from the mailbox; the request is issued here, at
     * task level, through the same Notification Manager path the stall retry already
     * uses. gSwitchMode carries the direction across the hop.
     *
     * ⚠ Only the FORWARD direction stays behind kSendModeSwitch. That gate exists
     * because binding the card at 1000 to switch it is the switcher extension's job
     * now; it has nothing to do with the way back, which is only ever reachable when
     * this driver already owns the card at 8204. */
    if (gSwitchMode == 1) {
        CsrModeSwitch(gDevice, 1);   /* reverse: back to HID-proxy */
        return;
    }
#if kSendModeSwitch
    CsrModeSwitch(gDevice, 0);   /* forward: to HCI */
#endif
}

/* ★★★★ ARM THE REVERSE SWITCH. Called from BT_ServiceMailbox, i.e. BELOW TASK LEVEL,
 * so it must not touch USB itself -- see the note in BT_DeferredSwitchIfPending.
 *
 * Returns true if the request was armed, false if it was refused. ⚠ Neither answer is
 * the SWITCH's result: a timeout is what success looks like for this request and only
 * the completion can report it, into kWSwVerdict. */
static Boolean BT_HandBackToProxy(void)
{
    if (gDevice == kNoDeviceRef || gDeviceGone) return false;
    /* ⚠ REFUSE RATHER THAN STOMP. gSwitchPB is one static parameter block still owned
     * by the USL while a request is in flight, and overwriting it is the defect the
     * forward path's guard exists to prevent. Recorded so a refusal is visible. */
    if (gSwitchBusy) { Note(kWSwVerdict, kSwToProxyBusy); return false; }
    /* ⚠ Reset the attempt budget. kSwMaxTries bounds an automatic re-enumerate loop on
     * the forward path; this is one deliberate user action and must not be refused
     * because forward attempts from an earlier instance spent the budget. */
    gCB[kWSwTried] = 0;
    gSwitchMode    = 1;
    gSwitchPending = true;
    BT_DeferRequest();          /* interrupt-safe and coalescing; the sanctioned hop */
    return true;
}

static OSStatus ProbeInitialize(USBDeviceRef device, USBDeviceDescriptorPtr pDesc, UInt32 busPower)
{
    EnsureBlock();          /* ⚠ BEFORE any Bump/Note. Task level; see EnsureBlock. */

    /* ★★★ ONE INSTANCE PER DEVICE. Run 47 bound the dongle four times and ran four
     * BTstacks against one radio. Rules 2a and 2b can both match a device that
     * declares 0xE0 at device level AND on interface 0, and each fragment copy is a
     * separate instance with its own globals -- so the interlock has to live in the
     * shared block, which EnsureBlock now adopts rather than duplicating.
     *
     * ⚠ Declines by RETURNING AN ERROR *and* by not starting the chain. The error is
     * the documented way to refuse a device, but this project has been caught before
     * assuming an API behaves as documented on this stack -- so the belt is that even
     * if the Expert keeps the instance anyway, it sits there doing nothing instead of
     * arming a second set of pipes. */
    if (gCB != NULL) {
        short i, n = (short)gCB[kWBoundCount];
        for (i = 0; i < n && i < 4; i++) {
            if (gCB[kWBound0 + i] == (unsigned long)device) {
                Bump(kWDupDeclined);
                return kUSBDeviceBusy;
            }
        }
        if (n < 4) {
            gCB[kWBound0 + n] = (unsigned long)device;
            gCB[kWBoundCount] = (unsigned long)(n + 1);
        }
    }

    /* ⚠⚠ CLEAR gDeviceGone ON EVERY NEW BIND, or it latches. Run 48 shows the dongle
     * binding and unbinding FIVE times in one session; if a fragment copy that had
     * seen a removal handled a later bind, the reader would refuse to arm for the rest
     * of the boot and the block would look exactly like a controller gone quiet --
     * run 16's ambiguity all over again. [[feedback_guards_must_not_latch]]. */
    gDeviceGone = false;

    /* ⚠⚠ AND THE LINK COUNT, for exactly the same reason -- which I did not apply to
     * gAclLinks when I introduced it in the same file where I had just fixed
     * gDeviceGone's latch. Run 52: ACL links up 2, ACL links down 0, four bind
     * cycles, and ACL re-arms parked stuck at 2. A link counted in one bind cycle
     * persisted into the next, where it referred to nothing, so the count never
     * returned to zero and the reader never parked again. Third latching guard this
     * session; the pattern is that a new piece of cross-bind state needs clearing
     * HERE, next to the others. */
    gAclLinks = 0;

    gDevice   = device;
    gBusPower = busPower;
    Bump(kWInit);

    /* ★★★ ARM THE PUMP TIMER HERE, AT TASK LEVEL. v5.5 armed it only from ConfigDone,
     * which runs at INTERRUPT level, and the timer never ran: 35 run-loop iterations
     * where a 100 ms timer would give hundreds. The audit had flagged
     * SetPersistentTimer as UNCLASSIFIED all along and I read the headline verdict as
     * covering it.
     *
     * ⚠ Placed AFTER gDevice is set and gDeviceGone cleared, so if the very first
     * firing beats ConfigDone it passes the guards and simply pumps an empty stack.
     * gCB is already set by EnsureBlock at the top of this function.
     *
     * ⚠ ALSO A LATCH-CLEAR SITE. gTimerBail and both rc words are per-instance state
     * that must reset here next to gDeviceGone and gAclLinks, or a bail recorded in one
     * bind cycle would be read as belonging to the next -- the third latching guard
     * this file has had, and the pattern is that new cross-bind state clears HERE. */
    /* ⚠⚠ THE BAIL MASK AND THE RETURN CODES RESET; THE RUN COUNTERS DO NOT, and that
     * distinction is a bug I shipped in v5.6. Resetting gTimerRuns per instance while
     * publishing it into a SHARED block means a fresh instance's zero overwrites the
     * previous instance's real total. Run 3: the panel read timer 1263 while the
     * driver was alive, then a re-bind after the panel quit reset it, and BTCheck --
     * which runs last -- reported "timer firings 0". I very nearly concluded the
     * timer had never run when it had fired 1263 times at exactly the requested rate.
     *
     * The mask and rc words genuinely are per-attempt facts and must not carry over.
     * A cumulative count is the opposite: it is only meaningful across the session. */
    gTimerBail   = 0;
    gTimerRcTask = 0;
    gTimerRcIrq  = 0;
    /* gTimerRuns and gMbxServiced deliberately NOT reset -- see above. Adopting the
     * block's existing values keeps the session total intact across a re-bind. */
    if (gCB != NULL) {
        if (gCB[kWTimerRuns]   > gTimerRuns)   gTimerRuns   = gCB[kWTimerRuns];
        if (gCB[kWMbxServiced] > gMbxServiced) gMbxServiced = gCB[kWMbxServiced];
    }
    BT_StartPumpTimerTask();
    Note(kWBusPower, busPower);

    /* ⚠⚠ RECORD *WHICH DEVICE* THIS INSTANCE BOUND. `pDesc` used to be
     * `#pragma unused` here, and that omission cost real analysis time: with the
     * driver binding more than one device, every block looked alike, and nine runs
     * were spent inferring "that one is the hub" from its interface class instead of
     * simply reading its VID/PID. Now that the descriptor carries TWO match rules --
     * the A1044 by VID/PID and any standard controller by interface class -- "which
     * block is which device" is the central question, and every block answers it. */
    if (pDesc != nil) {
        Note(kWIfaceDevClass, (unsigned long)pDesc->deviceClass);
        Note(kWIfaceVIDPID,
             ((unsigned long)USBToHostWord(pDesc->vendor) << 16) |
              (unsigned long)USBToHostWord(pDesc->product));
    }

    /* ★★ M0.0: IF THIS IS THE HID-PROXY A1044, OUR ONLY JOB IS TO SWITCH IT.
     *
     * Rule 1 matches 05ac:1000 at DEVICE level. That is the card's HID-proxy
     * identity, and in that state it has no 0xE0 interface at all -- run 31 measured
     * interface 0 as 03/01/01 (HID / Boot / Keyboard) and the 0xE0 search returned
     * kUSBNotFound. So there is nothing here to drive, and pressing on would claim
     * hardware we cannot drive while giving OS 9's own USBHIDKeyboardModule nothing
     * back, which is exactly what the coexistence rule forbids.
     *
     * Instead: send the switch and get out of the way. The card re-enumerates with
     * standard 0xE0 interfaces and RULE 2 binds it -- on interface class, so its new
     * product ID never has to be known here. Rule 1 is a mode switcher, not a driver.
     *
     * ⚠ Deliberately BEFORE BT_DeferInit/BT_KeyFileLoad: this instance never runs the
     * stack, so it needs neither, and the rule-2 instance does its own. */
    if (pDesc != nil && USBToHostWord(pDesc->vendor) == kA1044Vendor) {
        UInt16 pid = USBToHostWord(pDesc->product);

        if (pid == kA1044Product) {
            /* ⚠⚠ HID-PROXY: SWITCH IMMEDIATELY, FROM TASK LEVEL, AND DO NOT WALK FIRST.
             *
             * This is v2.4's sequence restored verbatim, and the restoration is the
             * point of this build. v2.4 sent the switch straight from here and the card
             * came back as 8202, visible and healthy (run 33). v2.5 walked this device
             * first and then switched from the walk's completion -- i.e. from INTERRUPT
             * level -- and the card left the bus until a Tiger boot cleared it (runs 35
             * to 37).
             *
             * Which half of that mattered was never isolated, so neither half is being
             * reintroduced. There is also nothing left to learn here: run 38 walked this
             * device with no switch and found the complete set -- two interfaces,
             * 03/01/01 boot keyboard and 03/01/02 boot mouse, and nothing else. The
             * proxy personality is fully described. Walking it again would re-run the
             * one sequence known to hang the card in exchange for a measurement we
             * already have. */
            Say(device, "\pOS9BTProbe: HID-proxy A1044 - deferring CSR mode switch", 0);
            /* ⚠ BT_DeferInit MUST come first and MUST be here: it calls NewNMUPP ->
             * NewRoutineDescriptor, a MixedMode allocator that is task-level only, and
             * this early return skips the BT_DeferInit further down. */
            BT_DeferInit();
            gSwitchPending = true;
            BT_DeferRequest();
            return noErr;
        }
        if (pid == kA1044Switched) {
            /* ★★ THE MEASUREMENT THIS BUILD EXISTS FOR. The post-switch personality has
             * never been described: rule 3 was written for it in v2.5, but the card hung
             * before it could bind, so 8202's interfaces are still unknown. Rule 2 did
             * not bind it in run 33, which tells us they are not 0xE0/01/01 -- and that
             * is the entire extent of what we know.
             *
             * Walk and STOP. We do not yet know what this personality is, and bringing a
             * stack up on an unidentified one would be the guess this project keeps
             * refusing to make. Walking here is also not the sequence that hung the card:
             * that was walking the PROXY device and then switching it. Nothing is
             * switched from this path. */
            Say(device, "\pOS9BTProbe: switched A1044 (8202) - scanning interfaces", 0);
            StartIfaceScan(device);
            return noErr;
        }
        if (pid == kA1044HCI) {
            /* ★★★ NO EARLY RETURN, DELIBERATELY. See the note at kA1044HCI: unlike
             * 8202, this personality is the one Apple's own CSRUSBBluetoothHCIController
             * claims, so the normal configuration chain is the correct treatment rather
             * than a guess. Logged and then allowed to fall through, so the run's log
             * says which personality was bound without changing what happens to it. */
            Say(device, "\pOS9BTProbe: A1044 at 8204 - Apple's HCI personality, configuring", 0);
        }
    }

    /* ⚠⚠ THE ONLY TASK-LEVEL WINDOW WE GET, AND BOTH OF THESE NEED IT.
     *
     * The Expert calls ProbeInitialize at task level; everything after the first USL
     * call in the chain it starts is secondary interrupt level. So:
     *
     *   BT_DeferInit()  builds the NMUPP, which is NewRoutineDescriptor -- a
     *                   MixedMode ALLOCATOR, on the audit's forbidden list. It can
     *                   only be built here.
     *   BT_KeyFileLoad() touches the File Manager, and must complete BEFORE
     *                   hci_init hands BTstack the link key db, so a stored key is
     *                   already in RAM the first time a peer asks for one. hci_init
     *                   runs later, from ConfigDone, at interrupt level. */
    BT_DeferInit();
    BT_AlertInit();
    BT_KeyFileLoad();
    BT_LoadShowAllFlag();

    /* ★★★★★★ v14.4: ASK FOR ONE TASK-LEVEL HOP, AND THIS IS WHY YOU HAD TO REBOOT TWICE
     * AFTER EVERY DRIVER UPDATE.
     *
     * BT_ClearSwitchMarker -- the "I came up, the switcher can stop standing down" signal
     * -- lives inside BT_DeferredSwitchIfPending, which runs ONLY from the NM defer
     * response. That response runs only when something calls BT_DeferRequest. A new
     * pairing does (db_put_link_key). An ordinary boot with an existing bond does NOT.
     *
     * ⇒ So on every boot that merely reconnected, the driver bound, worked perfectly, and
     * never cleared the marker the switcher had written before switching. The NEXT boot
     * then found a stale marker and correctly stood down -- card left in HID-proxy, no
     * BTP1 block -- and the user rebooted a second time to get Bluetooth back. Measured
     * 2026-09-21 across the before/after pair: `STALE MARKER - declined 1, switch
     * attempts 0, NO BLOCK FOUND` then `marker written, switching 1, BTP1 vE30`.
     *
     * ⚠⚠ AND IT EXPLAINS EVERY "UNEXPLAINED" STALE MARKER THIS PROJECT HAS CHASED. The
     * 09-21 09:34 boot, and the one before Gate 1's second pass, were both read as
     * incidental. They were this. The fallback was doing its job perfectly on a signal
     * that was never sent.
     *
     * ⚠ ONE REQUEST, HERE, IS ENOUGH: BT_DeferRequest is coalescing, the response runs
     * BT_DeferredSwitchIfPending unconditionally, and BT_ClearSwitchMarker returns
     * immediately once it has succeeded. It is also the same omission as v14.2's battery
     * flush -- mark the state, forget to ask for the hop -- which is now twice in one
     * file. If you add task-level work, ASK FOR TASK LEVEL. */
    BT_DeferRequest();

    /* ================= LOGGING PROBES -- remove once resolved ================
     * These live HERE, not in ValidateHW, because this is the routine the Expert
     * is PROVEN to call: it logs "calling driver initialize routine" and then
     * "driver initialization completed". There is no corresponding log line for
     * validateHWProc, so ValidateHW may never run and a probe there proves
     * nothing. That is why run 3's A/B probe came back empty.
     *
     * The state of play: our code runs, both logging APIs are silent from it, and
     * Prober's Status Level is already at its maximum of 5, while 45 "Driver -"
     * lines from Apple's own drivers appear in the same log. So the mechanism
     * works and something about OUR call is wrong.
     *
     * THE THEORY. The Expert renders our driver name as "S9BTProbe", losing the
     * leading O, while rendering USBHub0Apple and USBHIDKeyboardModule1.5.9
     * intact. The only read consistent with that is one byte late. Apply the same
     * one-byte-late read to a status string and it takes our first CHARACTER as
     * the length byte. Every message we send begins "OS9BTProbe:", so it claims
     * length 'O' = 79, and a copy into a Str63 rejects 79 outright and drops the
     * message. That accounts for total silence rather than garbled output.
     *
     * THE DISCRIMINATOR. Probe C begins with a SPACE (0x20 = 32), which passes a
     * length check either way, so the rendering tells us which read happened:
     *
     *   " OS9BTProbe probeC shifted-read-stops-HERE|tail-proves-normal"
     *      -> normal read, 61 chars, leading space and the tail both present.
     *         The shift theory is dead and the silence needs another cause.
     *
     *   "OS9BTProbe probeC shifted-read-s"
     *      -> SHIFTED read, cut at 32 chars, no leading space. Theory confirmed,
     *         and the fix is to pad every status string with a leading byte.
     *
     *   nothing at all
     *      -> not a length rejection either. Stop using the Expert log and build
     *         the counter block plus companion app, as FWFixCheck and the EHCI
     *         counters already do here. The static audit rules out a driver-side
     *         log file: this driver has no task-level context to drain a ring. */
    USBExpertStatus(device, "\pOS9BTProbe: probe A - plain USBExpertStatus", 0);
    USBExpertStatusLevel(kBTStatusLevel, device,
                         (StringPtr)"\pOS9BTProbe: probe B - USBExpertStatusLevel", 0);
    USBExpertStatus(device,
                    "\p OS9BTProbe probeC shifted-read-stops-HERE|tail-proves-normal", 0);
    USBExpertStatusLevel(kBTStatusLevel, device,
                         (StringPtr)"\p OS9BTProbe probeD shifted-read-stops-HERE|tail-proves-normal", 0);
    /* ====================== end logging probes ============================= */

    Say(device, "\pOS9BTProbe: Initialize - starting M0.1 pipe discovery", 0);

    InitPB(&gCfgPB, device, ConfigStep);
    gCfgPB.usbRefcon = kFindIface;
    gCfgPB.usbBuffer = nil;
    ConfigStep(&gCfgPB);        /* kick off the async chain */
    return noErr;
}

/* ---- the INTERFACE entry point ------------------------------------------- *
 * ⚠ THIS IS WHAT PHASE 1 NEEDED, and run 26 is what proved it.
 *
 * The Expert calls this instead of ProbeInitialize when it loads us as an INTERFACE
 * driver -- which is the only way it will ever offer us the A1044, whose device
 * descriptor is class 00/00/00 ("class is on the interfaces").
 *
 * ★ IT IS ALSO THE DIAGNOSTIC WE WERE MISSING. The bus dump in BTCheck can only read
 * DEVICE descriptors, so the A1044's interface class was still unknown. This proc is
 * handed `pInterface` directly, so it records the interface triplet whatever it turns
 * out to be. Even if we go no further, the run answers the question.
 *
 * We enter the existing config chain at kConfigIface, skipping the four steps that
 * exist only to turn a device ref into an interface ref: the Expert has already done
 * that work and handed us the result. */
static OSStatus ProbeInitializeInterface(UInt32 interfaceNum,
                                         USBInterfaceDescriptorPtr pInterface,
                                         USBDeviceDescriptorPtr pDevice,
                                         USBInterfaceRef interfaceRef)
{
    EnsureBlock();          /* ⚠ BEFORE any Bump/Note. Task level; see EnsureBlock. */
    Bump(kWIfaceInit);
    Note(kWIfaceInitNum, (unsigned long)interfaceNum);

    /* Record what we were actually given, before doing anything with it. */
    if (pInterface != nil) {
        Note(kWIfaceTriplet,
             ((unsigned long)pInterface->interfaceClass    << 16) |
             ((unsigned long)pInterface->interfaceSubClass <<  8) |
              (unsigned long)pInterface->interfaceProtocol);
    }
    if (pDevice != nil) {
        Note(kWIfaceDevClass, (unsigned long)pDevice->deviceClass);
        Note(kWIfaceVIDPID,
             ((unsigned long)USBToHostWord(pDevice->vendor) << 16) |
              (unsigned long)USBToHostWord(pDevice->product));
    }

    /* ⚠ There is no device ref on this path. gDevice stays 0, and Say() simply logs
     * against 0 -- harmless, since the Expert log has never carried our messages
     * anyway (established over four runs). The pipe searches all use the INTERFACE
     * reference, which is what we were given. */
    gIfaceRef = interfaceRef;
    gIfaceNum = (UInt16)interfaceNum;

    /* Task-level window, exactly as in ProbeInitialize: the Expert calls this at task
     * level, and both of these need it. See the note there. */
    BT_DeferInit();
    BT_KeyFileLoad();

    InitPB(&gCfgPB, interfaceRef, ConfigStep);
    gCfgPB.usbRefcon = kConfigIface;     /* skip find/set-config/new-ref/set-iface */
    gCfgPB.usbBuffer = nil;
    ConfigStep(&gCfgPB);
    return noErr;
}

static OSStatus ProbeFinalize(USBDeviceRef device, USBDeviceDescriptorPtr pDesc)
{
#pragma unused (pDesc)
    /* ★ v17.2: stamp the teardown, FIRST, before any quiesce can take time. Read
     * against kWStallFirstMs / kWStallLastMs this is the whole experiment: stalls in a
     * burst just before this instant mean the removal caused them; stalls spread across
     * the session before it mean they caused the removal. See kWStallFirstMs. */
    Note(kWFinalizeMs, (unsigned long)hal_time_ms());
    /* ⚠⚠⚠ QUIESCE EVERYTHING, AND SET THE FLAG FIRST.
     *
     * This aborted ONLY the interrupt pipe. The two bulk pipes were left outstanding,
     * so a bulk-IN completion could fire after Finalize had already flushed the key
     * file, torn down the defer trampoline and released the interlock -- landing in
     * AclInCompletion with the instance half dismantled. Nothing in that path checks
     * for "we are finalizing", it re-arms and it pumps BTstack.
     *
     * ★ That is a strong candidate for a freeze with NO MacsBug and NO NMI: it is not
     * an exception, it is interrupt-level code running against torn-down state. The
     * driver binds and unbinds repeatedly in a session (run 50: 14 Initialize calls
     * against 5 Finalize), so the window is entered often and the failure would land
     * at an arbitrary moment -- including while sitting idle.
     *
     * gDeviceGone is set BEFORE the aborts because both completion paths already test
     * it (v3.7/v3.9 added it for rude removal); setting it here reuses that existing
     * guard instead of inventing a second one. The aborts then retire the callbacks. */
    gDeviceGone = true;
    /* ⚠⚠ THE TIMER GOES FIRST, BEFORE THE PIPE ABORTS. It is the one thing here that
     * can re-enter BTstack and the transport on its own schedule with nobody having
     * asked it to, so leaving it running while the pipes are torn down would recreate
     * precisely the "interrupt-level code against torn-down state" freeze this
     * function's comment above describes. */
    BT_StopPumpTimer();
    if (gIntPipe) USBAbortPipeByReference(gIntPipe);
    if (gBulkIn)  USBAbortPipeByReference(gBulkIn);
    if (gBulkOut) USBAbortPipeByReference(gBulkOut);
    Bump(kWFinal);

    /* ⚠ TASK LEVEL AGAIN, and the last chance to write anything. The Expert calls
     * Finalize at task level, so the File Manager is legal here.
     *
     * The flush is belt-and-braces rather than the primary path: put_link_key
     * already asks for a deferred flush the moment a key changes, precisely so a
     * power cut does not lose it. This catches the case where the notification was
     * still queued, or was never serviced because no application event loop ran.
     *
     * ⚠ ORDER MATTERS. Flush BEFORE tearing the trampoline down, or the pending
     * notification is removed with the work still owed. */
    BT_KeyFileFlushIfDirty();
    /* ★ v14.2: last chance to get a reading onto disk. Same ordering rule as above --
     * before the trampoline goes, or the work is owed to a queue that no longer runs. */
    BT_BatteryFlushIfDirty();
    /* ★ mouse: give the cursor device back before the trampoline goes. It clears its
     * own pointer FIRST so an interrupt arriving mid-teardown sees none and drops its
     * report, rather than using a device being disposed. */
    BT_MouseFinalize();
    /* ★ the low-battery alert's record and string live in this fragment; a queued
     * notification must leave the Manager's queue before the fragment does. */
    BT_AlertFinalize();
    BT_DeferFinalize();

    /* ⚠⚠ RELEASE THIS DEVICE FROM THE BOUND LIST. Without this the interlock LATCHES:
     * unplug the dongle and plug it back in, and the new bind would be refused forever
     * because a stale ref sat in the list. That is precisely the shape
     * [[feedback_guards_must_not_latch]] describes -- a guard that becomes the cause of
     * its own symptom -- and it is why the entry is keyed on the device ref and cleared
     * here rather than being a one-way "we have started" flag. */
    if (gCB != NULL) {
        short i, n = (short)gCB[kWBoundCount];
        for (i = 0; i < n && i < 4; i++) {
            if (gCB[kWBound0 + i] == (unsigned long)device) {
                /* compact the list so the slots stay contiguous */
                short j;
                for (j = i; j < n - 1 && j < 3; j++)
                    gCB[kWBound0 + j] = gCB[kWBound0 + j + 1];
                gCB[kWBound0 + (n - 1)] = 0;
                gCB[kWBoundCount] = (unsigned long)(n - 1);
                break;
            }
        }
    }

    Say(device, "\pOS9BTProbe: Finalize - unloading", 0);
    return noErr;
}

static OSStatus ProbeNotify(UInt32 notification, void *pointer, UInt32 refCon)
{
    Bump(kWNotify);
    Note(kWNotifyCode, (unsigned long)notification);
    (void)pointer; (void)refCon;

    /* ⚠⚠⚠ TWO DIFFERENT ENUMS SHARE THE kNotify PREFIX, AND WE HAD THE WRONG ONE.
     *
     * This proc is a USBDDriverNotifyProcPtr, so `notification` is a
     * USBDriverNotification:
     *     0x01 kNotifySystemSleepRequest    0x02 kNotifySystemSleepDemand
     *     0x03 kNotifySystemSleepWakeUp     0x04 kNotifySystemSleepRevoke
     *     0x06 kNotifyHubEnumQuery          0x07 kNotifyChildMessage
     *     0x08 kNotifyExpertTerminating     0x0B kNotifyDriverBeingRemoved
     *     0x0E kNotifyAllowROMDriverRemoval
     *
     * The code below tested kNotifyRemoveDevice, which is 0x01 in a COMPLETELY
     * DIFFERENT enum -- the expert-services list (kNotifyAddDevice, kNotifyRemoveDevice,
     * kNotifyGetDeviceDescriptor...). In this proc 0x01 means SYSTEM SLEEP REQUEST.
     *
     * ⇒ The rude-removal handling added in v3.7/v3.9 has never fired on a removal. It
     * fires when the system asks to sleep, and then sets gDeviceGone and aborts the
     * interrupt pipe on a device that is still plugged in -- silently disabling the
     * driver until the next bind. Run 53 recorded `last notification 0x0B`, which is
     * not in the enum we were comparing against at all.
     *
     * ★ Both names are in USB.h, both begin kNotify, and nothing warns you. */
    if (notification == kNotifyDriverBeingRemoved) {
        /* The Expert is unloading US. Quiesce so no completion lands mid-teardown --
         * the same reasoning as ProbeFinalize. */
        Bump(kWNotifyRemoved);
        gDeviceGone = true;
        BT_StopPumpTimer();      /* ⚠ first, for the reason given in ProbeFinalize */
        if (gIntPipe) USBAbortPipeByReference(gIntPipe);
        if (gBulkIn)  USBAbortPipeByReference(gBulkIn);
        if (gBulkOut) USBAbortPipeByReference(gBulkOut);
        return noErr;
    }
    if (notification == kNotifySystemSleepRequest ||
        notification == kNotifySystemSleepDemand) {
        /* ⚠ COUNTED, NOT ACTED ON. Tearing the pipes down here is exactly the bug
         * above. A sleep request is not a removal, and what this driver should do about
         * sleep has never been designed -- so record it and say so rather than guess. */
        Bump(kWNotifySleep);
        return noErr;
    }
    if (notification == kNotifyExpertTerminating) {
        Bump(kWNotifyOther);
        gDeviceGone = true;
        return noErr;
    }
    Bump(kWNotifyOther);
    return noErr;
}

