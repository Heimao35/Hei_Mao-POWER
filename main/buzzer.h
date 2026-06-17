/**
 * @file buzzer.h
 * @brief GPIO17 无源蜂鸣器，非阻塞节拍播放。
 */
#ifndef BUZZER_H
#define BUZZER_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUZZER_GPIO GPIO_NUM_17

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

void buzzer_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */
