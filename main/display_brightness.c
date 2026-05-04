/**
 * @file display_brightness.c
 */
#include "display_brightness.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

#define TAG "disp_bri"

#define NVS_NAMESPACE "display"
#define NVS_KEY_UI_PCT "b_pct"

#define LCD_OPCODE_WRITE_CMD_QSPI  (0x02ULL)

static esp_lcd_panel_io_handle_t s_io;
static bool s_qspi;
static uint8_t s_ui_pct = 75;

static esp_err_t tx_cmd51(uint8_t v)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    int lcd_cmd = 0x51;
    if (s_qspi) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= (int)(LCD_OPCODE_WRITE_CMD_QSPI << 24);
    }
    return esp_lcd_panel_io_tx_param(s_io, lcd_cmd, &v, 1);
}

static uint8_t ui_pct_to_hw(uint8_t pct)
{
    if (pct > 100U) {
        pct = 100U;
    }
    const uint32_t span = (uint32_t)(DISPLAY_BRIGHTNESS_HW_MAX - DISPLAY_BRIGHTNESS_HW_MIN);
    return (uint8_t)(DISPLAY_BRIGHTNESS_HW_MIN + (pct * span) / 100U);
}

static esp_err_t nvs_init_once(void)
{
    static bool done;
    if (done) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }
    done = true;
    return ESP_OK;
}

void display_brightness_init(esp_lcd_panel_io_handle_t io, bool use_qspi_cmd_encoding)
{
    s_io = io;
    s_qspi = use_qspi_cmd_encoding;
}

uint8_t display_brightness_get_ui_pct(void)
{
    return s_ui_pct;
}

void display_brightness_apply_ui_pct(uint8_t pct_0_100)
{
    if (pct_0_100 > 100U) {
        pct_0_100 = 100U;
    }
    s_ui_pct = pct_0_100;
    const uint8_t hw = ui_pct_to_hw(s_ui_pct);
    esp_err_t e = tx_cmd51(hw);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "0x51 write failed: %s", esp_err_to_name(e));
    }
}

esp_err_t display_brightness_save_ui_pct(uint8_t pct_0_100)
{
    if (pct_0_100 > 100U) {
        pct_0_100 = 100U;
    }
    ESP_RETURN_ON_ERROR(nvs_init_once(), TAG, "nvs init");
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_UI_PCT, pct_0_100);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t display_brightness_boot_apply(void)
{
    uint8_t v = 75;
    esp_err_t inm = nvs_init_once();
    if (inm == ESP_OK) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
        if (err == ESP_OK) {
            err = nvs_get_u8(h, NVS_KEY_UI_PCT, &v);
            nvs_close(h);
            if (err != ESP_OK || v > 100U) {
                v = 75;
            }
        }
    }
    display_brightness_apply_ui_pct(v);
    return inm;
}
