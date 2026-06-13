/**
 * @file power_meter.h
 * @brief 功率计测量芯片 I2C 接口（电压/电流/功率），后续扩展具体驱动。
 */
#ifndef POWER_METER_H
#define POWER_METER_H

#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单次测量结果 */
typedef struct {
    float voltage_v; /**< 电压 (V) */
    float current_a; /**< 电流 (A) */
    float power_w;   /**< 功率 (W) */
} power_meter_reading_t;

/**
 * @brief 初始化测量芯片（I2C 从机地址与寄存器映射后续补充）。
 */
esp_err_t power_meter_init(i2c_port_t port);

/**
 * @brief 读取当前电压、电流、功率。
 */
esp_err_t power_meter_read(power_meter_reading_t *out);

#ifdef __cplusplus
}
#endif

#endif /* POWER_METER_H */
