/**
 * @file ina236.c
 * @brief TI INA236 I2C 寄存器读写与量程/报警配置。
 */
#include "ina236.h"

#include "i2c_bus_share.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ina236";

enum {
    REG_CONFIG       = 0x00,
    REG_SHUNT_VOLT   = 0x01,
    REG_BUS_VOLT     = 0x02,
    REG_POWER        = 0x03,
    REG_CURRENT      = 0x04,
    REG_CALIBRATION  = 0x05,
    REG_MASK_ENABLE  = 0x06,
    REG_ALERT_LIMIT  = 0x07,
    REG_MANUFACTURER = 0x3E,
    REG_DEVICE_ID    = 0x3F,
};

#define INA236_MANUFACTURER_ID  0x5449u
#define INA236_DEVICE_ID        0xA080u

#define CONFIG_MODE_CONT_SHUNT_BUS  0x07u
#define CONFIG_ADCRANGE_SHIFT       12
#define CONFIG_AVG_SHIFT            9
#define CONFIG_VBUSCT_SHIFT         6
#define CONFIG_VSHCT_SHIFT          3
#define CONFIG_RESERVED_BITS        (0x2u << 13)

#define MASK_SOL  (1u << 15)
#define MASK_SUL  (1u << 14)
#define MASK_BOL  (1u << 13)
#define MASK_BUL  (1u << 12)
#define MASK_POL  (1u << 11)
#define MASK_AFF  (1u << 4)
#define MASK_OVF  (1u << 2)
#define MASK_LEN  (1u << 0)

#define BUS_VOLT_LSB_V   0.0016f
#define SHUNT_LSB_COARSE 2.5e-6f
#define SHUNT_LSB_FINE   625e-9f

static float shunt_lsb_v(ina236_range_t range)
{
    return (range == INA236_RANGE_FINE) ? SHUNT_LSB_FINE : SHUNT_LSB_COARSE;
}

static float pick_current_lsb(float imax_a)
{
    const float min_lsb = imax_a / 32768.0f;
    const float max_lsb = 8.0f * min_lsb;
    /* 从小到大选取满足数据手册约束的最细 LSB */
    const float candidates[] = {
        1e-9f, 5e-9f, 10e-9f, 25e-9f, 50e-9f, 100e-9f, 200e-9f, 250e-9f,
        500e-9f, 1e-6f, 2e-6f, 5e-6f, 10e-6f, 25e-6f, 50e-6f, 100e-6f,
        200e-6f, 250e-6f, 500e-6f, 1e-3f,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (candidates[i] >= min_lsb && candidates[i] <= max_lsb) {
            return candidates[i];
        }
    }
    return min_lsb;
}

float ina236_get_current_lsb(const ina236_dev_t *dev)
{
    return dev ? dev->current_lsb : 0.0f;
}

float ina236_shunt_current_resolution_a(ina236_range_t range, float rshunt_ohm)
{
    if (rshunt_ohm <= 0.0f) {
        return 0.0f;
    }
    return shunt_lsb_v(range) / rshunt_ohm;
}

float ina236_cmrr_vos_per_vcm_v(float cmrr_db)
{
    return powf(10.0f, -cmrr_db / 20.0f);
}

float ina236_cmrr_zero_slope_a_per_v(float cmrr_db, float rshunt_ohm)
{
    if (rshunt_ohm <= 0.0f) {
        return 0.0f;
    }
    return ina236_cmrr_vos_per_vcm_v(cmrr_db) / rshunt_ohm;
}

static uint16_t calc_shunt_cal(float current_lsb, float rshunt_ohm, ina236_range_t range)
{
    float cal = 0.00512f / (current_lsb * rshunt_ohm);
    if (range == INA236_RANGE_FINE) {
        cal /= 4.0f;
    }
    if (cal < 0.0f) {
        cal = 0.0f;
    }
    if (cal > 32767.0f) {
        cal = 32767.0f;
    }
    return (uint16_t)(cal + 0.5f);
}

static esp_err_t reg_write_u16(ina236_dev_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_write_to_device(dev->i2c_port, dev->i2c_addr, buf, sizeof(buf),
                                      pdMS_TO_TICKS(50));
}

static esp_err_t reg_read_u16(ina236_dev_t *dev, uint8_t reg, uint16_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[2] = {0};
    esp_err_t err = i2c_master_write_read_device(dev->i2c_port, dev->i2c_addr, &reg, 1, raw, 2,
                                                 pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        return err;
    }
    *value = ((uint16_t)raw[0] << 8) | raw[1];
    return ESP_OK;
}

static esp_err_t probe_addr(i2c_port_t port, uint8_t addr, uint16_t *mfg_out, uint16_t *dev_id_out)
{
    uint8_t reg = REG_MANUFACTURER;
    uint8_t raw[2] = {0};
    esp_err_t err = i2c_master_write_read_device(port, addr, &reg, 1, raw, 2, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        return err;
    }
    const uint16_t mfg = ((uint16_t)raw[0] << 8) | raw[1];
    if (mfg_out) {
        *mfg_out = mfg;
    }
    if (mfg != INA236_MANUFACTURER_ID) {
        return ESP_ERR_NOT_FOUND;
    }

    reg = REG_DEVICE_ID;
    err = i2c_master_write_read_device(port, addr, &reg, 1, raw, 2, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        return err;
    }
    const uint16_t dev_id = ((uint16_t)raw[0] << 8) | raw[1];
    if (dev_id_out) {
        *dev_id_out = dev_id;
    }
    if ((dev_id & 0xFFF8u) != (INA236_DEVICE_ID & 0xFFF8u)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void profile_adc_timing(ina236_adc_profile_t profile, uint8_t *avg, uint8_t *vshct, uint8_t *vbusct)
{
    if (profile == INA236_ADC_PROFILE_PRECISION) {
        *avg    = 0x4u; /**< 128 次硬件平均 */
        *vshct  = 0x4u; /**< 1100 µs 分流转换 */
        *vbusct = 0x3u; /**< 588 µs 母线转换（功率/电压无需与分流同等精度） */
        return;
    }

    *avg    = 0x3u; /**< 64 次硬件平均 */
    *vshct  = 0x3u; /**< 588 µs */
    *vbusct = 0x3u;
}

static uint16_t build_config_word(ina236_range_t range, ina236_adc_profile_t profile)
{
    uint8_t avg = 0;
    uint8_t vshct = 0;
    uint8_t vbusct = 0;
    profile_adc_timing(profile, &avg, &vshct, &vbusct);

    uint16_t cfg = CONFIG_RESERVED_BITS;
    cfg |= ((uint16_t)avg << CONFIG_AVG_SHIFT);
    cfg |= ((uint16_t)vbusct << CONFIG_VBUSCT_SHIFT);
    cfg |= ((uint16_t)vshct << CONFIG_VSHCT_SHIFT);
    cfg |= CONFIG_MODE_CONT_SHUNT_BUS;
    if (range == INA236_RANGE_FINE) {
        cfg |= (1u << CONFIG_ADCRANGE_SHIFT);
    }
    return cfg;
}

static esp_err_t write_config_mode(ina236_dev_t *dev, ina236_range_t range)
{
    const uint16_t cfg = build_config_word(range, dev->adc_profile);
    return reg_write_u16(dev, REG_CONFIG, cfg);
}

int16_t ina236_shunt_v_to_limit(ina236_range_t range, float shunt_v)
{
    const float lsb = shunt_lsb_v(range);
    const float scaled = shunt_v / lsb;
    if (scaled > 32767.0f) {
        return 32767;
    }
    if (scaled < -32768.0f) {
        return -32768;
    }
    return (int16_t)((scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f));
}

esp_err_t ina236_init(ina236_dev_t *dev, i2c_port_t port, gpio_num_t alert_gpio,
                      const uint8_t *addr_candidates, size_t addr_count,
                      float rshunt_ohm, float imax_a, ina236_adc_profile_t adc_profile)
{
    if (!dev || !addr_candidates || addr_count == 0 || rshunt_ohm <= 0.0f || imax_a <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dev, 0, sizeof(*dev));
    dev->i2c_port    = port;
    dev->alert_gpio  = alert_gpio;
    dev->rshunt_ohm  = rshunt_ohm;
    dev->imax_a      = imax_a;
    dev->current_lsb = pick_current_lsb(imax_a);
    dev->adc_profile = adc_profile;

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < addr_count; i++) {
        uint16_t mfg = 0;
        uint16_t dev_id = 0;
        esp_err_t probe = probe_addr(port, addr_candidates[i], &mfg, &dev_id);
        if (probe == ESP_OK) {
            dev->i2c_addr = addr_candidates[i];
            err = ESP_OK;
            break;
        }
        if (probe == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "0x%02X: mfg=0x%04X dev=0x%04X (期望 mfg=0x5449 dev≈0xA080)",
                     addr_candidates[i], mfg, dev_id);
        } else {
            ESP_LOGW(TAG, "0x%02X: I2C %s", addr_candidates[i], esp_err_to_name(probe));
        }
    }
    i2c_bus_share_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "未检测到 INA236");
        return err;
    }

    dev->present = true;

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }
    err = ina236_set_range(dev, INA236_RANGE_FINE);
    i2c_bus_share_unlock();
    if (err != ESP_OK) {
        dev->present = false;
        ESP_LOGE(TAG, "量程/校准配置失败: %s", esp_err_to_name(err));
        return err;
    }

    if (alert_gpio != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << alert_gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io));
    }

    const float shunt_i_lsb = ina236_shunt_current_resolution_a(INA236_RANGE_FINE, rshunt_ohm);
    const char *profile_name = (adc_profile == INA236_ADC_PROFILE_PRECISION) ? "精测" : "快速";
    ESP_LOGI(TAG,
             "INA236 @ 0x%02X, Rshunt=%.4g ohm, Imax=%.4g A, 分流分辨率=%.2g A, 起始量程=±20.48mV, ADC=%s",
             dev->i2c_addr, (double)rshunt_ohm, (double)imax_a, (double)shunt_i_lsb, profile_name);
    return ESP_OK;
}

esp_err_t ina236_set_range(ina236_dev_t *dev, ina236_range_t range)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->i2c_addr) {
        return ESP_ERR_INVALID_STATE;
    }

    dev->range     = range;
    dev->shunt_cal = calc_shunt_cal(dev->current_lsb, dev->rshunt_ohm, range);

    esp_err_t err = write_config_mode(dev, range);
    if (err != ESP_OK) {
        return err;
    }
    return reg_write_u16(dev, REG_CALIBRATION, dev->shunt_cal);
}

esp_err_t ina236_config_alert(ina236_dev_t *dev, ina236_alert_func_t func, int16_t limit_raw)
{
    if (!dev || !dev->present) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t mask = MASK_LEN;
    switch (func) {
    case INA236_ALERT_SOL:
        mask |= MASK_SOL;
        break;
    case INA236_ALERT_SUL:
        mask |= MASK_SUL;
        break;
    case INA236_ALERT_BOL:
        mask |= MASK_BOL;
        break;
    case INA236_ALERT_BUL:
        mask |= MASK_BUL;
        break;
    case INA236_ALERT_POL:
        mask |= MASK_POL;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = reg_write_u16(dev, REG_ALERT_LIMIT, (uint16_t)limit_raw);
    if (err != ESP_OK) {
        return err;
    }
    return reg_write_u16(dev, REG_MASK_ENABLE, mask);
}

esp_err_t ina236_read_mask_enable(ina236_dev_t *dev, uint16_t *mask_enable)
{
    if (!dev || !dev->present) {
        return ESP_ERR_INVALID_STATE;
    }
    return reg_read_u16(dev, REG_MASK_ENABLE, mask_enable);
}

esp_err_t ina236_disable_alert(ina236_dev_t *dev)
{
    if (!dev || !dev->present) {
        return ESP_ERR_INVALID_STATE;
    }
    return reg_write_u16(dev, REG_MASK_ENABLE, MASK_LEN);
}

esp_err_t ina236_read(ina236_dev_t *dev, ina236_reading_t *out)
{
    if (!dev || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dev->present) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t raw_shunt = 0;
    uint16_t raw_bus   = 0;
    uint16_t mask      = 0;

    esp_err_t err = reg_read_u16(dev, REG_SHUNT_VOLT, &raw_shunt);
    if (err != ESP_OK) {
        return err;
    }
    err = reg_read_u16(dev, REG_BUS_VOLT, &raw_bus);
    if (err != ESP_OK) {
        return err;
    }
    (void)reg_read_u16(dev, REG_MASK_ENABLE, &mask);

    const int16_t shunt_signed = (int16_t)raw_shunt;

    out->shunt_v   = (float)shunt_signed * shunt_lsb_v(dev->range);
    out->bus_v     = (float)(raw_bus & 0x7FFFu) * BUS_VOLT_LSB_V;
    out->current_a = out->shunt_v / dev->rshunt_ohm;
    out->power_w   = out->current_a * out->bus_v;
    out->overflow  = (mask & MASK_OVF) != 0;
    out->range     = dev->range;
    return ESP_OK;
}
