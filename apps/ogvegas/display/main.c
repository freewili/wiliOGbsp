/* OG Vegas -- the BSP's showcase display app.
 *
 * Draws a full-screen image at startup, replays a short audio clip every ten
 * seconds, and runs a red comet on the seven WS2812 LEDs. It exists because it
 * is the one app in this tree that drives the LCD, the LED bar, I2S audio and
 * the power-off hold all at once, which is what makes it a useful "is this
 * board actually working" check.
 *
 * BUILD. It is a display app, so it reaches the board embedded in its main
 * companion (see the AGENTS.md warning about UF2-flashing a display
 * application):
 *
 *     python tools/fw.py build ogvegas
 *     python tools/fw.py flash ogvegas
 *
 * ASSETS. assets/ holds the sources; ogvegas_assets.c is generated from them
 * and checked in, so a normal build needs no image tooling. Regenerating does
 * need Pillow:
 *
 *     python tools/gen_ogvegas_assets.py \
 *         apps/ogvegas/display/assets/fwog.png \
 *         apps/ogvegas/display/assets/VivaLosVegas16khz.wav \
 *         apps/ogvegas/display/ogvegas_assets.c
 */
#include "fwog_display.h"
#include "ogvegas_assets.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

FWOG_POWER_DEFAULT();

#define AUDIO_PERIOD_MS 10000u
#define LED_STEP_MS 70u

static void draw_startup_image(void) {
    st7789_init_begin();
    const absolute_time_t deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(deadline)) {
        st7789_init_step();
        sleep_ms(1);
    }
    if (!st7789_ready()) {
        DIAG("[ogvegas] LCD initialization failed\n");
        return;
    }

    st7789_set_window(0u, 0u, ST7789_W, ST7789_H);
    st7789_blit(ogvegas_image, ogvegas_image_pixels);
    board_backlight(255);
}

static void render_red_comet(unsigned head) {
    static const uint8_t trail[FWOG_LED_COUNT] = {255u, 110u, 48u, 20u, 8u, 3u, 1u};
    for (unsigned pixel = 0; pixel < FWOG_LED_COUNT; ++pixel) {
        const unsigned distance = (head + FWOG_LED_COUNT - pixel) % FWOG_LED_COUNT;
        ws2812_set_color(pixel, trail[distance], 0u, 0u);
    }
    ws2812_process();
}

static void play_audio(void) {
    /* The buffer is the clip and nothing else -- no trailing silence. The
       driver parks the I2S state machine at end of playback, so the bus is
       genuinely quiet between replays and this app does not have to keep it
       fed (i2s_audio.h, trap 2). i2s_audio_start() unparks.

       The stop is defensive: at a 2 s clip and a 10 s period playback has
       always finished, but a shorter period must not be silently ignored --
       i2s_audio_start() returns false while a note is in flight. */
    if (!i2s_audio_is_idle()) i2s_audio_stop();
    const bool started =
        i2s_audio_start(ogvegas_audio, ogvegas_audio_samples, true, false);
    DIAG("[ogvegas] audio %s\n", started ? "started" : "start failed");
}

int main(void) {
    board_init();
    draw_startup_image();

    const bool leds_ok = ws2812_init(pio0, 0u);
    const bool audio_ok = i2s_audio_init(pio0, 2u);
    if (leds_ok) render_red_comet(0u);
    if (audio_ok) {
        i2s_audio_set_volume(8);
        play_audio();
    }

    absolute_time_t next_audio = make_timeout_time_ms(AUDIO_PERIOD_MS);
    absolute_time_t next_led = make_timeout_time_ms(LED_STEP_MS);
    unsigned led_head = 0u;

    while (true) {
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        const fwog_power_t power = fwog_power_poll(now_ms);
        i2s_audio_process();

        if (audio_ok && time_reached(next_audio)) {
            next_audio = make_timeout_time_ms(AUDIO_PERIOD_MS);
            play_audio();
        }

        if (leds_ok && !power.armed && time_reached(next_led)) {
            next_led = delayed_by_ms(next_led, LED_STEP_MS);
            led_head = (led_head + 1u) % FWOG_LED_COUNT;
            render_red_comet(led_head);
        }
        sleep_ms(2);
    }
}
