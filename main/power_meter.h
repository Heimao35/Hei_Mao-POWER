/**
 * @file power_meter.h
 * @brief 功率计测量接口（INA236：电压/电流/功率，ALERT 自动量程）。
 */
#ifndef POWER_METER_H
#define POWER_METER_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef POWER_METER_ALERT_GPIO
#define POWER_METER_ALERT_GPIO  GPIO_NUM_14
#endif

/** 单次测量结果 */
typedef struct {
    float voltage_v;  /**< 母线电压 (V) */
    float current_a;  /**< 电流 (A) */
    float power_w;    /**< 功率 (W) */
    bool  range_fine; /**< true=±20.48mV 精细量程, false=±81.92mV */
    bool  overflow;   /**< 芯片数学溢出（量程可能不足） */
} power_meter_reading_t;

/** 初始化 INA236 并启用 GPIO ALERT 自动量程。 */
esp_err_t power_meter_init(i2c_port_t port);

/** 读取当前电压、电流、功率。 */
esp_err_t power_meter_read(power_meter_reading_t *out);

/** 芯片是否已成功初始化。 */
bool power_meter_is_ready(void);

/** 电流寄存器分辨率 (A)，即 1 LSB 对应电流。 */
float power_meter_current_resolution_a(void);

/** 按分辨率格式化为带单位的字符串（A / mA）。 */
void power_meter_format_current(float current_a, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* POWER_METER_H */
