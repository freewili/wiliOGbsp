cmake_minimum_required(VERSION 3.13)
project(wiliogbsp_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()

# Standalone tree: no Pico SDK, no pico_sdk_init(). Pure-logic modules from
# ../bsp compile directly against the host C compiler, so tests run natively
# with no hardware. HOST_TEST strips the SDK-dependent parts of headers.
set(BSP ${CMAKE_CURRENT_SOURCE_DIR}/../bsp)

# No warnings claim on this branch means anything without this: every test
# binary is host-compiled, so this is the one place warnings are cheap to
# catch. Guarded by compiler ID because a Windows fallback with no MSYS gcc
# on PATH lands on cl.exe, which does not understand these flags.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

function(fwog_add_test name)
    add_executable(${name} ${name}.c ${CMAKE_CURRENT_SOURCE_DIR}/test_util.c ${ARGN})
    target_include_directories(${name} PRIVATE ${BSP} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE HOST_TEST=1)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

fwog_add_test(test_crc ${BSP}/common/crc.c)
fwog_add_test(test_link_frame ${BSP}/common/crc.c ${BSP}/common/link/link_frame.c)
fwog_add_test(test_clocks ${BSP}/common/clocks.c)
fwog_add_test(test_app_meta ${BSP}/common/crc.c ${BSP}/common/app_meta.c)
# The record fwOGappexplorer scans a UF2 for. Its layout is a wire format
# between two repositories, so the offsets are asserted, not assumed.
fwog_add_test(test_uf2_info ${BSP}/common/crc.c ${BSP}/common/uf2_info.c)
# FWOG_LINK_BAUD normally comes from the target build's cache variable; the
# host tree has no fwog_common, so pass the same default explicitly.
fwog_add_test(test_image_crc ${BSP}/common/crc.c)
fwog_add_test(test_uart_baud ${BSP}/common/link/link_uart.c)
target_compile_definitions(test_uart_baud PRIVATE FWOG_LINK_BAUD=12500000u)

fwog_add_test(test_bl_protocol
    ${BSP}/common/crc.c ${BSP}/common/app_meta.c
    ${BSP}/common/link/link_frame.c ${BSP}/common/link/bl_proto.c)
fwog_add_test(test_io_cfg ${BSP}/common/io_cfg.c)
fwog_add_test(test_io_proto ${BSP}/common/io_cfg.c ${BSP}/common/link/io_proto.c)
fwog_add_test(test_io_seq ${BSP}/common/io_cfg.c ${BSP}/common/io_seq.c)
fwog_add_test(test_io_pins ${BSP}/common/io_cfg.c ${BSP}/common/io_pins.c)

# The one place this file reaches into apps/ rather than bsp/: iotest_seq.c
# is pure decision logic (no SDK) with the same value as io_pins.c's own
# host test above -- catching a pin/header-number transcription slip before
# it reaches hardware -- so it earns the same treatment despite living
# outside bsp/. See apps/iotest/main/iotest_seq.h's header comment.
fwog_add_test(test_iotest_seq
    ${BSP}/common/io_cfg.c ${BSP}/common/io_pins.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/iotest/main/iotest_seq.c)
target_include_directories(test_iotest_seq PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/iotest/main)

# Same as fwog_add_test, but also puts one CPU's directory on the include
# path so "platform/board.h" and "watchdog/watchdog.h" resolve. Exactly one
# CPU per binary: the two board.h files share short macro names by design and
# would collide if both directories were visible at once.
function(fwog_add_cpu_test name cpu)
    add_executable(${name} ${name}.c ${CMAKE_CURRENT_SOURCE_DIR}/test_util.c ${ARGN})
    target_include_directories(${name} PRIVATE
        ${BSP}/${cpu}_cpu ${BSP} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE HOST_TEST=1)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

fwog_add_cpu_test(test_pinmap_display display)
fwog_add_cpu_test(test_pinmap_main main)

fwog_add_cpu_test(test_display_update main
    ${BSP}/common/crc.c ${BSP}/common/app_meta.c
    ${BSP}/common/link/bl_proto.c
    ${BSP}/main_cpu/display_update/display_update.c)

fwog_add_cpu_test(test_watchdog_depth main ${BSP}/main_cpu/watchdog/watchdog.c)

fwog_add_cpu_test(test_cc1101 main ${BSP}/main_cpu/radio/cc1101.c)

fwog_add_cpu_test(test_fpga_clk main ${BSP}/main_cpu/fpga/fpga_clk.c)

fwog_add_cpu_test(test_fs_geom main ${BSP}/main_cpu/fs/fs_geom.c)

# The whole of FatFs and the whole public API, against a RAM-backed diskio.
# fs_flash_disk.c is entirely behind #ifndef HOST_TEST, so fs_ramdisk.c takes
# its place and nothing in the SDK is reached. This is why the largest package
# in the roadmap has the most host coverage.
#
# The vendored FatFs is third-party and is NOT warning-clean under -Wall
# -Wextra; the flags are cleared for its three translation units only, exactly
# as bsp/CMakeLists.txt keeps FWOG_WARN_FLAGS off the SDK's own libraries.
fwog_add_cpu_test(test_fs_api main
    ${BSP}/main_cpu/fs/fs_geom.c
    ${BSP}/main_cpu/fs/fwog_fs.c
    ${BSP}/main_cpu/fs/ff/ff.c
    ${BSP}/main_cpu/fs/ff/ffunicode.c
    ${BSP}/main_cpu/fs/ff/ffsystem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_ramdisk.c)
set_source_files_properties(
    ${BSP}/main_cpu/fs/ff/ff.c
    ${BSP}/main_cpu/fs/ff/ffunicode.c
    ${BSP}/main_cpu/fs/ff/ffsystem.c
    PROPERTIES COMPILE_OPTIONS "-w")

fwog_add_cpu_test(test_console_parse display
    ${BSP}/display_cpu/bootloader/bl_console.c)

# Only the pure half compiles here: bl_flash.c's SDK half is behind
# HOST_TEST, so hardware/flash.h and hardware/sync.h are never reached.
fwog_add_cpu_test(test_bl_flash display
    ${BSP}/display_cpu/bootloader/bl_flash.c)

fwog_add_cpu_test(test_st7789 display
    ${BSP}/display_cpu/lcd/st7789.c)

fwog_add_cpu_test(test_lcd_text display
    ${BSP}/display_cpu/lcd/lcd_text.c)

fwog_add_cpu_test(test_ioexp display
    ${BSP}/display_cpu/io_expander/pcal6416.c)

fwog_add_cpu_test(test_bq25896 display
    ${BSP}/display_cpu/power/bq25896.c)

fwog_add_cpu_test(test_mcp7940 display
    ${BSP}/display_cpu/sensors/mcp7940.c)

fwog_add_cpu_test(test_lis3dh display
    ${BSP}/display_cpu/sensors/lis3dh.c)

fwog_add_cpu_test(test_ws2812 display
    ${BSP}/display_cpu/leds/ws2812_driver.c)

fwog_add_cpu_test(test_pdm display
    ${BSP}/display_cpu/pdm/pdm_mic.c
    ${BSP}/common/dsp/cic.c)

# The decimation path: cic.c is the filter, pdm_mic.c's pure half supplies
# pdm_mic_decode()'s byte-to-bit binding, spectrum.c is what lets the synthetic
# tone test check a FREQUENCY rather than just an amplitude.
fwog_add_cpu_test(test_cic display
    ${BSP}/common/dsp/cic.c
    ${BSP}/common/dsp/spectrum.c
    ${BSP}/display_cpu/pdm/pdm_mic.c)

fwog_add_test(test_spectrum ${BSP}/common/dsp/spectrum.c)

fwog_add_test(test_dcblock ${BSP}/common/dsp/dcblock.c)

fwog_add_cpu_test(test_i2s display
    ${BSP}/display_cpu/audio/i2s_audio.c)

fwog_add_cpu_test(test_ir display
    ${BSP}/display_cpu/ir/ir_comm.c)

fwog_add_cpu_test(test_buttons display
    ${BSP}/display_cpu/input/buttons.c)

fwog_add_cpu_test(test_ship_mode display
    ${BSP}/display_cpu/power/ship_mode.c)

fwog_add_cpu_test(test_bl_policy display
    ${BSP}/display_cpu/bootloader/bl_policy.c
    ${BSP}/display_cpu/bootloader/bl_ui.c)

fwog_add_cpu_test(test_bl_ui display
    ${BSP}/display_cpu/bootloader/bl_ui.c
    ${BSP}/display_cpu/lcd/lcd_text.c
    ${BSP}/display_cpu/lcd/st7789.c)

# bl_flash.c is here for its pure predicates only (its SDK half is behind
# HOST_TEST): the fake flash delegates to them, so this test doubles as a
# regression check that a real update survives the reserve and device guards.
fwog_add_cpu_test(test_bl_update display
    ${BSP}/common/crc.c ${BSP}/common/app_meta.c
    ${BSP}/common/link/bl_proto.c
    ${BSP}/display_cpu/bootloader/bl_receiver.c
    ${BSP}/display_cpu/bootloader/bl_flash.c)
