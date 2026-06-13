/**
 * @file power_meter.c
 * @brief 功率计测量芯片占位实现，后续接入真实 I2C 驱动。
 */
#include "power_meter.h"

#include "esp_log.h"

static const char *TAG = "power_meter";
static i2c_port_t s_port = I2C_NUM_0;
static bool       s_inited;

esp_err_t power_meter_init(i2c_port_t port)
{
    s_port   = port;
    s_inited = true;
    ESP_LOGI(TAG, "占位初始化完成 (port=%d)，待接入测量芯片驱动", (int)port);
    return ESP_OK;
}

esp_err_t power_meter_read(power_meter_reading_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 占位：后续通过 I2C 读取真实寄存器 */
    (void)s_port;
    out->voltage_v = 0.0f;
    out->current_a = 0.0f;
    out->power_w   = 0.0f;
    return ESP_OK;
}
