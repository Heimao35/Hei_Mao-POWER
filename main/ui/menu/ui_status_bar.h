/**
 * @file ui_status_bar.h
 * @brief 右上角状态栏（WiFi / MQTT / PD 连接指示）。
 */
#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_status_bar_init(lv_obj_t *screen);
/** 将状态栏标签置于 screen 最前，避免被全屏 overlay 遮挡。 */
void ui_status_bar_raise_to_front(void);
void ui_status_bar_sync_wifi(void);
void ui_status_bar_sync_mqtt(bool connected);
void ui_status_bar_sync_pd(bool enabled);
/** @param on_ready 滑入动画结束回调（可为 NULL，仅调用一次） */
void ui_status_bar_boot_slide_in(void (*on_ready)(void));

#ifdef __cplusplus
}
#endif

#endif /* UI_STATUS_BAR_H */
