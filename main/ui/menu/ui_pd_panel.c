/**
 * @file ui_pd_panel.c
 * @brief CH224 PD 诱骗控制界面。
 */
#include "ui_pd_panel.h"
#include "pd_spoof.h"
#include "power_meter.h"
#include "ui_power_config.h"

#include "lvgl.h"
#include <stdio.h>

#define BTN_W  168
#define BTN_H  72
#define BTN_GAP 12
#define VOLT_BTN_COUNT 5
#define PD_PANEL_W     460
#define SW_BOX_W  120
#define SW_BOX_H  96
#define SW_W      100
#define SW_H      48
#define SW_BOX_X  20
#define SW_BOX_Y  36
#define INFO_COL_X  188
#define TOAST_W  360
#define TOAST_H  44
#define TOAST_MARGIN_BOTTOM 24

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_sw;
static bool      s_sw_syncing;
static lv_obj_t *s_lbl_target;
static lv_obj_t *s_lbl_actual;
static lv_obj_t *s_lbl_imax;
static lv_obj_t *s_lbl_pg;
static lv_obj_t *s_volt_btns[VOLT_BTN_COUNT];
static pd_spoof_voltage_t s_volt_map[VOLT_BTN_COUNT] = {
    PD_SPOOF_VOLT_9V, PD_SPOOF_VOLT_12V, PD_SPOOF_VOLT_15V, PD_SPOOF_VOLT_20V, PD_SPOOF_VOLT_28V,
};

static void strip_decor(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

/** 创建无主题边框/滚动条的透明容器。 */
static lv_obj_t *create_transparent_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                                        lv_coord_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_NONE, LV_PART_MAIN);
    lv_obj_remove_style(box, NULL, LV_PART_SCROLLBAR | LV_STATE_ANY);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static void style_volt_btn(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, selected ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x5EEAD4), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? 0x1e4a6b : 0x243b5c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
}

static void update_volt_btn_highlight(pd_spoof_voltage_t sel)
{
    for (int i = 0; i < VOLT_BTN_COUNT; i++) {
        if (s_volt_btns[i] && lv_obj_is_valid(s_volt_btns[i])) {
            style_volt_btn(s_volt_btns[i], s_volt_map[i] == sel);
        }
    }
}

static void sw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_sw_syncing) {
        return;
    }
    const bool on = lv_obj_has_state(s_sw, LV_STATE_CHECKED);
    (void)pd_spoof_set_enabled(on);
}

typedef struct {
    pd_spoof_voltage_t v;
} volt_btn_ud_t;

static void volt_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    volt_btn_ud_t *ud = lv_event_get_user_data(e);
    if (!ud) {
        return;
    }
    (void)pd_spoof_select_voltage(ud->v);
}

/** 信息行标签：不设固定宽度，避免 LVGL 绘制对象矩形左缘细线。 */
static lv_obj_t *create_info_label(lv_obj_t *parent, lv_coord_t y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_remove_style_all(lb);
    lv_obj_add_flag(lb, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(lb, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(lb, INFO_COL_X, y);
    lv_obj_set_style_text_color(lb, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, font, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(lb, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(lb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_side(lb, LV_BORDER_SIDE_NONE, LV_PART_MAIN);
    lv_obj_set_style_outline_width(lb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lb, 0, LV_PART_MAIN);
    lv_obj_remove_style(lb, NULL, LV_PART_SCROLLBAR | LV_STATE_ANY);
    return lb;
}

static volt_btn_ud_t s_volt_ud[VOLT_BTN_COUNT];

static lv_obj_t *create_volt_button(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                                    int idx, pd_spoof_voltage_t v)
{
    s_volt_ud[idx].v = v;

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_pos(btn, x, y);
    style_volt_btn(btn, false);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_center(lb);

    lv_obj_add_event_cb(btn, volt_btn_cb, LV_EVENT_CLICKED, &s_volt_ud[idx]);
    return btn;
}

static void style_pd_switch(lv_obj_t *sw)
{
    const uint32_t track_off = 0x475569;
    const uint32_t track_on  = 0x5EEAD4;
    const uint32_t knob_off  = 0xCBD5E1;
    const uint32_t knob_on   = 0xFFFFFF;

    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_border_opa(sw, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(sw, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, SW_H / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, SW_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_clip_corner(sw, true, LV_PART_MAIN);

    /* 关闭：深灰轨道 + 浅灰滑块 */
    lv_obj_set_style_bg_color(sw, lv_color_hex(track_off), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(track_off), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(knob_off), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);

    /* 开启：青色轨道 + 白色滑块 */
    lv_obj_set_style_bg_color(sw, lv_color_hex(track_on), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(track_on), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(knob_on), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_CHECKED);

    /* remove_style_all 会清掉主题 anim_fast，需手动恢复滑块动画时长 */
    lv_obj_set_style_anim_time(sw, 120, LV_PART_MAIN);
}

void ui_pd_panel_create(lv_obj_t *parent)
{
    lv_obj_t *sw_box = create_transparent_box(parent, SW_BOX_X, SW_BOX_Y, SW_BOX_W, SW_BOX_H);
    lv_obj_set_flex_flow(sw_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sw_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sw_box, 8, LV_PART_MAIN);

    lv_obj_t *sw_lbl = lv_label_create(sw_box);
    lv_obj_remove_style_all(sw_lbl);
    lv_label_set_text(sw_lbl, "PD");
    lv_obj_set_style_text_align(sw_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(sw_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(sw_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    s_sw = lv_switch_create(sw_box);
    lv_obj_remove_style_all(s_sw);
    lv_obj_set_size(s_sw, SW_W, SW_H);
    style_pd_switch(s_sw);
    lv_obj_set_ext_click_area(s_sw, 16);
    lv_obj_add_event_cb(s_sw, sw_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_target = create_info_label(parent, 44, &lv_font_montserrat_28, 0xF8FAFC);
    lv_label_set_text(s_lbl_target, "Target: --");

    s_lbl_actual = create_info_label(parent, 88, &lv_font_montserrat_18, 0xCBD5E1);
    lv_label_set_text(s_lbl_actual, "Actual: -- V");

    s_lbl_imax = create_info_label(parent, 118, &lv_font_montserrat_18, 0xCBD5E1);
    lv_label_set_text(s_lbl_imax, "Imax: --");

    s_lbl_pg = create_info_label(parent, 148, &lv_font_montserrat_18, 0xCBD5E1);
    lv_label_set_text(s_lbl_pg, "PG: --");

    const lv_coord_t grid_y = 185;
    const lv_coord_t x0     = (PD_PANEL_W - (2 * BTN_W + BTN_GAP)) / 2;
    const lv_coord_t row1_y = grid_y + BTN_H + BTN_GAP;
    const lv_coord_t row2_y = row1_y + BTN_H + BTN_GAP;
    const lv_coord_t x1     = x0 + BTN_W + BTN_GAP;
    const lv_coord_t x_center = (PD_PANEL_W - BTN_W) / 2;

    s_volt_btns[0] = create_volt_button(parent, "9V", x0, grid_y, 0, PD_SPOOF_VOLT_9V);
    s_volt_btns[1] = create_volt_button(parent, "12V", x1, grid_y, 1, PD_SPOOF_VOLT_12V);
    s_volt_btns[2] = create_volt_button(parent, "15V", x0, row1_y, 2, PD_SPOOF_VOLT_15V);
    s_volt_btns[3] = create_volt_button(parent, "20V", x1, row1_y, 3, PD_SPOOF_VOLT_20V);
    s_volt_btns[4] = create_volt_button(parent, "28V", x_center, row2_y, 4, PD_SPOOF_VOLT_28V);

    ui_pd_panel_sync_from_driver();
}

void ui_pd_panel_sync_from_driver_ex(bool animate_switch)
{
    pd_spoof_status_t st = {0};
    if (pd_spoof_get_status(&st) != ESP_OK) {
        return;
    }

    if (s_sw && lv_obj_is_valid(s_sw)) {
        const bool cur = lv_obj_has_state(s_sw, LV_STATE_CHECKED);
        if (cur != st.enabled) {
            s_sw_syncing = true;
            if (st.enabled) {
                lv_obj_add_state(s_sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_sw, LV_STATE_CHECKED);
            }
            if (animate_switch) {
                lv_event_send(s_sw, LV_EVENT_VALUE_CHANGED, NULL);
            }
            s_sw_syncing = false;
        }
    }
    update_volt_btn_highlight(st.selected_voltage);
}

void ui_pd_panel_sync_from_driver(void)
{
    ui_pd_panel_sync_from_driver_ex(false);
}

static void toast_anim_set_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static lv_coord_t toast_shown_y(void)
{
    const lv_coord_t scr_h = lv_disp_get_ver_res(lv_disp_get_default());
    return scr_h - TOAST_H - TOAST_MARGIN_BOTTOM;
}

static lv_coord_t toast_hidden_y(void)
{
    return lv_disp_get_ver_res(lv_disp_get_default());
}

static void toast_hide_anim_ready(lv_anim_t *a)
{
    lv_obj_t *toast = (lv_obj_t *)a->var;
    if (toast && lv_obj_is_valid(toast)) {
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    }
}

static void toast_slide_out(void)
{
    if (!s_toast || !lv_obj_is_valid(s_toast)) {
        return;
    }

    lv_anim_del(s_toast, toast_anim_set_y);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_anim_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(s_toast), toast_hidden_y());
    lv_anim_set_time(&a, UI_POWER_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, toast_hide_anim_ready);
    lv_anim_start(&a);
}

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    s_toast_timer = NULL;
    toast_slide_out();
}

void ui_pd_panel_show_toggle_toast(bool enabled)
{
    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        return;
    }

    const lv_coord_t scr_w = lv_disp_get_hor_res(lv_disp_get_default());
    const lv_coord_t x0    = (scr_w - TOAST_W) / 2;
    const lv_coord_t y0    = toast_shown_y();
    const lv_coord_t y1    = toast_hidden_y();

    if (!s_toast || !lv_obj_is_valid(s_toast)) {
        s_toast = lv_obj_create(scr);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(s_toast, TOAST_W, TOAST_H);
        lv_obj_set_x(s_toast, x0);
        lv_obj_set_style_radius(s_toast, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1e4a6b), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, LV_PART_MAIN);
        strip_decor(s_toast);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lb = lv_label_create(s_toast);
        lv_obj_set_user_data(s_toast, lb);
        lv_obj_center(lb);
        lv_obj_set_style_text_color(lb, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    lv_obj_t *lb = (lv_obj_t *)lv_obj_get_user_data(s_toast);
    if (lb) {
        lv_label_set_text(lb, enabled ? "PD enabled" : "PD disabled");
    }

    lv_anim_del(s_toast, toast_anim_set_y);
    lv_obj_set_x(s_toast, x0);
    lv_obj_set_y(s_toast, y1);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, toast_anim_set_y);
    lv_anim_set_values(&a, y1, y0);
    lv_anim_set_time(&a, UI_POWER_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, 1000, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

void ui_pd_panel_refresh(void)
{
    (void)pd_spoof_poll();

    pd_spoof_status_t st = {0};
    if (pd_spoof_get_status(&st) != ESP_OK) {
        return;
    }

    char buf[48];

    if (s_lbl_target && lv_obj_is_valid(s_lbl_target)) {
        (void)snprintf(buf, sizeof(buf), "Target: %d V", (int)st.selected_voltage);
        lv_label_set_text(s_lbl_target, buf);
    }

    power_meter_reading_t pm = {0};
    if (power_meter_read(&pm) == ESP_OK && s_lbl_actual && lv_obj_is_valid(s_lbl_actual)) {
        (void)snprintf(buf, sizeof(buf), "Actual: %.2f V", (double)pm.voltage_v);
        lv_label_set_text(s_lbl_actual, buf);
    }

    if (s_lbl_imax && lv_obj_is_valid(s_lbl_imax)) {
        if (st.enabled && st.max_current_a > 0.0f) {
            (void)snprintf(buf, sizeof(buf), "Imax: %.2f A", (double)st.max_current_a);
        } else {
            (void)snprintf(buf, sizeof(buf), "Imax: --");
        }
        lv_label_set_text(s_lbl_imax, buf);
    }

    if (s_lbl_pg && lv_obj_is_valid(s_lbl_pg)) {
        const char *pg = st.pg_ok ? "OK" : "Not ready";
        const char *pd = st.pd_active ? "PD active" : "PD idle";
        (void)snprintf(buf, sizeof(buf), "PG: %s | %s", pg, pd);
        lv_label_set_text(s_lbl_pg, buf);
    }
}
