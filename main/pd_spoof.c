/**
 * @file pd_spoof.c
 * @brief PD 诱骗芯片占位实现，后续接入真实 I2C 驱动。
 */
#include "pd_spoof.h"

#include "esp_log.h"

static const char *TAG = "pd_spoof";
static bool s_inited;

esp_err_t pd_spoof_init(i2c_port_t port)
{
    (void)port;
    s_inited = true;
    ESP_LOGI(TAG, "占位初始化完成，待接入 PD 诱骗芯片驱动");
    return ESP_OK;
}
