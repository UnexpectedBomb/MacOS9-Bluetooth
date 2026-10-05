cmake_minimum_required(VERSION 3.13)
project(BTCheck C)

# Configure with the Retro68 PowerPC toolchain:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
#
# READ-ONLY DIAGNOSTIC, a normal PowerPC application. Double-click it under OS 9:
# it scans the System heap for the Bluetooth driver's 'BTP1' counter block and
# reports how far the driver actually got, because the USB Expert's log will not
# carry the driver's own messages. See the header comment in bt_check.c.
#
# The VERSION is in the target name so the user can confirm at a glance which
# build ran. Bump it every iteration, and the window title in bt_check.c to match.
#
# NOTE: no APM .img wrapping and no push script here. Only the MacBinary .bin is
# staged to the Pi (the .img is never used), so staging is done explicitly with
# usb2-ehci/scripts/stage-to-pi.sh.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# ⚠⚠ ONE PLACE FOR THE VERSION, and it had to become one.
#
# v46 wrote a log file called BTCheck_v45.log, because the number lived in FOUR
# independent places: this target name, and the log filename, window title and banner
# inside bt_check.c. The 'vers' resource had drifted further still -- it said 26.0
# while the file on disk said v45. A version that appears in more than one place is a
# version that will eventually disagree with itself, and when it does, the user is
# left reconciling filenames by hand to work out which run they are looking at.
#
# Bumping THIS is now the whole job: it names the app and is injected into the C so
# the log, the title and the banner cannot drift from it again.
set(BTCHECK_VER "99.105")

add_application(BTCheck_v${BTCHECK_VER}
    bt_check.c
    bt_check.r
    TYPE     "APPL"
    CREATOR  "BTCK"
)
target_compile_definitions(BTCheck_v${BTCHECK_VER}
    PRIVATE kBTCheckVer="${BTCHECK_VER}")

# Everything used here is InterfaceLib: SystemZone/BlockMoveData (Memory),
# FindFolder, FSMakeFSSpec/FSpCreate/FSpOpenDF/FSWrite/FSClose/FlushVol (Files),
# GetCurrentProcess/GetProcessInformation (Processes), the Toolbox and SysBeep.
# No NameRegistryLib and no PCILib: this app touches no hardware.

# ⚠ USBManagerLib is REQUIRED as of v21: the bus dump calls
# USBGetNextDeviceByClass / USBGetDeviceDescriptor, the app-level half of the USB
# Manager. Those are enumerate-and-read-descriptors only -- no transfers, which is
# all an application is permitted. Without this link the build fails at link time
# rather than silently, which is the outcome we want.
# ⚠ NameRegistryLib IS REQUIRED as of v96. The comment above said "No NameRegistryLib
# and no PCILib: this app touches no hardware" -- still true of PCILib, no longer true
# of the registry. The descriptor section reads the USB Expert's PUBLISHED properties
# (RegistryEntryIterate / RegistryPropertyIterate / RegistryPropertyGet), which is a
# read-only traversal of a data structure, not a hardware access. fw-regdump is the
# precedent: an application doing exactly this since 2026-08.
# ⚠ SoundLib is for M6's hardware-volume probe: Get/SetSoundOutputInfo live there,
# NOT in InterfaceLib. FindNextComponent and Get/SetDefaultOutputVolume resolve
# from InterfaceLib implicitly, so only these two needed a new library -- which is
# why the link failed on exactly those symbols and nothing else.
# ⚠ CursorDevicesGlue is a SEPARATE import library -- none of the fourteen
# CursorDevice* entry points live in InterfaceLib. The driver already learned this;
# BTCheck needs it now too, to READ what the machine's other mice are set to.
target_link_libraries(BTCheck_v${BTCHECK_VER} PRIVATE USBManagerLib NameRegistryLib SoundLib
                      CursorDevicesGlue)

# ⚠⚠ BTCheck'S IDEA OF WHICH DRIVER IT EXPECTS MUST COME FROM THE DRIVER.
#
# kExpectedDriverTag in bt_check.c drives a "WRONG DRIVER ... Do not analyse it" banner.
# On the v93 run it printed that over a log from the CORRECT v8.2 driver, because the
# constant still said 'v810' -- the check built to prevent a version mistake became one.
# Its own comment said 'v650' while the constant said 'v810', so the line had drifted
# twice with nothing to catch either.
#
# src/bt_probe.c's `gCB[kWBuild] = 0x...` is the authoritative value. Parse it and fail
# the build on disagreement. A comment asking a human to remember is not a mechanism.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/bt_check.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_probe.c)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_probe.c _drv_src)
string(REGEX MATCH "gCB\\[kWBuild\\] = (0x[0-9a-fA-F]+)UL" _dm "${_drv_src}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "could not parse the block build tag from src/bt_probe.c")
endif()
set(_drv_tag "${CMAKE_MATCH_1}")

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_check.c _chk_src)
string(REGEX MATCH "kExpectedDriverTag (0x[0-9a-fA-F]+)UL" _cm "${_chk_src}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "could not parse kExpectedDriverTag from bt_check.c")
endif()

if(NOT _drv_tag STREQUAL CMAKE_MATCH_1)
    message(FATAL_ERROR
        "DRIVER TAG MISMATCH: src/bt_probe.c stamps the block ${_drv_tag} but "
        "bt_check.c's kExpectedDriverTag is ${CMAKE_MATCH_1}. BTCheck would print "
        "\"WRONG DRIVER ... Do not analyse it\" over a perfectly good run -- which is "
        "exactly what happened on v93. Update kExpectedDriverTag AND its comment.")
endif()

if(TARGET retrocrt)
    add_dependencies(BTCheck_v${BTCHECK_VER} retrocrt)
endif()
