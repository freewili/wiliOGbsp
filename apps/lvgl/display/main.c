/* The LVGL example: a focused list you drive with the front-panel buttons.
 *
 * It exists to show the four things an LVGL app on this board has to get
 * right, and nothing else. Read it in this order:
 *
 *   1. BRING THE PANEL UP FIRST. fwog_lvgl_init() does not do it, and
 *      st7789_blit() silently discards everything before st7789_ready() --
 *      so an app that skips this renders into a void with LVGL reporting
 *      every flush as a success. That failure has actually happened here.
 *   2. FEED THE BUTTONS, do not poll them. fwog_power_poll() carries the
 *      debounce state the 6 s red-hold power-off depends on; a second
 *      fwog_buttons_poll() steals its edges. See lvgl/fwog_lvgl.h.
 *   3. ATTACH A GROUP. LVGL only delivers keys to widgets in a group bound
 *      to the input device. Forgetting this gives a UI that draws perfectly
 *      and ignores every button, with no error anywhere.
 *   4. CALL lv_timer_handler() EVERY ITERATION, and keep the loop tight.
 *
 * BUILD. LVGL is not vendored in this BSP, so this app is only declared when
 * a target named `lvgl` exists. The easy way is to let this repo fetch it:
 *
 *     cmake --preset target -DFWOG_LVGL_FETCH=ON
 *     cmake --build build --target lvgl_main
 *     python tools/fw.py flash lvgl
 *
 * Without that flag the app is skipped and the rest of the tree builds as
 * usual. See apps/lvgl/CMakeLists.txt.
 *
 * Controls: gray/red move, green selects, yellow/blue are left/right.
 * HOLDING RED FOR SIX SECONDS POWERS THE BOARD OFF -- that is the board's
 * behaviour, not this app's, and it is why red is the least-held direction.
 */
#include "fwog_display.h"
#include "lvgl/fwog_lvgl.h"
#include "pico/stdlib.h"

FWOG_POWER_DEFAULT();

static lv_obj_t *s_status;

static void item_clicked(lv_event_t *e) {
    const char *name = lv_list_get_button_text(lv_obj_get_parent(lv_event_get_target(e)),
                                               lv_event_get_target(e));
    lv_label_set_text_fmt(s_status, "selected: %s", name ? name : "?");
}

/* Panel bring-up is a stepped state machine, not a blocking call: it has a
 * deadline so a dead panel reports rather than hanging a board whose only
 * recovery is a button hold. */
static bool panel_up(void) {
    st7789_init_begin();
    const absolute_time_t deadline = make_timeout_time_ms(500);
    while (!st7789_ready() && !time_reached(deadline)) {
        st7789_init_step();
        sleep_ms(1);
    }
    if (!st7789_ready()) {
        DIAG("[lvgl] panel did not come up; not starting LVGL\n");
        return false;
    }
    return true;
}

static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FreeWili OG " LV_SYMBOL_SETTINGS);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "green selects");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, 220, 150);
    lv_obj_center(list);

    /* A group is what connects the keypad to the widgets. Without it the
       list draws and never moves. */
    lv_group_t *group = lv_group_create();
    lv_indev_set_group(fwog_lvgl_indev(), group);

    static const char *ITEMS[] = { "Radios", "Display", "Audio", "Sensors",
                                   "Storage", "About" };
    for (unsigned i = 0; i < count_of(ITEMS); i++) {
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_RIGHT, ITEMS[i]);
        lv_obj_add_event_cb(btn, item_clicked, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(group, btn);
    }
}

int main(void) {
    board_init();

    if (panel_up()) {
        fwog_lvgl_init();
        build_ui();
        board_backlight(255);
    }

    while (true) {
        const uint32_t now = to_ms_since_boot(get_absolute_time());

        /* One poll, and its result is what everything else reads. */
        const fwog_power_t power = fwog_power_poll(now);
        fwog_lvgl_feed_buttons(power.buttons);

        lv_timer_handler();
        sleep_ms(2);
    }
}
