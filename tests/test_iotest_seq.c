#include "test_util.h"
#include "iotest_seq.h"
#include "common/io_pins.h"

/* Every drive/sense GPIO and header-pin pair in the step table is asserted
 * against bsp/common/io_pins.c's own PINS[] table -- the same way
 * tests/test_io_pins.c asserts that table against the published pinout.
 * This is the check that catches a transcription slip here before it ever
 * points an operator at the wrong physical pin. */
static void test_step_table(void) {
    ASSERT_EQ(fwog_iotest_step_count(), 9u);
    ASSERT_TRUE(fwog_iotest_step_at(fwog_iotest_step_count()) == NULL);

    unsigned control_count = 0, i2c_count = 0;
    for (size_t i = 0; i < fwog_iotest_step_count(); i++) {
        const fwog_iotest_step_t *s = fwog_iotest_step_at(i);
        ASSERT_TRUE(s != NULL);

        if (s->is_control) control_count++;
        if (s->is_i2c) { i2c_count++; continue; }

        /* Drive pin: a real breakout output, and its header number matches
           io_pins.c's own table -- not just a plausible-looking constant. */
        const fwog_io_pin_t *d = fwog_io_pin_lookup(s->drive_gpio);
        ASSERT_TRUE(d != NULL);
        ASSERT_EQ(d->header_pin, s->drive_hdr);

        /* Sense pin: a real breakout input line (steps 1-3) or the shared
           gpio26 reused for steps 4-6, but ALWAYS a real header entry. */
        const fwog_io_pin_t *n = fwog_io_pin_lookup(s->sense_gpio);
        ASSERT_TRUE(n != NULL);
        ASSERT_EQ(n->header_pin, s->sense_hdr);

        /* Drive and sense must never be the same physical pin -- a step
           that "loops back" to itself would pass no matter what. */
        ASSERT_TRUE(s->drive_gpio != s->sense_gpio);
    }

    /* Exactly one control step (step 0), and exactly the two I2C lines. */
    ASSERT_EQ(control_count, 1u);
    ASSERT_TRUE(fwog_iotest_step_at(0)->is_control);
    ASSERT_EQ(i2c_count, 2u);

    /* The I2C steps name the documented SDA/SCL GPIOs, not a breakout table
       entry (io_pins.h is explicit that 16/17 are deliberately absent from
       that table -- I2C has no direction bit). */
    unsigned sda_gpio = 0, scl_gpio = 0;
    ASSERT_TRUE(fwog_io_i2c_line("sda", &sda_gpio));
    ASSERT_TRUE(fwog_io_i2c_line("scl", &scl_gpio));
    bool saw_sda = false, saw_scl = false;
    for (size_t i = 0; i < fwog_iotest_step_count(); i++) {
        const fwog_iotest_step_t *s = fwog_iotest_step_at(i);
        if (!s->is_i2c) continue;
        if (s->drive_gpio == sda_gpio) saw_sda = true;
        if (s->drive_gpio == scl_gpio) saw_scl = true;
    }
    ASSERT_TRUE(saw_sda);
    ASSERT_TRUE(saw_scl);

    /* All seven documented breakout OUTPUTS are covered as a drive pin
       somewhere in the walk -- the whole point of reusing gpio26 across
       steps 4-6 is that no output gets skipped. */
    fwog_io_cfg_t cfg;
    fwog_io_cfg_default(&cfg);
    for (size_t i = 0; i < fwog_io_pin_count(); i++) {
        const fwog_io_pin_t *p = fwog_io_pin_at(i);
        if (!fwog_io_pin_is_output(&cfg, p->gpio)) continue;
        bool covered = false;
        for (size_t j = 0; j < fwog_iotest_step_count(); j++) {
            const fwog_iotest_step_t *s = fwog_iotest_step_at(j);
            if (!s->is_i2c && s->drive_gpio == p->gpio) covered = true;
        }
        ASSERT_TRUE(covered);
    }
}

/* Mirrors tools/io_walk.py's loopback_verdict(): a pass requires BOTH
 * directions to track. Checking only the high case would let a line stuck
 * permanently high look correct. */
static void test_verdict(void) {
    ASSERT_EQ(fwog_iotest_verdict(true, false), FWOG_IOTEST_RESULT_PASS);
    ASSERT_EQ(fwog_iotest_verdict(true, true),  FWOG_IOTEST_RESULT_FAIL);
    ASSERT_EQ(fwog_iotest_verdict(false, false), FWOG_IOTEST_RESULT_FAIL);
    ASSERT_EQ(fwog_iotest_verdict(false, true), FWOG_IOTEST_RESULT_FAIL);
}

int main(void) {
    test_step_table();
    test_verdict();
    TEST_RETURN();
}
