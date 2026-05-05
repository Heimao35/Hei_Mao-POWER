/**
 * @file axp2101_battery.h
 * @brief AXP2101 fuel gauge / VBUS on shared I2C (mutex via @ref i2c_bus_share).
 */
#ifndef AXP2101_BATTERY_H
#define AXP2101_BATTERY_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Optional: called from IRQ worker after clearing PMU IRQ status (LVGL-safe: use @c lv_async_call inside). */
typedef void (*axp2101_battery_ui_notify_fn)(void);

esp_err_t axp2101_battery_init(i2c_port_t port, gpio_num_t irq_gpio);

/** Register callback invoked after IRQ servicing (typically schedules UI refresh). */
void axp2101_battery_set_ui_notify(axp2101_battery_ui_notify_fn fn);

esp_err_t axp2101_battery_read_soc(uint8_t *percent_0_100);

/** VBUS present (STATUS1 bit5, datasheet “VBUS good”). */
bool axp2101_battery_vbus_good(void);

/** Service IRQ status registers (W1C); call from dedicated task, not ISR. */
void axp2101_battery_service_irq_regs(void);

#ifdef __cplusplus
}
#endif

#endif /* AXP2101_BATTERY_H */
