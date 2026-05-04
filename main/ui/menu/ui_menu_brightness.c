/**
 * @file ui_menu_brightness.c
 */
#include "ui_menu_brightness.h"
#include "display_brightness.h"

#include "lvgl.h"

/* Vertical slider: LVGL maps top → max value, bottom → min (slide up = brighter). */
#define BRIGHT_SLIDER_W  88
#define BRIGHT_SLIDER_H  300

typedef struct {
    lv_obj_t *value_lbl;
} bright_slider_ud_t;

static int32_t slider_internal_to_ui_pct(const lv_obj_t *sl)
{
    return lv_slider_get_value(sl);
}

static void bright_slider_sync_label(lv_obj_t *sl)
{
    bright_slider_ud_t *ud = (bright_slider_ud_t *)lv_obj_get_user_data(sl);
    if (!ud || !ud->value_lbl) {
        return;
    }
    const int32_t pct = slider_internal_to_ui_pct(sl);
    lv_label_set_text_fmt(ud->value_lbl, "%d", (int)pct);
}

static void bright_slider_evt(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        const int32_t pct = slider_internal_to_ui_pct(sl);
        display_brightness_apply_ui_pct((uint8_t)pct);
        bright_slider_sync_label(sl);
    } else if (code == LV_EVENT_RELEASED) {
        const int32_t pct = slider_internal_to_ui_pct(sl);
        display_brightness_save_ui_pct((uint8_t)pct);
    }
}

void ui_menu_brightness_populate(lv_obj_t *panel)
{
    lv_obj_t *title = lv_label_create(panel);
    lv_obj_add_flag(title, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *sl = lv_slider_create(panel);
    lv_slider_set_range(sl, 0, 100);
    lv_obj_set_size(sl, BRIGHT_SLIDER_W, BRIGHT_SLIDER_H);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_radius(sl, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(sl, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_right(sl, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_top(sl, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(sl, 14, LV_PART_MAIN);

    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x5EEAD4), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 14, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xF1F5F9), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);

    static bright_slider_ud_t ud;
    ud.value_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(ud.value_lbl, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(ud.value_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_align(ud.value_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    /* Fixed width + centered text so "0".."100" stay visually centered in the bar. */
    lv_obj_set_width(ud.value_lbl, BRIGHT_SLIDER_W);
    lv_obj_set_height(ud.value_lbl, lv_font_get_line_height(&lv_font_montserrat_28));
    lv_label_set_long_mode(ud.value_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align_to(ud.value_lbl, sl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(ud.value_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(ud.value_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(ud.value_lbl);

    lv_obj_set_user_data(sl, &ud);
    lv_obj_add_event_cb(sl, bright_slider_evt, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, bright_slider_evt, LV_EVENT_RELEASED, NULL);

    const uint8_t pct = display_brightness_get_ui_pct();
    lv_slider_set_value(sl, (int32_t)pct, LV_ANIM_OFF);
    bright_slider_sync_label(sl);
}
