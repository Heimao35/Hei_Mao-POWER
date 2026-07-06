/**
 * @file buzzer.c
 * @brief GPIO14 无源蜂鸣器：LEDC PWM + esp_timer 非阻塞节拍/旋律。
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

/*
 * ========== 开机动画五音节提示音（在此修改） ==========
 * freq_hz     : 音高，单位 Hz（无源蜂鸣器常用 200~5000；0 = 休止）
 * duration_ms : 该音节持续时长，单位 ms
 *
 * 参考音高：C4=262  D4=294  E4=330  F4=349  G4=392  A4=440  B4=494
 *           C5=523  D5=587  E5=659  F5=698  G5=784  A5=880  B5=988
 */
static const buzzer_note_t s_boot_melody[] = {
    { 723, 140 }, /* 1 */
    { 787, 200 }, /* 2 */
    { 859, 140 }, /* 3 */
    { 787, 200 }, /* 4 */
    { 984, 140 }, /* 5 */
};

typedef enum {
    PLAY_MODE_IDLE = 0,
    PLAY_MODE_BEEP,
    PLAY_MODE_MELODY,
} play_mode_t;

static esp_timer_handle_t s_timer;
static bool               s_inited;
static play_mode_t        s_mode;

static bool        s_tone_on;
static uint8_t     s_beeps_remaining;

static const buzzer_note_t *s_melody_notes;
static size_t               s_melody_count;
static size_t               s_melody_idx;

static void buzzer_set_freq_hz(uint32_t freq_hz)
{
    if (freq_hz < 100U) {
        freq_hz = 100U;
    }
    (void)ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
}

static void buzzer_set_tone(bool on)
{
    const uint32_t duty = on ? BUZZER_DUTY_ON : 0;
    (void)ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    (void)ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static void schedule_timer(uint32_t ms)
{
    if (!s_timer || ms == 0) {
        return;
    }
    (void)esp_timer_stop(s_timer);
    (void)esp_timer_start_once(s_timer, (uint64_t)ms * 1000ULL);
}

static void melody_finish(void)
{
    s_mode         = PLAY_MODE_IDLE;
    s_melody_notes = NULL;
    s_melody_count = 0;
    s_melody_idx   = 0;
    s_tone_on      = false;
    buzzer_set_tone(false);
}

static void melody_step(void)
{
    if (!s_melody_notes || s_melody_idx >= s_melody_count) {
        melody_finish();
        return;
    }

    const buzzer_note_t note = s_melody_notes[s_melody_idx++];
    if (note.freq_hz > 0) {
        buzzer_set_freq_hz(note.freq_hz);
        buzzer_set_tone(true);
        s_tone_on = true;
    } else {
        buzzer_set_tone(false);
        s_tone_on = false;
    }

    const uint32_t ms = note.duration_ms > 0 ? note.duration_ms : 50U;
    schedule_timer(ms);
}

static void beep_timer_cb(void)
{
    if (s_tone_on) {
        buzzer_set_tone(false);
        s_tone_on = false;
        if (s_beeps_remaining > 0) {
            schedule_timer(BEEP_OFF_MS);
        } else {
            s_mode = PLAY_MODE_IDLE;
        }
        return;
    }

    buzzer_set_freq_hz(BUZZER_FREQ_HZ);
    buzzer_set_tone(true);
    s_tone_on = true;
    if (s_beeps_remaining > 0) {
        s_beeps_remaining--;
    }
    schedule_timer(BEEP_ON_MS);
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (s_mode == PLAY_MODE_MELODY) {
        melody_step();
        return;
    }
    if (s_mode == PLAY_MODE_BEEP) {
        beep_timer_cb();
    }
}

void buzzer_stop(void)
{
    if (s_timer) {
        (void)esp_timer_stop(s_timer);
    }
    s_mode            = PLAY_MODE_IDLE;
    s_tone_on         = false;
    s_beeps_remaining = 0;
    s_melody_notes    = NULL;
    s_melody_count    = 0;
    s_melody_idx      = 0;
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
    s_mode            = PLAY_MODE_BEEP;
    s_beeps_remaining = (uint8_t)(count - 1U);
    s_tone_on         = true;
    buzzer_set_freq_hz(BUZZER_FREQ_HZ);
    buzzer_set_tone(true);
    schedule_timer(BEEP_ON_MS);
}

void buzzer_play_melody(const buzzer_note_t *notes, size_t count, bool force)
{
    if (!s_inited || !notes || count == 0) {
        return;
    }
    if (!force && !audio_volume_buzzer_enabled()) {
        return;
    }

    buzzer_stop();
    s_mode         = PLAY_MODE_MELODY;
    s_melody_notes = notes;
    s_melody_count = count;
    s_melody_idx   = 0;
    melody_step();
}

void buzzer_play_boot_melody(void)
{
    buzzer_play_melody(s_boot_melody,
                       sizeof(s_boot_melody) / sizeof(s_boot_melody[0]),
                       true);
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
