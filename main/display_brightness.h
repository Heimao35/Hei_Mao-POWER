/**
 * @file display_brightness.h
 * @brief AMOLED write cmd 0x51 brightness + NVS persistence (UI 0–100%, HW min 0x2F).
 */
#ifndef DISPLAY_BRIGHTNESS_H
#define DISPLAY_BRIGHTNESS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum write value for reg 0x51 (avoid perceived black screen). */
#define DISPLAY_BRIGHTNESS_HW_MIN  0x2F
#define DISPLAY_BRIGHTNESS_HW_MAX  0xFF

void display_brightness_init(esp_lcd_panel_io_handle_t io, bool use_qspi_cmd_encoding);

/** Read NVS (or default), apply to panel; call after panel init, before UI. */
esp_err_t display_brightness_boot_apply(void);

/** Current UI percent 0–100 (after boot_apply / set). */
uint8_t display_brightness_get_ui_pct(void);

/** Live panel update from UI percent (maps to [HW_MIN, HW_MAX]). */
void display_brightness_apply_ui_pct(uint8_t pct_0_100);

/** Persist UI percent to NVS. */
esp_err_t display_brightness_save_ui_pct(uint8_t pct_0_100);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_BRIGHTNESS_H */
