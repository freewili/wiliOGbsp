/* The breakout-header test walk: which pin drives which, and how to judge
 * the result.
 *
 * Pure decision logic, no SDK -- main/main.c binds it to real hardware via
 * bsp/main_cpu/gpio/breakout.h. This is the half worth host-testing: the
 * table below is transcribed by hand from bsp/common/io_pins.c's PINS[] and
 * tools/io_walk.py's OUTPUTS/INPUT_PAIRS, and a transcription error here
 * would silently test the wrong pin on a real board.
 *
 * Nine steps: seven outputs, extended past the four natural loopback pairs
 * by reusing gpio26 as a shared sense pin for the three outputs with no
 * input partner (spi_cs, spi_sclk, gpio25) -- exactly what an operator did
 * by hand today, one wire moved at a time -- plus the two I2C lines, which
 * have no jumper at all (open-drain: pulled low and released, not driven). */
#ifndef FWOG_IOTEST_SEQ_H
#define FWOG_IOTEST_SEQ_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t     drive_gpio;   /* main-CPU GPIO number */
    uint8_t     sense_gpio;   /* 0 for an I2C step -- see is_i2c */
    uint8_t     drive_hdr;    /* breakout connector pin, for the operator */
    uint8_t     sense_hdr;    /* 0 for an I2C step */
    const char *label;        /* "uart_tx -> uart_rx" etc */
    bool        is_control;   /* step 0: the V-PINS-IN electrical precondition.
                                  If this fails, nothing after it is
                                  meaningful -- see fwog_iotest_result_t. */
    bool        is_i2c;       /* pull/release instead of drive/sense a pair */
} fwog_iotest_step_t;

#define FWOG_IOTEST_STEP_COUNT 9u

size_t                     fwog_iotest_step_count(void);
const fwog_iotest_step_t  *fwog_iotest_step_at(size_t i);

typedef enum {
    FWOG_IOTEST_RESULT_PENDING = 0,
    FWOG_IOTEST_RESULT_PASS,
    FWOG_IOTEST_RESULT_FAIL,
    FWOG_IOTEST_RESULT_SKIPPED,
    /* The control step (step 0) itself failed. Nothing after it ran --
       header pin 4 (V PINS IN) may have no voltage, or the output path is
       dead, and no other pin's result would mean anything measured under
       that condition. Mirrors tools/io_walk.py's PATH_DEAD_OR_NO_VPINS. */
    FWOG_IOTEST_RESULT_VOID,
    /* I2C-only: released and never rose within the driver's timeout. NOT a
       failure -- bsp/main_cpu/gpio/breakout.h's fwog_io_i2c_pull() doc is
       explicit that a line held low by another device on the bus is a
       legitimate state, indistinguishable here from a missing pull-up. The
       number is reported; the judgement stays the operator's. */
    FWOG_IOTEST_RESULT_WARN
} fwog_iotest_result_t;

/* Did a loopback pair behave? Mirrors tools/io_walk.py's loopback_verdict():
 * BOTH directions must track, not just one -- checking only the high case
 * would pass a line that is stuck high, which is exactly the fault this
 * walk exists to catch. */
fwog_iotest_result_t fwog_iotest_verdict(bool sense_high, bool sense_low);

#endif
