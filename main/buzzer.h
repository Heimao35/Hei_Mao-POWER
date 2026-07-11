/**
 * @file buzzer.h
 * @brief GPIO14 无源蜂鸣器，非阻塞节拍/旋律播放。
 */
#ifndef BUZZER_H
#define BUZZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUZZER_GPIO GPIO_NUM_14

/** 单音节：freq_hz=音高(Hz)，duration_ms=时长(ms)；freq_hz=0 为休止。 */
typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} buzzer_note_t;

typedef enum {
    BUZZER_PATTERN_PD_ON = 3,
    BUZZER_PATTERN_PD_OFF = 2,
    BUZZER_PATTERN_CONFIRM = 3,
    BUZZER_PATTERN_RANGE_UP = 3,
    BUZZER_PATTERN_RANGE_DOWN = 2,
} buzzer_pattern_t;

esp_err_t buzzer_init(void);

/** 受静音开关控制；若正在播放则重新开始。 */
void buzzer_play_pattern(buzzer_pattern_t pattern);

/** 忽略静音开关（用于刚关闭静音时的确认音）。 */
void buzzer_play_pattern_force(buzzer_pattern_t pattern);

/**
 * 非阻塞播放旋律。notes 在播放完成前须保持有效（可用 static 常量表）。
 * @param force true 时忽略静音开关。
 */
void buzzer_play_melody(const buzzer_note_t *notes, size_t count, bool force);

/** 开机动画配套五音节提示音（定义见 buzzer.c 顶部 s_boot_melody）。 */
void buzzer_play_boot_melody(void);

/** 手动零点校准完成：两声快速提示音（音调/时长见 buzzer.c s_zero_cal_melody 注释）。 */
void buzzer_play_zero_cal_done(void);

void buzzer_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */
