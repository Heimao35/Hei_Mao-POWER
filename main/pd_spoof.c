/**
 * @file pd_spoof.c
 * @brief CH224Q I2C 驱动与 CC 通断控制。
 *
 * CC MOS：低电平接通、高电平断开（与外部下拉默认接通一致）。
 */
#include "pd_spoof.h"

#include "i2c_bus_share.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pd_spoof";

enum {
    REG_STATUS  = 0x09,
    REG_VOLTAGE = 0x0A,
    REG_CURRENT = 0x50,
};

#define CH224_ADDR_0  0x22
#define CH224_ADDR_1  0x23

#define CC_SETTLE_MS  50

/** 0x09: bit3=PD 握手（CH224Q/A 手册） */
#define STATUS_PD_ACTIVE  (1u << 3)

static i2c_port_t         s_port;
static uint8_t            s_i2c_addr;
static bool               s_inited;
static bool               s_enabled;
static pd_spoof_voltage_t s_selected = PD_SPOOF_VOLT_9V;
static pd_spoof_voltage_t s_configured;
static float              s_max_current_a;
static bool               s_pd_active;
static pd_spoof_event_cb_t s_evt_cb;
static void              *s_evt_ud;
static TaskHandle_t       s_btn_task;

static void emit_event(pd_spoof_event_id_t id, bool from_button)
{
    if (!s_evt_cb) {
        return;
    }
    pd_spoof_event_t evt = {
        .id          = id,
        .enabled     = s_enabled,
        .voltage     = s_selected,
        .from_button = from_button,
    };
    s_evt_cb(&evt, s_evt_ud);
}

static esp_err_t reg_write_u8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(s_port, s_i2c_addr, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t reg_read_u8(uint8_t reg, uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_write_read_device(s_port, s_i2c_addr, &reg, 1, val, 1, pdMS_TO_TICKS(50));
}

static esp_err_t probe_addr(uint8_t addr)
{
    uint8_t st = 0;
    return i2c_master_write_read_device(s_port, addr, (uint8_t[]){REG_STATUS}, 1, &st, 1,
                                        pdMS_TO_TICKS(50));
}

static uint8_t voltage_to_code(pd_spoof_voltage_t v)
{
    switch (v) {
    case PD_SPOOF_VOLT_9V:
        return 1;
    case PD_SPOOF_VOLT_12V:
        return 2;
    case PD_SPOOF_VOLT_20V:
        return 4;
    case PD_SPOOF_VOLT_28V:
        return 5;
    default:
        return 1;
    }
}

/** 低电平接通 CC，高电平断开 CC。 */
static void cc_set_connected(bool connect)
{
    gpio_set_level(PD_SPOOF_CC_GPIO, connect ? 0 : 1);
}

static bool cc_is_connected(void)
{
    return gpio_get_level(PD_SPOOF_CC_GPIO) == 0;
}

static esp_err_t chip_write_voltage(pd_spoof_voltage_t v)
{
    if (s_i2c_addr == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint8_t code = voltage_to_code(v);
    esp_err_t err = reg_write_u8(REG_VOLTAGE, code);
    if (err == ESP_OK) {
        s_configured = v;
    }
    return err;
}

static esp_err_t pd_write_voltage_locked(pd_spoof_voltage_t v)
{
    esp_err_t err = chip_write_voltage(v);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 0x0A 档位失败: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t pd_apply_enabled(bool enable)
{
    if (enable) {
        if (s_enabled) {
            return ESP_OK;
        }
        cc_set_connected(true);
        vTaskDelay(pdMS_TO_TICKS(CC_SETTLE_MS));

        if (s_i2c_addr != 0) {
            if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
                return ESP_ERR_TIMEOUT;
            }
            esp_err_t err = pd_write_voltage_locked(s_selected);
            i2c_bus_share_unlock();
            if (err != ESP_OK) {
                return err;
            }
        }
        s_enabled = true;
        ESP_LOGI(TAG, "PD 已启动 %dV", (int)s_selected);
    } else {
        if (!s_enabled) {
            return ESP_OK;
        }
        cc_set_connected(false);
        vTaskDelay(pdMS_TO_TICKS(CC_SETTLE_MS));
        s_enabled       = false;
        s_max_current_a = 0.0f;
        s_pd_active     = false;
        ESP_LOGI(TAG, "PD 已关闭，CC 已断开");
    }
    return ESP_OK;
}

static void btn_task(void *arg)
{
    (void)arg;
    bool last = gpio_get_level(PD_SPOOF_BTN_GPIO);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(25));
        const bool now = gpio_get_level(PD_SPOOF_BTN_GPIO);
        if (last && !now) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(PD_SPOOF_BTN_GPIO) == 0) {
                const bool target = !s_enabled;
                if (pd_apply_enabled(target) == ESP_OK) {
                    emit_event(PD_SPOOF_EVT_TOGGLED, true);
                }
                while (gpio_get_level(PD_SPOOF_BTN_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        last = now;
    }
}

esp_err_t pd_spoof_init(i2c_port_t port)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_port = port;

    gpio_config_t cc_io = {
        .pin_bit_mask = (1ULL << PD_SPOOF_CC_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cc_io));
    /* 外部下拉默认已接通 CC；保持低电平与硬件一致 */
    cc_set_connected(true);

    gpio_config_t pg_io = {
        .pin_bit_mask = (1ULL << PD_SPOOF_PG_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pg_io));

    gpio_config_t btn_io = {
        .pin_bit_mask = (1ULL << PD_SPOOF_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_io));

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = probe_addr(CH224_ADDR_0);
    if (err == ESP_OK) {
        s_i2c_addr = CH224_ADDR_0;
    } else {
        err = probe_addr(CH224_ADDR_1);
        if (err == ESP_OK) {
            s_i2c_addr = CH224_ADDR_1;
        }
    }
    if (s_i2c_addr != 0) {
        (void)pd_write_voltage_locked(s_selected);
    }
    i2c_bus_share_unlock();

    if (s_i2c_addr == 0) {
        ESP_LOGW(TAG, "未检测到 CH224 (0x22/0x23)，PD 功能受限");
    } else {
        ESP_LOGI(TAG, "CH224 @ 0x%02X，CC=GPIO%d(低通/高断) PG=GPIO%d", s_i2c_addr,
                 (int)PD_SPOOF_CC_GPIO, (int)PD_SPOOF_PG_GPIO);
    }

    if (!s_btn_task) {
        (void)xTaskCreate(btn_task, "pd_btn", 3072, NULL, 4, &s_btn_task);
    }

    s_inited  = true;
    s_enabled = true;
    return ESP_OK;
}

void pd_spoof_set_event_cb(pd_spoof_event_cb_t cb, void *user_data)
{
    s_evt_cb = cb;
    s_evt_ud = user_data;
}

esp_err_t pd_spoof_select_voltage(pd_spoof_voltage_t voltage)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    s_selected = voltage;

    if (s_i2c_addr == 0) {
        emit_event(PD_SPOOF_EVT_VOLTAGE, false);
        return ESP_ERR_NOT_FOUND;
    }

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = pd_write_voltage_locked(voltage);
    i2c_bus_share_unlock();

    if (err == ESP_OK) {
        emit_event(PD_SPOOF_EVT_VOLTAGE, false);
        ESP_LOGI(TAG, "档位已切换 %dV", (int)voltage);
    }
    return err;
}

esp_err_t pd_spoof_set_enabled(bool enable)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = pd_apply_enabled(enable);
    if (err == ESP_OK) {
        emit_event(PD_SPOOF_EVT_TOGGLED, false);
    }
    return err;
}

esp_err_t pd_spoof_toggle(bool from_button)
{
    (void)from_button;
    return pd_spoof_set_enabled(!s_enabled);
}

esp_err_t pd_spoof_get_status(pd_spoof_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    out->enabled            = s_enabled;
    out->cc_connected       = cc_is_connected();
    out->selected_voltage   = s_selected;
    out->configured_voltage = s_configured;
    out->pg_ok              = gpio_get_level(PD_SPOOF_PG_GPIO) == 0;
    out->chip_present       = s_i2c_addr != 0;
    out->max_current_a      = s_max_current_a;
    out->pd_active          = s_pd_active;
    return ESP_OK;
}

esp_err_t pd_spoof_poll(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pd_active     = false;
    s_max_current_a = 0.0f;

    if (s_i2c_addr == 0 || !s_enabled) {
        return ESP_OK;
    }

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t st = 0;
    uint8_t cur = 0;
    esp_err_t err = reg_read_u8(REG_STATUS, &st);
    if (err == ESP_OK) {
        s_pd_active = (st & STATUS_PD_ACTIVE) != 0;
        if (s_pd_active) {
            if (reg_read_u8(REG_CURRENT, &cur) == ESP_OK) {
                s_max_current_a = (float)cur * 0.05f;
            }
        }
    }
    i2c_bus_share_unlock();
    return err;
}
