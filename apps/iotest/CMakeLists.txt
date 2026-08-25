# Guided breakout-header I/O test: an on-device LCD UI, no PC required beyond
# flashing it once. See tools/io_walk.py for the PC-side sibling this walks
# the same pairing table as, and AGENTS.md's app-layout section for why a
# display app and its main companion share one folder.
#
# LVGL is not vendored in this BSP (see bsp/display_cpu/lvgl/fwog_lvgl.h), so
# this pair is declared ONLY when a target named `lvgl` exists -- same guard
# as apps/lvgl, and for the same reason: every OTHER app in this tree builds
# with nothing but the Pico SDK, and that stays true if this one is skipped
# rather than made to fail the whole configure.
if(NOT TARGET fwog_display_lvgl)
    message(STATUS
        "fwog: skipping apps/iotest -- no `lvgl` target. "
        "Configure with -DFWOG_LVGL_FETCH=ON to build it.")
    return()
endif()

add_executable(iotest_display display/main.c display/iotest_ui.c proto/iotest_proto.c)
target_link_libraries(iotest_display fwog_display_lvgl)
# Both halves #include "proto/iotest_proto.h" -- a path relative to this
# folder, not to display/ or main/ -- so this folder itself must be on the
# include path.
target_include_directories(iotest_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
fwog_display_app(iotest_display
    VERSION 001
    DESCRIPTION "Guided breakout-header GPIO/I2C loopback test, LCD UI")

add_executable(iotest_main main/main.c main/iotest_seq.c proto/iotest_proto.c)
target_link_libraries(iotest_main fwog_main_bsp)
target_include_directories(iotest_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# No argument, so this carries iotest_display -- the app in this folder.
fwog_embed_display_image(iotest_main)
fwog_main_app(iotest_main
    VERSION 001
    DESCRIPTION "Guided breakout-header GPIO/I2C loopback test, sequencer")
