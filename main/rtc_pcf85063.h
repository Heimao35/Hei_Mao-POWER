/**
 * @file rtc_pcf85063.h
 * @brief PCF85063TP (NXP) I2C RTC — shared-bus safe (per-device mutex around transfers).
 *
 * Pins (this board): SDA/SCL on the existing touch I2C bus; INT is optional (input, open-drain from chip).
 *
 * I2C (ESP-IDF uses 7-bit address in API):
 * - Slave 7-bit address: 0x51 (datasheet fixed pattern 1010001b).
 * - First wire byte when writing: 0xA2 = (0x51 << 1) | 0; when reading: 0xA3 = (0x51 << 1) | 1.
 * - Register map used here: 0x00 Control_1, 0x01 Control_2, 0x04..0x0A time/date (burst from Seconds).
 *
 * OS bit: POR default has OS=1 in Seconds (Table 8) until cleared by a write with OS=0; init clears that case.
 */
#ifndef RTC_PCF85063_H
#define RTC_PCF85063_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize driver: I2C port must already be installed. Configures INT as input with pull-up.
 * Registers hooks with @c app_time when probe succeeds.
 */
esp_err_t rtc_pcf85063_init(i2c_port_t i2c_port, gpio_num_t int_gpio);

/** @return true if the last @ref rtc_pcf85063_init probe succeeded. */
bool rtc_pcf85063_present(void);

/**
 * Read civil time as stored in the RTC (matches local wall time last written).
 * Seconds use masked BCD (OS bit ignored for decode). Fails on I2C error or invalid BCD fields.
 */
esp_err_t rtc_pcf85063_read_local_tm(struct tm *out);

/** Write current libc local time (TZ) into the RTC; clears OS, uses STOP during burst write. */
esp_err_t rtc_pcf85063_write_system_local_time(void);

#ifdef __cplusplus
}
#endif

#endif /* RTC_PCF85063_H */
