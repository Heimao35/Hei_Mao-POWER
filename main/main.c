//2026-5-5 10:56
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_cst820.h"
#include "ui/menu/ui_menu_app.h"
#include "net_wifi.h"
#include "app_time.h"
#include "display_brightness.h"
#include "pmic_axp.h"
#include "led_rgb.h"

static const char *TAG = "example";
static SemaphoreHandle_t lvgl_mux = NULL;
static SemaphoreHandle_t te_sem = NULL; 
esp_lcd_touch_handle_t tp = NULL;
#define LCD_HOST    SPI2_HOST
#define TOUCH_HOST  I2C_NUM_0
#define LCD_BIT_PER_PIXEL       (16)
#define EXAMPLE_LCD_H_RES              460
#define EXAMPLE_LCD_V_RES              460

#define SDA               (GPIO_NUM_1)
#define SCL               (GPIO_NUM_2)
#define TOUCH_RST         (GPIO_NUM_3)
#define TOUCH_INT         (GPIO_NUM_4)
#define AMOLED_TE         (GPIO_NUM_5)
#define AMOLED_RST        (GPIO_NUM_6)
#define AMOLED_CS         (GPIO_NUM_7)
#define AMOLED_CLK        (GPIO_NUM_8)
#define AMOLED_D0         (GPIO_NUM_9)
#define AMOLED_D1         (GPIO_NUM_10)
#define AMOLED_D2         (GPIO_NUM_11)
#define AMOLED_D3         (GPIO_NUM_12)
#define AMOLED_PWREN      (GPIO_NUM_13)

/* Shorter strip + internal DMA only: SPI panel_io rejects PSRAM source; smaller buffers help dual DMA fit with Wi-Fi. */
#define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES / 8)
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (12 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     2

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 80},   
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x01}, 1, 0},
    {0x36, (uint8_t []){0xc0}, 1, 2},
    {0x53, (uint8_t []){0x20}, 1, 1},
    {0x63, (uint8_t []){0xFF}, 1, 1},
    {0x51, (uint8_t []){0x00}, 1, 1},
    {0x2A, (uint8_t []){0x00,0x0A,0x01,0xD5}, 4, 0},
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xCB}, 4, 0},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0x9F}, 1, 0},
};

static void IRAM_ATTR te_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(te_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    const int offsetx1 = area->x1 + 0x0A;
    const int offsetx2 = area->x2 + 0x0A;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    if (xSemaphoreTake(te_sem, pdMS_TO_TICKS(60)) != pdTRUE) {
        ESP_LOGW(TAG, "TE wait timeout, flushing without sync");
    }

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void example_lvgl_update_cb(lv_disp_drv_t *drv)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    switch (drv->rotated) 
    {
    case LV_DISP_ROT_NONE:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISP_ROT_90:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISP_ROT_180:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    case LV_DISP_ROT_270:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);
        break;
    }
}

void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    assert(tp);
    uint16_t tp_x;
    uint16_t tp_y;
    uint8_t tp_cnt = 0;
    esp_lcd_touch_read_data(tp);
    bool tp_pressed = esp_lcd_touch_get_coordinates(tp, &tp_x, &tp_y, NULL, &tp_cnt, 1);
    if (tp_pressed && tp_cnt > 0) 
    {
        data->point.x = tp_x;
        data->point.y = tp_y;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "Touch position: %d,%d", tp_x, tp_y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static bool example_lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void example_lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    while (1) 
    {
        if (example_lvgl_lock(-1)) 
        {
            task_delay_ms = lv_timer_handler();
            example_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) 
        {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) 
        {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

void app_main(void)
{
    te_sem = xSemaphoreCreateBinary();
    assert(te_sem != NULL && "Failed to create TE semaphore");
    
    gpio_config_t te_conf = {
        .pin_bit_mask = (1ULL << AMOLED_TE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&te_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(AMOLED_TE, te_isr_handler, NULL));
    xSemaphoreTake(te_sem, 0);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AMOLED_PWREN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK){
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
    }

    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(AMOLED_CLK,
                                                                 AMOLED_D0,
                                                                 AMOLED_D1,
                                                                 AMOLED_D2,
                                                                 AMOLED_D3,
                                                                 EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(AMOLED_CS,
                                                                                example_notify_lvgl_flush_ready,
                                                                                &disp_drv);
    sh8601_vendor_config_t vendor_config = 
    {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = 
        {
            .use_qspi_interface = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = 
    {
        .reset_gpio_num = AMOLED_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_LOGI(TAG, "Install CO5300 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    display_brightness_init(io_handle, true);
    (void)display_brightness_boot_apply();

    ESP_LOGI(TAG, "Enabling TE signal output");
    
    const i2c_config_t i2c_conf = 
    {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400 * 1000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0));
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle));
    const esp_lcd_touch_config_t tp_cfg = 
    {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .levels = 
        {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = 
        {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst820(tp_io_handle, &tp_cfg, &tp));
    
    lv_init();
    const size_t buf_pixels = (size_t)EXAMPLE_LCD_H_RES * (size_t)EXAMPLE_LVGL_BUF_HEIGHT;
    const size_t buf_bytes  = buf_pixels * sizeof(lv_color_t);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    assert(buf1);
    /* Second buffer must stay in internal DMA RAM: SPI color TX DMA cannot use PSRAM here (tx_color fails + WDT). */
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    if (buf2) {
        lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_pixels);
    } else {
        ESP_LOGW(TAG, "LVGL: no DMA buf2, using single buffer");
        lv_disp_draw_buf_init(&disp_buf, buf1, NULL, buf_pixels);
    }
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.drv_update_cb = example_lvgl_update_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = 
    {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    indev_drv.user_data = tp;
    lv_indev_drv_register(&indev_drv);
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    //disp_drv.rotated = LV_DISP_ROT_90;

    led_rgb_init();

    net_wifi_init();
    app_time_init();
    net_wifi_start_saved_reconnect_background();

    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
    //xTaskCreate(PMIC_GetBatteryLevel, "PMIC", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, 1, NULL);

    ESP_LOGI(TAG, "Display UI");
    if (example_lvgl_lock(-1)) 
    {
        ui_menu_app_init();
        example_lvgl_unlock();
    }
    
}