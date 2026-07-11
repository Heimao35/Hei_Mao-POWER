/**
 * @file power_meter.h
 * @brief 功率计测量接口（双 INA236 + MOS 通路切换，ALERT 自动量程）。
 */
#ifndef POWER_METER_H
#define POWER_METER_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 芯片1（大电流，5 mΩ）ALERT 引脚 */
#define POWER_METER_ALERT_HI_GPIO  GPIO_NUM_17
/** 芯片2（微电流，100 Ω）ALERT 引脚 */
#define POWER_METER_ALERT_LO_GPIO  GPIO_NUM_18
/** 采样通路 MOS 控制：高电平导通 → 芯片1 接入 */
#define POWER_METER_MOS_GPIO       GPIO_NUM_21

/** 测量通路 */
typedef enum {
    POWER_METER_PATH_HIGH = 0, /**< MOS 导通，芯片1（5 mΩ） */
    POWER_METER_PATH_LOW  = 1, /**< MOS 关断，芯片2（100 Ω） */
} power_meter_path_t;

/** 单次测量结果 */
typedef struct {
    float voltage_v;  /**< 母线电压 (V) */
    float current_a;  /**< 电流 (A) */
    float power_w;    /**< 功率 (W) */
    bool  range_fine; /**< true=±20.48mV 精细量程, false=±81.92mV */
    bool  overflow;   /**< 芯片数学溢出（量程可能不足） */
    power_meter_path_t path; /**< 当前测量通路 */
} power_meter_reading_t;

/** 初始化双 INA236、MOS 通路及 ALERT 自动量程。上电默认 MOS 导通（大电流通路）。 */
esp_err_t power_meter_init(i2c_port_t port);

/** 读取当前电压、电流、功率。 */
esp_err_t power_meter_read(power_meter_reading_t *out);

/** 芯片是否已成功初始化。 */
bool power_meter_is_ready(void);

/**
 * 启用/禁用自动量程与通路切换（含 ALERT 中断触发）。
 * 默认禁用；开机动画全部结束后再启用，避免打断启动提示音。
 */
void power_meter_set_auto_range_enabled(bool enabled);

/** 分流 ADC 换算的电流分辨率 (A)，随通路及精细/粗量程变化。 */
float power_meter_current_resolution_a(void);

/** 按分辨率格式化为带单位的字符串（A / mA / uA）。 */
void power_meter_format_current(float current_a, char *buf, size_t buf_len);

/** 格式化为 W 单位字符串（按量级 3~4 位小数，不切换 mW/uW）。 */
void power_meter_format_power(float power_w, char *buf, size_t buf_len);

/**
 * 清除 NVS 中保存的零点校准表（大/微电流通路），并立即写回。
 * 用于恢复出厂或重新建立电压-零点曲线。
 */
esp_err_t power_meter_clear_zero_cal_nvs(void);

/** 手动零点校准完成回调（在后台任务上下文调用，UI 反馈须 lv_async_call 投递到 LVGL 线程）。 */
typedef void (*power_meter_zero_cal_done_cb_t)(esp_err_t err, void *user_data);

/**
 * 启动手动零点校准（非阻塞）：对当前活动通路采样空载电流，按母线电压写入校零表并立即保存 NVS。
 * 调用前请移除负载；若已有校准任务在运行则返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t power_meter_start_manual_zero_cal(power_meter_zero_cal_done_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* POWER_METER_H */
