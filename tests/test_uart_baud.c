#include "test_util.h"
#include "common/link/link_uart.h"

int main(void) {
    /* clk_peri equals clk_sys only because the board header sets
       PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK; measured 48 MHz without
       it, which is why fwog_uart_baud_ok() below is load-bearing. */
    const uint32_t clk = 200000000u;

    /* The design rate. IBRD 1, FBRD 0 -- divisor exactly 1.0, which is the
       PL011's floor. There is no margin below this. */
    ASSERT_EQ(fwog_uart_actual_baud(clk, 12500000u), 12500000u);
    ASSERT_TRUE(fwog_uart_baud_ok(clk, 12500000u));

    /* The fallback ladder from the design doc, in order. The first two use
       the fractional divisor; the last is integer divisor 2. */
    ASSERT_EQ(fwog_uart_actual_baud(clk, 10000000u), 10000000u);
    ASSERT_TRUE(fwog_uart_baud_ok(clk, 10000000u));
    ASSERT_TRUE(fwog_uart_baud_ok(clk, 8333333u));
    ASSERT_EQ(fwog_uart_actual_baud(clk, 6250000u), 6250000u);
    ASSERT_TRUE(fwog_uart_baud_ok(clk, 6250000u));

    /* Anything above 12.5 Mbaud is unreachable, and the SDK does NOT say
       so: it clamps IBRD to 1 and returns 12.5 Mbaud. Without this check a
       -DFWOG_LINK_BAUD=20000000 would build, boot, and run at 12.5 with
       both CPUs quietly agreeing -- until somebody changed only one. */
    ASSERT_EQ(fwog_uart_actual_baud(clk, 20000000u), 12500000u);
    ASSERT_TRUE(!fwog_uart_baud_ok(clk, 20000000u));
    ASSERT_TRUE(!fwog_uart_baud_ok(clk, 12500001u * 2u));

    /* Ordinary rates still work, in case the link is ever run slow. */
    ASSERT_TRUE(fwog_uart_baud_ok(clk, 115200u));
    ASSERT_TRUE(fwog_uart_baud_ok(clk, 1000000u));

    /* At the stock 125 MHz clock -- the documented fallback if the 200 MHz
       overclock proves unstable -- 12.5 Mbaud is NOT reachable. */
    ASSERT_TRUE(!fwog_uart_baud_ok(125000000u, 12500000u));
    ASSERT_TRUE(fwog_uart_baud_ok(125000000u, 7812500u));

    /* Degenerate inputs must not divide by zero. */
    ASSERT_EQ(fwog_uart_actual_baud(clk, 0u), 0u);
    ASSERT_EQ(fwog_uart_actual_baud(0u, 115200u), 0u);
    ASSERT_TRUE(!fwog_uart_baud_ok(clk, 0u));
    ASSERT_TRUE(!fwog_uart_baud_ok(0u, 115200u));

    /* The configured rate must be reachable at the configured clock. This
       is the assertion that actually protects the build. */
    ASSERT_TRUE(fwog_uart_baud_ok(FWOG_SYS_CLK_KHZ * 1000u, FWOG_LINK_BAUD));

    /* --- anchored to real measurements, 2026-07-26 (the hardware record) ---
       These three numbers were read off the hardware by the bring-up smoke apps, so they
       tie this pure model to observed silicon rather than to the datasheet.
       115176 vs 115207 for the same requested 115200 is precisely the
       difference between a 48 MHz and a 200 MHz clk_peri, which is how the
       wrong clk_peri was caught in the first place. */
    ASSERT_EQ(fwog_uart_actual_baud(48000000u, 115200u), 115176u);
    ASSERT_EQ(fwog_uart_actual_baud(200000000u, 115200u), 115207u);
    ASSERT_EQ(fwog_uart_actual_baud(200000000u, 12500000u), 12500000u);

    /* And the guard that matters if the board header's
       PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK is ever lost: at 48 MHz the
       design rate must be refused outright, not silently run at a quarter. */
    ASSERT_TRUE(!fwog_uart_baud_ok(48000000u, 12500000u));

    TEST_RETURN();
}
