/**
 * @file ui_status_bar.h
 * @brief Top-right status strip (Wi-Fi indicator; room reserved for battery).
 */
#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create floating widgets on @p screen (call after screen exists, before load_scr). */
void ui_status_bar_init(lv_obj_t *screen);

/** Refresh Wi-Fi icon from current STA IP state (e.g. after UI-driven connect). */
void ui_status_bar_sync_wifi(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_STATUS_BAR_H */
