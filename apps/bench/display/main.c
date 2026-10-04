/* Bench-test console for the display CPU's nine newly-ported drivers.
 *
 * None of bq25896/mcp7940/lis3dh/ws2812/pdm/i2s/ir had ever run on hardware
 * before this app -- every one was host-tested and cross-compiled only.
 * This app brings each one
 * up, reports pass/fail per driver without letting one failure stop the
 * others, then hands control to a trivial USB CDC console so a host harness
 * (or a human) can exercise each peripheral individually and read back real
 * measurements while a camera watches the screen and LEDs.
 *
 * Two rules from the handoff, both load-bearing:
 *   1. pico_stdio_usb DROPS output when no host is attached -- it does not
 *      buffer. Nothing worth reading is printed until stdio_usb_connected()
 *      or a bounded timeout, exactly like lcd_display.
 *   2. A one-shot line is invisible to a console that attaches late, which is
 *      the normal case -- so the driver init summary is repeated on every
 *      heartbeat, not printed once at boot.
 *
 * PIO allocation (recon-dma.md): pio0 holds WS2812(sm0)+PDM(sm1)+I2S(sm2),
 * 16 of 32 words; pio1 holds IR alone (sm0/1/2), 25 of 32 words. Never
 * reassign these without re-checking that budget.
 *
 * ---- THIS APP CAN POWER THE BOARD OFF. Read before flashing it. ----
 *
 * Holding the RED button continuously for FWOG_SHIP_HOLD_MS (6 s) calls
 * fwog_ship_enter(), which sets BATFET_DIS and the board goes dark. That is a
 * one-way door while the board is dark (power/ship_mode.h); a GRAY hold wakes
 * it (measured, the hardware record) and so does a USB attach (datasheet plus the board
 * owner's report -- not measured here).
 * Any release before 6 s aborts and discards the countdown --
 * six one-second presses cannot do it -- and the LEDs show the countdown the
 * whole time, so it is not reachable by accident. But it IS reachable, which
 * it was not in earlier versions of this file.
 *
 * bq25896_start_charging() still only ever CLEARS BATFET_DIS (the enable
 * direction; see bq25896.h's ship-mode boundary comment). fwog_ship_enter()
 * is the one and only call in this app that sets it, and it is guarded by
 * nothing except the 6 s hold.
 *
 * Does NOT flash itself and does NOT open a serial port -- this app expects
 * a host to drive it over USB CDC after `fw flash`/`fw console`.
 */
#include "fwog_display.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#if FWOG_DIAG == FWOG_DIAG_USB
#include "pico/stdio_usb.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Red held 6 s powers the board off, with the LED countdown. Required of
   every display app; board_init() will not link without it. */
FWOG_POWER_DEFAULT();

#define BENCH_PI 3.14159265358979323846f

/* i2s_audio_init() hardcodes the state machine to 8000 Hz (see i2s_audio.c);
 * this is not a choice this app makes, it is what the driver actually
 * configures, so every tone this app synthesizes must target that rate. */
#define BENCH_TONE_SAMPLE_RATE_HZ 8000u
#define BENCH_TONE_MAX_MS         3000u
#define BENCH_TONE_MAX_SAMPLES    (BENCH_TONE_SAMPLE_RATE_HZ * BENCH_TONE_MAX_MS / 1000u)
#define BENCH_TONE_AMPLITUDE      12000

#define BENCH_MIC_MAX_MS 5000u

/* Result of each driver's bring-up, printed once at boot and repeated on
 * every heartbeat -- a console that attaches late (the normal case) must
 * still be able to read it. */
typedef struct {
    bool bq25896;
    bool mcp7940;
    bool lis3dh;
    bool ws2812;
    bool pdm;
    bool i2s;
    bool ir;
} bench_init_t;

static bench_init_t s_init;
static bool s_lcd_ready;

/* Receive buffer for main's breakout-direction requests (link_frame.h's
   fwog_link_rx_t embeds a 4160-byte payload buffer -- 4168 bytes total --
   so this MUST be static/file-scope, never a stack local: the SDK's default
   PICO_STACK_SIZE is 2048). */
static fwog_link_rx_t s_link_rx;
static bool s_link_up;

/* One static tone buffer. i2s_audio_start() keeps a pointer into this for
 * the whole duration of playback (fillBuffer() reads from it on every
 * refill), so it must outlive the command handler that fills it -- it
 * cannot be a stack array. */
static int16_t s_tone_buf[BENCH_TONE_MAX_SAMPLES];

/* What the main loop last saw on the buttons; published by `btn`.
 *
 * Filled from fwog_power_poll()'s return rather than by a second
 * fwog_buttons_poll() call: that call carries the debounce state the
 * ship-mode machine depends on, so polling again here would consume edges out
 * from under it. The console reports what the loop already saw; it never asks
 * the hardware itself. */
static fwog_buttons_t s_btn_last;

static const char *okstr(bool b) { return b ? "ok" : "fail"; }

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* strtol wrapper: base 0 accepts "0x.." as well as plain decimal. *ok is
 * false on an empty string, a non-numeric string, or trailing garbage. */
static long parse_long(const char *s, bool *ok) {
    char *end;
    long v = strtol(s, &end, 0);
    *ok = (s[0] != '\0') && (*end == '\0');
    return v;
}

static const char *chg_stat_name(bq25896_chg_stat_t s) {
    switch (s) {
    case BQ25896_CHG_NOT_CHARGING: return "not_charging";
    case BQ25896_CHG_PRE_CHARGE:   return "pre_charge";
    case BQ25896_CHG_FAST_CHARGE:  return "fast_charge";
    case BQ25896_CHG_DONE:         return "done";
    default:                       return "?";
    }
}

static const char *chg_fault_name(bq25896_chg_fault_t f) {
    switch (f) {
    case BQ25896_FAULT_NORMAL:           return "none";
    case BQ25896_FAULT_INPUT:            return "input";
    case BQ25896_FAULT_THERMAL_SHUTDOWN: return "thermal";
    case BQ25896_FAULT_TIMER_EXPIRED:    return "timer";
    default:                             return "?";
    }
}

/* Sub-block size for the microphone activity figure, in bytes. Capture runs
 * at PDM_SAMPLE_RATE_HZ * PDM_DECIMATION = 512 kHz, so one period of a 1 kHz
 * tone is 512 bits = 64 bytes; 16 bytes is a quarter of that period, short
 * enough to track the waveform rather than average it away.
 *
 * This app used to report whole-buffer 1-bit density instead. That figure is
 * DC bias, a tone is AC with zero mean, and averaging over a whole 2048-byte
 * buffer cancels it exactly -- see pdm_density_spread()'s header note for the
 * bench measurement that proved the old figure blind. */
#define BENCH_MIC_BLOCK_BYTES 16u

static void print_heartbeat(void) {
    bq25896_telemetry_t telem;
    const bool chg_ok = s_init.bq25896 && bq25896_read_all(&telem);

    /* Single-shot poll: at 100 Hz ODR a fresh sample is available every
       10 ms, and heartbeats are ~2 s apart, so this is not a freshness race
       the way the "accel" command's tighter retry loop has to be. */
    lis3dh_sample_t samp = {0};
    const bool accel_ok = s_init.lis3dh &&
                          lis3dh_process(LIS3DH_MOVE_THRESHOLD_DEFAULT, &samp, NULL);

    mcp7940_time_t t = {0};
    const bool rtc_ok = s_init.mcp7940 && mcp7940_read_time(&t);

    uint32_t mic_spread = 0u;
    bool mic_have = false;
    if (s_init.pdm) {
        const uint8_t *buf;
        size_t len;
        if (pdm_mic_take_raw_buffer(&buf, &len)) {
            mic_spread = pdm_density_spread(buf, len, BENCH_MIC_BLOCK_BYTES, NULL);
            mic_have = true;
        }
    }

    DIAG("HB init=bq25896:%s,mcp7940:%s,lis3dh:%s,ws2812:%s,pdm:%s,i2s:%s,"
         "ir:%s,st7789:%s "
         "chg=%s vbat_mv=%u vbus_mv=%u ichg_ma=%u "
         "ax=%d ay=%d az=%d "
         "rtc=20%02u-%02u-%02uT%02u:%02u:%02u rtc_cfg=%u rtc_osc=%u "
         "mic_spread=%u mic_fresh=%u up_ms=%u\n",
         okstr(s_init.bq25896), okstr(s_init.mcp7940), okstr(s_init.lis3dh),
         okstr(s_init.ws2812), okstr(s_init.pdm), okstr(s_init.i2s),
         okstr(s_init.ir), okstr(s_lcd_ready),
         chg_ok ? chg_stat_name(telem.chg_stat) : "err",
         chg_ok ? (unsigned)telem.vbat_mv : 0u,
         chg_ok ? (unsigned)telem.vbus_mv : 0u,
         chg_ok ? (unsigned)telem.ichg_ma : 0u,
         accel_ok ? (int)samp.x : 0, accel_ok ? (int)samp.y : 0,
         accel_ok ? (int)samp.z : 0,
         rtc_ok ? (unsigned)t.year : 0u, rtc_ok ? (unsigned)t.month : 0u,
         rtc_ok ? (unsigned)t.date : 0u, rtc_ok ? (unsigned)t.hour : 0u,
         rtc_ok ? (unsigned)t.min : 0u, rtc_ok ? (unsigned)t.sec : 0u,
         rtc_ok ? (unsigned)mcp7940_is_configured(&t) : 0u,
         rtc_ok ? (unsigned)mcp7940_is_oscillating(&t) : 0u,
         (unsigned)mic_spread, (unsigned)mic_have,
         (unsigned)to_ms_since_boot(get_absolute_time()));
}

/* ---- Command handlers -- each prints exactly one OK/ERR result line
 * (help and i2c print informational lines first, then their own OK). ---- */

static void cmd_help(void) {
    DIAG("commands:\n"
         "  help                                    this list\n"
         "  i2c                                     scan I2C1, list ACKing addrs\n"
         "  chg                                     BQ25896 charger telemetry\n"
         "  chg regs                                BQ25896 all 21 registers, raw hex\n"
         "  rtc get                                 MCP7940 time + configured/oscillating\n"
         "  rtc set <y> <mo> <d> <wd> <h> <mi> <s>   write MCP7940 time\n"
         "  accel                                    LIS3DH raw+mg x/y/z, WHO_AM_I\n"
         "  led <r> <g> <b>                         all WS2812 LEDs, one color\n"
         "  ledi <n> <r> <g> <b>                     one WS2812 LED by index\n"
         "  tone <hz> <ms>                           synth sine, play over I2S\n"
         "  vol <0-10>                               I2S playback volume\n"
         "  ant <r1 0-3> <r2 0-3> | ant default      CC1101 antenna path\n"
         "  ioexp                                    PCAL6416 readback: what it ACTUALLY holds\n"
         "  bootoe [0|1]                             MAIN_BOOT_OE: 0=red locked out of main, 1=RELEASED\n"
         "  mic <ms> [block_bytes]                   PDM sub-block density spread\n"
         "  pcm <ms>                                 CIC-decimated PCM: rms, peak, dominant Hz\n"
         "  ir <addr> <cmd>                          transmit one NEC frame\n"
         "  irrx                                     NEC frames since last call\n"
         "  screen <0-5>                             draw an LCD test pattern\n");
    /* Not a command, but the one behaviour of this app a bench operator can
       trigger by accident and cannot undo from the console. */
    DIAG("also: hold RED 6 s -> ship mode, board powers OFF "
         "(a GRAY hold or a USB attach wakes it)\n");
    DIAG("OK help 20 commands\n");
}

static void cmd_i2c(void) {
    uint8_t found[16];
    const size_t n = fwog_i2c_scan(found, sizeof(found));
    char list[96];
    size_t pos = 0;
    list[0] = '\0';
    for (size_t i = 0; i < n && pos + 8 < sizeof(list); i++) {
        const int w = snprintf(list + pos, sizeof(list) - pos, "%s0x%02X",
                               (i ? "," : ""), (unsigned)found[i]);
        if (w > 0) pos += (size_t)w;
    }
    DIAG("OK i2c count=%u addrs=%s\n", (unsigned)n, list);
}

static void cmd_chg(void) {
    if (!s_init.bq25896) { DIAG("ERR chg not-initialised\n"); return; }
    bq25896_telemetry_t t;
    if (!bq25896_read_all(&t)) { DIAG("ERR chg i2c-read-failed\n"); return; }
    DIAG("OK chg chg_stat=%s vbus_stat=%u fault=%s ts_rank=%u vbat_mv=%u "
         "vbus_mv=%u vbus_attached=%u ichg_ma=%u vsys_mv=%u\n",
         chg_stat_name(t.chg_stat), (unsigned)t.vbus_stat,
         chg_fault_name(t.chg_fault), (unsigned)t.ts_rank,
         (unsigned)t.vbat_mv, (unsigned)t.vbus_mv, (unsigned)t.vbus_attached,
         (unsigned)t.ichg_ma, (unsigned)t.vsys_mv);
}

/* 21 raw bytes, space-separated hex, so tools/bench.py can parse them with a
   split() and a strtol per field. Deliberately NOT decoded -- cmd_chg() above
   is the decoded view, and this one exists precisely for charger states this
   BSP's decoders do not model. Read-only: see bq25896.h on why the write-back
   companion (which would hit REG_RST) is not offered. */
static void cmd_chg_regs(void) {
    if (!s_init.bq25896) { DIAG("ERR chg regs not-initialised\n"); return; }
    uint8_t regs[BQ25896_REG_COUNT];
    if (!bq25896_read_raw_all(regs)) {
        DIAG("ERR chg regs i2c-read-failed\n");
        return;
    }
    /* 21 fields of "%02X" (42) + 20 separators + NUL = 63, in 64. */
    char hex[BQ25896_REG_COUNT * 3u + 1u];
    size_t pos = 0u;
    for (unsigned i = 0u; i < BQ25896_REG_COUNT; i++) {
        const int w = snprintf(hex + pos, sizeof(hex) - pos, "%s%02X",
                               (i ? " " : ""), (unsigned)regs[i]);
        if (w <= 0) break;
        pos += (size_t)w;
    }
    DIAG("OK chg regs count=%u bytes=%s\n", (unsigned)BQ25896_REG_COUNT, hex);
}

static void cmd_rtc_get(void) {
    mcp7940_time_t t;
    if (!mcp7940_read_time(&t)) {
        DIAG("ERR rtc get read-failed-or-out-of-range\n");
        return;
    }
    DIAG("OK rtc get 20%02u-%02u-%02u wd=%u %02u:%02u:%02u configured=%u "
         "oscillating=%u pwrfail=%u trim=%u\n",
         (unsigned)t.year, (unsigned)t.month, (unsigned)t.date,
         (unsigned)t.wkday, (unsigned)t.hour, (unsigned)t.min,
         (unsigned)t.sec, (unsigned)mcp7940_is_configured(&t),
         (unsigned)mcp7940_is_oscillating(&t), (unsigned)t.pwrfail,
         (unsigned)t.trim);
}

static void cmd_rtc_set(int argc, char **argv) {
    /* argv[0]="rtc" argv[1]="set" then 7 fields = 9 tokens. */
    if (argc != 9) {
        DIAG("ERR rtc set usage: rtc set <y> <mo> <d> <wd> <h> <mi> <s>\n");
        return;
    }
    bool ok[7];
    long y  = parse_long(argv[2], &ok[0]);
    long mo = parse_long(argv[3], &ok[1]);
    long d  = parse_long(argv[4], &ok[2]);
    long wd = parse_long(argv[5], &ok[3]);
    long h  = parse_long(argv[6], &ok[4]);
    long mi = parse_long(argv[7], &ok[5]);
    long s  = parse_long(argv[8], &ok[6]);
    for (unsigned i = 0; i < 7; i++) {
        if (!ok[i]) { DIAG("ERR rtc set bad numeric argument\n"); return; }
    }
    if (y >= 2000) y -= 2000;

    /* Preserve whatever OSCTRIM already holds -- mcp7940_write_time() writes
       it verbatim from t->trim, and this command has no business touching
       the crystal trim. Best-effort: a cold/unconfigured RTC that fails this
       read just gets trim=0, which is what an unwritten OSCTRIM already is. */
    uint8_t trim = 0u;
    mcp7940_time_t cur;
    if (mcp7940_read_time(&cur)) trim = cur.trim;

    mcp7940_time_t t = {0};
    t.year = (uint8_t)y; t.month = (uint8_t)mo; t.date = (uint8_t)d;
    t.wkday = (uint8_t)wd; t.hour = (uint8_t)h; t.min = (uint8_t)mi;
    t.sec = (uint8_t)s; t.trim = trim;

    if (!mcp7940_write_time(&t)) {
        DIAG("ERR rtc set write-failed-or-out-of-range\n");
        return;
    }
    DIAG("OK rtc set 20%02u-%02u-%02u wd=%u %02u:%02u:%02u\n",
         (unsigned)t.year, (unsigned)t.month, (unsigned)t.date,
         (unsigned)t.wkday, (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
}

static void cmd_accel(void) {
    if (!s_init.lis3dh) { DIAG("ERR accel not-initialised\n"); return; }

    uint8_t who = 0u;
    const bool who_ok =
        fwog_i2c_read_regs(I2C_ADDR_ACCEL, LIS3DH_REG_WHOAMI, &who, 1u);

    /* lis3dh_process() returns true both when a fresh sample arrived AND
       when there was simply no new data yet (ZYXDA not set) -- the header's
       own "Read-failure semantics" note says out_sample is left completely
       untouched on that second path. A sentinel that no real 10-bit-in-16-
       bit reading can produce (0x7FFF) lets this loop tell the two cases
       apart and retry across a couple of ODR periods (100 Hz -> 10 ms)
       instead of reporting a stale sentinel as if it were live data. */
    lis3dh_sample_t samp;
    samp.x = samp.y = samp.z = 0x7FFF;
    bool proc_ok = false;
    bool fresh = false;
    for (int attempt = 0; attempt < 25 && !fresh; attempt++) {
        proc_ok = lis3dh_process(LIS3DH_MOVE_THRESHOLD_DEFAULT, &samp, NULL);
        if (!proc_ok) break;
        if (samp.x != 0x7FFF || samp.y != 0x7FFF || samp.z != 0x7FFF) {
            fresh = true;
        } else {
            sleep_ms(1);
        }
    }
    if (!proc_ok) { DIAG("ERR accel i2c-read-failed\n"); return; }
    if (!fresh)   { DIAG("ERR accel no-new-data-timeout\n"); return; }

    const int32_t mgx = lis3dh_raw_to_mg(samp.x, LIS3DH_RANGE_2G);
    const int32_t mgy = lis3dh_raw_to_mg(samp.y, LIS3DH_RANGE_2G);
    const int32_t mgz = lis3dh_raw_to_mg(samp.z, LIS3DH_RANGE_2G);

    DIAG("OK accel x=%d y=%d z=%d mg_x=%d mg_y=%d mg_z=%d who_am_i=0x%02x "
         "(expect 0x33)\n",
         (int)samp.x, (int)samp.y, (int)samp.z, (int)mgx, (int)mgy, (int)mgz,
         who_ok ? (unsigned)who : 0xFFu);
}

static void cmd_led(int argc, char **argv) {
    if (!s_init.ws2812) { DIAG("ERR led not-initialised\n"); return; }
    if (argc != 4) { DIAG("ERR led usage: led <r> <g> <b>\n"); return; }
    bool okr, okg, okb;
    const long r = parse_long(argv[1], &okr);
    const long g = parse_long(argv[2], &okg);
    const long b = parse_long(argv[3], &okb);
    if (!okr || !okg || !okb || r < 0 || r > 255 || g < 0 || g > 255 ||
        b < 0 || b > 255) {
        DIAG("ERR led bad color (0-255 each)\n");
        return;
    }
    for (unsigned i = 0; i < FWOG_LED_COUNT; i++) {
        ws2812_set_color(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
    ws2812_process();
    DIAG("OK led r=%u g=%u b=%u count=%u\n", (unsigned)r, (unsigned)g,
         (unsigned)b, (unsigned)FWOG_LED_COUNT);
}

static void cmd_ledi(int argc, char **argv) {
    if (!s_init.ws2812) { DIAG("ERR ledi not-initialised\n"); return; }
    if (argc != 5) { DIAG("ERR ledi usage: ledi <n> <r> <g> <b>\n"); return; }
    bool okn, okr, okg, okb;
    const long n = parse_long(argv[1], &okn);
    const long r = parse_long(argv[2], &okr);
    const long g = parse_long(argv[3], &okg);
    const long b = parse_long(argv[4], &okb);
    if (!okn || n < 0 || (unsigned)n >= FWOG_LED_COUNT) {
        DIAG("ERR ledi bad index n=%ld (0..%u)\n", n,
             (unsigned)FWOG_LED_COUNT - 1u);
        return;
    }
    if (!okr || !okg || !okb || r < 0 || r > 255 || g < 0 || g > 255 ||
        b < 0 || b > 255) {
        DIAG("ERR ledi bad color (0-255 each)\n");
        return;
    }
    ws2812_set_color((unsigned)n, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    ws2812_process();
    DIAG("OK ledi n=%u r=%u g=%u b=%u\n", (unsigned)n, (unsigned)r,
         (unsigned)g, (unsigned)b);
}

static void cmd_tone(int argc, char **argv) {
    if (!s_init.i2s) { DIAG("ERR tone not-initialised\n"); return; }
    if (argc != 3) { DIAG("ERR tone usage: tone <hz> <ms>\n"); return; }
    bool okhz, okms;
    const long hz = parse_long(argv[1], &okhz);
    const long ms = parse_long(argv[2], &okms);
    if (!okhz || !okms || hz < 20 || hz > (long)(BENCH_TONE_SAMPLE_RATE_HZ / 2) ||
        ms <= 0) {
        DIAG("ERR tone bad args (hz 20-%u, ms > 0)\n",
             (unsigned)(BENCH_TONE_SAMPLE_RATE_HZ / 2));
        return;
    }
    if (!i2s_audio_is_idle()) {
        DIAG("ERR tone busy (a sound is already playing)\n");
        return;
    }

    unsigned ms_use = (unsigned)ms;
    if (ms_use > BENCH_TONE_MAX_MS) ms_use = BENCH_TONE_MAX_MS;
    unsigned count = (BENCH_TONE_SAMPLE_RATE_HZ * ms_use) / 1000u;
    if (count > BENCH_TONE_MAX_SAMPLES) count = BENCH_TONE_MAX_SAMPLES;

    for (unsigned i = 0; i < count; i++) {
        const float phase = 2.0f * BENCH_PI * (float)hz * (float)i /
                            (float)BENCH_TONE_SAMPLE_RATE_HZ;
        s_tone_buf[i] = (int16_t)((float)BENCH_TONE_AMPLITUDE * sinf(phase));
    }

    if (!i2s_audio_start(s_tone_buf, count, true, false)) {
        DIAG("ERR tone start-failed\n");
        return;
    }
    DIAG("OK tone hz=%ld ms=%u sample_rate=%u samples=%u\n", hz, ms_use,
         (unsigned)BENCH_TONE_SAMPLE_RATE_HZ, count);
}

/* Set the I2S playback volume for subsequent `tone` commands.
 *
 * This exists to make level dependence measurable, which is what separates a
 * digital format error from analog overdrive: scaling the samples scales a
 * format error's spectrum uniformly, while amplifier or speaker distortion
 * collapses far faster than the fundamental as the level drops. The
 * 2026-07-28 session had no way to vary the level and so could not make that
 * distinction. It is also the first thing that exercises
 * i2s_audio_set_volume() -- and therefore VOLUME_MULTIPLIERS -- on hardware
 * at all; every tone before this ran at the default of 10. */
static void cmd_vol(int argc, char **argv) {
    if (!s_init.i2s) { DIAG("ERR vol not-initialised\n"); return; }
    if (argc != 2) { DIAG("ERR vol usage: vol <0-10>\n"); return; }
    bool okv;
    const long v = parse_long(argv[1], &okv);
    if (!okv || v < 0 || v > 10) { DIAG("ERR vol bad args (0-10)\n"); return; }
    i2s_audio_set_volume((int)v);
    DIAG("OK vol %ld (multiplier %d/100)\n", v,
         (int)(i2s_audio_volume_multipliers[v] * 100.0f + 0.5f));
}

/* Set the CC1101 antenna/filter path on the PCAL6416.
 *
 * This chip lives on the DISPLAY CPU's I2C1 but almost everything it drives
 * belongs to MAIN -- including the antenna path for both radios (pcal6416.h).
 * That split is why this command exists here while the thing it affects is
 * measured over on apps/bench/main: main cannot reach the expander, and the
 * display cannot hear the radios.
 *
 * Fact 31 records that the expander ACKs but that nothing has ever confirmed
 * its bits do what the map says. With bench_main's radio-to-radio loopback as
 * a receiver, this command is the other half of the experiment that settles
 * the antenna field: change the path, measure the link.
 *
 * Every write starts from fwog_ioexp_default() and changes ONLY the two
 * antenna fields, so the breakout level-shifter directions and the I2C
 * pull-ups are never disturbed -- they are main's, they are in use, and this
 * command has no business moving them. */
/* `ioexp` -- read the PCAL6416 back over I2C and print what it ACTUALLY holds.
 *
 * This is the measurement nobody has ever taken, and its absence is why the
 * breakout direction work stalled: every "success" so far has been main's own
 * bookkeeping plus a link ack, and an ack proves the message ARRIVED, not that
 * the bits landed. fwog_ioexp_apply() writes OUTPUT0 (high byte) and OUTPUT1
 * (low byte) in one auto-incrementing transfer, so reading the pair back gives
 * the same 16-bit word fwog_ioexp_pack() produced -- or shows that it does
 * not.
 *
 * CONFIG0/1 are read too. They must be 0x0000: every pin an output. If they
 * are not, fwog_ioexp_init() never ran or something reset the part, and the
 * OUTPUT register would be inert no matter what is written to it -- which
 * would look exactly like "the direction bits do nothing". */
static void cmd_ioexp(void) {
    uint8_t out[2] = { 0xFFu, 0xFFu }, cfgr[2] = { 0xFFu, 0xFFu };
    const bool got_out = fwog_i2c_read_regs(FWOG_IOEXP_ADDR,
                                            PCAL6416_REG_OUTPUT0, out, 2u);
    const bool got_cfg = fwog_i2c_read_regs(FWOG_IOEXP_ADDR,
                                            PCAL6416_REG_CONFIG0, cfgr, 2u);
    if (!got_out || !got_cfg) {
        DIAG("ERR ioexp i2c-read-failed out=%d cfg=%d\n",
             (int)got_out, (int)got_cfg);
        return;
    }
    const unsigned w = (unsigned)((out[0] << 8) | out[1]);
    const unsigned c = (unsigned)((cfgr[0] << 8) | cfgr[1]);

    /* Decode the bits this investigation actually turns on. Names and values
       from fwog_ioexp_pack() in io_expander/pcal6416.c. */
    DIAG("  . io_config=%d  uart_tx_dir=%d uart_rx_dir=%d uart_rts_dir=%d "
         "uart_cts_dir=%d\n",
         (w & 0x0040u) ? 1 : 0, (w & 0x0800u) ? 1 : 0, (w & 0x0400u) ? 1 : 0,
         (w & 0x0200u) ? 1 : 0, (w & 0x0001u) ? 1 : 0);
    DIAG("  . spi_sclk_dir=%d spi_cs_dir=%d spi_rx_dir=%d spi_tx_dir=%d "
         "gpio25_dir=%d i2c_pullup=%d\n",
         (w & 0x8000u) ? 1 : 0, (w & 0x4000u) ? 1 : 0, (w & 0x2000u) ? 1 : 0,
         (w & 0x1000u) ? 1 : 0, (w & 0x0020u) ? 1 : 0, (w & 0x0080u) ? 1 : 0);
    DIAG("OK ioexp output=0x%04X config=0x%04X (config must be 0x0000 = all "
         "outputs)\n", w, c);
}

/* `btn` -- report the buttons as the main loop last saw them.
 *
 * Exists so a host script can WAIT for a real press instead of asking the
 * operator to hold a button across a conversational round trip. That matters
 * here specifically: holding RED for 6 s enters ship mode and powers the board
 * off, so any procedure that says "hold red and tell me when" is one slow reply
 * away from turning the board off. Polling this lets the press be short and the
 * host do the timing. */
static void cmd_btn(void) {
    const uint8_t d = s_btn_last.down;
    DIAG("OK btn down=0x%04X red=%d gray=%d yellow=%d green=%d blue=%d\n",
         (unsigned)d,
         (d & FWOG_BTN_BIT(FWOG_BTN_RED))    ? 1 : 0,
         (d & FWOG_BTN_BIT(FWOG_BTN_GRAY))   ? 1 : 0,
         (d & FWOG_BTN_BIT(FWOG_BTN_YELLOW)) ? 1 : 0,
         (d & FWOG_BTN_BIT(FWOG_BTN_GREEN))  ? 1 : 0,
         (d & FWOG_BTN_BIT(FWOG_BTN_BLUE))   ? 1 : 0);
}

/* `bootoe <0|1>` -- drive MAIN_BOOT_OE, the red button's lockout onto the MAIN
 * CPU's flash chip select. THE ONLY REASON THIS EXISTS is that the hardware record's
 * check has no negative control without it.
 *
 * ---- WHAT THIS PIN DOES, AND WHY 1 IS NOT A NEUTRAL VALUE ----
 *
 * Display GPIO 8 is the active-HIGH output enable of an SN74LVC1G126 whose
 * input is the RED BUTTON net and whose output reaches MAIN's QSPI_SS through
 * R15 (1 kOhm). QSPI_SS is main's flash chip select while running and its
 * BOOTSEL strap at reset.
 *
 *   0 = lockout ENGAGED, red disconnected from main. board_init_pins() sets
 *       this, matching the shipped firmware's obBootMainLockout.init(true,
 *       false, none). It is the running state and the safe state.
 *   1 = lockout RELEASED. A red press now reaches main's QSPI_SS. At RUNTIME
 *       that is expected to be harmless -- main drives QSPI_SS with a 4 mA pad
 *       and R15's 1 kOhm draws only ~3.3 mA, losing -- but AT MAIN'S RESET
 *       QSPI_SS is a high-impedance strap and red WILL be read as BOOTSEL.
 *
 * So `bootoe 1` plus a red press plus a main reset is expected to put main in
 * BOOTSEL. That is the point of the command, not a side effect. Recovery is
 * `fw flash bench`.
 *
 * Do NOT leave it at 1. Nothing here auto-reverts, deliberately: the test
 * needs the state held across main's reset, and a timeout that fired mid-test
 * would silently invalidate the result rather than protect anything. */
static void cmd_bootoe(int argc, char **argv) {
    if (argc < 2) {
        DIAG("OK bootoe level=%d (0=lockout engaged, red cannot reach main)\n",
             (int)gpio_get_out_level(PIN_MAIN_BOOT_OE));
        return;
    }
    bool okv;
    const long v = parse_long(argv[1], &okv);
    if (!okv || (v != 0 && v != 1)) {
        DIAG("ERR bootoe bad level (0=engage lockout, 1=RELEASE it)\n");
        return;
    }
    gpio_put(PIN_MAIN_BOOT_OE, v != 0);
    DIAG("OK bootoe level=%ld%s\n", v,
         v ? "  *** LOCKOUT RELEASED -- a red press now reaches MAIN's "
             "QSPI_SS; a main reset while red is held should enter BOOTSEL ***"
           : "  (lockout engaged)");
}

static void cmd_ant(int argc, char **argv) {
    static const char *ant_name[4] = { "isolation", "200MHz", "400MHz", "900MHz" };

    fwog_ant_t a1, a2;

    if (argc == 2 && streq(argv[1], "default")) {
        /* The DEFAULT ANTENNAS only. This used to load a whole
         * fwog_ioexp_default() and apply it, which also reset io_config and
         * all nine level-shifter directions -- main's, and in use -- while
         * this comment claimed the opposite. `ant default` restores the
         * antenna paths and nothing else; `iodir default` on main is what
         * restores directions. */
        fwog_ioexp_cfg_t d;
        fwog_ioexp_default(&d);
        a1 = d.radio1_ant;
        a2 = d.radio2_ant;
    } else if (argc == 3) {
        bool ok1, ok2;
        const long v1 = parse_long(argv[1], &ok1);
        const long v2 = parse_long(argv[2], &ok2);
        if (!ok1 || !ok2 || v1 < 0 || v1 > 3 || v2 < 0 || v2 > 3) {
            DIAG("ERR ant bad path (0=isolation 1=200MHz 2=400MHz 3=900MHz)\n");
            return;
        }
        a1 = (fwog_ant_t)v1;
        a2 = (fwog_ant_t)v2;
    } else {
        DIAG("ERR ant usage: ant <radio1 0-3> <radio2 0-3> | ant default\n");
        return;
    }

    /* Through the link handler's config -- the display's ONE copy of the
     * expander state. See io_expander/ioexp_link.h. */
    if (!fwog_ioexp_link_set_antennas(a1, a2)) {
        DIAG("ERR ant i2c-write-failed\n");
        return;
    }
    const uint16_t packed = fwog_ioexp_pack(fwog_ioexp_link_cfg());
    DIAG("OK ant radio1=%s radio2=%s packed=0x%04X "
         "(this chip does NOT reset with the RP2040 -- it holds until "
         "rewritten; directions are NOT touched)\n",
         ant_name[(int)a1], ant_name[(int)a2], (unsigned)packed);
}

static void cmd_mic(int argc, char **argv) {
    if (!s_init.pdm) { DIAG("ERR mic not-initialised\n"); return; }
    if (argc < 2 || argc > 3) {
        DIAG("ERR mic usage: mic <ms> [block_bytes]\n");
        return;
    }
    bool okms;
    const long ms = parse_long(argv[1], &okms);
    if (!okms || ms <= 0) { DIAG("ERR mic bad args (ms > 0)\n"); return; }
    unsigned ms_use = (unsigned)ms;
    if (ms_use > BENCH_MIC_MAX_MS) ms_use = BENCH_MIC_MAX_MS;

    /* Block size is tunable from the console because the right value depends
     * on the frequency being listened for, and a wrong one does not merely
     * degrade -- it NULLS. The block is a boxcar, so its response to a tone
     * goes to zero when the block spans exactly one full period: at the
     * 512 kHz capture rate that is 64 bytes for 1 kHz and 32 bytes for 2 kHz.
     * Keep the block at or below HALF a period of the frequency of interest
     * (16 bytes suits 2 kHz, 32 bytes suits 1 kHz). Larger blocks within that
     * bound are quieter, because a longer boxcar suppresses the sigma-delta's
     * high-frequency shaped quantisation noise faster than it attenuates the
     * signal. Being able to sweep this from the host is the difference between
     * tuning the instrument in seconds and tuning it one flash cycle at a
     * time. */
    unsigned block_bytes = BENCH_MIC_BLOCK_BYTES;
    if (argc == 3) {
        bool okb;
        const long bb = parse_long(argv[2], &okb);
        if (!okb || bb < 1 || (unsigned long)bb > PDM_RAW_BUFFER_BYTES / 2u) {
            DIAG("ERR mic bad block_bytes (1-%u)\n",
                 (unsigned)(PDM_RAW_BUFFER_BYTES / 2u));
            return;
        }
        block_bytes = (unsigned)bb;
    }

    const absolute_time_t deadline = make_timeout_time_ms(ms_use);
    uint32_t buffers = 0u, peak = 0u;
    uint64_t sum = 0u;
    /* `unsigned`, not uint32_t: on this target uint32_t is `unsigned long`,
       and pdm_density_spread() takes an `unsigned *`. */
    unsigned blocks = 0u;
    while (!time_reached(deadline)) {
        /* Keep any in-flight tone alive rather than starving it while this
           command busy-waits for the requested capture window. */
        i2s_audio_process();
        const uint8_t *buf;
        size_t len;
        if (pdm_mic_take_raw_buffer(&buf, &len)) {
            const uint32_t spread =
                pdm_density_spread(buf, len, block_bytes, &blocks);
            if (spread > peak) peak = spread;
            sum += spread;
            buffers++;
        }
    }
    const uint32_t avg = buffers ? (uint32_t)(sum / buffers) : 0u;
    DIAG("OK mic ms=%u buffers=%u peak_spread=%u avg_spread=%u blocks=%u "
         "block_bits=%u "
         "(peak-to-peak 1-density across %u-byte sub-blocks of each raw PDM "
         "buffer -- a boxcar decimation, so unlike the bit-density figure "
         "this replaces it can see AC; not a calibrated dB/RMS figure)\n",
         ms_use, (unsigned)buffers, (unsigned)peak, (unsigned)avg,
         (unsigned)blocks, (unsigned)(block_bytes * 8u),
         (unsigned)block_bytes);
}

/* ---- pcm: the decimated path ----
 * Everything here is file-scope static, never stack: the FFT scratch alone is
 * two float[256] arrays = 2 KB, and the SDK's default PICO_STACK_SIZE is 2048.
 * A stack array here would smash the stack on a board with no reachable
 * BOOTSEL button to recover it. */
static int16_t s_pcm[PDM_SAMPLE_BUFFER_SIZE];
static float   s_fft_re[FWOG_FFT_MAX_N];
static float   s_fft_im[FWOG_FFT_MAX_N];

static void cmd_pcm(int argc, char **argv) {
    if (!s_init.pdm) { DIAG("ERR pcm not-initialised\n"); return; }
    if (argc != 2) { DIAG("ERR pcm usage: pcm <ms>\n"); return; }
    bool okms;
    const long ms = parse_long(argv[1], &okms);
    if (!okms || ms <= 0) { DIAG("ERR pcm bad args (ms > 0)\n"); return; }
    unsigned ms_use = (unsigned)ms;
    if (ms_use > BENCH_MIC_MAX_MS) ms_use = BENCH_MIC_MAX_MS;

    /* ONE filter state for the whole capture, so consecutive raw buffers
       decimate continuously instead of restarting -- a reset per buffer puts a
       click at every 32 ms seam. */
    fwog_cic_t cic;
    fwog_cic_init(&cic);

    const absolute_time_t deadline = make_timeout_time_ms(ms_use);
    uint32_t buffers = 0u, rms_peak = 0u, amp_peak = 0u;
    unsigned spread_peak = 0u, blocks = 0u;
    bool have_pcm = false;

    while (!time_reached(deadline)) {
        /* Keep any in-flight tone alive rather than starving it while this
           command busy-waits for its capture window. */
        i2s_audio_process();
        const uint8_t *buf;
        size_t len;
        if (!pdm_mic_take_raw_buffer(&buf, &len)) continue;

        /* Geometry: 2048 raw bytes -> exactly PDM_SAMPLE_BUFFER_SIZE samples,
           so s_pcm is always exactly filled and never partially stale. */
        const size_t n = pdm_mic_decode(&cic, buf, len, s_pcm);
        if (n != PDM_SAMPLE_BUFFER_SIZE) {
            DIAG("ERR pcm geometry len=%u decoded=%u expected=%u\n",
                 (unsigned)len, (unsigned)n, (unsigned)PDM_SAMPLE_BUFFER_SIZE);
            return;
        }
        have_pcm = true;
        buffers++;

        const uint32_t rms = fwog_rms_i16(s_pcm, (unsigned)n);
        const uint16_t pk  = fwog_peak_i16(s_pcm, (unsigned)n);
        if (rms > rms_peak) rms_peak = rms;
        if (pk  > amp_peak) amp_peak = pk;

        /* The old metric, on the SAME buffer. Reported alongside on purpose:
           the two must move together across a stimulus sweep, and having both
           on one line is what makes that cross-check possible at all. */
        const unsigned spread =
            pdm_density_spread(buf, len, BENCH_MIC_BLOCK_BYTES, &blocks);
        if (spread > spread_peak) spread_peak = spread;
    }

    if (!have_pcm) {
        DIAG("ERR pcm no-buffers-captured (ms=%u)\n", ms_use);
        return;
    }

    /* Dominant frequency from the LAST decoded buffer only -- one 256-sample
       window at PDM_SAMPLE_RATE_HZ, i.e. a 31.25 Hz bin. DC-block first: the
       idle PDM stream is heavily biased and that bias otherwise floods the low
       bins. fwog_spectrum_dominant_bin() also skips bins 0-2 for the window
       leakage the blocker does not catch. */
    fwog_dcblock_inplace(s_pcm, PDM_SAMPLE_BUFFER_SIZE);
    const unsigned bin = fwog_spectrum_dominant_bin(s_pcm, FWOG_FFT_MAX_N,
                                                    s_fft_re, s_fft_im);
    const unsigned dom_hz = bin * PDM_SAMPLE_RATE_HZ / FWOG_FFT_MAX_N;

    DIAG("OK pcm ms=%u buffers=%u rms=%u peak=%u dom_hz=%u bin=%u "
         "bin_hz=%u spread=%u blocks=%u "
         "(3rd-order CIC at %ux; rms/peak are int16 counts, full-scale DC is "
         "16384, NOT a calibrated level; droop is -2.7 dB at 2 kHz and "
         "-11.8 dB at 4 kHz -- see common/dsp/cic.h)\n",
         ms_use, (unsigned)buffers, (unsigned)rms_peak, (unsigned)amp_peak,
         dom_hz, bin, (unsigned)(PDM_SAMPLE_RATE_HZ / FWOG_FFT_MAX_N),
         spread_peak, blocks, (unsigned)FWOG_CIC_DECIMATE);
}

static void cmd_ir(int argc, char **argv) {
    if (!s_init.ir) { DIAG("ERR ir not-initialised\n"); return; }
    if (argc != 3) { DIAG("ERR ir usage: ir <addr> <cmd>\n"); return; }
    bool oka, okc;
    const long addr = parse_long(argv[1], &oka);
    const long cmdv = parse_long(argv[2], &okc);
    if (!oka || !okc || addr < 0 || addr > 255 || cmdv < 0 || cmdv > 255) {
        DIAG("ERR ir bad args (addr/cmd 0-255)\n");
        return;
    }
    /* Standard 8-bit-address NEC frame: addr, ~addr, cmd, ~cmd, LSB first --
       matching ir_comm_init()'s out_shift(shift_right=true) on the
       nec_carrier_control state machine, which is the same bit-0-first
       order ir_nec_encode()'s own `code & (1 << bit)` loop assumes. */
    const uint32_t code = ((uint32_t)addr & 0xFFu) |
                          ((~(uint32_t)addr & 0xFFu) << 8) |
                          (((uint32_t)cmdv & 0xFFu) << 16) |
                          ((~(uint32_t)cmdv & 0xFFu) << 24);
    ir_comm_send(code);
    DIAG("OK ir addr=0x%02x cmd=0x%02x code=0x%08x\n", (unsigned)addr,
         (unsigned)cmdv, (unsigned)code);
}

static void cmd_irrx(void) {
    if (!s_init.ir) { DIAG("ERR irrx not-initialised\n"); return; }
    uint32_t code = 0u, last = 0u;
    unsigned count = 0u, bad = 0u;
    while (ir_comm_read(&code)) {
        last = code;
        count++;
        /* NEC's own integrity check. Three of 92 real frames from a household
           remote failed it (the hardware record), and before ir_nec_command_valid()
           existed they were reported here as though they were good. */
        if (!ir_nec_command_valid(code)) bad++;
    }
    if (count == 0u) { DIAG("ERR irrx none\n"); return; }
    DIAG("OK irrx count=%u bad_complement=%u last=0x%08x cmd=0x%02x "
         "addr16=0x%04x valid=%u classic=%u\n",
         count, bad, (unsigned)last, (unsigned)ir_nec_command(last),
         (unsigned)ir_nec_address16(last),
         (unsigned)ir_nec_command_valid(last),
         (unsigned)ir_nec_address_is_classic(last));
}

static void cmd_screen(int argc, char **argv) {
    if (!s_lcd_ready) { DIAG("ERR screen panel-not-ready\n"); return; }
    if (argc != 2) { DIAG("ERR screen usage: screen <0-5>\n"); return; }
    bool okn;
    const long n = parse_long(argv[1], &okn);
    if (!okn || n < 0 || n > 5) {
        DIAG("ERR screen bad pattern number (0-5)\n");
        return;
    }

    const uint16_t red    = st7789_rgb565(255, 0, 0);
    const uint16_t green  = st7789_rgb565(0, 255, 0);
    const uint16_t blue   = st7789_rgb565(0, 0, 255);
    const uint16_t white  = st7789_rgb565(255, 255, 255);
    const uint16_t yellow = st7789_rgb565(255, 255, 0);
    const uint16_t black  = 0x0000u;

    switch (n) {
    case 0:
        st7789_clear(black); st7789_dma_wait();
        DIAG("OK screen 0 black\n");
        break;
    case 1:
        st7789_clear(red); st7789_dma_wait();
        DIAG("OK screen 1 red\n");
        break;
    case 2:
        st7789_clear(green); st7789_dma_wait();
        DIAG("OK screen 2 green\n");
        break;
    case 3:
        st7789_clear(blue); st7789_dma_wait();
        DIAG("OK screen 3 blue\n");
        break;
    case 4: {
        const uint16_t hw = ST7789_W / 2, hh = ST7789_H / 2;
        st7789_fill_rect(0, 0, hw, hh, red);
        st7789_fill_rect(hw, 0, hw, hh, green);
        st7789_fill_rect(0, hh, hw, hh, blue);
        st7789_fill_rect(hw, hh, hw, hh, white);
        st7789_dma_wait();
        DIAG("OK screen 4 quadrants tl=red tr=green bl=blue br=white\n");
        break;
    }
    case 5:
        st7789_clear(black); st7789_dma_wait();
        st7789_fill_rect(1, 1, ST7789_W - 2, 1, white);              /* top */
        st7789_fill_rect(1, ST7789_H - 2, ST7789_W - 2, 1, white);   /* bottom */
        st7789_fill_rect(1, 1, 1, ST7789_H - 2, white);              /* left */
        st7789_fill_rect(ST7789_W - 2, 1, 1, ST7789_H - 2, white);   /* right */
        st7789_fill_rect(ST7789_W - 40, ST7789_H - 40, 40, 40, yellow);
        st7789_dma_wait();
        DIAG("OK screen 5 border corner=br size=40x40\n");
        break;
    default:
        DIAG("ERR screen bad pattern number (0-5)\n");
        break;
    }
}

/* Trivial whitespace tokenizer + dispatch. A blank line produces no output
   at all (not a command), matching the console's own "one result line per
   command" rule -- there was no command. */
static void process_line(char *line) {
    char *argv[10];
    int argc = 0;
    char *tok = strtok(line, " \t");
    while (tok && argc < 10) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0) return;

    const char *cmd = argv[0];
    if (streq(cmd, "help")) {
        cmd_help();
    } else if (streq(cmd, "i2c")) {
        cmd_i2c();
    } else if (streq(cmd, "chg")) {
        if (argc == 1) cmd_chg();
        else if (streq(argv[1], "regs")) cmd_chg_regs();
        else DIAG("ERR chg usage: chg | chg regs\n");
    } else if (streq(cmd, "rtc")) {
        if (argc >= 2 && streq(argv[1], "get")) cmd_rtc_get();
        else if (argc >= 2 && streq(argv[1], "set")) cmd_rtc_set(argc, argv);
        else DIAG("ERR rtc usage: rtc get | rtc set <y> <mo> <d> <wd> <h> <mi> <s>\n");
    } else if (streq(cmd, "accel")) {
        cmd_accel();
    } else if (streq(cmd, "led")) {
        cmd_led(argc, argv);
    } else if (streq(cmd, "ledi")) {
        cmd_ledi(argc, argv);
    } else if (streq(cmd, "tone")) {
        cmd_tone(argc, argv);
    } else if (streq(cmd, "vol")) {
        cmd_vol(argc, argv);
    } else if (streq(cmd, "btn")) {
        cmd_btn();
    } else if (streq(cmd, "bootoe")) {
        cmd_bootoe(argc, argv);
    } else if (streq(cmd, "ioexp")) {
        cmd_ioexp();
    } else if (streq(cmd, "ant")) {
        cmd_ant(argc, argv);
    } else if (streq(cmd, "mic")) {
        cmd_mic(argc, argv);
    } else if (streq(cmd, "pcm")) {
        cmd_pcm(argc, argv);
    } else if (streq(cmd, "ir")) {
        cmd_ir(argc, argv);
    } else if (streq(cmd, "irrx")) {
        cmd_irrx();
    } else if (streq(cmd, "screen")) {
        cmd_screen(argc, argv);
    } else {
        DIAG("ERR unknown command '%s'; try help\n", cmd);
    }
}

/* ---- Ship-mode countdown rendering ----
 * The driver owns no LED state (power/ship_mode.h): this mapping is the
 * application's, which is exactly what keeps the hold detector host-testable
 * with no dependency on ws2812_driver.h. It also mirrors the legacy firmware,
 * which writes the seven pixels from FreeWilliDisplay.cpp:392-400 -- the
 * application layer -- immediately before its own shutDown(). */

/* Ship mode now lives in the BSP (power/power_poll.h): the 6 s hold, the
   seven-pixel countdown, the save/restore around an aborted hold and the
   once-only fire were MOVED there so every display app gets them, not just
   this one. This app keeps only the call and the buttons it publishes. */

int main(void) {
    board_init();

    /* The bootloader deinits the link before jumping here (bl_jump.c), so
       the app must bring it back up itself to service main's breakout-
       direction requests (io_expander/ioexp_link.h). */
    s_link_up = fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_link_rx);

    /* pico_stdio_usb drops output when nothing is attached -- wait
       (bounded) for a host, exactly like lcd_display, so this app still
       runs start-to-finish on a board with no host attached. */
#if FWOG_DIAG == FWOG_DIAG_USB
    const absolute_time_t host_deadline = make_timeout_time_ms(5000);
    while (!stdio_usb_connected() && !time_reached(host_deadline)) {
        tight_loop_contents();
    }
    sleep_ms(300);
#endif

    DIAG("\n[bench_display] alive\n");
    DIAG("[bench_display] ioexp=%s (PCAL6416 at 0x21)\n",
         board_ioexp_ok() ? "ok" : "FAILED");
    DIAG("[bench_display] link=%s (services main's breakout-direction "
         "requests)\n", s_link_up ? "ok" : "FAILED");

    /* Bounded, not unbounded -- st7789_init_begin()/step() can leave
       ST7789_INIT_IDLE forever if clk_peri never reaches the panel rate
       (the hardware record), and there is no watchdog on this CPU to recover from a
       spin. 500 ms comfortably covers the ~125 ms a healthy init needs. */
    st7789_init_begin();
    const absolute_time_t lcd_deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(lcd_deadline)) st7789_init_step();
    s_lcd_ready = st7789_ready();
    DIAG("init st7789=%s\n", okstr(s_lcd_ready));
    if (s_lcd_ready) {
        st7789_clear(0x0000u);
        st7789_dma_wait();
        board_backlight(255);
    }

    /* Every driver gets its own line, and a failure here never stops the
       next one -- the whole point is to see which subsystems are alive in
       one pass. */
    s_init.bq25896 = bq25896_start_charging();
    DIAG("init bq25896=%s\n", okstr(s_init.bq25896));

    mcp7940_time_t boot_rtc;
    s_init.mcp7940 = mcp7940_read_time(&boot_rtc);
    DIAG("init mcp7940=%s\n", okstr(s_init.mcp7940));

    lis3dh_init();
    s_init.lis3dh = lis3dh_configure(LIS3DH_RANGE_2G);
    DIAG("init lis3dh=%s\n", okstr(s_init.lis3dh));

    /* PIO allocation from recon-dma.md: pio0 is shared by WS2812+PDM+I2S
       (16 of 32 words); pio1 is IR's alone (25 of 32 words). */
    s_init.ws2812 = ws2812_init(pio0, 0);
    DIAG("init ws2812=%s\n", okstr(s_init.ws2812));

    /* board_init_pins() already enabled the button pull-ups, so this only
       zeroes the debounce state. Required before fwog_buttons_poll(), which
       the ship-mode hold reads every loop iteration. */
    fwog_buttons_init();

    s_init.pdm = pdm_mic_init(pio0, 1);
    DIAG("init pdm=%s\n", okstr(s_init.pdm));

    s_init.i2s = i2s_audio_init(pio0, 2);
    DIAG("init i2s=%s\n", okstr(s_init.i2s));

    s_init.ir = ir_comm_init(pio1, 0);
    DIAG("init ir=%s\n", okstr(s_init.ir));

    DIAG("[bench_display] ready; type 'help' for commands\n");
    DIAG("[bench_display] WARNING: holding RED for %u s enters ship mode -- "
         "the board powers OFF. Attach USB or hold GRAY >2 s to wake it.\n",
         FWOG_SHIP_HOLD_MS / 1000u);

    static char line_buf[128];
    unsigned line_len = 0u;
    absolute_time_t next_hb = make_timeout_time_ms(2000);

    while (true) {
        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            if (c == '\r' || c == '\n') {
                if (line_len > 0u) {
                    line_buf[line_len] = '\0';
                    process_line(line_buf);
                    line_len = 0u;
                }
            } else if (c == '\b' || c == 0x7F) {
                if (line_len > 0u) line_len--;
            } else if (line_len < sizeof(line_buf) - 1u) {
                line_buf[line_len++] = (char)c;
            }
            /* else: line too long, extra characters silently dropped. */
        }

        /* Keeps any in-flight `tone` playback's ping-pong buffers fed even
           when no command is running. */
        i2s_audio_process();

        /* Ship mode. Polled every iteration rather than on a timer:
           fwog_btn_step() measures elapsed time instead of counting calls, so
           an irregular rate is fine and the 5 ms debounce is enforced inside
           it. The render inside is separately rate-limited. */
        s_btn_last = fwog_power_poll(
            to_ms_since_boot(get_absolute_time())).buttons;

        /* Service main's breakout-direction requests. */
        {
            uint8_t b;
            while (s_link_up && fwog_link_uart_read(&b)) {
                size_t len = 0u;
                if (fwog_link_rx_byte(&s_link_rx, b, &len))
                    (void)fwog_ioexp_link_handle(s_link_rx.buf, len);
            }
        }

        if (time_reached(next_hb)) {
            next_hb = make_timeout_time_ms(2000);
            print_heartbeat();
        }
    }
}
