cmake_minimum_required(VERSION 3.13)
project(BTPanel C)

# Configure with the Retro68 PowerPC toolchain:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build
#
# CONTAINER SPIKE for the Bluetooth control panel. See docs/CPANEL-SPIKE.md.
#
# ⚠ WHY THIS IS AN APPLICATION AND NOT A 'cdev'. The classic control panel is a 68K
# 'cdev' code resource driven by initDev/hitDev/macDev against a DITL at -4064, and
# NONE of that protocol exists in the Universal Interfaces this project builds
# against -- checked, not assumed. Apple dropped it before OS 9 and implemented
# control panels as applications in the Control Panels folder from Mac OS 8.5 on.
# That is also the one container this project has already proven: BTCheck.
#
# The file is named "Bluetooth" with no version in it, deliberately, following the
# convention the user set for the extension: the ARTIFACT carries the version in its
# .bin filename, the installed file has a stable name so dropping a newer build in
# prompts to replace the older one, and Get Info shows which version is there.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

# ⚠⚠⚠ THIS CHECK IS DUPLICATED FROM ../CMakeLists.txt ON PURPOSE, AND THAT IS THE WHOLE
# POINT OF PUTTING IT HERE.
#
# cpanel is a SEPARATE CMake PROJECT -- the top level does not add_subdirectory() it --
# so every cross-binary guard up there (kWEnd, the mailbox-command check, this one) runs
# only when the DRIVER is configured. A panel-only build, which is what every UI change
# is, ran none of them. A guard that does not run on the build you actually do is not a
# guard; it is a comment with a CMake syntax.
#
# ⚠ It reads ../src/bt_probe.c, which is always present: the panel is never built from
# outside this repository.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-block-words.py
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

add_application(Bluetooth
    bt_panel.c
    bt_panel.r
    TYPE     "APPL"
    CREATOR  "BTcp"
)

# ⚠ AppearanceLib IS REQUIRED. The platinum look comes from the Appearance Manager --
# SetThemeWindowBackground, DrawThemeListBoxFrame, DrawThemePrimaryGroup,
# DrawThemeSecondaryGroup, RegisterAppearanceClient -- and NONE of those are in
# InterfaceLib. Without this the build fails at link with six undefined references
# rather than silently, which is the outcome we want.
#
# Everything else is InterfaceLib: SystemZone/BlockMoveData (Memory), the List
# Manager and the rest of the Toolbox. No USBManagerLib -- unlike BTCheck this panel
# touches no hardware at all, it only reads the driver's block from the System heap.
# ⚠ USBManagerLib is required as of 0.5: the panel now looks at the USB bus itself
# rather than guessing why no driver is loaded. USBGetNextDeviceByClass and
# USBGetDeviceDescriptor are the app-level half of the USB Manager -- enumerate and
# read descriptors, no transfers -- which is all an application may do. BTCheck has
# used them since its v21.
# ⚠ IconServicesLib is a SEPARATE import library, not part of InterfaceLib or
# AppearanceLib -- the same split that caught the Cursor Device Manager. The
# rename dialog's note icon (GetIconRef/PlotIconRef/ReleaseIconRef) lives only
# here, and without it the link fails on exactly those three symbols.
target_link_libraries(Bluetooth PRIVATE AppearanceLib USBManagerLib IconServicesLib)

# ⚠⚠ MAKE THE Rez #include DEPENDENCY REAL. bt_panel.r does `#include "bt_icon.r"`,
# and CMake does not track Rez includes -- so editing only the icon file leaves Rez
# skipped and the build keeps stamping the OLD ARTWORK with no error at all. This
# project has already been bitten by exactly that with splash.r and PICTs
# (feedback_rez_include_deps).
#
# The fix is a stamp target that touches bt_panel.r whenever bt_icon.r changes, which
# is what makes Rez re-run. Touching a source file from the build is inelegant; a
# silently stale icon is worse.
#
# ⚠ BOTH #included .r FILES MUST BE LISTED. bt_panel.r now includes bt_icon.r (the
# panel's own Finder icon, ID 128) AND bt_device_icons.r (the per-kind row icons,
# IDs 200..205). Adding the second include without adding it here would reintroduce
# exactly the failure this target exists to prevent, for the new file only -- editing
# the device artwork would leave Rez skipped and keep stamping the old icons silently.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/.bt_icon.stamp
    COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${CMAKE_CURRENT_SOURCE_DIR}/bt_panel.r
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/.bt_icon.stamp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bt_icon.r
            ${CMAKE_CURRENT_SOURCE_DIR}/bt_device_icons.r
            ${CMAKE_CURRENT_SOURCE_DIR}/bt_radio_icons.r
            # ⚠ v15.4: the DOCUMENT icon (ID 129) drawn on every file the stack
            # generates. Listed the moment the include was added, which is the only
            # time this is ever remembered.
            ${CMAKE_CURRENT_SOURCE_DIR}/bt_prefs_icon.r
    COMMENT "icon artwork changed -- forcing bt_panel.r to re-Rez")
add_custom_target(bt_icon_dep DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/.bt_icon.stamp)
add_dependencies(Bluetooth bt_icon_dep)

# ⚠ Rez stamps type and creator but has NO option for Finder flags, so a freshly
# Rez'd file has fdFlags = 0 -- and without kHasBundle the Finder never reads the
# BNDL/FREF and THE ICON SILENTLY DOES NOT APPEAR. No error, no warning, just the
# generic application icon. Same patcher the CPU Temp CSM uses.
#
# ⚠ ATTACHED TO Bluetooth_APPL, NOT Bluetooth. add_application builds the executable
# in target `Bluetooth` and then Rezzes the .bin in a SEPARATE target `<name>_APPL`,
# so a POST_BUILD on `Bluetooth` runs before the .bin exists and fails with
# FileNotFoundError. Found by doing exactly that.
add_custom_command(TARGET Bluetooth_APPL POST_BUILD
    COMMAND /usr/bin/env python3
            # ⚠⚠ RENAMED, and the old name was a real bug. Both this file and
            # bluetooth/CMakeLists.txt said "${CMAKE_CURRENT_SOURCE_DIR}/scripts/
            # set-custom-icon.py", which resolved to a DIFFERENT script for each: the
            # driver got bluetooth/scripts/ (recomputes the MacBinary CRC), the panel
            # got cpanel/scripts/ (did not). The panel therefore shipped a stale CRC in
            # every build for about twenty revisions. Distinct names now.
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/set-finder-flags.py
            ${CMAKE_CURRENT_BINARY_DIR}/Bluetooth.bin APPLBTcp
    COMMENT "Setting kHasBundle | kHasCustomIcon on Bluetooth.bin")

# ⚠⚠ A VERSIONED COPY ON DISK, INTERNAL NAME UNCHANGED -- the same split the driver
# and BTCheck already use, and the panel was the one artifact still staged as a bare
# "Bluetooth.bin". That made the Pi share ambiguous about which panel was there, which
# is exactly the confusion versioned filenames exist to remove.
#
# ⚠ THE COPY IS WHAT GETS VERSIONED, NEVER THE MACBINARY'S INTERNAL NAME. add_application
# already stamps that as "Bluetooth", and it MUST stay so: dropping the file into Control
# Panels then prompts to REPLACE the installed panel instead of leaving two side by side,
# which is the failure that killed run 27 for the driver. Get Info shows the version from
# the 'vers' resource.
#
# ⚠ set(PANEL_VER) is maintained by scripts/bump-version.py alongside the 'vers'
# resource, so it cannot drift -- a second hand-typed copy of a version is how the
# BTCheck log filename ended up one revision behind for twenty builds.
set(PANEL_VER "19.2")
add_custom_command(TARGET Bluetooth_APPL POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_BINARY_DIR}/Bluetooth.bin
            ${CMAKE_CURRENT_BINARY_DIR}/Bluetooth-${PANEL_VER}.bin
    COMMENT "versioned copy: Bluetooth-${PANEL_VER}.bin (internal name stays Bluetooth)")

# ⚠⚠ THE VERSION IS WRITTEN IN FOUR PLACES AND ALL FOUR MUST AGREE.
#
# PANEL_VER above names the staged file. bt_panel.r carries the 'vers' resource Get Info
# shows, and the 'BTcp' signature. And bt_panel.c hardcodes it into the status line the
# panel writes to the shared volume -- which is the ONE the reader of a hardware run
# actually sees first.
#
# That fourth copy went stale on 7.3: the status file said "BTPanel 7.2" and the run
# looked like it had been done with the wrong binary. Nothing but a subtle argument about
# block layout ruled that out. A build that cannot be trusted to state its own version is
# worse than a build that fails, so fail it.
# ⚠⚠ AND THE GUARD MUST RE-RUN WHEN THE SOURCE CHANGES, WHICH IT DID NOT AT FIRST.
#
# These checks run at CONFIGURE time. `cmake --build` does not re-configure just
# because a .c file changed, so the first version of this guard sat silent through the
# exact edit it exists to catch -- verified by deliberately staling the string and
# watching the build succeed. Listing the two files as configure dependencies makes any
# edit to them force a re-configure, so the check actually runs. Same lesson as the
# v6.8 mtime failure: a check that can be skipped is not a check.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/bt_panel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bt_panel.r)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_panel.c _panel_src)
string(FIND "${_panel_src}" "\"BTPanel ${PANEL_VER}  ticks \"" _stat_at)
if(_stat_at EQUAL -1)
    message(FATAL_ERROR
        "PANEL VERSION MISMATCH: PANEL_VER is ${PANEL_VER} but bt_panel.c's status "
        "line does not say \"BTPanel ${PANEL_VER}\". That string is the first thing "
        "read out of a hardware run, and a stale one makes the log look like the wrong "
        "binary was installed. Fix the PStrCat in bt_panel.c.")
endif()

# And the 'vers' resource, same class of bug, same cost to check.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/bt_panel.r _panel_rez)
string(FIND "${_panel_rez}" "\"${PANEL_VER}\"" _vers_at)
if(_vers_at EQUAL -1)
    message(FATAL_ERROR
        "PANEL VERSION MISMATCH: PANEL_VER is ${PANEL_VER} but bt_panel.r's 'vers' "
        "resource does not contain the string \"${PANEL_VER}\". Get Info shows that "
        "string, so a stale one is the only version the user ever reads.")
endif()

if(TARGET retrocrt)
    add_dependencies(Bluetooth retrocrt)
endif()
