/**
 * @file ui_menu_wheel.h
 * @brief Vertical snap wheel: left column, center snap, distance-based zoom and color.
 */
#ifndef UI_MENU_WHEEL_H
#define UI_MENU_WHEEL_H

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_menu_wheel ui_menu_wheel_t;

/**
 * Create a wheel inside @p parent (typically a full-screen panel).
 * @param items  array of C strings; must remain valid for wheel lifetime
 * @param count  number of items
 * @param content_top_reserve  extra flex padding top+bottom (e.g. UI_MENU_SUBMENU_HEADER_RESERVE_PX); use 0 on main menu
 */
ui_menu_wheel_t *ui_menu_wheel_create(lv_obj_t *parent, const char *const *items, uint32_t count,
                                       lv_coord_t content_top_reserve);

void ui_menu_wheel_delete(ui_menu_wheel_t *wheel);

/** Index of the row whose center is closest to the screen vertical center. */
uint32_t ui_menu_wheel_get_selected_index(const ui_menu_wheel_t *wheel);

lv_obj_t *ui_menu_wheel_get_root(const ui_menu_wheel_t *wheel);

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_WHEEL_H */
