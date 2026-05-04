#include "pmic_axp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"

static const char *TAG = "AXP2101";

// 启用充电功能（设置REG18H[1]）
static void PMIC_EnableCharging(i2c_port_t i2c_num) {
    uint8_t reg_addr = 0x18;
    uint8_t current_value;
    
    // 先读取当前寄存器值
    esp_err_t err = i2c_master_write_read_device(
        i2c_num, 0x34, &reg_addr, 1, &current_value, 1, pdMS_TO_TICKS(1000)
    );
    
    if (err == ESP_OK) {
        current_value |= (1 << 1);  // 设置bit1为1（充电使能）
        uint8_t tx_buf[2] = {reg_addr, current_value};
        err = i2c_master_write_to_device(i2c_num, 0x34, tx_buf, 2, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Charging enabled");
    } else {
        ESP_LOGE(TAG, "Failed to enable charging: %d", err);
    }
}

// 设置快充电流为300mA（配置REG62H[4:0] = 0b01001）
static void PMIC_SetFastChargeCurrent(i2c_port_t i2c_num) {
    uint8_t tx_buf[2] = {0x62, 0x09};  // 0x09对应300mA
    esp_err_t err = i2c_master_write_to_device(i2c_num, 0x34, tx_buf, 2, pdMS_TO_TICKS(1000));
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Charge current set to 300mA");
    } else {
        ESP_LOGE(TAG, "Failed to set current: %d", err);
    }
}

void PMIC_GetBatteryLevel(void *arg) {
    i2c_port_t i2c_num = (i2c_port_t)(int)arg;
    ESP_LOGI(TAG, "Starting PMIC task");
    uint8_t tx_buffer[1];
    uint8_t rx_buffer[1];
    bool is_fast_charge_enabled = false;
    PMIC_EnableCharging(i2c_num);

    while (1) {
        tx_buffer[0] = 0xA4;
        esp_err_t err = i2c_master_write_read_device(
            i2c_num, 0x34, tx_buffer, 1, rx_buffer, 1, pdMS_TO_TICKS(1000)
        );

        if (err == ESP_OK) {
            uint8_t battery_level = rx_buffer[0];
            ESP_LOGI(TAG, "Battery Level: %d%%", battery_level);
            if (battery_level < 80) {
                if (!is_fast_charge_enabled) {
                    PMIC_SetFastChargeCurrent(i2c_num);
                    is_fast_charge_enabled = true;
                }
            } else {
                // 电量≥80%时可选恢复默认电流（按需实现）
                //PMIC_SetChargeCurrent(i2c_num, DEFAULT_VALUE);
                is_fast_charge_enabled = false;
            }
        } else {
            ESP_LOGE(TAG, "Read battery failed: %d", err);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}