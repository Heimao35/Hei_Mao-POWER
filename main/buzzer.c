/**
 * @file buzzer.c
 * @brief GPIO17 无源蜂鸣器：LEDC PWM + esp_timer 非阻塞节拍。
 */
#include "buzzer.h"

#include "audio_volume.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "buzzer"

#define BUZZER_LEDC_TIMER   LEDC_TIMER_1
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1
#define BUZZER_FREQ_HZ      3000
#define BUZZER_DUTY_ON      512

#define BEEP_ON_MS  60
#define BEEP_OFF_MS 60

static esp_timer_handle_t s_timer;
static bool                 s_inited;
static bool                 s_tone_on;
static uint8_t              s_beeps_remaining;

static void buzzer_set_tone(bool on)
{
    const uint32_t duty = on ? BUZZER_DUTY_ON : 0;
    (void)ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    (void)ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void schedule_timer(uint32_t ms)
{
    if (!s_timer) {
        return;
    }
    (void)esp_timer_stop(s_timer);
    (void)esp_timer_start_once(s_timer, (uint64_t)ms * 1000ULL);
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (s_tone_on) {
        buzzer_set_tone(false);
        s_tone_on = false;
        if (s_beeps_remaining > 0) {
            schedule_timer(BEEP_OFF_MS);
        }
        return;
    }

    buzzer_set_tone(true);
    s_tone_on = true;
    if (s_beeps_remaining > 0) {
        s_beeps_remaining--;
    }
    schedule_timer(BEEP_ON_MS);
}

void buzzer_stop(void)
{
    if (s_timer) {
        (void)esp_timer_stop(s_timer);
    }
    s_tone_on         = false;
    s_beeps_remaining = 0;
    buzzer_set_tone(false);
}

static void buzzer_play_beeps(uint8_t count, bool force)
{
    if (!s_inited || count == 0) {
        return;
    }
    if (!force && !audio_volume_buzzer_enabled()) {
        return;
    }

    buzzer_stop();
    s_beeps_remaining = (uint8_t)(count - 1U);
    s_tone_on         = true;
    buzzer_set_tone(true);
    schedule_timer(BEEP_ON_MS);
}

esp_err_t buzzer_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = BUZZER_LEDC_MODE,
        .timer_num       = BUZZER_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = BUZZER_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t ch_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .channel    = BUZZER_LEDC_CHANNEL,
        .timer_sel  = BUZZER_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BUZZER_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t targs = {
        .callback = timer_cb,
        .name     = "buzzer",
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "无源蜂鸣器 GPIO%d @ %dHz", (int)BUZZER_GPIO, BUZZER_FREQ_HZ);
    return ESP_OK;
}

void buzzer_play_pattern(buzzer_pattern_t pattern)
{
    buzzer_play_beeps((uint8_t)pattern, false);
}

void buzzer_play_pattern_force(buzzer_pattern_t pattern)
{
    buzzer_play_beeps((uint8_t)pattern, true);
}
