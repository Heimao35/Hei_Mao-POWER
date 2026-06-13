/**
 * @file ui_power_config.h
 * @brief 功率计 UI 手势与动画参数。
 */
#ifndef UI_POWER_CONFIG_H
#define UI_POWER_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/** 滑动动画时长 (ms)。 */
#define UI_POWER_SLIDE_MS           320

/** 启动时状态栏滑入动画 (ms)。 */
#define UI_POWER_BOOT_ANIM_MS         480
#define UI_POWER_BOOT_STATUS_SLIDE_PX 56
#define UI_POWER_BOOT_CLOCK_SLIDE_PX  180

/** 触发返回/切换的最小滑动距离 (px)。 */
#define UI_POWER_SWIPE_MIN_PX       50

/** 主方向滑动需超过副方向 * 此比例 / 100。 */
#define UI_POWER_SWIPE_RATIO        120

/** 顶部设置菜单展开高度 (px)。 */
#define UI_POWER_TOP_SHEET_H        180

/** 顶部/底部面板背景色（与主界面 0x0f172a 区分）。 */
#define UI_POWER_TOP_SHEET_BG       0x1a2d4d
#define UI_POWER_SHEET_BG           UI_POWER_TOP_SHEET_BG

/** 主界面背景色 */
#define UI_POWER_MAIN_BG            0x0f172a

#ifdef __cplusplus
}
#endif

#endif /* UI_POWER_CONFIG_H */
