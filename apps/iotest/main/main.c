/* Guided breakout-header I/O test: the sequencer.
 *
 * MAIN owns the pins -- the FPGA is on main's own SPI0 and the RP2040 pads
 * are main's own GPIOs -- so MAIN drives the walk and DISPLAY only shows it
 * and reports button presses back. See apps/iotest/proto/iotest_proto.h for
 * the wire messages and ../../../AGENTS.md / the plan this app was built
 * from for the full design rationale.
 *
 * NEVER BLOCKS ON THE OPERATOR. The watchdog window is 8.3 s; an operator
 * can take minutes to move a jumper wire. Every wait for a DISPLAY message
 * is a non-blocking poll inside the main loop, which kicks the watchdog on
 * every single pass regardless of what state the walk is in. This is the one
 * real correctness rule this file exists to get right -- see
 * bsp/main_cpu/gpio/breakout.h's wait_ack(), which blocks for up to 250 ms
 * and is fine for THAT use (a machine-paced round trip), but must never be
 * copied here where the round trip is paced by a human. */
#include "fwog_main.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include "iotest_seq.h"
#include "proto/iotest_proto.h"

FWOG_WATCHDOG_DEFAULT();

typedef enum {
    ST_SEND_STEP,
    ST_WAIT_STEP_ACK,
    ST_WAIT_CONFIRM,
    ST_SEND_SUMMARY,
    ST_DONE
} state_t;

static fwog_link_rx_t  s_rx;
static uint8_t         s_results[FWOG_IOTEST_STEP_COUNT];
static size_t          s_step;
static uint8_t         s_seq;
static state_t         s_state;
static absolute_time_t s_step_ack_deadline;

static void reset_walk(void) {
    for (size_t i = 0; i < FWOG_IOTEST_STEP_COUNT; i++) {
        s_results[i] = (uint8_t)FWOG_IOTEST_RESULT_PENDING;
    }
    s_step  = 0u;
    s_state = ST_SEND_STEP;
}

/* Everything else on the header depends on this: header pin 4 (V PINS IN)
   needs 1.1-5.5 V or nothing here means anything -- and there is no ADC path
   to check that directly, so step 0's loopback IS the check, exactly as in
   tools/io_walk.py's phase 0. */
static const char *step_text(size_t i) {
    static char buf[FWOG_IOTEST_TEXT_LEN];
    const fwog_iotest_step_t *s = fwog_iotest_step_at(i);
    if (s->is_control) {
        snprintf(buf, sizeof buf, "Wire hdr%u->hdr%u (also checks V PINS IN)",
                 s->drive_hdr, s->sense_hdr);
    } else if (s->is_i2c) {
        snprintf(buf, sizeof buf, "%s: no wire needed", s->label);
    } else {
        snprintf(buf, sizeof buf, "Wire hdr%u -> hdr%u", s->drive_hdr, s->sense_hdr);
    }
    return buf;
}

static void send_step_show(void) {
    const fwog_iotest_step_t *s = fwog_iotest_step_at(s_step);
    const uint8_t last = (s_step == 0u) ? (uint8_t)FWOG_IOTEST_RESULT_PENDING
                                         : s_results[s_step - 1u];
    uint8_t payload[sizeof(fwog_iotest_step_show_t)];
    const size_t n = fwog_iotest_proto_build_step_show(
        payload, sizeof payload, s_seq, (uint8_t)s_step,
        (uint8_t)FWOG_IOTEST_STEP_COUNT, s->drive_hdr, s->sense_hdr, last,
        step_text(s_step));
    if (n == 0u) return;
    fwog_link_uart_send_frame(payload, n);
    s_state = ST_WAIT_STEP_ACK;
    s_step_ack_deadline = make_timeout_time_ms(2000);
}

static void send_summary(void) {
    bool overall = true;
    for (size_t i = 0; i < FWOG_IOTEST_STEP_COUNT; i++) {
        if (s_results[i] != FWOG_IOTEST_RESULT_PASS &&
            s_results[i] != FWOG_IOTEST_RESULT_WARN &&
            s_results[i] != FWOG_IOTEST_RESULT_SKIPPED) {
            overall = false;
        }
    }
    uint8_t payload[sizeof(fwog_iotest_summary_t)];
    const size_t n = fwog_iotest_proto_build_summary(
        payload, sizeof payload, s_seq, s_results,
        (uint8_t)FWOG_IOTEST_STEP_COUNT, overall);
    if (n == 0u) return;
    fwog_link_uart_send_frame(payload, n);
    s_state = ST_DONE;
}

/* Run one step's drive/sense (or I2C pull/release) and return its verdict.
   Never blocks past FWOG_IO_ACK_TIMEOUT_MS/FWOG_I2C_RISE_TIMEOUT_US, both of
   which are far inside the watchdog window -- this is the machine-paced
   half of the walk, not the operator-paced half. */
static fwog_iotest_result_t run_step(const fwog_iotest_step_t *s) {
    if (s->is_i2c) {
        bool low_lvl = true, high_lvl = false;
        uint32_t rise_us = FWOG_I2C_RISE_NONE;
        (void)fwog_io_i2c_pull(s->drive_gpio, true, NULL);
        (void)fwog_io_i2c_sense(s->drive_gpio, &low_lvl);
        if (low_lvl) return FWOG_IOTEST_RESULT_FAIL;   /* drove low, reads high: dead */
        (void)fwog_io_i2c_pull(s->drive_gpio, false, &rise_us);
        (void)fwog_io_i2c_sense(s->drive_gpio, &high_lvl);
        if (rise_us == FWOG_I2C_RISE_NONE) return FWOG_IOTEST_RESULT_WARN;
        return high_lvl ? FWOG_IOTEST_RESULT_PASS : FWOG_IOTEST_RESULT_WARN;
    }

    bool hi = false, lo = true;
    if (fwog_io_pin_drive(s->drive_gpio, true) != FWOG_IO_PIN_OK)
        return FWOG_IOTEST_RESULT_FAIL;
    (void)fwog_io_pin_read(s->sense_gpio, &hi);
    if (fwog_io_pin_drive(s->drive_gpio, false) != FWOG_IO_PIN_OK)
        return FWOG_IOTEST_RESULT_FAIL;
    (void)fwog_io_pin_read(s->sense_gpio, &lo);
    return fwog_iotest_verdict(hi, lo);
}

static void handle_confirm(const fwog_iotest_confirm_t *c) {
    if (c->seq != s_seq) return;   /* stale reply to an earlier attempt */

    if (s_state == ST_DONE) {
        if ((fwog_iotest_action_t)c->action == FWOG_IOTEST_ACTION_RESTART) {
            reset_walk();
        }
        return;
    }
    if (s_state != ST_WAIT_CONFIRM) return;

    const fwog_iotest_step_t *s = fwog_iotest_step_at(s_step);
    const fwog_iotest_action_t action = (fwog_iotest_action_t)c->action;

    if (action == FWOG_IOTEST_ACTION_SKIP) {
        s_results[s_step] = (uint8_t)FWOG_IOTEST_RESULT_SKIPPED;
    } else {
        /* CONFIRM and RETRY both run the step; RETRY just does not advance. */
        s_results[s_step] = (uint8_t)run_step(s);
    }

    if (action == FWOG_IOTEST_ACTION_RETRY) {
        s_seq++;
        send_step_show();   /* same s_step, updated last_result */
        return;
    }

    if (s->is_control && s_results[s_step] != (uint8_t)FWOG_IOTEST_RESULT_PASS) {
        for (size_t i = 1; i < FWOG_IOTEST_STEP_COUNT; i++) {
            s_results[i] = (uint8_t)FWOG_IOTEST_RESULT_VOID;
        }
        s_step = FWOG_IOTEST_STEP_COUNT;   /* skip straight to summary */
    } else {
        s_step++;
    }

    s_seq++;
    if (s_step >= FWOG_IOTEST_STEP_COUNT) {
        s_state = ST_SEND_SUMMARY;
    } else {
        send_step_show();
    }
}

int main(void) {
    board_init();
    (void)fwog_display_update_run();
    board_init_i2c();   /* required before any I2C pull/sense -- see
                            common/io_pins.h; bench_main never calls this,
                            don't repeat that gap here */

    fwog_io_cfg_t cfg;
    fwog_io_cfg_default(&cfg);
    (void)fwog_io_dir_apply(&cfg);   /* establishes IO_CONFIG=DISABLED in
                                         main's tracking -- required before
                                         the FPGA SPI-group pins (steps 4-6)
                                         can be driven at all */

    (void)fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_rx);

    reset_walk();

    while (true) {
        board_watchdog_kick();   /* every pass, unconditionally */

        uint8_t b;
        while (fwog_link_uart_read(&b)) {
            size_t len = 0u;
            if (!fwog_link_rx_byte(&s_rx, b, &len)) continue;
            const uint8_t t = fwog_iotest_proto_type(s_rx.buf, len);
            if (t == FWOG_IOTEST_MSG_STEP_ACK && s_state == ST_WAIT_STEP_ACK) {
                const fwog_iotest_step_ack_t *a =
                    (const fwog_iotest_step_ack_t *)s_rx.buf;
                if (a->seq == s_seq) s_state = ST_WAIT_CONFIRM;
            } else if (t == FWOG_IOTEST_MSG_CONFIRM) {
                handle_confirm((const fwog_iotest_confirm_t *)s_rx.buf);
            }
        }

        switch (s_state) {
        case ST_SEND_STEP:
            send_step_show();
            break;
        case ST_WAIT_STEP_ACK:
            /* Display may not have booted yet (or missed the frame) --
               resend rather than wait forever. Still bounded well inside
               the watchdog window. */
            if (time_reached(s_step_ack_deadline)) send_step_show();
            break;
        case ST_SEND_SUMMARY:
            send_summary();
            break;
        case ST_WAIT_CONFIRM:
        case ST_DONE:
        default:
            break;
        }

        sleep_ms(2);
    }
}
