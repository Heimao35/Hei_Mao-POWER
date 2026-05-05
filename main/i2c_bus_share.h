/**
 * @file i2c_bus_share.h
 * @brief Single mutex for I2C_NUM_0 (touch, RTC, PMIC) — take before any multi-byte transfer.
 */
#ifndef I2C_BUS_SHARE_H
#define I2C_BUS_SHARE_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Create mutex; call once after @c i2c_driver_install on that port. */
void i2c_bus_share_init(void);

bool i2c_bus_share_lock(TickType_t timeout_ticks);
void i2c_bus_share_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* I2C_BUS_SHARE_H */
