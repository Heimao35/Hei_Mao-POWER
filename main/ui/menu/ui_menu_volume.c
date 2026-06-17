/**
 * @file ui_menu_volume.c
 * @brief 静音开关设置页。
 */
#include "ui_menu_volume.h"
#include "audio_volume.h"
#include "buzzer.h"

#include "lvgl.h"

static void mute_switch_evt(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        const bool enabled     = lv_obj_has_state(sw, LV_STATE_CHECKED);
        const bool was_enabled = audio_volume_buzzer_enabled();
        audio_volume_set_buzzer_enabled(enabled);
        (void)audio_volume_save_buzzer_enabled(enabled);
        if (!was_enabled && enabled) {
            buzzer_play_pattern_force(BUZZER_PATTERN_CONFIRM);
        }
    }
}

void ui_menu_volume_populate(lv_obj_t *panel)
{
    lv_obj_t *title = lv_label_create(panel);
    lv_obj_add_flag(title, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(title, "Volume");
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, 320, 56);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Mute");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);

    lv_obj_t *sw = lv_switch_create(row);
    if (audio_volume_buzzer_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, mute_switch_evt, LV_EVENT_VALUE_CHANGED, NULL);
}
