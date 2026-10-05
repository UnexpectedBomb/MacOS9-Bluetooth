cmake_minimum_required(VERSION 3.13)
project(OS9BTProbe C)

#
#  OS9-Bluetooth -- M0.0 transport-spike driver.
#  Build:
#    cmake -S . -B build \
#      -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#    cmake --build build
#  Output: build/OS9BTProbe.ndrv  (file type 'ndrv', creator 'usbd')
#  Install into the OS 9 Extensions folder; watch USB Prober's Expert log.
#

# ---- BTstack, pinned at v1.8.2 --------------------------------------------
# The compile set is enumerated in vendor/btstack-files.txt, never a wildcard, so
# an accidental new dependency is a build error rather than a silent expansion of
# how much third-party code runs below task level. See docs/M2-DESIGN.md.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/btstack-files.txt BTSTACK_LIST)
set(BTSTACK_SOURCES "")
foreach(line ${BTSTACK_LIST})
    string(STRIP "${line}" line)
    if(line AND NOT line MATCHES "^#")
        list(APPEND BTSTACK_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/vendor/btstack/${line})
    endif()
endforeach()
list(LENGTH BTSTACK_SOURCES BTSTACK_COUNT)
message(STATUS "BTstack: ${BTSTACK_COUNT} source files from vendor/btstack-files.txt")

add_library(OS9BTProbe SHARED
    src/bt_probe.c
    src/bt_hci_m1.c              # M1's parser: compiled, no longer started
    src/bt_btstack.c
    src/bt_linkkey_db.c
    # ⚠ Dependency-free on purpose, so tests/run-tests.sh can compile it with the HOST
    # compiler. Do not add includes to it. See src/bt_bootreport.h.
    src/bt_bootreport.c
    # ⚠ Also dependency-free, also host-tested. See src/bt_hidident.h.
    src/bt_hidident.c
    # M5 step 2c: the Toolbox half of injection. ⚠ NOT host-testable, deliberately
    # thin -- everything provable off-target lives in bt_bootreport.c instead.
    src/bt_inject.c
    src/bt_defer.c
    src/bt_keyfile.c
    src/btstack_run_loop_os9.c
    src/hci_transport_os9.c
    ${BTSTACK_SOURCES})

# ⚠ -DTYPE_BOOL=1 IS REQUIRED, not a nicety. BTstack's btstack_bool.h includes
# <stdbool.h>, which makes `false` and `true` macros; Apple's MacTypes.h then
# fails to parse its own `enum { false = 0, true = 1 }`. TYPE_BOOL is MacTypes.h's
# documented escape hatch for compilers that have a real bool.
target_compile_definitions(OS9BTProbe PRIVATE TYPE_BOOL=1)
target_include_directories(OS9BTProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/btstack/src)

# (Retro68's GCC treats "\p" literals as Pascal strings by default -- no flag needed.)

set_target_properties(OS9BTProbe PROPERTIES
    LINK_FLAGS -Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/src/bt_probe.exp)

# USB device drivers link the USB Services Library (USL) + USB Manager.
# ⚠ SoundLib is for M6's HARDWARE volume write: Set/GetSoundOutputInfo live there,
# not in InterfaceLib. Get/SetDefaultOutputVolume and FindNextComponent resolve
# from InterfaceLib implicitly -- the same split that made BTCheck's link fail on
# exactly those two symbols and nothing else.
# ⚠ CursorDevicesGlue is a SEPARATE import library, not part of InterfaceLib. The mouse
# path needs it: InterfaceLib exports plenty of Cursor *shape* calls (SetCursor,
# HideCursor, GetCCursor) but NOT ONE of the fourteen CursorDevice* entry points, which
# live only in libCursorDevicesGlue.a. Without it the link fails on a subset of them --
# misleadingly, since all fourteen are equally absent.
target_link_libraries(OS9BTProbe USBServicesLib USBManagerLib DriverServicesLib SoundLib
                      CursorDevicesGlue)

add_custom_command(OUTPUT OS9BTProbe.pef
    COMMAND ${MAKE_PEF} libOS9BTProbe.so -o OS9BTProbe.pef
    DEPENDS OS9BTProbe)

# MakePEF emits mainSection=1 / mainOffset=0xffffffff, which points out of bounds.
# Every stock OS 9 USB class driver declares no main symbol at all. See the script.
add_custom_command(OUTPUT OS9BTProbe.nomain.pef
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pef-clear-main.py
            OS9BTProbe.pef OS9BTProbe.nomain.pef
    DEPENDS OS9BTProbe.pef ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pef-clear-main.py)

if (REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Version-stamp the installed artifact so it's unambiguous which build is in
# the Extensions folder (matches the 'vers' 0.2 resource and the log strings).
#
# ⚠ THE FILENAME MUST BEGIN "USB". Proved on hardware 2026-08-30: the USB Expert
# only scans Extensions files whose name starts with USB. A correct ndrv/usbd
# driver named anything else is skipped in silence and the device fails with
# fnfErr (-43). See HANDOFF.md §10.
#
# ⚠ ONE PLACE FOR THE DRIVER VERSION, same fix as bt-check/CMakeLists.txt.
#
# BTCheck v46 shipped writing a log called BTCheck_v45.log because its version lived in
# four hand-maintained places. This file had five copies of "v4.1" in the output names
# alone. Bumping DRIVER_VER is now the whole job for the filename; the 'vers' resource
# and the block build tag in src/bt_probe.c still need to move with it, and there is a
# check below that fails the build if they disagree.
set(DRIVER_VER "17.6")

add_custom_command(OUTPUT USBBluetoothSupport-v${DRIVER_VER}.ndrv
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/bt_probe.r
            ${REZ_INCLUDE_FLAG} -I${CMAKE_CURRENT_SOURCE_DIR}
            -o USBBluetoothSupport-v${DRIVER_VER}.ndrv --cc USBBluetoothSupport-v${DRIVER_VER}.raw.bin
            -t "ndrv" -c "usbd"
            --data OS9BTProbe.nomain.pef
    DEPENDS OS9BTProbe.nomain.pef
            ${CMAKE_CURRENT_SOURCE_DIR}/bt_probe.r
            ${CMAKE_CURRENT_SOURCE_DIR}/icons/bt_icons.r)

# The icon resources are only half of a custom icon; the file also needs the
# hasCustomIcon Finder flag, which Rez cannot set. This also recomputes the
# MacBinary CRC, which StuffIt Expander checks.
#
# ⚠ Rez does NOT track #include dependencies, so icons/bt_icons.r is listed as an
# explicit DEPENDS above. Without that, editing only the icons leaves Rez skipped
# and the old icon silently shipped -- the same trap that bit the PICT workflow.
add_custom_command(OUTPUT USBBluetoothSupport-v${DRIVER_VER}.bin
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py
            USBBluetoothSupport-v${DRIVER_VER}.raw.bin USBBluetoothSupport-v${DRIVER_VER}.bin
            # ⚠⚠ FIXED INTERNAL NAME, VERSIONED FILE ON DISK. The .bin keeps its
            # version so the Pi share is unambiguous, but the file it EXPANDS TO is
            # always "USBBluetoothSupport". Dropping a new one into Extensions then
            # prompts to REPLACE the old one, which structurally prevents two drivers
            # coexisting -- the failure that killed run 27. Get Info shows the version
            # from the 'vers' resource.
            #
            # ⚠ The name MUST still begin "USB": the Expert silently skips any
            # Extensions file that does not (proved 2026-08-30 by renaming alone; see
            # HANDOFF §10). No space after USB, because the proven-scanned example is
            # USBBTProbe-v0.2 and this is not the run to test a new variable.
            # ★★★★ "USB Bluetooth Support", WITH SPACES, PROVED 2026-10-03.
            # The Expert's rule is that a scanned filename BEGINS "USB" (measured
            # 2026-08-30: a non-USB name never appeared in the scan at all and the
            # device failed fnfErr -43, silently). Whether a SPACE after USB was
            # allowed had never been tested -- the comment here used to say so and
            # decline to test it. It is allowed: renamed by hand, rebooted, and the
            # driver came up with its block live and refreshing. Reads far better,
            # and matches the 2003 prior art's "USB Bluetooth Driver".
            # ⚠ --name <value>, NOT --name=value: set-custom-icon.py parses argv by
            # hand. Same trap the CSM rename hit; it fails with only a usage line.
            --name "USB Bluetooth Support"
    DEPENDS USBBluetoothSupport-v${DRIVER_VER}.ndrv
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py)

# ⚠⚠ PROVE THE ARTIFACT MATCHES THE SOURCE. v6.8 was staged and RUN with its
# diagnostic gate compiled as 0 while bt_pump.h said 1 -- sub-second edits defeated
# make's mtime comparison, the rebuild was silently skipped, and a hardware boot was
# spent on a build that answered nothing. Nothing in the source tree or the make output
# was wrong; only the binary knew. See scripts/verify-probe-gate.py.
add_custom_command(TARGET OS9BTProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "-- checking the probe gate in the built driver")
add_custom_command(OUTPUT probe-gate.verified
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify-probe-gate.py
            USBBluetoothSupport-v${DRIVER_VER}.ndrv
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bt_pump.h
    COMMAND ${CMAKE_COMMAND} -E touch probe-gate.verified
    DEPENDS USBBluetoothSupport-v${DRIVER_VER}.ndrv
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify-probe-gate.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/bt_pump.h)

add_custom_target(ndrv ALL DEPENDS USBBluetoothSupport-v${DRIVER_VER}.bin
                                   probe-gate.verified)

# ===========================================================================
#  USBBluetoothSwitch -- the second extension, and the reason there are two
# ===========================================================================
# One 'ndrv' exports ONE TheUSBDriverDescription, and on this stack nothing beyond its
# FIRST record has ever demonstrably matched anything. So one file can express exactly
# one match rule -- and this project needs two live at once, because the CSR mode
# switch makes the card vanish at 05AC:1000 and reappear at 05AC:8204.
#
# Two rules, two files. It also sidesteps the only-first-record question rather than
# fighting it: each file gets its own record 1, so the design holds whether that
# inference is right or wrong. See the header of src/bt_switch.c.
#
# ⚠ SEPARATE TARGET, NOT A SECOND SOURCE FILE IN OS9BTProbe. It must be its own PEF
# with its own exports, and it deliberately links NONE of BTstack -- it opens no pipe
# and runs no stack. Sharing the driver's library would drag 17 BTstack sources into a
# file whose whole job is one control request.
set(SWITCH_VER "1.7")

add_library(OS9BTSwitch SHARED src/bt_switch.c)
target_compile_definitions(OS9BTSwitch PRIVATE TYPE_BOOL=1)
target_include_directories(OS9BTSwitch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(OS9BTSwitch PROPERTIES
    LINK_FLAGS -Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/src/bt_switch.exp)
# ⚠ InterfaceLib ADDED IN SWITCHER 1.2: FindFolder / FSMakeFSSpec / FSpDelete, for
# the one-shot pair-mode flag SwWantsPairMode consumes. Task level only -- the
# Expert calls Initialize at task level -- and every failure path declines the card,
# which is the state where the keyboard works.
target_link_libraries(OS9BTSwitch USBServicesLib USBManagerLib DriverServicesLib
                                  InterfaceLib)

add_custom_command(OUTPUT OS9BTSwitch.pef
    COMMAND ${MAKE_PEF} libOS9BTSwitch.so -o OS9BTSwitch.pef
    DEPENDS OS9BTSwitch)

# Same mainSection fix as the driver: MakePEF emits an out-of-bounds main and every
# stock OS 9 USB class driver declares none at all.
add_custom_command(OUTPUT OS9BTSwitch.nomain.pef
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pef-clear-main.py
            OS9BTSwitch.pef OS9BTSwitch.nomain.pef
    DEPENDS OS9BTSwitch.pef ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pef-clear-main.py)

# ⚠⚠ THE FILENAME MUST BEGIN "USB". Proved on hardware 2026-08-30: the USB Expert only
# scans Extensions files whose name starts with USB. A correct ndrv/usbd driver named
# anything else is skipped in SILENCE and the device fails with fnfErr (-43). This
# applies to the switcher exactly as much as to the driver, and getting it wrong here
# would look identical to "the switch does not work".
add_custom_command(OUTPUT USBBluetoothSwitch-v${SWITCH_VER}.ndrv
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/bt_switch.r
            ${REZ_INCLUDE_FLAG} -I${CMAKE_CURRENT_SOURCE_DIR}
            -o USBBluetoothSwitch-v${SWITCH_VER}.ndrv
            --cc USBBluetoothSwitch-v${SWITCH_VER}.raw.bin
            -t "ndrv" -c "usbd"
            --data OS9BTSwitch.nomain.pef
    DEPENDS OS9BTSwitch.nomain.pef ${CMAKE_CURRENT_SOURCE_DIR}/bt_switch.r)

# ⚠ set-custom-icon.py is used here ONLY for its --name and CRC recomputation. This
# file carries no icon family, on purpose: a generic extension icon is visibly
# different from the driver's Bluetooth one, and the two get dragged in and out
# independently. A look-alike icon is exactly what would cost a run.
add_custom_command(OUTPUT USBBluetoothSwitch-v${SWITCH_VER}.bin
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py
            USBBluetoothSwitch-v${SWITCH_VER}.raw.bin
            USBBluetoothSwitch-v${SWITCH_VER}.bin
            --name USBBluetoothSwitch
            # ⚠ This file carries NO icon family, so hasCustomIcon must NOT be set --
            # it would tell the Finder to find an icon that is not there, which can
            # render as a BLANK icon. Blank reads as corrupt, which is the opposite of
            # the intent: the switcher should look plainly DIFFERENT from the driver's
            # Bluetooth icon, not broken.
            --no-icon-flag
    DEPENDS USBBluetoothSwitch-v${SWITCH_VER}.ndrv
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py)

add_custom_target(switch ALL DEPENDS USBBluetoothSwitch-v${SWITCH_VER}.bin)

# ⚠⚠ THE PAIR-MODE FLAG FILENAME IS A CONTRACT BETWEEN TWO SEPARATELY BUILT BINARIES.
#
# cpanel/bt_panel.c CREATES "Bluetooth Pair At Restart" in the Preferences folder and
# src/bt_switch.c LOOKS FOR IT at its next Initialize. Nothing links them, so a typo or
# a rename in either one fails in the worst possible way: silently. The panel would
# report success, the user would restart, and the switcher would find no flag and idle
# -- indistinguishable from "the switcher ignored it".
#
# This project's whole history of cross-binary constants says to enforce it: the block
# terminator kWEnd, the driver tag kExpectedDriverTag, the mailbox command numbers.
# Same shape, same guard.
#
# ⚠⚠⚠ EVERY FILE ANY GUARD IN THIS DIRECTORY READS MUST BE LISTED HERE, and two were
# not. A configure-time guard only runs when configure runs, and configure only runs
# when a listed file changes -- so an unlisted file is a guard that stays SILENT
# through the exact edit it exists to catch. That defect was already found once this
# session in the version-string guard, and it was still present here: the list held
# bt_switch.c and bt_panel.c, while the kWEnd guard below also reads src/bt_probe.c
# and bt-check/bt_check.c.
#
# MEASURED at v8.8, not reasoned: `touch cpanel/bt_panel.c` re-ran configure,
# `touch src/bt_probe.c` and `touch bt-check/bt_check.c` did not. src/bt_probe.c is
# the AUTHORITATIVE side of kWEnd and every block growth starts there, so the guard
# against the mistake that "has now bitten SEVEN times" could not fire on the edit
# that causes it. ⇒ Add a file here in the same commit that adds a guard reading it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bt_switch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cpanel/bt_panel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bt_probe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt-check/bt_check.c)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/bt_switch.c _sw_src)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/cpanel/bt_panel.c _pn_src)
string(FIND "${_sw_src}" "\\pBluetooth Pair At Restart" _sw_flag)
string(FIND "${_pn_src}" "\\pBluetooth Pair At Restart" _pn_flag)
if(_sw_flag EQUAL -1 OR _pn_flag EQUAL -1)
    message(FATAL_ERROR
        "PAIR-MODE FLAG NAME MISMATCH. Both src/bt_switch.c (which consumes it) and "
        "cpanel/bt_panel.c (which creates it) must contain the Pascal string "
        "\"\\pBluetooth Pair At Restart\". Found in switcher: ${_sw_flag}, in panel: "
        "${_pn_flag} (-1 means absent). A rename in one binary only makes pairing mode "
        "silently never start -- the panel reports success and the switcher idles.")
endif()

# ⚠⚠ THIS GUARD USED TO DEMAND THE WORD "DIAGNOSTIC", AND ITS PREMISE HAS EXPIRED.
#
# It existed because installing the switcher took away a working keyboard: it claimed
# the A1044 at EVERY boot, so a keyboard paired through the card's on-chip stack stopped
# typing until the extension was dragged out of Extensions. Get Info had to say so.
#
# Switcher 1.2 is idle at boot. It claims the card only for the one boot after the panel
# asks for pairing mode, and every failure path declines -- which is the state where the
# keyboard works. The hazard the word warned about is gone, so keeping the word would be
# a warning about nothing, and warnings about nothing are how real ones get ignored.
#
# ⭐ The guard is not deleted, it is REPOINTED at what is now true. It has now been
# repointed TWICE, and both times it fired first and made the change deliberate:
#   1.2  "idle at boot"            -- the extension stopped claiming the card by default
#   1.3  "claims the A1044 at boot" -- it claims by default again, because the driver can
#        now TYPE (M5, 2026-09-14), so owning the card no longer costs the user a
#        keyboard. Its own text said "if that ever changes again, say so there and change
#        this check deliberately", and this is that.
#   1.4  string UNCHANGED, and that is the right answer rather than an oversight: 1.4 adds
#        one extra switch attempt per boot when the previous one died on a stale device
#        ref, which changes nothing about whether the extension claims the card at
#        startup or what protects the user when the driver does not come up. Those two
#        facts are the whole job of this string, and both still hold. The version numeral
#        moves because the LOGIC moved -- see the v1.4 block in src/bt_switch.c.
# ⚠ The fact whoever finds this extension needs is whether it takes the keyboard away at
# startup. 1.3 DOES claim the card -- so the string must also say what protects them,
# which is the one-boot fallback when the driver does not come up.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_switch.r _sw_rez)
if(NOT _sw_rez MATCHES "Bluetooth Switch ${SWITCH_VER} - claims the A1044 at boot")
    message(FATAL_ERROR
        "bt_switch.r's LONG vers string must read \"Bluetooth Switch ${SWITCH_VER} - "
        "idle at boot; ...\". Get Info shows the LONG string, and the one thing anyone "
        "finding this extension needs to know is whether it takes the keyboard away at "
        "startup. As of 1.2 it does not -- if that ever changes again, say so there and "
        "change this check deliberately.")
endif()

# ⚠⚠ THE THREE COPIES OF THE DRIVER VERSION MUST AGREE, AND THIS PROVES IT AT
# CONFIGURE TIME RATHER THAN LEAVING IT TO A HARDWARE RUN.
#
# The filename comes from DRIVER_VER above. The 'vers' resource and the counter-block
# build tag both live in src/bt_probe.c and cannot be injected (one is Rez-adjacent
# data, the other is scanned for as a literal by EnsureBlock). A mismatch is NOT
# cosmetic: BTCheck and the control panel both gate on the block tag, so a driver whose
# tag says v410 while the file says v4.2 is rejected by its own readers -- which reads
# as "no driver found" and gets diagnosed as an install fault. That has cost a run.
# The tag is the four ASCII bytes 'v', major, minor, '0' -- so each digit becomes
# 0x3<digit>, NOT the raw digit.
#
# ⚠⚠ THE MAJOR MAY BE TWO DIGITS SINCE v10.0, and this block's own comment used to say
# "single-digit major and minor only [...] a two-digit part would need a different
# encoding entirely". It was right, and the driver then hit 9.9. The encoding it needed
# is one character: majors 10 and up continue into the letters, 10 -> 'A' (0x41), which
# keeps the tag four bytes and keeps it monotonic ('9' is 0x39, 'A' is 0x41, so
# 'v990' < 'vA00'). scripts/bump-version.py majchar() is the same mapping, and this
# check exists precisely to catch the two drifting apart.
#
# ⚠ THE MINOR IS STILL ONE DIGIT. 4.9 goes to 5.0, never 4.10.
string(REPLACE "." ";" _vparts "${DRIVER_VER}")
list(GET _vparts 0 _vmaj)
list(GET _vparts 1 _vmin)
if(_vmaj LESS 10)
    set(_vmajhex "3${_vmaj}")                      # '0'..'9' = 0x30..0x39
else()
    # A literal table rather than arithmetic, so this needs no OUTPUT_FORMAT support
    # and is obvious to read: 'A'..'Z' for majors 10..35.
    set(_LETTER_HEX 41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F
                    50 51 52 53 54 55 56 57 58 59 5A)
    math(EXPR _vmajidx "${_vmaj} - 10")
    list(LENGTH _LETTER_HEX _letters_n)
    if(_vmajidx GREATER_EQUAL _letters_n)
        message(FATAL_ERROR
            "DRIVER_VER major ${_vmaj} is past 'Z'. The block tag encodes the major as "
            "ONE byte (0-9 then A-Z), so 35 is the ceiling. Time for a wider tag.")
    endif()
    list(GET _LETTER_HEX ${_vmajidx} _vmajhex)
endif()
set(EXPECTED_TAG "0x76${_vmajhex}3${_vmin}30UL")   # 'v' major minor '0'
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/bt_probe.c _probe_src)
string(FIND "${_probe_src}" "${EXPECTED_TAG}" _tag_at)
if(_tag_at EQUAL -1)
    message(FATAL_ERROR
        "DRIVER_VER is ${DRIVER_VER} so the block build tag must be ${EXPECTED_TAG}, "
        "but src/bt_probe.c does not contain it. Update BOTH tag sites (EnsureBlock's "
        "scan and the initialiser), or BTCheck and the control panel will reject this "
        "driver as the wrong version.")
endif()

# ⚠⚠ AND THE 'vers' RESOURCE, which the first version of this check did NOT cover --
# so v4.2 shipped with a correct block tag and a 'vers' still reading 4.1. Get Info and
# the control panel both read 'vers', so the user was shown "v4.1" by a driver that was
# genuinely v4.2 and had no way to tell which was lying. A version the user can SEE is
# exactly the one that must not drift.
# ⚠⚠⚠ THE BLOCK SIZE MUST AGREE ACROSS ALL THREE PROGRAMS, and this has now bitten
# SEVEN times -- more than any other single mistake on this project.
#
# The counter block is FOUND by matching the 'ENDS' terminator at kWEnd. Grow the block
# and kWEnd moves, so a reader still looking at the old word does not print stale
# numbers -- it prints NO BLOCK FOUND, which reads as "the driver is not loaded" and
# gets diagnosed as an install fault. Every previous occurrence was caught by a human
# noticing; this catches it at configure time instead.
# ⚠⚠⚠ EVERY COMMAND THE PANEL CAN SEND MUST HAVE A CASE IN THE DRIVER'S SWITCH.
#
# kCmdDelStoredKey existed in both binaries from v8.2, the panel sent it, and the driver
# had NO `case` for it -- so every card-side delete fell through to `default:` and was
# answered "unknown command". It survived because the driver carries a four-line comment
# describing that command immediately above a DIFFERENT case, so the switch reads as
# though it handles it. Three hardware runs went into diagnosing a delete that was never
# dispatched.
#
# A comment is not a case. This checks the thing itself: for every MailboxSend(kCmdX)
# in the panel, require a `case kCmdX:` in bt_probe.c.
string(REGEX MATCHALL "MailboxSend\\(kCmd[A-Za-z]+" _sent "${_pn_src}")
foreach(_m1 IN LISTS _sent)
    string(REGEX REPLACE "MailboxSend\\(" "" _cmd "${_m1}")
    string(FIND "${_probe_src}" "case ${_cmd}:" _has)
    if(_has EQUAL -1)
        message(FATAL_ERROR
            "  MAILBOX COMMAND WITH NO HANDLER: cpanel/bt_panel.c sends ${_cmd} but\n"
            "  src/bt_probe.c has no 'case ${_cmd}:' in its mailbox switch. The driver\n"
            "  would answer 'unknown command' and do nothing, silently. Add the case, or\n"
            "  stop sending it. See the note at kCmdDelStoredKey.")
    endif()
endforeach()

# ⚠⚠⚠ AND THE SAME AGREEMENT, WORD BY WORD. kWEnd catches the block being RESIZED. It
# says nothing about an individual index drifting, and an index that drifts does not
# fail loudly -- the panel reads whatever unrelated counter now sits at that number and
# reports it as fact. That is not hypothetical: panel 13.1 showed a keyboard that was
# typing as "Linked, not ready" because the status test read the dormant HID path's
# counter, which is the same failure with the wrong NAME instead of the wrong number.
#
# The check is a script rather than a regex here because the driver's words mostly have
# IMPLICIT enum values, and a regex that only sees `kWFoo = 123` silently skips two
# thirds of them and then reports clean.
# ⚠⚠ A TIMER THAT IS NEVER ADDED TO THE RUN LOOP NEVER FIRES, AND NOTHING SAYS SO.
# 15.0's connect sweep called set_timer without add_timer and reported 0 passes on a run
# loop that had turned 2061 times. Four other timers in the same file got it right on the
# next line. Checked per timer VARIABLE, because a count would have looked close enough.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-timer-pairing.py
    RESULT_VARIABLE _tp_rc
    OUTPUT_VARIABLE _tp_out
    ERROR_VARIABLE  _tp_err)
if(NOT _tp_rc EQUAL 0)
    message(FATAL_ERROR "${_tp_err}\n"
        "  Pair every btstack_run_loop_set_timer() with btstack_run_loop_add_timer().")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-block-words.py
    RESULT_VARIABLE _bw_rc
    OUTPUT_VARIABLE _bw_out
    ERROR_VARIABLE  _bw_err)
if(NOT _bw_rc EQUAL 0)
    message(FATAL_ERROR "${_bw_err}\n"
        "  The panel and the driver disagree about the counter block's layout. Fix the "
        "index in THIS commit: nothing about this fails at run time, it just reports "
        "the wrong number.")
endif()
message(STATUS "${_bw_out}")

string(REGEX MATCH "kWEnd = ([0-9]+)" _m "${_probe_src}")
set(_drv_end "${CMAKE_MATCH_1}")
if(NOT _drv_end)
    message(FATAL_ERROR "could not parse kWEnd from src/bt_probe.c")
endif()
foreach(_reader bt-check/bt_check.c cpanel/bt_panel.c)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/${_reader} _rsrc)
    string(REGEX MATCH "kWEnd[ ]*=?[ ]*([0-9]+)" _rm "${_rsrc}")
    if(NOT CMAKE_MATCH_1 STREQUAL _drv_end)
        message(FATAL_ERROR
            "BLOCK SIZE MISMATCH: src/bt_probe.c has kWEnd = ${_drv_end} but "
            "${_reader} has kWEnd = ${CMAKE_MATCH_1}. The block is found by matching "
            "'ENDS' at kWEnd, so this reader would report NO BLOCK FOUND rather than "
            "stale numbers. Update it in THIS commit.")
    endif()
endforeach()

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_probe.r _probe_rez)
string(FIND "${_probe_rez}" "\"${DRIVER_VER}\"" _vers_at)
if(_vers_at EQUAL -1)
    message(FATAL_ERROR
        "DRIVER_VER is ${DRIVER_VER} but bt_probe.r's 'vers' resource does not contain "
        "the string \"${DRIVER_VER}\". Update the BCD bytes AND the version string, or "
        "Get Info and the control panel will report the wrong version.")
endif()
