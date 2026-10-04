/* Display half of the board smoke test: the echo end of the link sweep.
 *
 * Brings the link up at FWOG_LINK_BAUD -- the production rate, with RTS/CTS,
 * exactly as every other app runs it -- and returns every byte it receives
 * XORed with SMOKETEST_XOR. main/main.c sends the bytes and does the counting.
 * See that file for what the test is for.
 *
 * Otherwise an ordinary display app: it arrives inside smoketest_main's UF2,
 * and red held 6 s powers the board off. */
#include "fwog_display.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

FWOG_POWER_DEFAULT();

/* XORed rather than returned verbatim, so a shorted or tied-back pair cannot
   pass as a working link. Must match main/main.c. */
#define SMOKETEST_XOR 0xA5u

int main(void) {
    board_init();

    /* The bootloader deinits the link before jumping here (bl_jump.c), so
       the app brings it up again. fwog_link_uart_init() refuses -- rather
       than silently running at a quarter rate -- if clk_peri is not where
       the board header puts it. */
    const bool link_up = fwog_link_uart_init(FWOG_LINK_BAUD);

    /* At most one byte waiting for TX FIFO space. Holding it here instead of
       blocking in uart_putc() keeps fwog_power_poll() running even if main
       stops reading and CTS stays deasserted. */
    bool pending = false;
    uint8_t out = 0;
    uint32_t echoed = 0;
    absolute_time_t next_report = make_timeout_time_ms(1000);

    while (true) {
        fwog_power_poll(to_ms_since_boot(get_absolute_time()));

        if (link_up) {
            /* Bounded, so a flood on RX cannot starve the power poll. */
            for (int n = 0; n < 64; n++) {
                if (pending) {
                    if (!uart_is_writable(FWOG_LINK_UART)) break;
                    uart_putc_raw(FWOG_LINK_UART, out);
                    pending = false;
                    echoed++;
                }
                uint8_t b;
                if (!fwog_link_uart_read(&b)) break;
                out = (uint8_t)(b ^ SMOKETEST_XOR);
                pending = true;
            }
        }

        /* Repeated, because USB CDC enumerates after the first lines would
           have been written. */
        if (time_reached(next_report)) {
            next_report = make_timeout_time_ms(1000);
            DIAG("[smoketest_display] clk_sys %u clk_peri %u  link %s  echoed %u\n",
                 (unsigned)clock_get_hz(clk_sys), (unsigned)clock_get_hz(clk_peri),
                 link_up ? "up" : "DOWN (rate unreachable)", (unsigned)echoed);
        }
    }
}
