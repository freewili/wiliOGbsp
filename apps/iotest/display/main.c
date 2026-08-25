/* Guided breakout-header I/O test: the LCD UI.
 *
 * DISPLAY only shows what MAIN tells it and reports button presses back --
 * MAIN owns the pins and the pass/fail logic (see ../main/main.c). This
 * file's whole job is: bring the panel and LVGL up, service the link every
 * iteration so MAIN's own fwog_io_dir_apply()/fwog_io_pin_drive() calls
 * don't time out waiting for THIS CPU's expander acks (bsp/main_cpu/gpio/
 * breakout.h's FWOG_IO_ACK_TIMEOUT_MS budget), and translate our own
 * apps/iotest/proto/iotest_proto.h messages into iotest_ui.h calls.
 *
 * Bring-up order matters and is copied from apps/lvgl/display/main.c: panel
 * ready, THEN fwog_lvgl_init() -- st7789_blit() silently discards anything
 * flushed before the panel is up, so LVGL would render into a void with no
 * error anywhere if this were reversed. */
#include "fwog_display.h"
#include "lvgl/fwog_lvgl.h"
#include "proto/iotest_proto.h"
#include "iotest_ui.h"

FWOG_POWER_DEFAULT();

static fwog_link_rx_t s_rx;

static bool panel_up(void) {
    st7789_init_begin();
    const absolute_time_t deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(deadline)) {
        st7789_init_step();
        sleep_ms(1);
    }
    return st7789_ready();
}

int main(void) {
    board_init();

    /* The bootloader deinits the link before jumping here (bl_jump.c), same
       as apps/bench/display -- without this, MAIN's fwog_io_dir_apply() etc.
       would time out on every call, not just the FPGA SPI-group pins. */
    (void)fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_rx);

    if (panel_up()) {
        fwog_lvgl_init();
        iotest_ui_build();
        board_backlight(255);
    } else {
        DIAG("[iotest_display] panel did not come up; not starting LVGL\n");
    }

    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());

        /* One poll, and its result is what everything else reads -- see
           lvgl/fwog_lvgl.h on why buttons are fed here rather than polled a
           second time. */
        const fwog_power_t power = fwog_power_poll(now);
        fwog_lvgl_feed_buttons(power.buttons);

        uint8_t b;
        while (fwog_link_uart_read(&b)) {
            size_t len = 0u;
            if (!fwog_link_rx_byte(&s_rx, b, &len)) continue;

            /* io_expander's 0x20-0x22 messages first -- this is the whole
               reason this loop must run every iteration regardless of UI
               state, see the file header. */
            if (fwog_ioexp_link_handle(s_rx.buf, len)) continue;

            const uint8_t t = fwog_iotest_proto_type(s_rx.buf, len);
            if (t == FWOG_IOTEST_MSG_STEP_SHOW) {
                const fwog_iotest_step_show_t *m =
                    (const fwog_iotest_step_show_t *)s_rx.buf;
                iotest_ui_show_step(m);
                /* Ack AFTER the redraw, not before -- this proves the step
                   was actually rendered, not merely received. The real
                   correctness backstop is that lv_timer_handler() below
                   runs unconditionally every iteration regardless of ack
                   timing, so a lost ack costs MAIN one resend, not a
                   stuck UI. */
                uint8_t ack[sizeof(fwog_iotest_step_ack_t)];
                const size_t n = fwog_iotest_proto_build_step_ack(
                    ack, sizeof ack, m->seq);
                if (n > 0u) fwog_link_uart_send_frame(ack, n);
            } else if (t == FWOG_IOTEST_MSG_SUMMARY) {
                iotest_ui_show_summary((const fwog_iotest_summary_t *)s_rx.buf);
            }
        }

        lv_timer_handler();
        sleep_ms(2);
    }
}
