/**
 * @file power_meter.c
 * @brief 双 INA236 功率计：MOS 通路切换 + 芯片内 ADCRANGE 自动量程。
 *
 * 硬件：
 * - 芯片1 (A0=GND): 5 mΩ 分流，ALERT=GPIO17，大电流通路（MOS 导通）
 * - 芯片2 (A0=VS):  100 Ω 分流，ALERT=GPIO18，微电流通路（MOS 关断）
 * - MOS 控制 GPIO21：高=导通芯片1，低=关断切换至芯片2
 */
#include "power_meter.h"

#include "ina236.h"
#include "i2c_bus_share.h"
#include "buzzer.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "power_meter";

/** 芯片1：5 mΩ，粗量程 ±81.92 mV → 约 16.4 A */
#define RSHUNT_HI_OHM   0.005f
#define IMAX_HI_A       16.0f

/** 芯片2：100 Ω，粗量程 ±81.92 mV → 约 819 µA */
#define RSHUNT_LO_OHM   100.0f
#define IMAX_LO_A       0.001f

/** 精细量程 (±20.48 mV) 上切阈值：约 80% 满量程 */
#define RANGE_UP_SHUNT_V    0.016f
/** 粗量程 (±81.92 mV) 下切阈值（带迟滞） */
#define RANGE_DOWN_SHUNT_V  0.004f

/** 通路切换迟滞（A）：小→大 800 µA（芯片2），大→小 1 mA（芯片1 零点噪声约 ±1 mA） */
#define PATH_UP_A    8.0e-4f
#define PATH_DOWN_A  1.0e-3f
/** 连续满足阈值的采样次数，抑制抖动 */
#define PATH_SWITCH_STABLE_COUNT  3
/** MOS 切换后模拟稳定等待 (ms)，非阻塞计时 */
#define PATH_SETTLE_MS  50
/** 通路切换后禁止再次切换 (ms) */
#define PATH_SWITCH_COOLDOWN_MS  1000
/** 小→大升档后保持大电流通路最短时间 (ms)，避免芯片1 零点噪声立即降档 */
#define PATH_HIGH_HOLD_MS  2000

static const uint8_t s_addr_hi[] = { INA236_ADDR_A0_GND_A, INA236_ADDR_A0_GND_B };
static const uint8_t s_addr_lo[] = { INA236_ADDR_A0_VS_A, INA236_ADDR_A0_VS_B };

static ina236_dev_t      s_ina_hi;
static ina236_dev_t      s_ina_lo;
static power_meter_path_t s_active_path;
static bool              s_inited;
static bool              s_auto_range_enabled;
static SemaphoreHandle_t s_alert_sem;
static TaskHandle_t      s_alert_task;
static uint8_t           s_path_stable_cnt;
static int8_t              s_path_pending; /**< -1 待降档, +1 待升档, 0 无 */
static TickType_t        s_path_cooldown_until;
static TickType_t        s_path_settle_until;
static TickType_t        s_high_path_hold_until;
static volatile bool     s_path_switch_busy;

static bool path_in_cooldown(void)
{
    return (int32_t)(xTaskGetTickCount() - s_path_cooldown_until) < 0;
}

static bool path_is_settling(void)
{
    return (int32_t)(xTaskGetTickCount() - s_path_settle_until) < 0;
}

static bool path_in_high_hold(void)
{
    return s_active_path == POWER_METER_PATH_HIGH &&
           (int32_t)(xTaskGetTickCount() - s_high_path_hold_until) < 0;
}

static bool auto_range_paused(void)
{
    return path_in_cooldown() || path_is_settling() || s_path_switch_busy;
}

static void path_arm_cooldown(void)
{
    s_path_cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(PATH_SWITCH_COOLDOWN_MS);
}

static void clear_chip_alert_latch_locked(ina236_dev_t *dev)
{
    if (!dev->present) {
        return;
    }
    uint16_t mask = 0;
    (void)ina236_read_mask_enable(dev, &mask);
    (void)mask;
}

static ina236_dev_t *active_dev(void)
{
    return (s_active_path == POWER_METER_PATH_HIGH) ? &s_ina_hi : &s_ina_lo;
}

static void mos_set_high_path(bool hi_path)
{
    gpio_set_level(POWER_METER_MOS_GPIO, hi_path ? 1 : 0);
}

static esp_err_t apply_range_alerts(ina236_dev_t *dev, ina236_range_t range)
{
    esp_err_t err;
    if (range == INA236_RANGE_FINE) {
        const int16_t limit = ina236_shunt_v_to_limit(INA236_RANGE_FINE, RANGE_UP_SHUNT_V);
        err = ina236_config_alert(dev, INA236_ALERT_SOL, limit);
    } else {
        const int16_t limit = ina236_shunt_v_to_limit(INA236_RANGE_COARSE, RANGE_DOWN_SHUNT_V);
        err = ina236_config_alert(dev, INA236_ALERT_SUL, limit);
    }
    return err;
}

static esp_err_t switch_chip_range(ina236_dev_t *dev, ina236_range_t range, bool play_buzzer)
{
    const ina236_range_t prev = dev->range;

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ina236_set_range(dev, range);
    if (err == ESP_OK) {
        err = apply_range_alerts(dev, range);
    }
    i2c_bus_share_unlock();

    if (err == ESP_OK && s_inited && play_buzzer && prev != range) {
        if (range == INA236_RANGE_COARSE) {
            buzzer_play_pattern(BUZZER_PATTERN_RANGE_UP);
        } else {
            buzzer_play_pattern(BUZZER_PATTERN_RANGE_DOWN);
        }
    }
    return err;
}

static esp_err_t configure_active_chip(ina236_dev_t *dev, ina236_range_t range)
{
    return switch_chip_range(dev, range, false);
}

static esp_err_t switch_path(power_meter_path_t path, ina236_range_t start_range, bool play_buzzer)
{
    if (s_path_switch_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    const power_meter_path_t prev = s_active_path;
    if (prev == path) {
        return ESP_OK;
    }

    const bool to_high = (path == POWER_METER_PATH_HIGH);
    s_path_switch_busy = true;

    /* 先更新通路状态，避免切换等待期间 ALERT/读数逻辑重复触发 */
    s_active_path       = path;
    s_path_stable_cnt   = 0;
    s_path_pending      = 0;
    s_path_settle_until = xTaskGetTickCount() + pdMS_TO_TICKS(PATH_SETTLE_MS);

    mos_set_high_path(to_high);

    if (to_high) {
        s_high_path_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(PATH_HIGH_HOLD_MS);
        start_range            = INA236_RANGE_FINE;
    }

    ina236_dev_t *dev      = active_dev();
    ina236_dev_t *inactive = to_high ? &s_ina_lo : &s_ina_hi;

    esp_err_t err = configure_active_chip(dev, start_range);
    if (err == ESP_OK && i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        clear_chip_alert_latch_locked(&s_ina_hi);
        clear_chip_alert_latch_locked(&s_ina_lo);
        (void)ina236_disable_alert(inactive);
        i2c_bus_share_unlock();
    }

    path_arm_cooldown();

    if (err == ESP_OK && s_inited && play_buzzer) {
        if (to_high) {
            buzzer_play_pattern(BUZZER_PATTERN_RANGE_UP);
        } else {
            buzzer_play_pattern(BUZZER_PATTERN_RANGE_DOWN);
        }
    }

    s_path_switch_busy = false;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "通路切换 → %s (MOS=%s, INA236 @ 0x%02X, %s)",
                 to_high ? "大电流/芯片1" : "微电流/芯片2",
                 to_high ? "导通" : "关断",
                 dev->i2c_addr,
                 start_range == INA236_RANGE_FINE ? "±20.48mV" : "±81.92mV");
    }
    return err;
}

static void clear_chip_alert_latch(ina236_dev_t *dev)
{
    if (!dev->present) {
        return;
    }
    uint16_t mask = 0;
    (void)ina236_read_mask_enable(dev, &mask);
    (void)mask;
}

static void process_chip_alert(ina236_dev_t *dev)
{
    if (!s_auto_range_enabled || auto_range_paused() || !dev->present || dev != active_dev()) {
        return;
    }

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        return;
    }

    uint16_t mask = 0;
    if (ina236_read_mask_enable(dev, &mask) != ESP_OK) {
        i2c_bus_share_unlock();
        return;
    }

    const bool aff = (mask & (1u << 4)) != 0;
    const bool sol = (mask & (1u << 15)) != 0;
    const bool sul = (mask & (1u << 14)) != 0;
    const ina236_range_t cur = dev->range;
    i2c_bus_share_unlock();

    if (!aff) {
        return;
    }

    if (cur == INA236_RANGE_FINE && sol) {
        if (s_active_path == POWER_METER_PATH_LOW) {
            ESP_LOGI(TAG, "ALERT 0x%02X: 芯片2 精细量程饱和 → ±81.92mV", dev->i2c_addr);
            (void)switch_chip_range(dev, INA236_RANGE_COARSE, true);
        } else if (s_active_path == POWER_METER_PATH_HIGH) {
            ESP_LOGI(TAG, "ALERT 0x%02X: 分流过压 → ±81.92mV", dev->i2c_addr);
            (void)switch_chip_range(dev, INA236_RANGE_COARSE, true);
        }
    } else if (cur == INA236_RANGE_COARSE && sul) {
        ESP_LOGI(TAG, "ALERT 0x%02X: 分流欠压 → ±20.48mV", dev->i2c_addr);
        (void)switch_chip_range(dev, INA236_RANGE_FINE, true);
    }
}

static void IRAM_ATTR alert_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t wake = pdFALSE;
    if (s_alert_sem) {
        xSemaphoreGiveFromISR(s_alert_sem, &wake);
    }
    if (wake) {
        portYIELD_FROM_ISR();
    }
}

static void alert_worker_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_alert_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        process_chip_alert(&s_ina_hi);
        process_chip_alert(&s_ina_lo);
    }
}

static void evaluate_path_switch(const ina236_reading_t *raw)
{
    if (!s_auto_range_enabled || auto_range_paused()) {
        return;
    }

    const float abs_i = fabsf(raw->current_a);

    if (s_active_path == POWER_METER_PATH_HIGH) {
        if (path_in_high_hold()) {
            s_path_pending    = 0;
            s_path_stable_cnt = 0;
            return;
        }

        /* 大电流通路：芯片1 读数低于下阈值时切至微电流芯片 */
        if (abs_i < PATH_DOWN_A && !raw->overflow) {
            if (s_path_pending != -1) {
                s_path_pending = -1;
                s_path_stable_cnt = 1;
            } else {
                s_path_stable_cnt++;
            }
            if (s_path_stable_cnt >= PATH_SWITCH_STABLE_COUNT && s_ina_lo.present) {
                ESP_LOGI(TAG, "电流 %.2f mA < 阈值，切换至微电流通路", (double)(abs_i * 1000.0f));
                (void)switch_path(POWER_METER_PATH_LOW, INA236_RANGE_FINE, true);
            }
        } else {
            s_path_pending = 0;
            s_path_stable_cnt = 0;
        }
    } else {
        bool need_high = false;

        if (raw->overflow) {
            need_high = true;
        } else if (abs_i > PATH_UP_A) {
            if (s_path_pending != 1) {
                s_path_pending    = 1;
                s_path_stable_cnt = 1;
            } else {
                s_path_stable_cnt++;
            }
            if (s_path_stable_cnt >= PATH_SWITCH_STABLE_COUNT) {
                need_high = true;
            }
        } else {
            s_path_pending    = 0;
            s_path_stable_cnt = 0;
        }

        if (need_high) {
            ESP_LOGI(TAG, "电流 %.2f mA > 阈值，切换至大电流通路", (double)(abs_i * 1000.0f));
            (void)switch_path(POWER_METER_PATH_HIGH, INA236_RANGE_FINE, true);
        }
    }
}

static esp_err_t init_alert_isr(void)
{
    s_alert_sem = xSemaphoreCreateBinary();
    if (!s_alert_sem) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_num_t alert_pins[2] = { POWER_METER_ALERT_HI_GPIO, POWER_METER_ALERT_LO_GPIO };
    const ina236_dev_t *devs[2]    = { &s_ina_hi, &s_ina_lo };
    for (size_t i = 0; i < 2; i++) {
        if (!devs[i]->present) {
            continue;
        }
        gpio_isr_handler_add(alert_pins[i], alert_isr_handler, NULL);
        gpio_set_intr_type(alert_pins[i], GPIO_INTR_NEGEDGE);
    }

    BaseType_t ok = xTaskCreate(alert_worker_task, "ina_alert", 3072, NULL, 5, &s_alert_task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t power_meter_init(i2c_port_t port)
{
    if (s_inited) {
        return ESP_OK;
    }

    gpio_config_t mos_cfg = {
        .pin_bit_mask = (1ULL << POWER_METER_MOS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&mos_cfg));
    mos_set_high_path(true);

    esp_err_t err = ina236_init(&s_ina_hi, port, POWER_METER_ALERT_HI_GPIO,
                                s_addr_hi, sizeof(s_addr_hi), RSHUNT_HI_OHM, IMAX_HI_A);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "芯片1 (大电流) 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = ina236_init(&s_ina_lo, port, POWER_METER_ALERT_LO_GPIO,
                      s_addr_lo, sizeof(s_addr_lo), RSHUNT_LO_OHM, IMAX_LO_A);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "芯片2 (微电流) 未就绪: %s，仅使用芯片1", esp_err_to_name(err));
        memset(&s_ina_lo, 0, sizeof(s_ina_lo));
    }

    s_active_path = POWER_METER_PATH_HIGH;
    err = configure_active_chip(&s_ina_hi, INA236_RANGE_COARSE);
    if (err != ESP_OK) {
        return err;
    }

    err = init_alert_isr();
    if (err != ESP_OK) {
        return err;
    }

    s_inited = true;

    const float hi_res = ina236_shunt_current_resolution_a(INA236_RANGE_COARSE, RSHUNT_HI_OHM);
    const float lo_res = ina236_shunt_current_resolution_a(INA236_RANGE_FINE, RSHUNT_LO_OHM);
    const float lo_off = INA236_SHUNT_OFFSET_V_MAX / RSHUNT_LO_OHM;
    ESP_LOGI(TAG,
             "功率计就绪 | MOS=GPIO%d 默认导通 | 芯片1 ALERT=GPIO%d | 芯片2 ALERT=GPIO%d",
             (int)POWER_METER_MOS_GPIO, (int)POWER_METER_ALERT_HI_GPIO,
             (int)POWER_METER_ALERT_LO_GPIO);
    ESP_LOGI(TAG,
             "大电流: Rshunt=%.0f mΩ, 分辨率≈%.0f µA | 微电流: Rshunt=%.0f Ω, 分辨率≈%.1f nA, 零点≈±%.0f nA",
             (double)(RSHUNT_HI_OHM * 1000.0f), (double)(hi_res * 1e6f),
             (double)RSHUNT_LO_OHM, (double)(lo_res * 1e9f), (double)(lo_off * 1e9f));
    return ESP_OK;
}

esp_err_t power_meter_read(power_meter_reading_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || !s_ina_hi.present) {
        return ESP_ERR_INVALID_STATE;
    }
    ina236_dev_t *dev = active_dev();

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }

    ina236_reading_t raw = {0};
    esp_err_t err = ina236_read(dev, &raw);
    i2c_bus_share_unlock();
    if (err != ESP_OK) {
        return err;
    }

    out->voltage_v  = raw.bus_v;
    out->current_a  = raw.current_a;
    out->power_w    = raw.power_w;
    out->range_fine = (raw.range == INA236_RANGE_FINE);
    out->overflow   = raw.overflow;
    out->path       = s_active_path;

    if (s_auto_range_enabled && !auto_range_paused()) {
        if (s_active_path == POWER_METER_PATH_HIGH && raw.overflow &&
            raw.range == INA236_RANGE_FINE) {
            (void)switch_chip_range(dev, INA236_RANGE_COARSE, true);
        }
        evaluate_path_switch(&raw);
    }

    return ESP_OK;
}

bool power_meter_is_ready(void)
{
    return s_inited && s_ina_hi.present;
}

void power_meter_set_auto_range_enabled(bool enabled)
{
    if (s_auto_range_enabled == enabled) {
        return;
    }

    s_auto_range_enabled  = enabled;
    s_path_stable_cnt     = 0;
    s_path_pending        = 0;
    s_path_cooldown_until = 0;
    s_path_settle_until   = 0;
    s_high_path_hold_until = 0;

    if (!enabled || !s_inited) {
        return;
    }

    if (i2c_bus_share_lock(pdMS_TO_TICKS(100))) {
        clear_chip_alert_latch(&s_ina_hi);
        clear_chip_alert_latch(&s_ina_lo);
        i2c_bus_share_unlock();
    }

    ESP_LOGI(TAG, "自动量程切换已启用");
}

float power_meter_current_resolution_a(void)
{
    if (!s_inited) {
        return ina236_shunt_current_resolution_a(INA236_RANGE_COARSE, RSHUNT_HI_OHM);
    }
    ina236_dev_t *dev = active_dev();
    return ina236_shunt_current_resolution_a(dev->range, dev->rshunt_ohm);
}

void power_meter_format_current(float current_a, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    const float abs_a = fabsf(current_a);

    if (abs_a >= 1.0f) {
        (void)snprintf(buf, buf_len, "%.2f A", (double)current_a);
    } else if (abs_a >= 0.001f) {
        (void)snprintf(buf, buf_len, "%.2f mA", (double)(current_a * 1000.0f));
    } else {
        (void)snprintf(buf, buf_len, "%.1f uA", (double)(current_a * 1e6f));
    }
}

void power_meter_format_power(float power_w, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    const float abs_w = fabsf(power_w);

    if (abs_w >= 1.0f) {
        (void)snprintf(buf, buf_len, "%.3f W", (double)power_w);
    } else {
        (void)snprintf(buf, buf_len, "%.4f W", (double)power_w);
    }
}
