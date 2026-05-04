/**
 * @file ui_menu_rgb.c
 */
#include "ui_menu_rgb.h"
#include "led_rgb.h"

#include "lvgl.h"

static void rgb_switch_evt(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    led_rgb_set_rainbow_running(on);
}

void ui_menu_rgb_populate(lv_obj_t *panel)
{
    lv_obj_t *title = lv_label_create(panel);
    lv_obj_add_flag(title, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(title, "RGB");
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, "WS2812");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, -64, 8);

    lv_obj_t *sw = lv_switch_create(panel);
    lv_obj_set_size(sw, 76, 42);
    lv_obj_align(sw, LV_ALIGN_CENTER, 64, 8);
    lv_obj_add_event_cb(sw, rgb_switch_evt, LV_EVENT_VALUE_CHANGED, NULL);
    if (led_rgb_is_rainbow_running()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
}
