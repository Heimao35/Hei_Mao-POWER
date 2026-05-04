/**
 * @file ui_menu_brightness.h
 * @brief Full-screen brightness page (vertical slider, 0–100 label).
 */
#ifndef UI_MENU_BRIGHTNESS_H
#define UI_MENU_BRIGHTNESS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build UI on @p panel (themed full-screen panel). Registers slider value handlers. */
void ui_menu_brightness_populate(lv_obj_t *panel);

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_BRIGHTNESS_H */
