/**
 * @file ui_menu_config.h
 * @brief Visual and interaction tuning for the wheel menu (easy to tweak).
 */
#ifndef UI_MENU_CONFIG_H
#define UI_MENU_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/** Width of the scroll strip (left side of the panel). */
#define UI_MENU_WHEEL_COL_WIDTH   280

/** Fixed row height for each menu entry (snapping unit). */
#define UI_MENU_WHEEL_ROW_HEIGHT  60

/** Vertical gap between rows (flex pad_row). */
#define UI_MENU_WHEEL_ROW_GAP     10

/** Row center within this distance (px) of snap line = highlight; else dim. */
#define UI_MENU_WHEEL_HIGHLIGHT_PX  36

/**
 * Extra scroll content padding at top/bottom for submenu wheels (header is floating over full-screen scroll).
 * Keeps snap center at full panel height while rows clear the title band.
 */
#define UI_MENU_SUBMENU_HEADER_RESERVE_PX  52

/** Slide transition duration (ms). */
#define UI_MENU_SLIDE_MS          320

/** Boot intro: main menu wheel slides in from the left (px). */
#define UI_MENU_BOOT_WHEEL_SLIDE_PX  200
/** Boot intro: clock root slides in from the right (px). */
#define UI_MENU_BOOT_CLOCK_SLIDE_PX  180
/** Boot intro: status strip slides down from above (px). */
#define UI_MENU_BOOT_STATUS_SLIDE_PX 56
/** Boot intro duration (ms); wheel, clock, and status run in parallel. */
#define UI_MENU_BOOT_ANIM_MS         480

/** Minimum horizontal drag (px) to trigger "back" on submenu. */
#define UI_MENU_BACK_SWIPE_MIN_PX 50

/** Horizontal must exceed vertical * this ratio to count as back swipe. */
#define UI_MENU_BACK_SWIPE_RATIO  120

#ifdef __cplusplus
}
#endif

#endif /* UI_MENU_CONFIG_H */
