/**
 * @file axp2101_battery.c
 */
#include "axp2101_battery.h"
#include "i2c_bus_share.h"
#include "pmic_axp.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "axp2101";

#define AXP2101_ADDR7  AXP2101_SLAVE_ADDRESS

static i2c_port_t                   s_port = I2C_NUM_MAX;
static gpio_num_t                   s_irq_gpio = GPIO_NUM_NC;
static SemaphoreHandle_t            s_irq_sem;
static axp2101_battery_ui_notify_fn s_notify;
static bool                         s_inited;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(s_port, AXP2101_ADDR7, &reg, 1U, val, 1U, pdMS_TO_TICKS(80));
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(s_port, AXP2101_ADDR7, buf, sizeof(buf), pdMS_TO_TICKS(80));
}

static esp_err_t reg_rmw(uint8_t reg, uint8_t mask, uint8_t set)
{
    uint8_t v = 0;
    esp_err_t e = reg_read(reg, &v);
    if (e != ESP_OK) {
        return e;
    }
    v = (uint8_t)((v & (uint8_t)~mask) | (uint8_t)(set & mask));
    return reg_write(reg, v);
}

static void axp_irq_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_irq_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        axp2101_battery_service_irq_regs();
        if (s_notify) {
            s_notify();
        }
    }
}

static void IRAM_ATTR axp_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    if (s_irq_sem) {
        xSemaphoreGiveFromISR(s_irq_sem, &hp);
    }
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

void axp2101_battery_set_ui_notify(axp2101_battery_ui_notify_fn fn)
{
    s_notify = fn;
}

void axp2101_battery_service_irq_regs(void)
{
    if (s_port >= I2C_NUM_MAX) {
        return;
    }
    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return;
    }

    for (unsigned i = 0; i < XPOWERS_AXP2101_INTSTS_CNT; i++) {
        const uint8_t reg = (uint8_t)(XPOWERS_AXP2101_INTSTS1 + i);
        uint8_t st = 0;
        if (reg_read(reg, &st) != ESP_OK || st == 0U) {
            continue;
        }
        (void)reg_write(reg, st);
    }

    i2c_bus_share_unlock();
}

esp_err_t axp2101_battery_read_soc(uint8_t *percent_0_100)
{
    if (!percent_0_100 || s_port >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t reg = XPOWERS_AXP2101_BAT_PERCENT_DATA;
    uint8_t pct = 0;
    esp_err_t err = i2c_master_write_read_device(s_port, AXP2101_ADDR7, &reg, 1U, &pct, 1U, pdMS_TO_TICKS(80));

    i2c_bus_share_unlock();

    if (err != ESP_OK) {
        return err;
    }
    if (pct > 100U) {
        pct = 100U;
    }
    *percent_0_100 = pct;
    return ESP_OK;
}

bool axp2101_battery_vbus_good(void)
{
    if (s_port >= I2C_NUM_MAX) {
        return false;
    }
    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return false;
    }
    uint8_t st1 = 0;
    const esp_err_t e = reg_read(XPOWERS_AXP2101_STATUS1, &st1);
    i2c_bus_share_unlock();
    if (e != ESP_OK) {
        return false;
    }
    return (st1 & XPOWERS_AXP2101_STATUS1_VBUS_GOOD) != 0U;
}

esp_err_t axp2101_battery_init(i2c_port_t port, gpio_num_t irq_gpio)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (port >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_irq_gpio = irq_gpio;

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t       chip_id = 0;
    const uint8_t reg_id  = XPOWERS_AXP2101_IC_TYPE;
    esp_err_t     err     = i2c_master_write_read_device(port, AXP2101_ADDR7, &reg_id, 1U, &chip_id, 1U, pdMS_TO_TICKS(80));
    if (err != ESP_OK || chip_id != XPOWERS_AXP2101_CHIP_ID) {
        i2c_bus_share_unlock();
        ESP_LOGW(TAG, "AXP2101 probe failed (reg03=%02X, err=%s)", chip_id, esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;
    }

    s_port = port;

    /* Fuel gauge on (reg18[3]); cell charge on (reg18[1]) for typical setups */
    (void)reg_rmw(XPOWERS_AXP2101_CHARGE_GAUGE_WDT_CTRL, (uint8_t)(1U << 3), (uint8_t)(1U << 3));
    (void)reg_rmw(XPOWERS_AXP2101_CHARGE_GAUGE_WDT_CTRL, (uint8_t)(1U << 1), (uint8_t)(1U << 1));

    /* VBUS insert / remove IRQ enable (reg41 bit7, bit6) */
    uint8_t en2 = 0;
    if (reg_read(XPOWERS_AXP2101_INTEN2, &en2) == ESP_OK) {
        en2 = (uint8_t)(en2 | (1U << 7) | (1U << 6));
        (void)reg_write(XPOWERS_AXP2101_INTEN2, en2);
    }

    /* Clear stale IRQ status */
    for (unsigned i = 0; i < XPOWERS_AXP2101_INTSTS_CNT; i++) {
        const uint8_t reg = (uint8_t)(XPOWERS_AXP2101_INTSTS1 + i);
        uint8_t st = 0;
        if (reg_read(reg, &st) == ESP_OK && st != 0U) {
            (void)reg_write(reg, st);
        }
    }

    i2c_bus_share_unlock();

    if (irq_gpio != GPIO_NUM_NC) {
        s_irq_sem = xSemaphoreCreateBinary();
        if (!s_irq_sem) {
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(axp_irq_task, "axp_irq", 3072, NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "IRQ task create failed");
        } else {
            gpio_config_t io = {
                .pin_bit_mask = (1ULL << (unsigned)irq_gpio),
                .mode         = GPIO_MODE_INPUT,
                .pull_up_en   = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_NEGEDGE,
            };
            esp_err_t ge = gpio_config(&io);
            if (ge != ESP_OK) {
                ESP_LOGW(TAG, "IRQ GPIO config failed: %s", esp_err_to_name(ge));
            } else {
                ge = gpio_isr_handler_add(irq_gpio, axp_irq_isr, NULL);
                if (ge != ESP_OK) {
                    ESP_LOGW(TAG, "IRQ handler add failed: %s", esp_err_to_name(ge));
                }
            }
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "AXP2101 ready (fuel gauge + VBUS IRQ)");
    return ESP_OK;
}
