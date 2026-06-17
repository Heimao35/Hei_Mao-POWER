/**
 * @file audio_volume.c
 * @brief Mute 开关状态 NVS 持久化（开启 = 蜂鸣器启用）。
 */
#include "audio_volume.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "audio_vol"
#define NVS_NAMESPACE   "audio"
#define NVS_KEY_BUZZER  "bz_en"
#define NVS_KEY_MUTED   "muted"

static bool s_buzzer_enabled;

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

void audio_volume_init(void)
{
    s_buzzer_enabled = false;
    if (nvs_init_once() != ESP_OK) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, NVS_KEY_BUZZER, &v);
    if (err == ESP_OK) {
        s_buzzer_enabled = (v != 0);
    } else {
        err = nvs_get_u8(h, NVS_KEY_MUTED, &v);
        if (err == ESP_OK) {
            /* 旧版 muted=1 表示静音；新版开关开启才响 */
            s_buzzer_enabled = (v == 0);
        }
    }
    nvs_close(h);
}

bool audio_volume_buzzer_enabled(void)
{
    return s_buzzer_enabled;
}

void audio_volume_set_buzzer_enabled(bool enabled)
{
    s_buzzer_enabled = enabled;
}

esp_err_t audio_volume_save_buzzer_enabled(bool enabled)
{
    s_buzzer_enabled = enabled;
    if (nvs_init_once() != ESP_OK) {
        return ESP_FAIL;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_BUZZER, enabled ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
