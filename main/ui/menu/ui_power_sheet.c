/**
 * @file ui_power_sheet.c
 * @brief 顶部设置菜单（WIFI / Brightness / Volume）与底部 PD 全屏面板。
 */
#include "ui_power_sheet.h"
#include "ui_power_config.h"

#include "lvgl.h"
#include <stdbool.h>

static lv_obj_t                *s_top_sheet;
static lv_obj_t                *s_bottom_sheet;
static lv_coord_t               s_scr_h;
static bool                     s_top_open;
static bool                     s_bottom_open;
static ui_power_sheet_action_cb_t s_action_cb;
static void                    *s_action_ud;

typedef struct {
    ui_power_sheet_action_t action;
} btn_ud_t;

static void menu_btn_cb(lv_event_t *e)
{
    btn_ud_t *ud = (btn_ud_t *)lv_event_get_user_data(e);
    if (!ud || !s_action_cb) {
        return;
    }
    s_action_cb(ud->action, s_action_ud);
}

static lv_obj_t *create_menu_btn(lv_obj_t *parent, const char *label, ui_power_sheet_action_t action)
{
    static btn_ud_t uds[3];

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 120, 44);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x243b5c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(lb);

    uds[action].action = action;
    lv_obj_add_event_cb(btn, menu_btn_cb, LV_EVENT_CLICKED, &uds[action]);
    return btn;
}

void ui_power_sheet_create(lv_obj_t *screen, ui_power_sheet_action_cb_t cb, void *user_data)
{
    s_action_cb   = cb;
    s_action_ud   = user_data;
    s_top_open    = false;
    s_bottom_open = false;

    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(screen));
    const lv_coord_t h = lv_disp_get_ver_res(lv_obj_get_disp(screen));
    s_scr_h = h;

    /* 顶部设置菜单 */
    s_top_sheet = lv_obj_create(screen);
    lv_obj_add_flag(s_top_sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_top_sheet, w, UI_POWER_TOP_SHEET_H);
    lv_obj_set_style_bg_color(s_top_sheet, lv_color_hex(UI_POWER_TOP_SHEET_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_top_sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_top_sheet, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_top_sheet, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_top_sheet, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_top_sheet, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_sheet, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_top_sheet, 16, LV_PART_MAIN);
    lv_obj_align(s_top_sheet, LV_ALIGN_TOP_MID, 0, -UI_POWER_TOP_SHEET_H);
    lv_obj_clear_flag(s_top_sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    create_menu_btn(s_top_sheet, "WIFI", UI_POWER_SHEET_WIFI);
    create_menu_btn(s_top_sheet, "Brightness", UI_POWER_SHEET_BRIGHTNESS);
    create_menu_btn(s_top_sheet, "Volume", UI_POWER_SHEET_VOLUME);

    /* 底部 PD 全屏面板 */
    s_bottom_sheet = lv_obj_create(screen);
    lv_obj_add_flag(s_bottom_sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_bottom_sheet, w, h);
    lv_obj_set_style_bg_color(s_bottom_sheet, lv_color_hex(UI_POWER_SHEET_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bottom_sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bottom_sheet, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bottom_sheet, 0, LV_PART_MAIN);
    lv_obj_align(s_bottom_sheet, LV_ALIGN_TOP_MID, 0, h);
    lv_obj_clear_flag(s_bottom_sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pd_title = lv_label_create(s_bottom_sheet);
    lv_label_set_text(pd_title, "PD Spoof");
    lv_obj_set_style_text_color(pd_title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(pd_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(pd_title, LV_ALIGN_TOP_MID, 0, 48);
}

lv_obj_t *ui_power_sheet_get_top(void)
{
    return s_top_sheet;
}

lv_obj_t *ui_power_sheet_get_bottom(void)
{
    return s_bottom_sheet;
}

lv_coord_t ui_power_sheet_screen_h(void)
{
    return s_scr_h;
}

bool ui_power_sheet_top_is_open(void)
{
    return s_top_open;
}

bool ui_power_sheet_bottom_is_open(void)
{
    return s_bottom_open;
}

void ui_power_sheet_set_top_open(bool open)
{
    s_top_open = open;
}

void ui_power_sheet_set_bottom_open(bool open)
{
    s_bottom_open = open;
}
