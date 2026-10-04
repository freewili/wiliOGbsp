# Board smoke test: a continuous error count on the inter-CPU link at
# FWOG_LINK_BAUD, plus both CPUs' clocks. main/main.c says what it is for.
# A normal pair -- `fw build smoketest`, `fw flash smoketest`.
add_executable(smoketest_display display/main.c)
target_link_libraries(smoketest_display fwog_display_bsp)
fwog_display_app(smoketest_display
    VERSION 001
    DESCRIPTION "Link smoke test, display half: echoes every link byte back XORed, reports clocks")

add_executable(smoketest_main main/main.c)
target_link_libraries(smoketest_main fwog_main_bsp)
fwog_embed_display_image(smoketest_main)
fwog_main_app(smoketest_main
    VERSION 001
    DESCRIPTION "Link smoke test: sweeps all 256 byte values over the link each second and counts errors")
