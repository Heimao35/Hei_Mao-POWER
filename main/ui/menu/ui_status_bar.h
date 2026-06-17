/**
 * @file ui_status_bar.h
 * @brief 右上角状态栏（WiFi 连接指示）。
 */
#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_status_bar_init(lv_obj_t *screen);
void ui_status_bar_sync_wifi(void);
void ui_status_bar_sync_pd(bool enabled);
void ui_status_bar_boot_slide_in(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_STATUS_BAR_H */
