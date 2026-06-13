/**
 * @file pd_spoof.h
 * @brief PD 诱骗芯片 I2C 控制接口，后续扩展具体协议。
 */
#ifndef PD_SPOOF_H
#define PD_SPOOF_H

#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 PD 诱骗芯片。
 */
esp_err_t pd_spoof_init(i2c_port_t port);

#ifdef __cplusplus
}
#endif

#endif /* PD_SPOOF_H */
