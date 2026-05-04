/**
 * @file ui_menu_wifi.c
 */
#include "ui_menu_wifi.h"
#include "net_wifi.h"
#include "ui_status_bar.h"

#include "esp_err.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

#define WIFI_ROW_H           36
#define WIFI_ROWS_TOTAL      NET_WIFI_SCAN_MAX_APS
#define WIFI_ROWS_VISIBLE    5
#define WIFI_LIST_PAD        8
#define WIFI_LIST_H          (WIFI_ROWS_VISIBLE * WIFI_ROW_H + WIFI_LIST_PAD * 2)
#define WIFI_ANIM_MS         220
#define WIFI_SEL_ANIM_MS     180

typedef struct ui_wifi_ctx ui_wifi_ctx_t;

struct ui_wifi_ctx {
    lv_obj_t *panel;
    lv_obj_t *info_box;
    lv_obj_t *scroll;
    lv_obj_t *scroll_inner;
    lv_obj_t *sel_box;
    lv_obj_t *rows[WIFI_ROWS_TOTAL];
    lv_obj_t *inputs_col;
    lv_obj_t *ta_ssid;
    lv_obj_t *ta_pass;
    lv_obj_t *btn_connect;
    lv_obj_t *lbl_btn;
    lv_obj_t *kb;
    lv_obj_t *dismiss;
    lv_coord_t inputs_y_rest;
    lv_coord_t inputs_y_edit;
    lv_coord_t kb_h;
    int        selected_idx;
    uint8_t    scan_retry_count;
    lv_timer_t *conn_lbl_timer;
    bool       kb_open;
};

static void scan_retry_cb(lv_timer_t *t);

static void connect_label_reset_cb(lv_timer_t *t)
{
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)t->user_data;
    if (c) {
        c->conn_lbl_timer = NULL;
    }
    if (c && c->panel && lv_obj_is_valid(c->panel) && c->lbl_btn && lv_obj_is_valid(c->lbl_btn)) {
        lv_label_set_text(c->lbl_btn, "Connect");
        lv_obj_set_style_text_color(c->lbl_btn, lv_color_hex(0x0f172a), LV_PART_MAIN);
        lv_obj_clear_state(c->btn_connect, LV_STATE_DISABLED);
    }
    lv_timer_del(t);
}

static void schedule_connect_label_reset(ui_wifi_ctx_t *c, uint32_t delay_ms)
{
    if (!c) {
        return;
    }
    if (c->conn_lbl_timer) {
        lv_timer_del(c->conn_lbl_timer);
        c->conn_lbl_timer = NULL;
    }
    lv_timer_t *tm = lv_timer_create(connect_label_reset_cb, delay_ms, c);
    if (tm) {
        lv_timer_set_repeat_count(tm, 1);
        c->conn_lbl_timer = tm;
    }
}

static void panel_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_obj_t      *panel = lv_event_get_target(e);
    ui_wifi_ctx_t *c     = (ui_wifi_ctx_t *)lv_event_get_user_data(e);
    lv_obj_set_user_data(panel, NULL);
    if (c) {
        if (c->conn_lbl_timer) {
            lv_timer_del(c->conn_lbl_timer);
            c->conn_lbl_timer = NULL;
        }
        free(c);
    }
}

static void style_ta_one_line(lv_obj_t *ta)
{
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_height(ta, 40);
    lv_obj_set_style_radius(ta, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ta, 10, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xF1F5F9), LV_PART_MAIN);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, LV_PART_MAIN);
}

static void anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void inputs_move_to(ui_wifi_ctx_t *c, lv_coord_t y_tgt, uint32_t tms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c->inputs_col);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(c->inputs_col), y_tgt);
    lv_anim_set_time(&a, tms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void wifi_kb_close(ui_wifi_ctx_t *c);

static void kb_debounce_cb(lv_timer_t *t)
{
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)t->user_data;
    if (!c) {
        return;
    }
    if (lv_obj_has_state(c->ta_ssid, LV_STATE_FOCUSED) || lv_obj_has_state(c->ta_pass, LV_STATE_FOCUSED)) {
        return;
    }
    wifi_kb_close(c);
}

static void ta_focus_evt(lv_event_t *e)
{
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)lv_event_get_user_data(e);
    lv_event_code_t  code = lv_event_get_code(e);
    if (!c) {
        return;
    }
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_t *ta = lv_event_get_target(e);
        if (!c->kb_open) {
            c->kb_open = true;
            lv_coord_t kw = lv_disp_get_hor_res(lv_obj_get_disp(c->panel)) - 6;
            lv_obj_set_size(c->kb, kw, c->kb_h);
            lv_obj_align(c->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_add_flag(c->info_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(c->btn_connect, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(c->dismiss, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(c->kb, LV_OBJ_FLAG_HIDDEN);
            lv_keyboard_set_textarea(c->kb, ta);
            inputs_move_to(c, c->inputs_y_edit, WIFI_ANIM_MS);
            lv_obj_move_foreground(c->inputs_col);
            lv_obj_move_foreground(c->kb);
        } else {
            lv_keyboard_set_textarea(c->kb, ta);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_timer_t *tm = lv_timer_create(kb_debounce_cb, 90, c);
        if (tm) {
            lv_timer_set_repeat_count(tm, 1);
        }
    }
}

static void wifi_kb_close(ui_wifi_ctx_t *c)
{
    if (!c->kb_open) {
        return;
    }
    c->kb_open = false;
    lv_keyboard_set_textarea(c->kb, NULL);
    lv_obj_add_flag(c->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->dismiss, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(c->info_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(c->btn_connect, LV_OBJ_FLAG_HIDDEN);
    inputs_move_to(c, c->inputs_y_rest, WIFI_ANIM_MS);
}

static void dismiss_evt(lv_event_t *e)
{
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !c) {
        return;
    }
    lv_obj_clear_state(c->ta_ssid, LV_STATE_FOCUSED);
    lv_obj_clear_state(c->ta_pass, LV_STATE_FOCUSED);
    wifi_kb_close(c);
}

static void sel_move_to_row(ui_wifi_ctx_t *c, int idx)
{
    if (idx < 0 || idx >= WIFI_ROWS_TOTAL) {
        return;
    }
    const lv_coord_t y = WIFI_LIST_PAD / 2 + (lv_coord_t)idx * WIFI_ROW_H;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c->sel_box);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(c->sel_box), y);
    lv_anim_set_time(&a, WIFI_SEL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void row_click_evt(lv_event_t *e)
{
    ui_wifi_ctx_t *c   = (ui_wifi_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t      *row = lv_event_get_current_target(e);
    if (!c || !row) {
        return;
    }
    int idx = -1;
    for (int i = 0; i < WIFI_ROWS_TOTAL; i++) {
        if (c->rows[i] == row) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (!lbl) {
        return;
    }
    const char *txt = lv_label_get_text(lbl);
    if (!txt || txt[0] == '\0' || strcmp(txt, "-") == 0 || strcmp(txt, "…") == 0 || strcmp(txt, "...") == 0) {
        return;
    }
    c->selected_idx = idx;
    lv_textarea_set_text(c->ta_ssid, txt);
    sel_move_to_row(c, idx);
}

static void scan_done_cb(esp_err_t err, const net_wifi_ap_info_t *aps, int n, void *user_data)
{
    lv_obj_t *panel = (lv_obj_t *)user_data;
    if (!panel || !lv_obj_is_valid(panel)) {
        return;
    }
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)lv_obj_get_user_data(panel);
    if (!c) {
        return;
    }
    if (err != ESP_OK) {
        lv_label_set_text(lv_obj_get_child(c->rows[0], 0), "Scan error");
        for (int j = 1; j < WIFI_ROWS_TOTAL; j++) {
            lv_label_set_text(lv_obj_get_child(c->rows[j], 0), "-");
        }
        lv_obj_invalidate(c->info_box);
        return;
    }
    for (int i = 0; i < WIFI_ROWS_TOTAL; i++) {
        lv_obj_t *lbl = lv_obj_get_child(c->rows[i], 0);
        if (i < n && aps && aps[i].ssid[0]) {
            lv_label_set_text(lbl, aps[i].ssid);
        } else {
            lv_label_set_text(lbl, "-");
        }
    }
    if (n == 0 && err == ESP_OK && c->scan_retry_count < 1U) {
        c->scan_retry_count++;
        lv_timer_t *tr = lv_timer_create(scan_retry_cb, 2000, panel);
        if (tr) {
            lv_timer_set_repeat_count(tr, 1);
        }
    }
    if (n > 0 && aps && aps[0].ssid[0]) {
        lv_textarea_set_text(c->ta_ssid, aps[0].ssid);
        c->selected_idx = 0;
        sel_move_to_row(c, 0);
    }
    lv_obj_invalidate(c->info_box);
}

static void scan_retry_cb(lv_timer_t *t)
{
    lv_obj_t *panel = (lv_obj_t *)t->user_data;
    if (panel && lv_obj_is_valid(panel)) {
        net_wifi_scan_request(scan_done_cb, panel);
    }
}

static void scan_timer_cb(lv_timer_t *t)
{
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)t->user_data;
    if (c && c->panel) {
        net_wifi_scan_request(scan_done_cb, c->panel);
    }
}

static void connect_done_cb(bool ok, void *user_data)
{
    lv_obj_t *panel = (lv_obj_t *)user_data;
    if (!panel || !lv_obj_is_valid(panel)) {
        return;
    }
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)lv_obj_get_user_data(panel);
    if (!c || !c->btn_connect) {
        return;
    }
    lv_label_set_text(c->lbl_btn, ok ? "Success" : "Failed");
    lv_obj_set_style_text_color(c->lbl_btn,
                                ok ? lv_color_hex(0x14532d) : lv_color_hex(0x991b1b),
                                LV_PART_MAIN);
    if (ok) {
        ui_status_bar_sync_wifi();
    }
    schedule_connect_label_reset(c, 1000);
}

static void connect_btn_evt(lv_event_t *e)
{
    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !c) {
        return;
    }
    if (c->kb_open) {
        wifi_kb_close(c);
    }
    const char *ssid = lv_textarea_get_text(c->ta_ssid);
    const char *pwd  = lv_textarea_get_text(c->ta_pass);
    if (!ssid || ssid[0] == '\0') {
        lv_label_set_text(c->lbl_btn, "Need SSID");
        lv_obj_set_style_text_color(c->lbl_btn, lv_color_hex(0x7f1d1d), LV_PART_MAIN);
        schedule_connect_label_reset(c, 1000);
        return;
    }
    if (c->conn_lbl_timer) {
        lv_timer_del(c->conn_lbl_timer);
        c->conn_lbl_timer = NULL;
    }
    lv_obj_add_state(c->btn_connect, LV_STATE_DISABLED);
    lv_label_set_text(c->lbl_btn, "Connecting");
    lv_obj_set_style_text_color(c->lbl_btn, lv_color_hex(0x0f172a), LV_PART_MAIN);
    net_wifi_connect_request(ssid, pwd, connect_done_cb, c->panel);
}

void ui_menu_wifi_populate(lv_obj_t *panel)
{
    lv_disp_t *disp = lv_obj_get_disp(panel);
    lv_coord_t pw   = lv_disp_get_hor_res(disp);
    lv_coord_t ph   = lv_disp_get_ver_res(disp);
    if (pw <= 0) {
        pw = lv_obj_get_width(panel);
    }
    if (ph <= 0) {
        ph = lv_obj_get_height(panel);
    }

    ui_wifi_ctx_t *c = (ui_wifi_ctx_t *)calloc(1, sizeof(*c));
    if (!c) {
        return;
    }
    c->panel          = panel;
    c->selected_idx   = -1;
    c->kb_h           = ph / 2;
    c->inputs_y_rest  = 44 + WIFI_LIST_H + 8;
    /* With half-screen keyboard, keep inputs in upper band */
    c->inputs_y_edit  = 32;

    lv_obj_set_user_data(panel, c);
    lv_obj_add_event_cb(panel, panel_delete_cb, LV_EVENT_DELETE, c);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_obj_add_flag(title, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    c->dismiss = lv_obj_create(panel);
    lv_obj_set_size(c->dismiss, pw, ph);
    lv_obj_align(c->dismiss, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(c->dismiss, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->dismiss, 0, LV_PART_MAIN);
    lv_obj_add_flag(c->dismiss, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->dismiss, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->dismiss, dismiss_evt, LV_EVENT_CLICKED, c);

    const lv_coord_t list_box_w = pw - 24;
    const lv_coord_t list_box_h = WIFI_LIST_H;

    c->info_box = lv_obj_create(panel);
    lv_obj_set_size(c->info_box, list_box_w, list_box_h);
    lv_obj_align(c->info_box, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_radius(c->info_box, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c->info_box, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->info_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->info_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(c->info_box, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_pad_all(c->info_box, 0, LV_PART_MAIN);

    c->scroll = lv_obj_create(c->info_box);
    lv_obj_set_size(c->scroll, list_box_w - 8, list_box_h - 8);
    lv_obj_align(c->scroll, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(c->scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c->scroll, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(c->scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c->scroll, LV_SCROLLBAR_MODE_AUTO);

    const lv_coord_t inner_w = list_box_w - 16;
    c->scroll_inner = lv_obj_create(c->scroll);
    lv_obj_set_size(c->scroll_inner, inner_w, WIFI_ROW_H * WIFI_ROWS_TOTAL + WIFI_LIST_PAD);
    lv_obj_align(c->scroll_inner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(c->scroll_inner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->scroll_inner, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c->scroll_inner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(c->scroll_inner, LV_OBJ_FLAG_SCROLLABLE);

    c->sel_box = lv_obj_create(c->scroll_inner);
    lv_obj_set_size(c->sel_box, inner_w - 8, WIFI_ROW_H - 4);
    lv_obj_set_pos(c->sel_box, 4, WIFI_LIST_PAD / 2);
    lv_obj_set_style_radius(c->sel_box, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->sel_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->sel_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(c->sel_box, lv_color_hex(0x5EEAD4), LV_PART_MAIN);
    lv_obj_clear_flag(c->sel_box, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < WIFI_ROWS_TOTAL; i++) {
        lv_obj_t *row = lv_obj_create(c->scroll_inner);
        c->rows[i]    = row;
        lv_obj_set_size(row, inner_w - 8, WIFI_ROW_H - 2);
        lv_obj_set_pos(row, 4, WIFI_LIST_PAD / 2 + i * WIFI_ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_evt, LV_EVENT_CLICKED, c);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "...");
        lv_obj_set_width(lbl, inner_w - 20);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }

    c->inputs_col = lv_obj_create(panel);
    lv_obj_set_width(c->inputs_col, pw - 24);
    lv_obj_set_height(c->inputs_col, LV_SIZE_CONTENT);
    lv_obj_set_pos(c->inputs_col, 12, c->inputs_y_rest);
    lv_obj_clear_flag(c->inputs_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c->inputs_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->inputs_col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c->inputs_col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(c->inputs_col, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(c->inputs_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c->inputs_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *la = lv_label_create(c->inputs_col);
    lv_label_set_text(la, "SSID");
    lv_obj_set_style_text_color(la, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(la, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_width(la, LV_PCT(100));

    c->ta_ssid = lv_textarea_create(c->inputs_col);
    lv_obj_set_width(c->ta_ssid, LV_PCT(100));
    lv_textarea_set_max_length(c->ta_ssid, NET_WIFI_SSID_MAX_LEN);
    style_ta_one_line(c->ta_ssid);
    lv_obj_add_event_cb(c->ta_ssid, ta_focus_evt, LV_EVENT_FOCUSED, c);
    lv_obj_add_event_cb(c->ta_ssid, ta_focus_evt, LV_EVENT_DEFOCUSED, c);

    lv_obj_t *lb = lv_label_create(c->inputs_col);
    lv_label_set_text(lb, "Password");
    lv_obj_set_style_text_color(lb, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_width(lb, LV_PCT(100));

    c->ta_pass = lv_textarea_create(c->inputs_col);
    lv_obj_set_width(c->ta_pass, LV_PCT(100));
    lv_textarea_set_max_length(c->ta_pass, NET_WIFI_PASS_MAX_LEN);
    style_ta_one_line(c->ta_pass);
    lv_obj_add_event_cb(c->ta_pass, ta_focus_evt, LV_EVENT_FOCUSED, c);
    lv_obj_add_event_cb(c->ta_pass, ta_focus_evt, LV_EVENT_DEFOCUSED, c);

    c->btn_connect = lv_obj_create(panel);
    lv_obj_set_size(c->btn_connect, pw - 48, 48);
    lv_obj_align(c->btn_connect, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(c->btn_connect, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c->btn_connect, lv_color_hex(0x5EEAD4), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->btn_connect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(c->btn_connect, 0, LV_PART_MAIN);
    lv_obj_add_flag(c->btn_connect, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->btn_connect, connect_btn_evt, LV_EVENT_CLICKED, c);

    c->lbl_btn = lv_label_create(c->btn_connect);
    lv_obj_add_flag(c->lbl_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(c->lbl_btn, "Connect");
    lv_obj_set_style_text_color(c->lbl_btn, lv_color_hex(0x0f172a), LV_PART_MAIN);
    lv_obj_set_style_text_font(c->lbl_btn, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(c->lbl_btn);

    c->kb = lv_keyboard_create(panel);
    lv_obj_set_size(c->kb, pw - 6, c->kb_h);
    lv_obj_align(c->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(c->kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(c->kb, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(c->kb, &lv_font_montserrat_22, LV_PART_ITEMS);
    lv_obj_set_style_pad_row(c->kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_pad_column(c->kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(c->kb, 6, LV_PART_MAIN);
    lv_obj_add_flag(c->kb, LV_OBJ_FLAG_HIDDEN);
    /* lv_keyboard_create() already attaches lv_keyboard_def_event_cb on VALUE_CHANGED only; do not add a second handler. */

    lv_timer_t *once = lv_timer_create(scan_timer_cb, 450, c);
    if (once) {
        lv_timer_set_repeat_count(once, 1);
    }

    lv_obj_update_layout(panel);
}
