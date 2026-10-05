cmake_minimum_required(VERSION 3.13)
project(BTCsm C)

# Configure with the Retro68 PowerPC toolchain:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
#
# "Bluetooth" Control Strip module: a battery gauge for paired devices, a click to
# switch between them, and a shortcut into pairing mode.
#
# ⚠ 1.0 was NOT a gauge, on the measured belief that the level was unobtainable. That
# was true of the HID-proxy path and wrong as a general claim -- our own HCI stack reads
# it over HIDP GET_REPORT on undeclared vendor report IDs. docs/CSM-BATTERY-FEASIBILITY.md
# carries both the original study and the correction.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

set(CSM_VER "2.5")

# ⚠⚠ A NATIVE 'cfrg' FRAGMENT, NOT AN 'sdev' RESOURCE. Both load and run, but the
# 'sdev'-resource form is a second-class module the Control Strip will not let you drag
# out of the strip. Every stock module is a cfrg with the PEF in the data fork.
add_library(BTCsm SHARED bt_csm.c)
if(TARGET retrocrt)
    add_dependencies(BTCsm retrocrt)
endif()

# ⚠ The entry point is EXPORTED, not set with the linker's -e -- which drops the symbol
# from the export table. patch-pef-main.py points the fragment `main` at it after
# MakePEF, the same pattern the native 'ndrv' drivers use.
set_target_properties(BTCsm PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.exp")

# ControlStripLib: SBTrackPopupMenu / SBShowHelpString.
# USBManagerLib: USBGetNextDeviceByClass / USBGetDeviceDescriptor -- the app-level half
# of the USB Manager, enumerate-and-read-descriptors only, no transfers. That is how
# this module reads which personality the card is wearing instead of scanning the
# System heap for a block whose terminator index keeps moving.
# QuickDraw / Menus / Files / Folders / Memory resolve from InterfaceLib.
target_link_libraries(BTCsm ControlStripLib USBManagerLib)

add_custom_command(OUTPUT BTCsm.pef
    COMMAND ${MAKE_PEF} libBTCsm.so -o BTCsm.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch-pef-main.py
            BTCsm.pef ControlStripModule
    DEPENDS BTCsm ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch-pef-main.py)

if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Rez the resource fork ('cfrg' + bundle + 'vers'), PEF in the DATA FORK (--data),
# file type 'sdev' so the strip recognises it, creator 'BTcs'.
#
# ⭐ 1.2 CARRIES A REAL ICON FAMILY, so --no-icon-flag is GONE and kHasCustomIcon is set.
# 1.0/1.1 passed it because they had no family, and setting the flag on a file with no
# icon renders as a BLANK icon -- which reads as a corrupt file. Both conditions must
# move together: the family in icons/bt_csm_icon.r and the flag here.
#
# ⚠⚠ --bundle IS SEPARATE AND IS REQUIRED. 1.0 shipped with fdFlags=0x0000 because the
# comment here claimed the patcher "still sets kHasBundle" while the script only ever
# set kHasCustomIcon -- this directory's copy had drifted from cpu-temp's, which had it
# right. Without kHasBundle the Finder never reads the BNDL/FREF, so the module cannot
# be dragged out of the strip, which is the ONLY reason it is built as a 'cfrg'
# fragment instead of the much simpler 'sdev' resource. It loads and runs either way,
# so nothing looks broken; the capability is just quietly absent.
set(SET_ICON ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-custom-icon.py)
# ⚠⚠ THE EXPANDED NAME IS "Bluetooth Strip", NOT "Bluetooth". It used to be the
# latter -- byte-for-byte the control panel's name -- so the two could never sit in
# one folder, and on screen nothing would say which of them you were looking at. The
# Control Strip's own tooltip shows this name too, where "Bluetooth" is ambiguous and
# "Bluetooth Strip" is not.
add_custom_command(OUTPUT BluetoothStrip-v${CSM_VER}.bin
    COMMAND ${REZ} ${REZ_INCLUDE_FLAG}
                   ${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.r
                   -o BluetoothStrip-v${CSM_VER}.raw.bin
                   -t sdev -c BTcs
                   --data BTCsm.pef
    COMMAND /usr/bin/env python3 ${SET_ICON}
            BluetoothStrip-v${CSM_VER}.raw.bin BluetoothStrip-v${CSM_VER}.bin
            --name "Bluetooth Strip" --bundle
    # ⚠⚠ icons/bt_csm_icon.r IS LISTED EXPLICITLY. CMake does not track what a .r file
    # #includes, so without this line editing or regenerating the icon leaves the OLD
    # icon in the built module with no rebuild and no warning -- the exact failure this
    # repo hit with PICT resources (feedback_rez_include_deps).
    DEPENDS BTCsm.pef ${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.r
            ${CMAKE_CURRENT_SOURCE_DIR}/icons/bt_csm_icon.r
            ${CMAKE_CURRENT_SOURCE_DIR}/icons/bt_csm_cell_icon.r ${SET_ICON})

# ⚠ NAME MUST NOT DIFFER FROM THE LIBRARY TARGET ONLY BY CASE. macOS is
# case-INSENSITIVE by default, so a target named "btcsm" beside the library "BTCsm"
# makes CMake generate two colliding CMakeFiles/<name>.dir directories -- and the
# build then fails with the useless "No rule to make target
# CMakeFiles/BTCsm.dir/depend". Cost twenty minutes to spot; the suffix avoids it.
add_custom_target(BTCsm_sdev ALL DEPENDS BluetoothStrip-v${CSM_VER}.bin)

# ---------------------------------------------------------------------------
#  Cross-binary contracts, enforced rather than remembered
# ---------------------------------------------------------------------------
#
# ⚠⚠ THIS MODULE SHARES THREE FILE FORMATS WITH THREE OTHER BINARIES, and nothing links
# any of them:
#
#   Preferences:Bluetooth Device Kinds      panel WRITES, this module READS
#   Preferences:Bluetooth Pair At Restart   panel + this module WRITE, switcher CONSUMES
#   Preferences:Bluetooth Battery           driver WRITES, this module READS
#
# A rename or a magic change in one binary fails silently in the worst way: the module
# lists nothing, or shows no gauge, or writes a flag nobody consumes, and it is
# indistinguishable from "the feature does not work". This project's whole history of
# cross-binary constants says to enforce it -- kWEnd, kExpectedDriverTag, the mailbox
# command numbers.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.r
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpanel/bt_panel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_switch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_keyfile.c)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.c _csm_src)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../cpanel/bt_panel.c _pn_src)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_switch.c _sw_src)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_keyfile.c _kf_src)

foreach(_pair "Bluetooth Device Kinds;_pn_src" "Bluetooth Pair At Restart;_sw_src"
              "Bluetooth Battery;_kf_src")
    list(GET _pair 0 _name)
    list(GET _pair 1 _other)
    string(FIND "${_csm_src}" "\\p${_name}" _a)
    string(FIND "${${_other}}"  "\\p${_name}" _b)
    if(_a EQUAL -1 OR _b EQUAL -1)
        message(FATAL_ERROR
            "SHARED FILENAME MISMATCH for \"${_name}\": csm/bt_csm.c=${_a}, the other "
            "binary=${_b} (-1 means absent). Both must contain the Pascal string, or "
            "the feature silently does nothing and looks like a bug rather than a "
            "rename.")
    endif()
endforeach()

# ⚠ And the format magic, which is just as silent when it drifts: a CSM reading the
# panel's file with the wrong magic simply lists no devices.
string(FIND "${_csm_src}" "0x42544344UL" _csm_magic)
string(FIND "${_pn_src}"  "0x42544344UL" _pn_magic)
if(_csm_magic EQUAL -1 OR _pn_magic EQUAL -1)
    message(FATAL_ERROR
        "kCodMagic MISMATCH: 'BTCD' (0x42544344UL) must appear in BOTH csm/bt_csm.c "
        "and cpanel/bt_panel.c. csm=${_csm_magic}, panel=${_pn_magic}.")
endif()

# ⚠⚠ THE BATTERY FILE'S MAGIC *AND* ITS FORMAT VERSION.
#
# The magic is the same silent-drift risk as kCodMagic. The VERSION is worse, and it is
# checked because bt_keyfile.c says in its own comment that a version 2 is expected once
# BatteryState's meaning is established: the driver would start writing longer records,
# this module would correctly reject them as an unknown format, and the gauge would
# simply go blank. Nothing would crash and nothing would log -- it would look exactly
# like a keyboard that stopped reporting. Bump both, together, or fail here.
string(FIND "${_csm_src}" "0x42544241UL" _csm_bat)
string(FIND "${_kf_src}"  "0x42544241UL" _kf_bat)
if(_csm_bat EQUAL -1 OR _kf_bat EQUAL -1)
    message(FATAL_ERROR
        "kBatMagic MISMATCH: 'BTBA' (0x42544241UL) must appear in BOTH csm/bt_csm.c "
        "and src/bt_keyfile.c. csm=${_csm_bat}, driver=${_kf_bat}.")
endif()

string(REGEX MATCH "kBatFmtVer[ \t]+([0-9]+)UL" _m "${_csm_src}")
set(_csm_batver "${CMAKE_MATCH_1}")
string(REGEX MATCH "kBatFmtVer[ \t]+([0-9]+)UL" _m "${_kf_src}")
set(_kf_batver "${CMAKE_MATCH_1}")
if(NOT _csm_batver OR NOT _kf_batver OR NOT _csm_batver STREQUAL _kf_batver)
    message(FATAL_ERROR
        "kBatFmtVer MISMATCH between csm/bt_csm.c (\"${_csm_batver}\") and "
        "src/bt_keyfile.c (\"${_kf_batver}\"). The reader would reject the writer's "
        "file and the gauge would silently go blank.")
endif()

# ⚠⚠ THE CELL ICON'S RESOURCE ID, IN BOTH PLACES. SBGetDetachIconSuite takes a NUMBER,
# so kIconID in bt_csm.c and the ID on the families in icons/bt_csm_cell_icon.r must be
# the same one. If they drift the suite comes back NULL, the cell quietly falls back to a
# text label, and nothing anywhere says why -- it reads as a design choice rather than a
# dangling reference. Exactly the class of silent cross-file defect that cost this
# project the battery file's address encoding.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/icons/bt_csm_cell_icon.r)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/icons/bt_csm_cell_icon.r _cell_rez)
string(REGEX MATCH "#define kIconID[ \t]+(-?[0-9]+)" _m "${_csm_src}")
set(_c_iconid "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "data '(ics#|ics4|ics8)' \\(([0-9-]+)\\)" _rez_ids "${_cell_rez}")
if(NOT _c_iconid)
    message(FATAL_ERROR "could not find kIconID in csm/bt_csm.c")
endif()
foreach(_hit ${_rez_ids})
    string(REGEX MATCH "\\(([0-9-]+)\\)" _unused "${_hit}")
    if(NOT CMAKE_MATCH_1 STREQUAL _c_iconid)
        message(FATAL_ERROR
            "CELL ICON ID MISMATCH: bt_csm.c has kIconID=${_c_iconid} but "
            "icons/bt_csm_cell_icon.r declares \"${_hit}\". SBGetDetachIconSuite would "
            "find nothing and the cell would fall back to text with no error.")
    endif()
endforeach()
list(LENGTH _rez_ids _rez_n)
if(NOT _rez_n EQUAL 3)
    message(FATAL_ERROR
        "icons/bt_csm_cell_icon.r must declare ics#, ics4 AND ics8 (found ${_rez_n}). "
        "ics# carries the transparency MASK -- without it the strip background does not "
        "show through and the glyph sits in a solid block.")
endif()

# ⚠⚠ THE DEVICE-NAMES FILE: filename, magic, format version AND RECORD WIDTH.
#
# The width is the one that fails worst. Records are FIXED SIZE (2 longs + kNameChars),
# so if the driver writes 24-byte names and the reader expects something else, every
# record after the first is misaligned -- and the symptom is garbled device names, which
# reads as a text or encoding bug rather than as a size disagreement. Nothing links the
# two binaries, so it is checked here.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_pump.h)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../src/bt_pump.h _pump_src)

string(FIND "${_csm_src}" "\\pBluetooth Device Names" _csm_nmf)
string(FIND "${_kf_src}"  "\\pBluetooth Device Names" _kf_nmf)
if(_csm_nmf EQUAL -1 OR _kf_nmf EQUAL -1)
    message(FATAL_ERROR
        "SHARED FILENAME MISMATCH for \"Bluetooth Device Names\": csm=${_csm_nmf}, "
        "src/bt_keyfile.c=${_kf_nmf} (-1 means absent). The CSM would show class-of-device "
        "labels forever and nothing would say why.")
endif()

string(FIND "${_csm_src}" "0x42544E4DUL" _csm_nmm)
string(FIND "${_kf_src}"  "0x42544E4DUL" _kf_nmm)
if(_csm_nmm EQUAL -1 OR _kf_nmm EQUAL -1)
    message(FATAL_ERROR
        "kNmMagic MISMATCH: 'BTNM' (0x42544E4DUL) must appear in BOTH csm/bt_csm.c and "
        "src/bt_keyfile.c. csm=${_csm_nmm}, driver=${_kf_nmm}.")
endif()

string(REGEX MATCH "kNmFmtVer[ \t]+([0-9]+)UL" _m "${_csm_src}")
set(_csm_nmver "${CMAKE_MATCH_1}")
string(REGEX MATCH "kNmFmtVer[ \t]+([0-9]+)UL" _m "${_kf_src}")
set(_kf_nmver "${CMAKE_MATCH_1}")
if(NOT _csm_nmver OR NOT _kf_nmver OR NOT _csm_nmver STREQUAL _kf_nmver)
    message(FATAL_ERROR
        "kNmFmtVer MISMATCH: csm=\"${_csm_nmver}\" driver=\"${_kf_nmver}\".")
endif()

string(REGEX MATCH "#define kNameChars[ \t]+([0-9]+)" _m "${_pump_src}")
set(_drv_nchars "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define kNameChars[ \t]+([0-9]+)" _m "${_csm_src}")
set(_csm_nchars "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define kNmChars[ \t]+([0-9]+)" _m "${_kf_src}")
set(_kf_nchars "${CMAKE_MATCH_1}")
if(NOT _drv_nchars OR NOT _csm_nchars OR NOT _kf_nchars
   OR NOT _drv_nchars STREQUAL _csm_nchars OR NOT _drv_nchars STREQUAL _kf_nchars)
    message(FATAL_ERROR
        "NAME FIELD WIDTH MISMATCH -- fixed-size records would misalign, showing garbled "
        "names: src/bt_pump.h kNameChars=\"${_drv_nchars}\", "
        "src/bt_keyfile.c kNmChars=\"${_kf_nchars}\", "
        "csm/bt_csm.c kNameChars=\"${_csm_nchars}\".")
endif()

# ⚠ BOTH 'vers' strings must carry CSM_VER. Get Info shows the LONG one and the Finder's
# list views show the SHORT one, and a build has already shipped from this repo with the
# two disagreeing -- see reference_os9_getinfo_shows_long_vers_string. A half-bumped
# release is invisible until someone is trying to work out which build they installed,
# which is exactly when they cannot afford the ambiguity.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_csm.r _csm_rez)
string(REGEX MATCHALL "\"${CSM_VER}[ ,\"]" _vers_hits "${_csm_rez}")
list(LENGTH _vers_hits _vers_n)
if(_vers_n LESS 2)
    message(FATAL_ERROR
        "bt_csm.r must carry CSM_VER (\"${CSM_VER}\") in BOTH the short and long 'vers' "
        "strings; found ${_vers_n} occurrence(s). Bump both, or Get Info and the Finder "
        "disagree about which build this is.")
endif()
