/**
 * @file ui_pd_panel.h
 * @brief PD 诱骗控制面板 UI。
 */
#ifndef UI_PD_PANEL_H
#define UI_PD_PANEL_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_pd_panel_create(lv_obj_t *parent);
void ui_pd_panel_refresh(void);

/** 同步开关/高亮状态（外部事件回调调用，需在 LVGL 线程）。 */
void ui_pd_panel_sync_from_driver(void);

/** 同步开关状态；animate_switch 为 true 时播放滑块动画（如 GPIO0 触发）。 */
void ui_pd_panel_sync_from_driver_ex(bool animate_switch);

/** 显示物理按键触发的状态横幅（1 秒后自动收起）。 */
void ui_pd_panel_show_toggle_toast(bool enabled);

/** 显示与 PD 横幅相同样式的自定义消息（1 秒后自动收起）。 */
void ui_pd_panel_show_message_toast(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* UI_PD_PANEL_H */
