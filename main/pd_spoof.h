/**
 * @file pd_spoof.h
 * @brief CH224Q PD 诱骗：I2C 档位、MOS 控制脚 (GPIO16)、PG 状态 (GPIO15)、物理按键 (GPIO0)。
 */
#ifndef PD_SPOOF_H
#define PD_SPOOF_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PD_SPOOF_CC_GPIO
#define PD_SPOOF_CC_GPIO  GPIO_NUM_16
#endif

#ifndef PD_SPOOF_PG_GPIO
#define PD_SPOOF_PG_GPIO  GPIO_NUM_15
#endif

#ifndef PD_SPOOF_BTN_GPIO
#define PD_SPOOF_BTN_GPIO GPIO_NUM_0
#endif

typedef enum {
    PD_SPOOF_VOLT_5V  = 5,
    PD_SPOOF_VOLT_9V  = 9,
    PD_SPOOF_VOLT_12V = 12,
    PD_SPOOF_VOLT_15V = 15,
    PD_SPOOF_VOLT_20V = 20,
    PD_SPOOF_VOLT_28V = 28,
} pd_spoof_voltage_t;

typedef struct {
    bool                 enabled;
    bool                 cc_connected;
    pd_spoof_voltage_t   selected_voltage;
    pd_spoof_voltage_t   configured_voltage;
    float                max_current_a;
    bool                 pg_ok;
    bool                 pd_active;
    bool                 chip_present;
} pd_spoof_status_t;

typedef enum {
    PD_SPOOF_EVT_TOGGLED = 0,
    PD_SPOOF_EVT_VOLTAGE,
} pd_spoof_event_id_t;

typedef struct {
    pd_spoof_event_id_t id;
    bool                enabled;
    pd_spoof_voltage_t  voltage;
    bool                from_button;
    bool                from_remote;
} pd_spoof_event_t;

typedef void (*pd_spoof_event_cb_t)(const pd_spoof_event_t *evt, void *user_data);

/** 初始化 GPIO / I2C；MOS 控制脚保持上拉高电平，默认 PD 关闭且芯片请求 5V。 */
esp_err_t pd_spoof_init(i2c_port_t port);

void pd_spoof_set_event_cb(pd_spoof_event_cb_t cb, void *user_data);

/** 预选档位；PD 关闭时仅更新选中项，PD 开启时立即关闭并写 5V（需再次开启才诱骗）。 */
esp_err_t pd_spoof_select_voltage(pd_spoof_voltage_t voltage);

/** 启动/关闭 PD；关闭时写 5V，开启时写当前预选档位。 */
esp_err_t pd_spoof_set_enabled(bool enable);

/** 远程/MQTT 启停 PD，触发与物理按键一致的 UI 反馈（弹窗、开关动画、蜂鸣）。 */
esp_err_t pd_spoof_set_enabled_remote(bool enable);

/** 静默启停 PD（不触发事件），供远程复合命令等内部流程使用。 */
esp_err_t pd_spoof_set_enabled_quiet(bool enable);

/** 仅更新预选档位（PD 须已关闭），不触发事件。 */
esp_err_t pd_spoof_preset_voltage(pd_spoof_voltage_t voltage);

/** 切换启停（物理按键 / UI 开关）。 */
esp_err_t pd_spoof_toggle(bool from_button);

esp_err_t pd_spoof_get_status(pd_spoof_status_t *out);

/** 读取 PG、协议状态、最大电流（需持有 I2C 锁）。 */
esp_err_t pd_spoof_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PD_SPOOF_H */
