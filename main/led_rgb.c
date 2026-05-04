#include "led_rgb.h"
#include "led_strip_encoder.h"

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define LED_NUM 2
#define LED_GPIO 38
#define RMT_RESOLUTION_HZ 10000000
#define LED_BRIGHTNESS 40

static const char *TAG = "led_rgb";

static uint8_t led_data[LED_NUM * 3];
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_led_encoder;
static TaskHandle_t s_led_task;
static bool s_inited;
static portMUX_TYPE s_led_mux = portMUX_INITIALIZER_UNLOCKED;

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 60;
    uint16_t remainder = (h % 60) * 256 / 60;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
}

static void push_led_buffer(void)
{
    if (!s_chan || !s_led_encoder) {
        return;
    }
    uint8_t buf[sizeof(led_data)];
    portENTER_CRITICAL(&s_led_mux);
    memcpy(buf, led_data, sizeof(buf));
    portEXIT_CRITICAL(&s_led_mux);
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_led_encoder, buf, sizeof(buf), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_transmit: %s", esp_err_to_name(err));
        return;
    }
    (void)rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(500));
}

static void set_all_pixels(uint8_t red, uint8_t green, uint8_t blue)
{
    portENTER_CRITICAL(&s_led_mux);
    for (int i = 0; i < LED_NUM; i++) {
        led_data[i * 3 + 0] = green;
        led_data[i * 3 + 1] = red;
        led_data[i * 3 + 2] = blue;
    }
    portEXIT_CRITICAL(&s_led_mux);
    push_led_buffer();
}

static void led_rgb_task(void *arg)
{
    (void)arg;
    uint16_t hue = 0;
    for (;;) {
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, LED_BRIGHTNESS, &r, &g, &b);
        set_all_pixels(r, g, b);
        hue = (uint16_t)((hue + 2) % 360);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

void led_rgb_init(void)
{
    if (s_inited) {
        return;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_chan));
    ESP_ERROR_CHECK(rmt_enable(s_chan));

    led_strip_encoder_config_t enc_cfg = { .resolution = RMT_RESOLUTION_HZ };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc_cfg, &s_led_encoder));

    set_all_pixels(0, 0, 0);
    s_inited = true;
    ESP_LOGI(TAG, "WS2812 x%d on GPIO%d", LED_NUM, LED_GPIO);
}

void led_rgb_set_rainbow_running(bool on)
{
    if (!s_inited) {
        led_rgb_init();
    }
    if (on) {
        if (s_led_task == NULL) {
            if (xTaskCreate(led_rgb_task, "led_rgb", 3072, NULL, 5, &s_led_task) != pdPASS) {
                ESP_LOGE(TAG, "xTaskCreate failed");
            }
        }
    } else {
        if (s_led_task != NULL) {
            TaskHandle_t t = s_led_task;
            s_led_task = NULL;
            vTaskDelete(t);
        }
        set_all_pixels(0, 0, 0);
    }
}

bool led_rgb_is_rainbow_running(void)
{
    return s_led_task != NULL;
}
