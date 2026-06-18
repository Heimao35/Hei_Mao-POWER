/**
 * @file ui_boot_splash.h
 * @brief 开机品牌动画：Hei_Mao-POWER 启动画面。
 */
#ifndef UI_BOOT_SPLASH_H
#define UI_BOOT_SPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 播放开机动画（需先调用 ui_power_app_prepare_hidden）。
 * overlay 挂在主 screen 上，结束后向下滑出，与底部 PD 面板相同。
 */
void ui_boot_splash_play(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_BOOT_SPLASH_H */
