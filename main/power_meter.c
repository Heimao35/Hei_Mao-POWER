/**
 * @file power_meter.c
 * @brief INA236 功率计：测量读取与 ALERT 中断自动量程切换。
 */
#include "power_meter.h"

#include "ina236.h"
#include "i2c_bus_share.h"
#include "buzzer.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>

static const char *TAG = "power_meter";

#ifndef POWER_METER_ALERT_GPIO
#define POWER_METER_ALERT_GPIO  GPIO_NUM_14
#endif

/** 精细量程 (±20.48mV) 上切阈值：约 80% 满量程 */
#define RANGE_UP_SHUNT_V    0.016f
/** 粗量程 (±81.92mV) 下切阈值（带迟滞） */
#define RANGE_DOWN_SHUNT_V  0.004f

static ina236_dev_t      s_ina;
static bool              s_inited;
static SemaphoreHandle_t s_alert_sem;
static TaskHandle_t      s_alert_task;

static esp_err_t apply_range_alerts(ina236_range_t range)
{
    esp_err_t err;
    if (range == INA236_RANGE_FINE) {
        const int16_t limit = ina236_shunt_v_to_limit(INA236_RANGE_FINE, RANGE_UP_SHUNT_V);
        err = ina236_config_alert(&s_ina, INA236_ALERT_SOL, limit);
        ESP_LOGI(TAG, "量程 ±20.48mV，SOL 阈值 %.1f mV (raw=%d)",
                 (double)(RANGE_UP_SHUNT_V * 1000.0f), (int)limit);
    } else {
        const int16_t limit = ina236_shunt_v_to_limit(INA236_RANGE_COARSE, RANGE_DOWN_SHUNT_V);
        err = ina236_config_alert(&s_ina, INA236_ALERT_SUL, limit);
        ESP_LOGI(TAG, "量程 ±81.92mV，SUL 阈值 %.1f mV (raw=%d)",
                 (double)(RANGE_DOWN_SHUNT_V * 1000.0f), (int)limit);
    }
    return err;
}

static esp_err_t switch_range(ina236_range_t range)
{
    const ina236_range_t prev = s_ina.range;

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ina236_set_range(&s_ina, range);
    if (err == ESP_OK) {
        err = apply_range_alerts(range);
    }

    uint16_t mask = 0;
    (void)ina236_read_mask_enable(&s_ina, &mask);
    (void)mask;

    i2c_bus_share_unlock();

    if (err == ESP_OK && s_inited && prev != range) {
        if (range == INA236_RANGE_COARSE) {
            buzzer_play_pattern(BUZZER_PATTERN_RANGE_UP);
        } else {
            buzzer_play_pattern(BUZZER_PATTERN_RANGE_DOWN);
        }
    }
    return err;
}

static void IRAM_ATTR alert_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t wake = pdFALSE;
    if (s_alert_sem) {
        xSemaphoreGiveFromISR(s_alert_sem, &wake);
    }
    if (wake) {
        portYIELD_FROM_ISR();
    }
}

static void alert_worker_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_alert_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
            continue;
        }

        uint16_t mask = 0;
        if (ina236_read_mask_enable(&s_ina, &mask) != ESP_OK) {
            i2c_bus_share_unlock();
            continue;
        }

        const bool aff = (mask & (1u << 4)) != 0;
        const bool sol = (mask & (1u << 15)) != 0;
        const bool sul = (mask & (1u << 14)) != 0;
        ina236_range_t cur = s_ina.range;
        i2c_bus_share_unlock();

        if (!aff) {
            continue;
        }

        if (cur == INA236_RANGE_FINE && sol) {
            ESP_LOGI(TAG, "ALERT: 分流过压，切换至 ±81.92mV 粗量程");
            (void)switch_range(INA236_RANGE_COARSE);
        } else if (cur == INA236_RANGE_COARSE && sul) {
            ESP_LOGI(TAG, "ALERT: 分流欠压，切换至 ±20.48mV 精细量程");
            (void)switch_range(INA236_RANGE_FINE);
        }
    }
}

esp_err_t power_meter_init(i2c_port_t port)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err = ina236_init(&s_ina, port, POWER_METER_ALERT_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "INA236 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = switch_range(INA236_RANGE_FINE);
    if (err != ESP_OK) {
        return err;
    }

    s_alert_sem = xSemaphoreCreateBinary();
    if (!s_alert_sem) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t isr_svc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_svc != ESP_OK && isr_svc != ESP_ERR_INVALID_STATE) {
        return isr_svc;
    }
    gpio_isr_handler_add(POWER_METER_ALERT_GPIO, alert_isr_handler, NULL);
    gpio_set_intr_type(POWER_METER_ALERT_GPIO, GPIO_INTR_NEGEDGE);

    BaseType_t ok = xTaskCreate(alert_worker_task, "ina_alert", 3072, NULL, 5, &s_alert_task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    const float lsb = ina236_get_current_lsb(&s_ina);
    const float shunt_adc_lsb_a = 625e-9f / INA236_RSHUNT_OHM;
    const float offset_a = INA236_SHUNT_OFFSET_V_MAX / INA236_RSHUNT_OHM;
    ESP_LOGI(TAG, "功率计就绪 ALERT=GPIO%d | 电流分辨率 %.1f mA | 分流ADC %.1f mA | 零点误差约 ±%.1f mA",
             (int)POWER_METER_ALERT_GPIO, (double)(lsb * 1000.0f), (double)(shunt_adc_lsb_a * 1000.0f),
             (double)(offset_a * 1000.0f));
    return ESP_OK;
}

esp_err_t power_meter_read(power_meter_reading_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || !s_ina.present) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    ina236_reading_t raw = {0};
    esp_err_t err = ina236_read(&s_ina, &raw);
    i2c_bus_share_unlock();
    if (err != ESP_OK) {
        return err;
    }

    out->voltage_v  = raw.bus_v;
    out->current_a  = raw.current_a;
    out->power_w    = raw.power_w;
    out->range_fine = (raw.range == INA236_RANGE_FINE);
    out->overflow   = raw.overflow;

    /* 溢出时软件侧也尝试升档（ALERT 可能尚未触发） */
    if (raw.overflow && raw.range == INA236_RANGE_FINE) {
        (void)switch_range(INA236_RANGE_COARSE);
    }

    return ESP_OK;
}

bool power_meter_is_ready(void)
{
    return s_inited && s_ina.present;
}

float power_meter_current_resolution_a(void)
{
    return ina236_get_current_lsb(&s_ina);
}

void power_meter_format_current(float current_a, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    const float lsb = power_meter_current_resolution_a();
    const float abs_a = fabsf(current_a);

    if (abs_a >= 1.0f) {
        (void)snprintf(buf, buf_len, "%.2f A", (double)current_a);
    } else if (abs_a >= 0.001f) {
        (void)snprintf(buf, buf_len, "%.1f mA", (double)(current_a * 1000.0f));
    } else if (lsb > 0.0f && abs_a >= lsb * 0.5f) {
        const double ua = (double)(current_a * 1e6f);
        if (lsb < 0.0001f) {
            (void)snprintf(buf, buf_len, "%.1f uA", ua);
        } else {
            (void)snprintf(buf, buf_len, "%.0f uA", ua);
        }
    } else {
        (void)snprintf(buf, buf_len, "0 uA");
    }
}
