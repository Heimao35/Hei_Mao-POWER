/**
 * @file ui_power_main.c
 * @brief 电压/电流/功率数值显示与折线图显示。
 */
#include "ui_power_main.h"
#include "ui_power_config.h"
#include "power_meter.h"

#include "esp_err.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHART_POINT_COUNT  60
#define CHART_NORM_MAX     100
#define CHART_W            380
#define CHART_H            300

#define NUM_ROW_H          80
#define NUM_ROW_Y0         88
#define NUM_ROW_GAP        24
#define NUM_LABEL_X        16
#define NUM_LABEL_W        148
#define NUM_VALUE_X        168
#define NUM_VALUE_W        230

/** 固定档位跨度：按窗口峰峰值选档，居中显示 */
static const float s_span_v[] = {0.5f, 2.0f, 8.0f, 30.0f};
static const float s_span_i[] = {0.05f, 0.2f, 1.0f, 5.5f};
static const float s_span_p[] = {2.0f, 10.0f, 40.0f, 150.0f};

typedef struct {
    float    buf[CHART_POINT_COUNT];
    uint16_t count;
    uint16_t head;
    float    disp_min;
    float    disp_max;
    uint8_t  tier_idx;
} chart_hist_t;

static ui_power_view_mode_t s_mode = UI_POWER_VIEW_NUMERIC;
static lv_obj_t            *s_numeric_root;
static lv_obj_t            *s_chart_root;
static lv_obj_t            *s_lbl_voltage;
static lv_obj_t            *s_lbl_current;
static lv_obj_t            *s_lbl_power;
static lv_obj_t            *s_chart;
static lv_obj_t            *s_lbl_chart_v;
static lv_obj_t            *s_lbl_chart_i;
static lv_obj_t            *s_lbl_chart_p;
static lv_obj_t            *s_lbl_chart_scale;
static lv_chart_series_t   *s_ser_v;
static lv_chart_series_t   *s_ser_i;
static lv_chart_series_t   *s_ser_p;
static lv_coord_t           s_scr_w;
static chart_hist_t         s_hist_v;
static chart_hist_t         s_hist_i;
static chart_hist_t         s_hist_p;

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

static void chart_hist_reset(chart_hist_t *h)
{
    memset(h, 0, sizeof(*h));
}

static void chart_hist_reset_all(void)
{
    chart_hist_reset(&s_hist_v);
    chart_hist_reset(&s_hist_i);
    chart_hist_reset(&s_hist_p);
}

static void chart_hist_push(chart_hist_t *h, float sample)
{
    h->buf[h->head] = sample;
    h->head = (h->head + 1) % CHART_POINT_COUNT;
    if (h->count < CHART_POINT_COUNT) {
        h->count++;
    }
}

static float chart_hist_at(const chart_hist_t *h, uint16_t age)
{
    if (age >= h->count) {
        return 0.0f;
    }
    const uint16_t newest = (h->head + CHART_POINT_COUNT - 1) % CHART_POINT_COUNT;
    const uint16_t idx    = (newest + CHART_POINT_COUNT - age) % CHART_POINT_COUNT;
    return h->buf[idx];
}

static void chart_hist_window_minmax(const chart_hist_t *h, float *rmin, float *rmax)
{
    *rmin = h->buf[0];
    *rmax = h->buf[0];
    for (uint16_t i = 1; i < h->count; i++) {
        if (h->buf[i] < *rmin) {
            *rmin = h->buf[i];
        }
        if (h->buf[i] > *rmax) {
            *rmax = h->buf[i];
        }
    }
}

static void chart_hist_update_tier(chart_hist_t *h, const float *spans, uint8_t span_cnt)
{
    if (h->count == 0 || span_cnt == 0) {
        return;
    }

    float rmin = 0.0f;
    float rmax = 0.0f;
    chart_hist_window_minmax(h, &rmin, &rmax);

    float peak = rmax - rmin;
    if (peak < spans[0] * 0.05f) {
        peak = spans[0] * 0.05f;
    }

    uint8_t need = span_cnt - 1;
    for (uint8_t i = 0; i < span_cnt; i++) {
        if (peak <= spans[i] * 0.82f) {
            need = i;
            break;
        }
    }

    if (need > h->tier_idx) {
        h->tier_idx = need;
    } else if (need < h->tier_idx && h->tier_idx > 0) {
        if (peak <= spans[h->tier_idx - 1] * 0.65f) {
            h->tier_idx--;
        }
    }

    const float span = spans[h->tier_idx];
    const float mid  = (rmax + rmin) * 0.5f;
    h->disp_min = mid - span * 0.5f;
    h->disp_max = mid + span * 0.5f;
    if (h->disp_min < 0.0f) {
        h->disp_min = 0.0f;
        h->disp_max = span;
    }
}

static lv_coord_t norm_to_chart(float v, float vmin, float vmax)
{
    if (vmax <= vmin) {
        return CHART_NORM_MAX / 2;
    }
    float t = (v - vmin) / (vmax - vmin);
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return (lv_coord_t)(t * (float)CHART_NORM_MAX + 0.5f);
}

static void chart_series_apply(lv_obj_t *chart, lv_chart_series_t *ser, const chart_hist_t *h)
{
    if (!chart || !ser || h->count == 0) {
        return;
    }

    const uint16_t n = CHART_POINT_COUNT;
    for (uint16_t i = 0; i < n; i++) {
        lv_coord_t y = LV_CHART_POINT_NONE;
        const uint16_t age = (uint16_t)(n - 1 - i);
        if (age < h->count) {
            const float raw = chart_hist_at(h, age);
            y = norm_to_chart(raw, h->disp_min, h->disp_max);
        }
        lv_chart_set_value_by_id(chart, ser, i, y);
    }
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
    create_metric_row(root, "Current", NUM_ROW_Y0 + NUM_ROW_H + NUM_ROW_GAP, &s_lbl_current, 0xF8FAFC);
    create_metric_row(root, "Power (W)", NUM_ROW_Y0 + 2 * (NUM_ROW_H + NUM_ROW_GAP), &s_lbl_power, 0x5EEAD4);

    return root;
}

static void create_chart_legend_row(lv_obj_t *parent, lv_coord_t x_ofs, lv_coord_t y_ofs,
                                    const char *name, uint32_t color, lv_obj_t **val_lbl)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 110, 22);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, x_ofs, y_ofs);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    strip_obj_decor(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sq = lv_obj_create(row);
    lv_obj_set_size(sq, 8, 8);
    lv_obj_align(sq, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(sq, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sq, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, LV_PART_MAIN);
    strip_obj_decor(sq);

    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, name);
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, LV_PART_MAIN);
    strip_obj_decor(lb);

    *val_lbl = lv_label_create(row);
    lv_label_set_text(*val_lbl, "--");
    lv_obj_align(*val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(*val_lbl, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(*val_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    strip_obj_decor(*val_lbl);
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);
    strip_obj_decor(title);

    s_chart = lv_chart_create(root);
    lv_obj_set_size(s_chart, CHART_W, CHART_H);
    lv_obj_align(s_chart, LV_ALIGN_CENTER, 0, 8);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINT_COUNT);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_NORM_MAX);
    lv_chart_set_div_line_count(s_chart, 4, 0);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x334155), LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    /* 隐藏转折点圆点，仅绘制折线 */
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    strip_obj_decor(s_chart);

    s_ser_v = lv_chart_add_series(s_chart, lv_color_hex(0x38BDF8), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_i = lv_chart_add_series(s_chart, lv_color_hex(0xFBBF24), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_p = lv_chart_add_series(s_chart, lv_color_hex(0x5EEAD4), LV_CHART_AXIS_PRIMARY_Y);

    create_chart_legend_row(root, -120, -52, "V", 0x38BDF8, &s_lbl_chart_v);
    create_chart_legend_row(root, 0, -52, "I", 0xFBBF24, &s_lbl_chart_i);
    create_chart_legend_row(root, 120, -52, "P", 0x5EEAD4, &s_lbl_chart_p);

    s_lbl_chart_scale = lv_label_create(root);
    lv_label_set_text(s_lbl_chart_scale, "Scale: --");
    lv_obj_set_width(s_lbl_chart_scale, CHART_W);
    lv_label_set_long_mode(s_lbl_chart_scale, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl_chart_scale, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_chart_scale, lv_color_hex(0x64748B), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_chart_scale, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_lbl_chart_scale, LV_ALIGN_BOTTOM_MID, 0, -20);
    strip_obj_decor(s_lbl_chart_scale);

    chart_hist_reset_all();
    return root;
}

static void update_readings(power_meter_reading_t *rd, char *v_buf, size_t v_len,
                            char *i_buf, size_t i_len, char *p_buf, size_t p_len,
                            bool with_unit)
{
    if (with_unit) {
        (void)snprintf(v_buf, v_len, "%.2f V", (double)rd->voltage_v);
        (void)snprintf(p_buf, p_len, "%.1f W", (double)rd->power_w);
    } else {
        (void)snprintf(v_buf, v_len, "%.2f", (double)rd->voltage_v);
        (void)snprintf(p_buf, p_len, "%.2f", (double)rd->power_w);
    }
    if (rd->overflow) {
        (void)snprintf(i_buf, i_len, "OL");
    } else {
        power_meter_format_current(rd->current_a, i_buf, i_len);
    }
}

static void chart_update_scale_label(void)
{
    if (!s_lbl_chart_scale || !lv_obj_is_valid(s_lbl_chart_scale)) {
        return;
    }
    char buf[128];
    (void)snprintf(buf, sizeof(buf),
                     "V span %.1g  |  I span %.2g A  |  P span %.0f W",
                     (double)s_span_v[s_hist_v.tier_idx],
                     (double)s_span_i[s_hist_i.tier_idx],
                     (double)s_span_p[s_hist_p.tier_idx]);
    lv_label_set_text(s_lbl_chart_scale, buf);
}

static void chart_push_sample(float v, float i, float p)
{
    chart_hist_push(&s_hist_v, v);
    chart_hist_push(&s_hist_i, i);
    chart_hist_push(&s_hist_p, p);

    chart_hist_update_tier(&s_hist_v, s_span_v, (uint8_t)(sizeof(s_span_v) / sizeof(s_span_v[0])));
    chart_hist_update_tier(&s_hist_i, s_span_i, (uint8_t)(sizeof(s_span_i) / sizeof(s_span_i[0])));
    chart_hist_update_tier(&s_hist_p, s_span_p, (uint8_t)(sizeof(s_span_p) / sizeof(s_span_p[0])));

    if (s_chart && lv_obj_is_valid(s_chart)) {
        chart_series_apply(s_chart, s_ser_v, &s_hist_v);
        chart_series_apply(s_chart, s_ser_i, &s_hist_i);
        chart_series_apply(s_chart, s_ser_p, &s_hist_p);
        lv_chart_refresh(s_chart);
    }
    chart_update_scale_label();
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
    if (mode == UI_POWER_VIEW_CHART && s_mode != UI_POWER_VIEW_CHART) {
        chart_hist_reset_all();
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

    char v_buf[16];
    char i_buf[24];
    char p_buf[16];
    char cv_buf[16];
    char ci_buf[24];
    char cp_buf[16];
    update_readings(&rd, v_buf, sizeof(v_buf), i_buf, sizeof(i_buf), p_buf, sizeof(p_buf), false);
    update_readings(&rd, cv_buf, sizeof(cv_buf), ci_buf, sizeof(ci_buf), cp_buf, sizeof(cp_buf), true);

    if (s_lbl_voltage && lv_obj_is_valid(s_lbl_voltage)) {
        lv_label_set_text(s_lbl_voltage, v_buf);
    }
    if (s_lbl_current && lv_obj_is_valid(s_lbl_current)) {
        lv_label_set_text(s_lbl_current, i_buf);
    }
    if (s_lbl_power && lv_obj_is_valid(s_lbl_power)) {
        lv_label_set_text(s_lbl_power, p_buf);
    }

    if (s_mode != UI_POWER_VIEW_CHART) {
        return;
    }

    if (s_lbl_chart_v && lv_obj_is_valid(s_lbl_chart_v)) {
        lv_label_set_text(s_lbl_chart_v, cv_buf);
    }
    if (s_lbl_chart_i && lv_obj_is_valid(s_lbl_chart_i)) {
        lv_label_set_text(s_lbl_chart_i, ci_buf);
    }
    if (s_lbl_chart_p && lv_obj_is_valid(s_lbl_chart_p)) {
        lv_label_set_text(s_lbl_chart_p, cp_buf);
    }

    const float i_abs = fabsf(rd.current_a);
    chart_push_sample(rd.voltage_v, i_abs, rd.power_w);
}
