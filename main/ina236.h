/**
 * @file ina236.h
 * @brief TI INA236 电流/电压/功率监测芯片底层 I2C 寄存器驱动。
 */
#ifndef INA236_H
#define INA236_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** INA236A：A0 引脚接 GND / VS / SDA / SCL 时的 7 位 I2C 地址 */
#define INA236_ADDR_A0_GND_A  0x40
#define INA236_ADDR_A0_VS_A   0x41
#define INA236_ADDR_A0_SDA_A  0x42
#define INA236_ADDR_A0_SCL_A  0x43

/** INA236B：A0 引脚接 GND / VS / SDA / SCL 时的 7 位 I2C 地址 */
#define INA236_ADDR_A0_GND_B  0x48
#define INA236_ADDR_A0_VS_B   0x49
#define INA236_ADDR_A0_SDA_B  0x4A
#define INA236_ADDR_A0_SCL_B  0x4B

/** 分流 ADC 零点偏移上限 (V)，数据手册 ±5 µV */
#define INA236_SHUNT_OFFSET_V_MAX  5.0e-6f

typedef enum {
    INA236_RANGE_COARSE = 0, /**< ADCRANGE=0, ±81.92 mV */
    INA236_RANGE_FINE   = 1, /**< ADCRANGE=1, ±20.48 mV */
} ina236_range_t;

typedef enum {
    INA236_ALERT_SOL = 0,
    INA236_ALERT_SUL,
    INA236_ALERT_BOL,
    INA236_ALERT_BUL,
    INA236_ALERT_POL,
} ina236_alert_func_t;

typedef struct {
    float shunt_v;   /**< 分流电压 (V) */
    float bus_v;     /**< 母线电压 (V) */
    float current_a; /**< 电流 (A)，由 shunt_v / RSHUNT 换算 */
    float power_w;   /**< 功率 (W)，由 current_a × bus_v 换算 */
    bool  overflow;  /**< 数学溢出标志 */
    ina236_range_t range;
} ina236_reading_t;

typedef struct {
    i2c_port_t     i2c_port;
    uint8_t        i2c_addr;
    gpio_num_t     alert_gpio;
    ina236_range_t range;
    float          rshunt_ohm;
    float          imax_a;
    float          current_lsb;
    uint16_t       shunt_cal;
    bool           present;
} ina236_dev_t;

/** 按候选地址表探测并初始化 INA236（校验 Manufacturer / Device ID）。 */
esp_err_t ina236_init(ina236_dev_t *dev, i2c_port_t port, gpio_num_t alert_gpio,
                      const uint8_t *addr_candidates, size_t addr_count,
                      float rshunt_ohm, float imax_a);

/** 返回 SHUNT_CAL 使用的 CURRENT 寄存器 LSB (A)。 */
float ina236_get_current_lsb(const ina236_dev_t *dev);

/** 分流 ADC 换算的电流分辨率 (A)：shunt_voltage_lsb / RSHUNT。 */
float ina236_shunt_current_resolution_a(ina236_range_t range, float rshunt_ohm);

/**
 * @brief 设置分流 ADC 量程并更新校准寄存器。
 */
esp_err_t ina236_set_range(ina236_dev_t *dev, ina236_range_t range);

/**
 * @brief 配置 ALERT 引脚比较功能（单一路径，SOL/SUL/BOL/BUL/POL 互斥）。
 */
esp_err_t ina236_config_alert(ina236_dev_t *dev, ina236_alert_func_t func, int16_t limit_raw);

/** 读取 Mask/Enable 并清除转换就绪/报警锁存（读寄存器 0x06）。 */
esp_err_t ina236_read_mask_enable(ina236_dev_t *dev, uint16_t *mask_enable);

/** 读取电压、电流、功率。 */
esp_err_t ina236_read(ina236_dev_t *dev, ina236_reading_t *out);

/** 将分流电压 (V) 转为 Alert Limit 寄存器原始值。 */
int16_t ina236_shunt_v_to_limit(ina236_range_t range, float shunt_v);

#ifdef __cplusplus
}
#endif

#endif /* INA236_H */
