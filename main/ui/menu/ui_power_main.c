/**
 * @file ui_power_main.c
 * @brief 电压/电流/功率数值显示与折线图显示。
 */
#include "ui_power_main.h"
#include "ui_power_config.h"
#include "power_meter.h"

#include "esp_err.h"
#include "lvgl.h"
#include <stdio.h>

#define CHART_POINT_COUNT  60
#define NUM_ROW_H          80
#define NUM_ROW_Y0         88
#define NUM_ROW_GAP        24
#define NUM_LABEL_X        16
#define NUM_LABEL_W        148
#define NUM_VALUE_X        168
#define NUM_VALUE_W        230

static ui_power_view_mode_t s_mode = UI_POWER_VIEW_NUMERIC;
static lv_obj_t            *s_numeric_root;
static lv_obj_t            *s_chart_root;
static lv_obj_t            *s_lbl_voltage;
static lv_obj_t            *s_lbl_current;
static lv_obj_t            *s_lbl_power;
static lv_obj_t            *s_chart;
static lv_chart_series_t   *s_ser_v;
static lv_chart_series_t   *s_ser_i;
static lv_chart_series_t   *s_ser_p;
static lv_coord_t           s_scr_w;

static void strip_obj_decor(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void apply_view_bg(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_POWER_MAIN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    strip_obj_decor(root);
}

/** 显示层不拦截触摸，让手势事件传递到 main_panel。 */
static void touch_pass_through_tree(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    const uint32_t cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        touch_pass_through_tree(lv_obj_get_child(obj, i));
    }
}

static void create_metric_row(lv_obj_t *parent, const char *title, lv_coord_t y,
                              lv_obj_t **val_lbl, uint32_t val_color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), NUM_ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    strip_obj_decor(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_pos(t, NUM_LABEL_X, (NUM_ROW_H - lv_font_get_line_height(&lv_font_montserrat_18)) / 2);
    lv_obj_set_width(t, NUM_LABEL_W);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(t, lv_color_hex(0x64748B), LV_PART_MAIN);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);
    strip_obj_decor(t);

    *val_lbl = lv_label_create(row);
    lv_obj_set_pos(*val_lbl, NUM_VALUE_X, (NUM_ROW_H - lv_font_get_line_height(&lv_font_montserrat_48)) / 2);
    lv_obj_set_width(*val_lbl, NUM_VALUE_W);
    lv_label_set_text(*val_lbl, "--");
    lv_label_set_long_mode(*val_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(*val_lbl, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(*val_lbl, lv_color_hex(val_color), LV_PART_MAIN);
    lv_obj_set_style_text_font(*val_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
    strip_obj_decor(*val_lbl);
}

static lv_obj_t *create_numeric_view(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    apply_view_bg(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    create_metric_row(root, "Voltage (V)", NUM_ROW_Y0, &s_lbl_voltage, 0xF8FAFC);
    create_metric_row(root, "Current (A)", NUM_ROW_Y0 + NUM_ROW_H + NUM_ROW_GAP, &s_lbl_current, 0xF8FAFC);
    create_metric_row(root, "Power (W)", NUM_ROW_Y0 + 2 * (NUM_ROW_H + NUM_ROW_GAP), &s_lbl_power, 0x5EEAD4);

    return root;
}

static lv_obj_t *create_chart_view(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    apply_view_bg(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Trend");
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);
    strip_obj_decor(title);

    s_chart = lv_chart_create(root);
    lv_obj_set_size(s_chart, 380, 300);
    lv_obj_align(s_chart, LV_ALIGN_CENTER, 0, 24);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINT_COUNT);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    /* 仅保留水平网格线，避免竖线残留 */
    lv_chart_set_div_line_count(s_chart, 4, 0);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x334155), LV_PART_ITEMS);
    strip_obj_decor(s_chart);

    s_ser_v = lv_chart_add_series(s_chart, lv_color_hex(0x38BDF8), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_i = lv_chart_add_series(s_chart, lv_color_hex(0xFBBF24), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_p = lv_chart_add_series(s_chart, lv_color_hex(0x5EEAD4), LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *legend = lv_obj_create(root);
    lv_obj_set_size(legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, LV_PART_MAIN);
    strip_obj_decor(legend);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 16, LV_PART_MAIN);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -24);

    const char *names[] = {"V", "I", "P"};
    const uint32_t cols[] = {0x38BDF8, 0xFBBF24, 0x5EEAD4};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lrow = lv_obj_create(legend);
        lv_obj_set_size(lrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(lrow, LV_OPA_TRANSP, LV_PART_MAIN);
        strip_obj_decor(lrow);
        lv_obj_set_flex_flow(lrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(lrow, 4, LV_PART_MAIN);

        lv_obj_t *sq = lv_obj_create(lrow);
        lv_obj_set_size(sq, 10, 10);
        lv_obj_set_style_radius(sq, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sq, lv_color_hex(cols[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, LV_PART_MAIN);
        strip_obj_decor(sq);

        lv_obj_t *lb = lv_label_create(lrow);
        lv_label_set_text(lb, names[i]);
        lv_obj_set_style_text_color(lb, lv_color_hex(0x94A3B8), LV_PART_MAIN);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, LV_PART_MAIN);
        strip_obj_decor(lb);
    }

    return root;
}

void ui_power_main_create(lv_obj_t *parent)
{
    s_scr_w = lv_disp_get_hor_res(lv_obj_get_disp(parent));
    s_mode  = UI_POWER_VIEW_NUMERIC;
    s_numeric_root = create_numeric_view(parent);
    s_chart_root   = create_chart_view(parent);
    touch_pass_through_tree(s_numeric_root);
    touch_pass_through_tree(s_chart_root);

    lv_obj_add_flag(s_chart_root, LV_OBJ_FLAG_HIDDEN);
    ui_power_main_set_mode(UI_POWER_VIEW_NUMERIC);
}

ui_power_view_mode_t ui_power_main_get_mode(void)
{
    return s_mode;
}

void ui_power_main_reset_views(void)
{
    if (!s_scr_w) {
        s_scr_w = lv_disp_get_hor_res(lv_disp_get_default());
    }
    if (s_numeric_root && lv_obj_is_valid(s_numeric_root)) {
        lv_obj_set_x(s_numeric_root, (s_mode == UI_POWER_VIEW_NUMERIC) ? 0 : -s_scr_w);
    }
    if (s_chart_root && lv_obj_is_valid(s_chart_root)) {
        lv_obj_set_x(s_chart_root, (s_mode == UI_POWER_VIEW_CHART) ? 0 : s_scr_w);
    }
}

void ui_power_main_set_mode(ui_power_view_mode_t mode)
{
    if (mode >= UI_POWER_VIEW_COUNT) {
        return;
    }
    s_mode = mode;
    if (mode == UI_POWER_VIEW_NUMERIC) {
        lv_obj_clear_flag(s_numeric_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chart_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_numeric_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_chart_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_power_main_reset_views();
}

lv_obj_t *ui_power_main_get_numeric_view(void)
{
    return s_numeric_root;
}

lv_obj_t *ui_power_main_get_chart_view(void)
{
    return s_chart_root;
}

void ui_power_main_refresh(void)
{
    power_meter_reading_t rd = {0};
    if (power_meter_read(&rd) != ESP_OK) {
        return;
    }

    char buf[24];
    if (s_lbl_voltage && lv_obj_is_valid(s_lbl_voltage)) {
        (void)snprintf(buf, sizeof(buf), "%.2f", (double)rd.voltage_v);
        lv_label_set_text(s_lbl_voltage, buf);
    }
    if (s_lbl_current && lv_obj_is_valid(s_lbl_current)) {
        (void)snprintf(buf, sizeof(buf), "%.3f", (double)rd.current_a);
        lv_label_set_text(s_lbl_current, buf);
    }
    if (s_lbl_power && lv_obj_is_valid(s_lbl_power)) {
        (void)snprintf(buf, sizeof(buf), "%.2f", (double)rd.power_w);
        lv_label_set_text(s_lbl_power, buf);
    }

    if (s_mode != UI_POWER_VIEW_CHART) {
        return;
    }
    if (s_chart && lv_obj_is_valid(s_chart) && s_ser_v && s_ser_i && s_ser_p) {
        lv_chart_set_next_value(s_chart, s_ser_v, (int32_t)(rd.voltage_v * 10.0f));
        lv_chart_set_next_value(s_chart, s_ser_i, (int32_t)(rd.current_a * 1000.0f));
        lv_chart_set_next_value(s_chart, s_ser_p, (int32_t)(rd.power_w * 2.0f));
        lv_chart_refresh(s_chart);
    }
}
