cmake_minimum_required(VERSION 3.13)

# RP2040 board header. Do NOT pass -DPICO_BOARD on the command line — that
# overrides this cached value and silently reverts to the wrong config.
set(PICO_BOARD freewili_og CACHE STRING "Board type" FORCE)
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/bsp/boards")

# UART0 is the inter-CPU link on BOTH CPUs and must never carry stdio. The SDK
# would otherwise default stdio to UART and corrupt the link. Each app also
# calls fwog_configure_stdio(); this is the belt to that pair of braces.
set(PICO_STDIO_UART 0 CACHE BOOL "stdio over UART (forced off: UART0 is the link)" FORCE)

include(pico_sdk_import.cmake)
project(wiliOGBsp C CXX ASM)
set(CMAKE_C_STANDARD 11)
# genimage.py runs at build time to embed the display firmware.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

pico_sdk_init()

# Optionally fetch LVGL, so `apps/lvgl` can actually be built from a plain
# checkout. OFF by default: every other app here builds with nothing but the
# Pico SDK, and turning that into "and a network fetch" for everybody would be
# a poor trade for one example.
#
# THIS LIVES IN THE TOP-LEVEL FILE ON PURPOSE. A project consuming this BSP
# does add_subdirectory(<bsp>/bsp) and never sees this, so the library's
# contract is unchanged: bsp/CMakeLists.txt still only builds
# fwog_display_lvgl against an `lvgl` target somebody else supplied. This is a
# convenience of THIS repo's own build, not part of the BSP.
#
# It must come BEFORE add_subdirectory(bsp): bsp/CMakeLists.txt decides
# whether to define fwog_display_lvgl by testing for the target.
option(FWOG_LVGL_FETCH "Fetch LVGL so the apps/lvgl example can build" OFF)
if(FWOG_LVGL_FETCH)
    include(FetchContent)
    # Pinned, not floating: the port is developed against this tag and LVGL 9
    # has moved its display/indev APIs before.
    FetchContent_Declare(lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v9.2.2
        GIT_SHALLOW TRUE)
    set(LV_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/bsp/display_cpu/lvgl/lv_conf.h
        CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(lvgl)
    message(STATUS "fwog: LVGL fetched; the apps/lvgl example will be built")
endif()

add_subdirectory(bsp)

# Each app opts in; add new apps here. `fw new-app` scaffolds the directory
# but does not edit this file.
# Version string for the bootloader banner and for the display image the
# main CPU embeds. Evaluated at CONFIGURE time -- which is fine, because
# `fw build` configures on every invocation.
#
# Deliberately NOT compiled into display applications: their bytes must not
# change just because the git description did, or every commit would
# perturb the image CRC32 and force a reflash with no code change.
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE FWOG_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _fwog_git_rc)
if(NOT _fwog_git_rc EQUAL 0 OR FWOG_GIT_DESCRIBE STREQUAL "")
    set(FWOG_GIT_DESCRIBE "dev")   # building outside a repository
endif()
message(STATUS "fwog version: ${FWOG_GIT_DESCRIBE}")

# The git COMMIT time, for the UF2 info record's build_ts. Deliberately NOT
# the wall clock: a configure-time timestamp would make two builds of
# identical source produce different bytes, for no information gained.
#
# Like FWOG_GIT_DESCRIBE above, this never reaches a display APPLICATION --
# see fwog_add_uf2_info() in bsp/CMakeLists.txt.
execute_process(
    COMMAND git log -1 --format=%ct
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE FWOG_GIT_COMMIT_TS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _fwog_ts_rc)
if(NOT _fwog_ts_rc EQUAL 0 OR NOT FWOG_GIT_COMMIT_TS MATCHES "^[0-9]+$")
    set(FWOG_GIT_COMMIT_TS 0)   # building outside a repository
endif()

# The app offset appears as a literal in three places: app_meta.h,
# fwog_display_app()'s linker region, and the UF2 the bootloader jumps into.
# app_meta.h is the source of truth; this parses it so a change there breaks
# the build here instead of producing display apps the bootloader cannot run.
file(READ ${CMAKE_CURRENT_LIST_DIR}/bsp/common/app_meta.h _fwog_meta_h)
string(REGEX MATCH "#define FWOG_APP_OFFSET[ \t]+(0x[0-9A-Fa-f]+)u"
       _m "${_fwog_meta_h}")
if(NOT CMAKE_MATCH_1 STREQUAL "0x21000")
    message(FATAL_ERROR
        "FWOG_APP_OFFSET is ${CMAKE_MATCH_1} in bsp/common/app_meta.h but "
        "fwog_display_app() links at 0x10021000. Update bsp/CMakeLists.txt.")
endif()

# One folder per app, and ORDER NO LONGER MATTERS between them.
#
# A display app and its main companion are one deliverable -- the main UF2
# carries the display image inside it -- so apps/<name>/CMakeLists.txt
# declares both, display first. fwog_embed_display_image() defaults to that
# pair, so the display half always exists by the time the main half asks for
# it. A folder with no main/ (lcd, bl) simply has no companion.
#
# This list was previously ordered all-displays-then-all-mains, because
# FWOG_DISPLAY_FIRMWARE resolved a target at configure time and lcd_display,
# declared last, was unreachable as a payload.
#
# ONE ordering rule survives, and it is why lcd and bl come first: the
# DISPLAY-ONLY apps have no main app of their own, so the only way to get one
# onto a board is -DFWOG_DISPLAY_FIRMWARE=<it> plus somebody else's main app,
# and that override still has to name a target that already exists. Declaring
# the companionless folders first makes the override work with every main app
# in the tree. A PAIRED display never needs it -- build that pair's own main.
add_subdirectory(apps/lcd)
add_subdirectory(apps/bl)

add_subdirectory(apps/template)
add_subdirectory(apps/smoke)
add_subdirectory(apps/bench)
add_subdirectory(apps/ogvegas)
# Declares nothing unless LVGL is available -- see apps/lvgl/CMakeLists.txt.
add_subdirectory(apps/lvgl)
# Same LVGL guard as apps/lvgl -- see apps/iotest/CMakeLists.txt.
add_subdirectory(apps/iotest)

# Neither a display app nor a main app: it runs on a CPU nobody has identified
# yet, links fwog_common only, and embeds nothing. Its position in this list is
# therefore free -- the display-before-main ordering above exists only for
# fwog_embed_display_image(), which cpuprobe does not call. Read
# apps/cpuprobe/CMakeLists.txt before changing it.
add_subdirectory(apps/cpuprobe)
