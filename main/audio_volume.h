/**
 * @file audio_volume.h
 * @brief Volume 页 Mute 开关：开启时蜂鸣器工作，关闭时不工作（NVS 掉电保存）。
 */
#ifndef AUDIO_VOLUME_H
#define AUDIO_VOLUME_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void audio_volume_init(void);

/** Mute 开关是否开启（开启 = 蜂鸣器启用）。 */
bool audio_volume_buzzer_enabled(void);
void audio_volume_set_buzzer_enabled(bool enabled);
esp_err_t audio_volume_save_buzzer_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_VOLUME_H */
