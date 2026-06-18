/**
 * @file ui_power_app.h
 * @brief 功率计 UI 入口：主界面、手势导航、设置子页。
 */
#ifndef UI_POWER_APP_H
#define UI_POWER_APP_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 构建功率计 UI 并加载为当前屏幕。 */
void ui_power_app_init(void);

/** 构建主界面并保持内容区透明（开机动画过渡用）。 */
void ui_power_app_prepare_hidden(void);

/** 主界面内容淡入（开机动画结束后调用）。 */
void ui_power_app_fade_in(void);

/** 当前主界面 screen（开机动画过渡挂载 overlay 用）。 */
lv_obj_t *ui_power_app_get_screen(void);

/** 当前主界面 stage（与主 panel 同级，用于 overlay 滑动）。 */
lv_obj_t *ui_power_app_get_stage(void);

/**
 * 将全屏 overlay 向下滑出（与设置页/子页相同的 start_slide_y 逻辑）。
 * @p on_done 在 LVGL 线程回调，可为 NULL。
 */
void ui_power_app_slide_overlay_down(lv_obj_t *overlay, void (*on_done)(void));

#ifdef __cplusplus
}
#endif

#endif /* UI_POWER_APP_H */
