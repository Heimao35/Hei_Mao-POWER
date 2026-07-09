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
#include "nvs_flash.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
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

/** 微电流通路：空载校零（电压变化时暂停更新，避免把负载误判为零点） */
#define ZERO_TRACK_THRESH_A       12.0e-6f
#define ZERO_TRACK_DELTA_A        2.0e-6f
#define ZERO_TRACK_STABLE_SAMPLES 4U
#define ZERO_POINT_MERGE_V        0.15f
#define ZERO_POINT_MIN_DV         0.20f
#define ZERO_V_TRANSIENT_V        0.08f
/** 学习到的 dI_zero/dV 限幅（含 MOS 漏电流等系统项，远大于 CMRR 理论值） */
#define ZERO_SLOPE_MAX_LO_A_V     5.0e-6f
#define ZERO_SLOPE_MAX_HI_A_V     2.0e-3f
/** 每通路最多校零点数量（分段线性插值） */
#define ZERO_CAL_MAX_POINTS       8U
#define ZERO_CAL_NVS_MAGIC        0x504Du
#define ZERO_CAL_NVS_VERSION      1U
#define ZERO_CAL_NVS_NAMESPACE    "pm_cal"
#define ZERO_CAL_NVS_KEY_HI       "zero_hi"
#define ZERO_CAL_NVS_KEY_LO       "zero_lo"
#define ZERO_CAL_NVS_DEBOUNCE_MS  5000U
#define ZERO_CAL_IDLE_MERGE_ALPHA 0.12f

/** 显示用 EMA：微电流更平滑，大电流更快响应 */
#define FILTER_ALPHA_LO  0.35f
#define FILTER_ALPHA_HI  0.55f

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

/** 空载校零：多电压点分段线性插值 offset(V)，NVS 掉电保存。 */
typedef struct {
    float   v[ZERO_CAL_MAX_POINTS];
    float   i[ZERO_CAL_MAX_POINTS];
    uint8_t count;
    float   v_prev;
    bool    v_prev_valid;
} zero_cal_t;

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  count;
    float    v[ZERO_CAL_MAX_POINTS];
    float    i[ZERO_CAL_MAX_POINTS];
} zero_cal_nvs_blob_t;

static zero_cal_t        s_zero_hi;
static zero_cal_t        s_zero_lo;
static float             s_i_filtered;
static bool              s_i_filter_valid;
static float             s_i_prev_raw;
static uint8_t           s_zero_stable_cnt;
static power_meter_path_t s_filter_path;
static bool              s_nvs_ready;
static bool              s_zero_cal_dirty;
static TickType_t        s_zero_cal_save_at;

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

static zero_cal_t *zero_cal_for_path(power_meter_path_t path)
{
    return (path == POWER_METER_PATH_LOW) ? &s_zero_lo : &s_zero_hi;
}

static float rshunt_for_path(power_meter_path_t path)
{
    return (path == POWER_METER_PATH_LOW) ? RSHUNT_LO_OHM : RSHUNT_HI_OHM;
}

static float zero_slope_max(power_meter_path_t path)
{
    return (path == POWER_METER_PATH_LOW) ? ZERO_SLOPE_MAX_LO_A_V : ZERO_SLOPE_MAX_HI_A_V;
}

static float zero_slope_default(power_meter_path_t path)
{
    return ina236_cmrr_zero_slope_a_per_v(INA236_CMRR_DB_MIN, rshunt_for_path(path));
}

static float clampf(float x, float lo, float hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

static void reset_zero_cal_runtime(zero_cal_t *zc)
{
    zc->v_prev       = 0.0f;
    zc->v_prev_valid = false;
}

static void reset_current_filter(power_meter_path_t path)
{
    s_i_filter_valid  = false;
    s_i_prev_raw      = 0.0f;
    s_zero_stable_cnt = 0;
    s_filter_path     = path;
}

static void zero_cal_sort_points(zero_cal_t *zc)
{
    for (uint8_t i = 1; i < zc->count; i++) {
        const float tv = zc->v[i];
        const float ti = zc->i[i];
        int8_t j       = (int8_t)i - 1;
        while (j >= 0 && zc->v[j] > tv) {
            zc->v[j + 1] = zc->v[j];
            zc->i[j + 1] = zc->i[j];
            j--;
        }
        zc->v[j + 1] = tv;
        zc->i[j + 1] = ti;
    }
}

static int zero_cal_find_near(const zero_cal_t *zc, float bus_v)
{
    for (uint8_t idx = 0; idx < zc->count; idx++) {
        if (fabsf(bus_v - zc->v[idx]) < ZERO_POINT_MERGE_V) {
            return (int)idx;
        }
    }
    return -1;
}

static float zero_segment_offset(float v0, float i0, float v1, float i1, float bus_v,
                               power_meter_path_t path)
{
    const float dv = v1 - v0;
    if (fabsf(dv) < 1e-6f) {
        return i0;
    }

    float slope = (i1 - i0) / dv;
    const float lim = zero_slope_max(path);
    slope = clampf(slope, -lim, lim);
    return i0 + slope * (bus_v - v0);
}

static float zero_offset_at_voltage(const zero_cal_t *zc, float bus_v, power_meter_path_t path)
{
    if (zc->count == 0) {
        return 0.0f;
    }
    if (zc->count == 1) {
        return zc->i[0] + zero_slope_default(path) * (bus_v - zc->v[0]);
    }

    if (bus_v <= zc->v[0]) {
        return zero_segment_offset(zc->v[0], zc->i[0], zc->v[1], zc->i[1], bus_v, path);
    }

    const uint8_t last = (uint8_t)(zc->count - 1U);
    if (bus_v >= zc->v[last]) {
        return zero_segment_offset(zc->v[last - 1U], zc->i[last - 1U], zc->v[last], zc->i[last],
                                   bus_v, path);
    }

    for (uint8_t k = 0; k + 1U < zc->count; k++) {
        if (bus_v >= zc->v[k] && bus_v <= zc->v[k + 1U]) {
            return zero_segment_offset(zc->v[k], zc->i[k], zc->v[k + 1U], zc->i[k + 1U], bus_v,
                                       path);
        }
    }

    return zc->i[last];
}

static void zero_cal_mark_dirty(void)
{
    s_zero_cal_dirty = true;
    s_zero_cal_save_at = xTaskGetTickCount() + pdMS_TO_TICKS(ZERO_CAL_NVS_DEBOUNCE_MS);
}

static esp_err_t zero_cal_nvs_init_once(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    s_nvs_ready = true;
    return ESP_OK;
}

static bool zero_cal_blob_valid(const zero_cal_nvs_blob_t *blob)
{
    if (!blob || blob->magic != ZERO_CAL_NVS_MAGIC || blob->version != ZERO_CAL_NVS_VERSION) {
        return false;
    }
    if (blob->count == 0 || blob->count > ZERO_CAL_MAX_POINTS) {
        return false;
    }

    for (uint8_t i = 0; i < blob->count; i++) {
        if (!isfinite(blob->v[i]) || !isfinite(blob->i[i])) {
            return false;
        }
        if (blob->v[i] < -0.5f || blob->v[i] > 52.0f) {
            return false;
        }
        if (fabsf(blob->i[i]) > 0.05f) {
            return false;
        }
    }
    return true;
}

static void zero_cal_from_blob(zero_cal_t *zc, const zero_cal_nvs_blob_t *blob)
{
    memset(zc, 0, sizeof(*zc));
    zc->count = blob->count;
    memcpy(zc->v, blob->v, blob->count * sizeof(zc->v[0]));
    memcpy(zc->i, blob->i, blob->count * sizeof(zc->i[0]));
    zero_cal_sort_points(zc);
}

static void zero_cal_to_blob(const zero_cal_t *zc, zero_cal_nvs_blob_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic   = ZERO_CAL_NVS_MAGIC;
    blob->version = ZERO_CAL_NVS_VERSION;
    blob->count   = zc->count;
    memcpy(blob->v, zc->v, zc->count * sizeof(blob->v[0]));
    memcpy(blob->i, zc->i, zc->count * sizeof(blob->i[0]));
}

static esp_err_t zero_cal_load_path(const char *key, zero_cal_t *zc)
{
    zero_cal_nvs_blob_t blob = {0};
    size_t len               = sizeof(blob);

    nvs_handle_t h;
    esp_err_t err = nvs_open(ZERO_CAL_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(h, key, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(blob) || !zero_cal_blob_valid(&blob)) {
        return ESP_ERR_INVALID_VERSION;
    }

    zero_cal_from_blob(zc, &blob);
    return ESP_OK;
}

static esp_err_t zero_cal_save_path(const char *key, const zero_cal_t *zc)
{
    if (zc->count == 0) {
        return ESP_OK;
    }

    zero_cal_nvs_blob_t blob = {0};
    zero_cal_to_blob(zc, &blob);

    nvs_handle_t h;
    esp_err_t err = nvs_open(ZERO_CAL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, key, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void zero_cal_load_all(void)
{
    memset(&s_zero_hi, 0, sizeof(s_zero_hi));
    memset(&s_zero_lo, 0, sizeof(s_zero_lo));

    if (zero_cal_nvs_init_once() != ESP_OK) {
        ESP_LOGW(TAG, "零点 NVS 初始化失败，使用空表");
        return;
    }

    if (zero_cal_load_path(ZERO_CAL_NVS_KEY_HI, &s_zero_hi) == ESP_OK) {
        ESP_LOGI(TAG, "已加载大电流零点表: %u 点", (unsigned)s_zero_hi.count);
    }
    if (zero_cal_load_path(ZERO_CAL_NVS_KEY_LO, &s_zero_lo) == ESP_OK) {
        ESP_LOGI(TAG, "已加载微电流零点表: %u 点", (unsigned)s_zero_lo.count);
    }
}

static void zero_cal_save_all(void)
{
    if (zero_cal_nvs_init_once() != ESP_OK) {
        return;
    }

    esp_err_t err_hi = zero_cal_save_path(ZERO_CAL_NVS_KEY_HI, &s_zero_hi);
    esp_err_t err_lo = zero_cal_save_path(ZERO_CAL_NVS_KEY_LO, &s_zero_lo);
    if (err_hi == ESP_OK && err_lo == ESP_OK) {
        ESP_LOGI(TAG, "零点表已保存 (大=%u 微=%u 点)", (unsigned)s_zero_hi.count,
                 (unsigned)s_zero_lo.count);
    } else {
        ESP_LOGW(TAG, "零点表保存失败 hi=%s lo=%s", esp_err_to_name(err_hi),
                 esp_err_to_name(err_lo));
    }
}

static void zero_cal_nvs_poll(void)
{
    if (!s_zero_cal_dirty) {
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - s_zero_cal_save_at) < 0) {
        return;
    }

    zero_cal_save_all();
    s_zero_cal_dirty = false;
}

static void zero_cal_insert_point(zero_cal_t *zc, float bus_v, float raw_a)
{
    uint8_t pos = 0;
    while (pos < zc->count && zc->v[pos] < bus_v) {
        pos++;
    }

    if (zc->count < ZERO_CAL_MAX_POINTS) {
        for (uint8_t j = zc->count; j > pos; j--) {
            zc->v[j] = zc->v[j - 1U];
            zc->i[j] = zc->i[j - 1U];
        }
        zc->v[pos]  = bus_v;
        zc->i[pos]  = raw_a;
        zc->count   = (uint8_t)(zc->count + 1U);
        return;
    }

    uint8_t replace = 0;
    float best_dist = fabsf(bus_v - zc->v[0]);
    for (uint8_t k = 1; k < zc->count; k++) {
        const float d = fabsf(bus_v - zc->v[k]);
        if (d < best_dist) {
            best_dist = d;
            replace   = k;
        }
    }
    zc->v[replace] = bus_v;
    zc->i[replace] = raw_a;
    zero_cal_sort_points(zc);
}

static void zero_cal_merge_idle_point(zero_cal_t *zc, float bus_v, float raw_a)
{
    bool changed = false;
    const int near = zero_cal_find_near(zc, bus_v);

    if (near >= 0) {
        const float before = zc->i[near];
        zc->i[near] += ZERO_CAL_IDLE_MERGE_ALPHA * (raw_a - zc->i[near]);
        changed = fabsf(zc->i[near] - before) > ZERO_TRACK_DELTA_A * 0.1f;
    } else if (zc->count == 0) {
        zc->v[0]  = bus_v;
        zc->i[0]  = raw_a;
        zc->count = 1;
        changed   = true;
    } else if (zc->count == 1 && fabsf(bus_v - zc->v[0]) < ZERO_POINT_MIN_DV) {
        const float before = zc->i[0];
        zc->i[0] += ZERO_CAL_IDLE_MERGE_ALPHA * (raw_a - zc->i[0]);
        changed = fabsf(zc->i[0] - before) > ZERO_TRACK_DELTA_A * 0.1f;
    } else {
        zero_cal_insert_point(zc, bus_v, raw_a);
        changed = true;
    }

    if (changed) {
        zero_cal_mark_dirty();
    }
}

static bool bus_voltage_is_transient(zero_cal_t *zc, float bus_v)
{
    bool trans = false;
    if (zc->v_prev_valid) {
        trans = fabsf(bus_v - zc->v_prev) > ZERO_V_TRANSIENT_V;
    }
    zc->v_prev       = bus_v;
    zc->v_prev_valid = true;
    return trans;
}

static float filter_alpha_for_path(power_meter_path_t path)
{
    return (path == POWER_METER_PATH_LOW) ? FILTER_ALPHA_LO : FILTER_ALPHA_HI;
}

static float apply_zero_compensation(float raw_a, float bus_v, power_meter_path_t path)
{
    zero_cal_t *zc = zero_cal_for_path(path);

    const float offset    = zero_offset_at_voltage(zc, bus_v, path);
    const float corrected = raw_a - offset;
    const bool  v_trans   = bus_voltage_is_transient(zc, bus_v);

    if (!v_trans &&
        fabsf(raw_a) < ZERO_TRACK_THRESH_A &&
        fabsf(raw_a - s_i_prev_raw) < ZERO_TRACK_DELTA_A) {
        if (s_zero_stable_cnt < UINT8_MAX) {
            s_zero_stable_cnt++;
        }
    } else {
        s_zero_stable_cnt = 0;
    }

    if (!v_trans && s_zero_stable_cnt >= ZERO_TRACK_STABLE_SAMPLES) {
        zero_cal_merge_idle_point(zc, bus_v, raw_a);
    }

    s_i_prev_raw = raw_a;
    return corrected;
}

static float apply_display_filter(float corrected_a, power_meter_path_t path)
{
    if (!s_i_filter_valid || s_filter_path != path) {
        s_i_filtered     = corrected_a;
        s_i_filter_valid = true;
        s_filter_path    = path;
        return corrected_a;
    }

    const float alpha = filter_alpha_for_path(path);
    s_i_filtered = alpha * corrected_a + (1.0f - alpha) * s_i_filtered;
    return s_i_filtered;
}

static float process_current_reading(float raw_a, float bus_v, power_meter_path_t path)
{
    const float corrected = apply_zero_compensation(raw_a, bus_v, path);
    return apply_display_filter(corrected, path);
}

static float corrected_current_for_switch(const ina236_reading_t *raw, power_meter_path_t path)
{
    const zero_cal_t *zc = zero_cal_for_path(path);
    return raw->current_a - zero_offset_at_voltage(zc, raw->bus_v, path);
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
    reset_current_filter(path);
    reset_zero_cal_runtime(zero_cal_for_path(path));

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

    const float abs_i = fabsf(corrected_current_for_switch(raw, s_active_path));

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

    zero_cal_load_all();

    esp_err_t err = ina236_init(&s_ina_hi, port, POWER_METER_ALERT_HI_GPIO,
                                s_addr_hi, sizeof(s_addr_hi), RSHUNT_HI_OHM, IMAX_HI_A,
                                INA236_ADC_PROFILE_FAST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "芯片1 (大电流) 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = ina236_init(&s_ina_lo, port, POWER_METER_ALERT_LO_GPIO,
                      s_addr_lo, sizeof(s_addr_lo), RSHUNT_LO_OHM, IMAX_LO_A,
                      INA236_ADC_PROFILE_PRECISION);
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
    const float lo_off_typ = INA236_SHUNT_OFFSET_V_TYP / RSHUNT_LO_OHM;
    const float lo_off_max = INA236_SHUNT_OFFSET_V_MAX / RSHUNT_LO_OHM;
    ESP_LOGI(TAG,
             "功率计就绪 | MOS=GPIO%d 默认导通 | 芯片1 ALERT=GPIO%d | 芯片2 ALERT=GPIO%d",
             (int)POWER_METER_MOS_GPIO, (int)POWER_METER_ALERT_HI_GPIO,
             (int)POWER_METER_ALERT_LO_GPIO);
    ESP_LOGI(TAG,
             "大电流: Rshunt=%.0f mΩ, 分辨率≈%.0f µA, AVG=16 | "
             "微电流: Rshunt=%.0f Ω, 分辨率≈%.1f nA, AVG=128, 芯片零点≈±%.0f nA (max ±%.0f nA)",
             (double)(RSHUNT_HI_OHM * 1000.0f), (double)(hi_res * 1e6f),
             (double)RSHUNT_LO_OHM, (double)(lo_res * 1e9f),
             (double)(lo_off_typ * 1e9f), (double)(lo_off_max * 1e9f));
    ESP_LOGI(TAG, "零点校准: 每通路最多 %u 点, NVS 命名空间 \"%s\"",
             (unsigned)ZERO_CAL_MAX_POINTS, ZERO_CAL_NVS_NAMESPACE);
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

    const float current_a = process_current_reading(raw.current_a, raw.bus_v, s_active_path);

    out->voltage_v  = raw.bus_v;
    out->current_a  = current_a;
    out->power_w    = current_a * raw.bus_v;
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

    zero_cal_nvs_poll();

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
        (void)snprintf(buf, buf_len, "%.2f uA", (double)(current_a * 1e6f));
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

esp_err_t power_meter_clear_zero_cal_nvs(void)
{
    memset(&s_zero_hi, 0, sizeof(s_zero_hi));
    memset(&s_zero_lo, 0, sizeof(s_zero_lo));
    s_zero_cal_dirty = false;

    if (zero_cal_nvs_init_once() != ESP_OK) {
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(ZERO_CAL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t e1 = nvs_erase_key(h, ZERO_CAL_NVS_KEY_HI);
    if (e1 == ESP_ERR_NVS_NOT_FOUND) {
        e1 = ESP_OK;
    }
    esp_err_t e2 = nvs_erase_key(h, ZERO_CAL_NVS_KEY_LO);
    if (e2 == ESP_ERR_NVS_NOT_FOUND) {
        e2 = ESP_OK;
    }
    err = nvs_commit(h);
    nvs_close(h);

    if (e1 != ESP_OK) {
        return e1;
    }
    if (e2 != ESP_OK) {
        return e2;
    }

    ESP_LOGI(TAG, "NVS 零点校准表已清除");
    return err;
}
