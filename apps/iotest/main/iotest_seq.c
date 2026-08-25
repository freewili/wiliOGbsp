#include "iotest_seq.h"
#include <stddef.h>

/* Header-pin numbers and GPIOs transcribed from bsp/common/io_pins.c's
 * PINS[] table; tests/test_iotest_seq.c asserts this table against that one
 * the same way tests/test_io_pins.c asserts it against the published
 * pinout, so a transcription slip here fails a host test instead of
 * silently testing the wrong physical pin. */
static const fwog_iotest_step_t STEPS[FWOG_IOTEST_STEP_COUNT] = {
    { 27, 26,  3, 14, "gpio27 -> gpio26",   true,  false },
    {  8,  9,  9,  5, "uart_tx -> uart_rx", false, false },
    { 11, 10, 11,  7, "uart_rts -> uart_cts", false, false },
    { 15, 12, 13, 12, "spi_tx -> spi_rx",   false, false },
    { 13, 26,  1, 14, "spi_cs -> gpio26",   false, false },
    { 14, 26, 15, 14, "spi_sclk -> gpio26", false, false },
    { 25, 26, 17, 14, "gpio25 -> gpio26",   false, false },
    { 16,  0, 10,  0, "I2C sda",            false, true  },
    { 17,  0,  8,  0, "I2C scl",            false, true  },
};

size_t fwog_iotest_step_count(void) {
    return FWOG_IOTEST_STEP_COUNT;
}

const fwog_iotest_step_t *fwog_iotest_step_at(size_t i) {
    return (i < FWOG_IOTEST_STEP_COUNT) ? &STEPS[i] : NULL;
}

fwog_iotest_result_t fwog_iotest_verdict(bool sense_high, bool sense_low) {
    return (sense_high && !sense_low) ? FWOG_IOTEST_RESULT_PASS
                                       : FWOG_IOTEST_RESULT_FAIL;
}
