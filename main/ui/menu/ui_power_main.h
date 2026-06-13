/**
 * @file ui_power_main.h
 * @brief 主界面：数值式与折线图式两种显示模式。
 */
#ifndef UI_POWER_MAIN_H
#define UI_POWER_MAIN_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_POWER_VIEW_NUMERIC = 0,
    UI_POWER_VIEW_CHART,
    UI_POWER_VIEW_COUNT
} ui_power_view_mode_t;

/** 在父容器上创建主显示区（含两种视图），默认显示数值式。 */
void ui_power_main_create(lv_obj_t *parent);

/** 获取当前视图模式。 */
ui_power_view_mode_t ui_power_main_get_mode(void);

/** 切换到指定视图（无动画，由 app 层负责滑动动画时可只改数据）。 */
void ui_power_main_set_mode(ui_power_view_mode_t mode);

/** 动画结束后复位视图坐标，避免残留绘制。 */
void ui_power_main_reset_views(void);

/** 获取数值视图与折线图视图根对象（用于横向滑动动画）。 */
lv_obj_t *ui_power_main_get_numeric_view(void);
lv_obj_t *ui_power_main_get_chart_view(void);

/** 定时刷新测量数据（由 app 层 timer 调用）。 */
void ui_power_main_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_POWER_MAIN_H */
