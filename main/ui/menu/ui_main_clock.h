/**
 * @file ui_main_clock.h
 * @brief Main-menu rolling digit clock (HH above / MM below screen center, right side).
 */
#ifndef UI_MAIN_CLOCK_H
#define UI_MAIN_CLOCK_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create clock on @p main_panel so it slides with the main menu. */
void ui_main_clock_create(lv_obj_t *main_panel);

/** Slide clock in from the right (after @ref ui_main_clock_create and layout). */
void ui_main_clock_boot_slide_in(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MAIN_CLOCK_H */
