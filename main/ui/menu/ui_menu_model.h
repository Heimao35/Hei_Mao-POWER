/**
 * @file ui_menu_model.h
 * @brief Static menu tree: one main level and per-item submenus (extend by editing tables).
 */
#ifndef UI_MENU_MODEL_H
#define UI_MENU_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MENU_MAIN_COUNT 5
#define UI_MENU_SUB_COUNT   4

/** Indices into @ref ui_menu_model_get_table() and per-item `sub[]` (for navigation hooks). */
#define UI_MENU_MAIN_IDX_SYSTEM      0
#define UI_MENU_MAIN_IDX_DISPLAY     1
/** System submenu indices (see @ref ui_menu_model_get_table() System row). */
#define UI_MENU_SUB_IDX_SYSTEM_WIFI    0
#define UI_MENU_SUB_IDX_SYSTEM_RGB     3
/** Display submenu */
#define UI_MENU_SUB_IDX_BRIGHTNESS     0

typedef struct {
    const char *title;
    const char *sub[UI_MENU_SUB_COUNT];
} ui_menu_main_item_t;

/** Read-only menu definition (English labels for wide font coverage). */
const ui_menu_main_item_t *ui_menu_model_get_table(void);
uint32_t ui_menu_model_main_count(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_MODEL_H */
