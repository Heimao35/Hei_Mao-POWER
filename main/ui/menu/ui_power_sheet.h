/**
 * @file ui_power_sheet.h
 * @brief 顶部设置下拉菜单与底部 PD 诱骗上拉面板。
 */
#ifndef UI_POWER_SHEET_H
#define UI_POWER_SHEET_H

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_POWER_SHEET_WIFI = 0,
    UI_POWER_SHEET_BRIGHTNESS,
    UI_POWER_SHEET_VOLUME,
} ui_power_sheet_action_t;

typedef void (*ui_power_sheet_action_cb_t)(ui_power_sheet_action_t action, void *user_data);

/** 创建顶部设置菜单与底部 PD 面板（初始隐藏）。 */
void ui_power_sheet_create(lv_obj_t *screen, ui_power_sheet_action_cb_t cb, void *user_data);

lv_obj_t *ui_power_sheet_get_top(void);
lv_obj_t *ui_power_sheet_get_bottom(void);

/** 顶部菜单是否已展开。 */
bool ui_power_sheet_top_is_open(void);
bool ui_power_sheet_bottom_is_open(void);

void ui_power_sheet_set_top_open(bool open);
void ui_power_sheet_set_bottom_open(bool open);

/** 屏幕高度（底部全屏面板动画用）。 */
lv_coord_t ui_power_sheet_screen_h(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_POWER_SHEET_H */
