/**
 * @file audio_volume.h
 * @brief 音量/静音控制接口（NVS 掉电保存）。
 */
#ifndef AUDIO_VOLUME_H
#define AUDIO_VOLUME_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void audio_volume_init(void);
bool audio_volume_is_muted(void);
void audio_volume_set_muted(bool muted);
esp_err_t audio_volume_save_muted(bool muted);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_VOLUME_H */
