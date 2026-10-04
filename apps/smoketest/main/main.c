/* Main half of the board smoke test: a soak of the inter-CPU link at the
 * rate the BSP actually uses.
 *
 * WHAT IT IS FOR. FWOG_LINK_BAUD defaults to 12.5 Mbaud, the PL011 ceiling at
 * a divisor of exactly 1.0, and that rate has never been measured on the
 * inter-CPU trace (link_uart.h). A display update passing proves the link
 * once, pass/fail. This counts errors, continuously, so a marginal trace
 * shows up as a rate rather than as an occasional failed update -- and it is
 * how to choose a step down the `fw build --baud N` ladder:
 *
 *     fw build smoketest --baud 10000000 && fw flash smoketest
 *
 * HOW. After the normal display bring-up (fwog_display_update_run(), which
 * leaves main's end of the link up at FWOG_LINK_BAUD), every round sends all
 * 256 byte values in 16-byte bursts and checks each comes back XORed with
 * SMOKETEST_XOR. Bursts, so bytes go back to back at full rate and RTS/CTS
 * is exercised; 16, half the PL011 RX FIFO, so the echoes of one burst
 * always fit and neither side's flow control can deadlock the other.
 *
 * Also reported, both sides: clk_sys and clk_peri. clk_peri must equal
 * clk_sys; 48 MHz means PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK was lost
 * from the board header (AGENTS.md, "Invariants"). */
#include "fwog_main.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

FWOG_WATCHDOG_DEFAULT();

/* Must match display/main.c. */
#define SMOKETEST_XOR 0xA5u

#define BURST 16u

/* Generous at any rate on the --baud ladder, where a byte takes under 2 us:
   it covers the display being mid-DIAG when a burst arrives. Short enough
   that a dead link costs a round only 16 timeouts, ~0.3 s. */
#define BYTE_TIMEOUT_US 20000u

typedef struct {
    unsigned ok, corrupt, lost;
    bool tx_stalled;
    uint8_t bad_tx, bad_rx;     /* the first corrupt byte, if any */
} round_t;

static void drain_rx(void) {
    uint8_t b;
    while (fwog_link_uart_read(&b)) {}
}

/* Not fwog_link_uart_write(): that blocks forever if CTS never asserts --
   a display that never reached its echo loop -- and this test must report
   that, not sit in it until the watchdog fires. */
static bool put_byte(uint8_t b) {
    const absolute_time_t end = make_timeout_time_us(BYTE_TIMEOUT_US);
    while (!uart_is_writable(FWOG_LINK_UART)) {
        if (time_reached(end)) return false;
    }
    uart_putc_raw(FWOG_LINK_UART, b);
    return true;
}

static bool get_byte(uint8_t *b) {
    const absolute_time_t end = make_timeout_time_us(BYTE_TIMEOUT_US);
    while (!fwog_link_uart_read(b)) {
        if (time_reached(end)) return false;
    }
    return true;
}

static void run_round(round_t *r) {
    *r = (round_t){0};
    for (unsigned base = 0; base < 256u; base += BURST) {
        board_watchdog_kick();
        drain_rx();             /* a late byte from the last burst would alias */

        unsigned sent = 0;
        while (sent < BURST && put_byte((uint8_t)(base + sent))) sent++;
        if (sent < BURST) r->tx_stalled = true;

        for (unsigned i = 0; i < sent; i++) {
            const uint8_t tx = (uint8_t)(base + i);
            uint8_t rx;
            if (!get_byte(&rx)) {
                r->lost += sent - i;    /* nothing more is coming this burst */
                break;
            }
            if (rx == (uint8_t)(tx ^ SMOKETEST_XOR)) {
                r->ok++;
            } else {
                if (!r->corrupt) { r->bad_tx = tx; r->bad_rx = rx; }
                r->corrupt++;
            }
        }
        r->lost += BURST - sent;
    }
}

int main(void) {
    board_init();
    const fwog_display_result_t d = fwog_display_update_run();
    const bool link_up = d != FWOG_DISP_LINK_DOWN;

    uint32_t rounds = 0, clean = 0;
    uint64_t total_ok = 0, total_bad = 0;

    while (true) {
        board_watchdog_kick();

        DIAG("[smoketest_main] clk_sys %u clk_peri %u  link %u baud  display: %s\n",
             (unsigned)clock_get_hz(clk_sys), (unsigned)clock_get_hz(clk_peri),
             (unsigned)FWOG_LINK_BAUD, fwog_display_result_text(d));

        if (!link_up) {
            DIAG("[smoketest_main] link DOWN: %u baud is unreachable from this "
                 "clk_peri; nothing to test\n", (unsigned)FWOG_LINK_BAUD);
            sleep_ms(1000);
            continue;
        }

        round_t r;
        run_round(&r);
        rounds++;
        total_ok += r.ok;
        total_bad += r.corrupt + r.lost;
        if (r.ok == 256u) clean++;

        if (r.ok == 256u) {
            DIAG("[smoketest_main] round %u: LINK OK 256/256\n", (unsigned)rounds);
        } else if (r.ok == 0u && r.corrupt == 0u) {
            DIAG("[smoketest_main] round %u: nothing came back%s -- is "
                 "smoketest_display running? (display: %s)\n", (unsigned)rounds,
                 r.tx_stalled ? ", and TX stalled on CTS" : "",
                 fwog_display_result_text(d));
        } else {
            DIAG("[smoketest_main] round %u: ok=%u corrupt=%u lost=%u%s",
                 (unsigned)rounds, r.ok, r.corrupt, r.lost,
                 r.tx_stalled ? " (TX stalled on CTS)" : "");
            if (r.corrupt) {
                DIAG("  first bad: sent 0x%02X got 0x%02X (expected 0x%02X)",
                     r.bad_tx, r.bad_rx, (uint8_t)(r.bad_tx ^ SMOKETEST_XOR));
            }
            DIAG("\n");
        }
        DIAG("[smoketest_main] totals: %u/%u rounds clean, %llu bytes ok, "
             "%llu bad\n", (unsigned)clean, (unsigned)rounds,
             (unsigned long long)total_ok, (unsigned long long)total_bad);

        sleep_ms(1000);         /* well inside the 8.3 s watchdog */
    }
}
