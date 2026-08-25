#include "iotest_ui.h"
#include "fwog_display.h"
#include "lvgl/fwog_lvgl.h"
#include <stdio.h>

/* Echoed back in every CONFIRM this UI sends -- see iotest_proto.h's header
   comment on why every request/reply pair carries a seq: without it, a
   button press queued during the wrong step could be misapplied. Updated
   whenever a STEP_SHOW or SUMMARY is rendered, never anywhere else. */
static uint8_t s_last_seq;

static lv_obj_t *s_step_screen, *s_summary_screen;
static lv_obj_t *s_hdr_label, *s_instr_label, *s_result_label, *s_progress_label;
static lv_obj_t *s_summary_list, *s_summary_overall;
static lv_group_t *s_step_group, *s_summary_group;

static const char *result_text(uint8_t r) {
    switch ((fwog_iotest_result_t)r) {
    case FWOG_IOTEST_RESULT_PENDING: return "";
    case FWOG_IOTEST_RESULT_PASS:    return "PASS";
    case FWOG_IOTEST_RESULT_FAIL:    return "FAIL";
    case FWOG_IOTEST_RESULT_SKIPPED: return "SKIPPED";
    case FWOG_IOTEST_RESULT_VOID:    return "VOID (control failed)";
    case FWOG_IOTEST_RESULT_WARN:    return "WARN";
    default:                         return "?";
    }
}

static uint32_t result_color(uint8_t r) {
    switch ((fwog_iotest_result_t)r) {
    case FWOG_IOTEST_RESULT_PASS:    return 0x30D030u;   /* green */
    case FWOG_IOTEST_RESULT_FAIL:    return 0xD03030u;   /* red */
    case FWOG_IOTEST_RESULT_VOID:    return 0xD03030u;   /* red */
    case FWOG_IOTEST_RESULT_SKIPPED: return 0x909090u;   /* gray */
    case FWOG_IOTEST_RESULT_WARN:    return 0xD0A030u;   /* amber */
    default:                         return 0xE0E0E0u;   /* pending: near-white */
    }
}

static void send_confirm(fwog_iotest_action_t action) {
    uint8_t payload[sizeof(fwog_iotest_confirm_t)];
    const size_t n = fwog_iotest_proto_build_confirm(payload, sizeof payload,
                                                      s_last_seq, action);
    if (n > 0u) fwog_link_uart_send_frame(payload, n);
}

static void on_confirm(lv_event_t *e)  { (void)e; send_confirm(FWOG_IOTEST_ACTION_CONFIRM); }
static void on_retry(lv_event_t *e)    { (void)e; send_confirm(FWOG_IOTEST_ACTION_RETRY); }
static void on_skip(lv_event_t *e)     { (void)e; send_confirm(FWOG_IOTEST_ACTION_SKIP); }
static void on_restart(lv_event_t *e)  { (void)e; send_confirm(FWOG_IOTEST_ACTION_RESTART); }

static lv_obj_t *make_button(lv_obj_t *parent, lv_group_t *group,
                              const char *text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(group, btn);
    return btn;
}

static void build_step_screen(void) {
    s_step_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_step_screen, lv_color_hex(0x101010), 0);

    lv_obj_t *title = lv_label_create(s_step_screen);
    lv_label_set_text(title, "Breakout I/O Test");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_progress_label = lv_label_create(s_step_screen);
    lv_obj_align(s_progress_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    s_hdr_label = lv_label_create(s_step_screen);
    lv_obj_set_style_text_font(s_hdr_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_hdr_label, LV_ALIGN_TOP_MID, 0, 40);

    s_instr_label = lv_label_create(s_step_screen);
    lv_label_set_long_mode(s_instr_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_instr_label, 280);
    lv_obj_align(s_instr_label, LV_ALIGN_TOP_MID, 0, 80);

    s_result_label = lv_label_create(s_step_screen);
    lv_obj_align(s_result_label, LV_ALIGN_TOP_MID, 0, 130);

    /* Nothing here requires holding a button -- RED is never bound to any of
       these three, only to LV_KEY_DOWN's group-focus movement, which this
       three-button row degrades safely if pressed. The only RED hazard is
       the pre-existing, unavoidable one: holding it 6 s powers the board
       off, same as every other app on this BSP. */
    lv_obj_t *row = lv_obj_create(s_step_screen);
    lv_obj_set_size(row, 300, 60);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_step_group = lv_group_create();
    lv_indev_set_group(fwog_lvgl_indev(), s_step_group);
    make_button(row, s_step_group, "Confirm", on_confirm);
    make_button(row, s_step_group, "Retry", on_retry);
    make_button(row, s_step_group, "Skip", on_skip);
}

static void build_summary_screen(void) {
    s_summary_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_summary_screen, lv_color_hex(0x101010), 0);

    lv_obj_t *title = lv_label_create(s_summary_screen);
    lv_label_set_text(title, "Test Summary");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_summary_overall = lv_label_create(s_summary_screen);
    lv_obj_align(s_summary_overall, LV_ALIGN_TOP_MID, 0, 28);

    s_summary_list = lv_list_create(s_summary_screen);
    lv_obj_set_size(s_summary_list, 300, 150);
    lv_obj_align(s_summary_list, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *btn = lv_button_create(s_summary_screen);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Restart");
    lv_obj_center(lbl);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(btn, on_restart, LV_EVENT_CLICKED, NULL);

    s_summary_group = lv_group_create();
    lv_group_add_obj(s_summary_group, btn);
}

void iotest_ui_build(void) {
    build_step_screen();
    build_summary_screen();
    lv_screen_load(s_step_screen);
}

void iotest_ui_show_step(const fwog_iotest_step_show_t *m) {
    s_last_seq = m->seq;

    if (m->drive_hdr != 0u && m->sense_hdr != 0u) {
        lv_label_set_text_fmt(s_hdr_label, "hdr%u " LV_SYMBOL_RIGHT " hdr%u",
                              m->drive_hdr, m->sense_hdr);
    } else if (m->drive_hdr != 0u) {
        lv_label_set_text_fmt(s_hdr_label, "hdr%u", m->drive_hdr);
    } else {
        lv_label_set_text(s_hdr_label, "");
    }

    /* text[] is NUL-padded but not guaranteed NUL-terminated within
       FWOG_IOTEST_TEXT_LEN if a future instruction were ever exactly that
       long -- guard the read the same way link payloads are always treated
       here, as untrusted-length rather than assumed-terminated. */
    char text[FWOG_IOTEST_TEXT_LEN + 1u];
    snprintf(text, sizeof text, "%.*s", (int)FWOG_IOTEST_TEXT_LEN, m->text);
    lv_label_set_text(s_instr_label, text);

    if (m->last_result == (uint8_t)FWOG_IOTEST_RESULT_PENDING) {
        lv_label_set_text(s_result_label, "");
    } else {
        lv_label_set_text_fmt(s_result_label, "Previous: %s",
                              result_text(m->last_result));
    }
    lv_obj_set_style_text_color(s_result_label,
                                lv_color_hex(result_color(m->last_result)), 0);

    lv_label_set_text_fmt(s_progress_label, "%u/%u",
                          (unsigned)m->step_index + 1u, (unsigned)m->step_count);

    lv_indev_set_group(fwog_lvgl_indev(), s_step_group);
    if (lv_screen_active() != s_step_screen) lv_screen_load(s_step_screen);
}

void iotest_ui_show_summary(const fwog_iotest_summary_t *m) {
    s_last_seq = m->seq;

    lv_label_set_text_fmt(s_summary_overall, "Overall: %s",
                          m->overall_pass ? "PASS" : "FAIL");
    lv_obj_set_style_text_color(s_summary_overall,
        lv_color_hex(m->overall_pass ? 0x30D030u : 0xD03030u), 0);

    /* lv_list_add_button(), not lv_list_add_text() -- the only lv_list row
       constructor already proven against this BSP's vendored LVGL build, in
       apps/lvgl/display/main.c. These rows are not clickable (not added to
       a group), just used for the icon+label row layout. */
    lv_obj_clean(s_summary_list);
    for (uint8_t i = 0; i < m->step_count && i < FWOG_IOTEST_MAX_STEPS; i++) {
        char line[40];
        snprintf(line, sizeof line, "step %u: %s", (unsigned)i + 1u,
                 result_text(m->results[i]));
        lv_obj_t *row = lv_list_add_button(s_summary_list, NULL, line);
        lv_obj_set_style_text_color(row, lv_color_hex(result_color(m->results[i])), 0);
    }

    lv_indev_set_group(fwog_lvgl_indev(), s_summary_group);
    lv_screen_load(s_summary_screen);
}
