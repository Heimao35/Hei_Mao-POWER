/**
 * @file rtc_pcf85063.c
 */
#include "rtc_pcf85063.h"
#include "app_time.h"
#include "i2c_bus_share.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "rtc_pcf85063";

#define PCF85063_I2C_ADDR_7BIT 0x51

#define REG_CTRL1   0x00U
#define REG_CTRL2   0x01U
#define REG_SECONDS 0x04U

/* Control_1 */
#define CTRL1_STOP  (1U << 5)

#define RTC_CAL_PERIOD_US (60ULL * 1000000ULL)

static i2c_port_t         s_port = I2C_NUM_MAX;
static esp_timer_handle_t s_cal_timer;
static bool              s_present;

static uint8_t bin2bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int bcd2bin(uint8_t b)
{
    return (int)((b >> 4) * 10 + (b & 0x0FU));
}

static esp_err_t i2c_write_reg_burst(uint8_t start_reg, const uint8_t *data, size_t len)
{
    if (!data || len > 16U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[17];
    buf[0] = start_reg;
    memcpy(&buf[1], data, len);
    return i2c_master_write_to_device(s_port, PCF85063_I2C_ADDR_7BIT, buf, len + 1U, pdMS_TO_TICKS(50));
}

static esp_err_t i2c_read_regs(uint8_t start_reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(s_port, PCF85063_I2C_ADDR_7BIT, &start_reg, 1U, data, len, pdMS_TO_TICKS(50));
}

/**
 * Write 7-byte time block at REG_SECONDS using STOP around the burst.
 * @pre I2C bus mutex held (@ref i2c_bus_share_lock).
 */
static esp_err_t write_datetime_block_locked(const uint8_t block[7])
{
    uint8_t c1 = 0;
    esp_err_t err = i2c_read_regs(REG_CTRL1, &c1, 1U);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t c1_stop = (uint8_t)(c1 | CTRL1_STOP);
    err = i2c_write_reg_burst(REG_CTRL1, &c1_stop, 1U);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_write_reg_burst(REG_SECONDS, block, 7U);
    if (err == ESP_OK) {
        err = i2c_write_reg_burst(REG_CTRL1, &c1, 1U);
    } else {
        (void)i2c_write_reg_burst(REG_CTRL1, &c1, 1U);
    }
    return err;
}

/** Decode raw[7] from REG_SECONDS..YEARS; @p sec uses masked seconds (OS ignored). */
static bool decode_raw_time(const uint8_t raw[7], int *sec, int *min, int *hour, int *mday, int *wday, int *mon, int *year2)
{
    const int s  = bcd2bin((uint8_t)(raw[0] & 0x7FU));
    const int mi = bcd2bin(raw[1]);
    const int h  = bcd2bin((uint8_t)(raw[2] & 0x3FU));
    const int d  = bcd2bin((uint8_t)(raw[3] & 0x3FU));
    const int wd = (int)(raw[4] & 0x07U);
    const int mo = bcd2bin((uint8_t)(raw[5] & 0x1FU));
    const int y2 = bcd2bin(raw[6]);

    if (s < 0 || s > 59 || mi < 0 || mi > 59 || h < 0 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12 || wd < 0 || wd > 6 || y2 < 0 ||
        y2 > 99) {
        return false;
    }
    *sec   = s;
    *min   = mi;
    *hour  = h;
    *mday  = d;
    *wday  = wd;
    *mon   = mo;
    *year2 = y2;
    return true;
}

/**
 * After POR, the Seconds register has OS=1 by default (Table 8) until cleared by a write with OS=0.
 * That is not necessarily a stopped oscillator. Rewrite the same counters to clear OS.
 * @pre I2C bus mutex held.
 */
static esp_err_t clear_por_os_flag_locked(const uint8_t raw[7])
{
    int sec, min, hour, mday, wday, mon, y2;
    if (!decode_raw_time(raw, &sec, &min, &hour, &mday, &wday, &mon, &y2)) {
        return ESP_FAIL;
    }

    const uint8_t block[7] = {
        (uint8_t)(bin2bcd(sec) & 0x7FU),
        bin2bcd(min),
        bin2bcd(hour),
        bin2bcd(mday),
        (uint8_t)(wday & 0x07),
        bin2bcd(mon),
        bin2bcd(y2),
    };
    return write_datetime_block_locked(block);
}

static bool rtc_reader_bridge(struct tm *out)
{
    return rtc_pcf85063_read_local_tm(out) == ESP_OK;
}

static void rtc_cal_worker(void *arg)
{
    (void)arg;
    if (!s_present || !app_time_wall_clock_synced()) {
        vTaskDelete(NULL);
        return;
    }
    esp_err_t e = rtc_pcf85063_write_system_local_time();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "RTC cal: write failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGD(TAG, "RTC cal: updated from SNTP-backed system time");
    }
    vTaskDelete(NULL);
}

static void cal_timer_cb(void *arg)
{
    (void)arg;
    if (!s_present || !app_time_wall_clock_synced()) {
        return;
    }
    if (xTaskCreate(rtc_cal_worker, "rtc_cal", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "RTC cal: failed to spawn task");
    }
}

static void ensure_cal_timer(void)
{
    if (s_cal_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &cal_timer_cb,
        .name = "rtc_cal_min",
    };
    if (esp_timer_create(&args, &s_cal_timer) != ESP_OK) {
        ESP_LOGW(TAG, "RTC cal: esp_timer_create failed");
        s_cal_timer = NULL;
    }
}

/**
 * SNTP sync: (1) write RTC once immediately from libc local time; (2) arm 1-minute periodic calibration.
 * Periodic timer is not restarted if already running (avoids SNTP re-sync resetting phase).
 */
static void on_wall_sync_hook(void)
{
    if (!s_present) {
        return;
    }

    /* Immediate sync: SNTP has already set libc time before this hook runs. */
    if (xTaskCreate(rtc_cal_worker, "rtc_cal_now", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Immediate RTC sync: failed to spawn task");
    }

    ensure_cal_timer();
    if (!s_cal_timer) {
        return;
    }
    if (esp_timer_is_active(s_cal_timer)) {
        return;
    }
    const esp_err_t e = esp_timer_start_periodic(s_cal_timer, RTC_CAL_PERIOD_US);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "RTC cal: periodic start failed: %s", esp_err_to_name(e));
    }
}

static void on_net_lost_hook(void)
{
    if (s_cal_timer) {
        (void)esp_timer_stop(s_cal_timer);
    }
}

static void int_pin_init(gpio_num_t int_gpio)
{
    if (int_gpio == GPIO_NUM_NC) {
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << (unsigned)int_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "INT GPIO config failed: %s", esp_err_to_name(e));
    }
}

esp_err_t rtc_pcf85063_init(i2c_port_t i2c_port, gpio_num_t int_gpio)
{
    if (i2c_port >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_port = i2c_port;
    s_present = false;

    int_pin_init(int_gpio);

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t c1probe = 0;
    esp_err_t err = i2c_read_regs(REG_CTRL1, &c1probe, 1U);
    if (err == ESP_OK) {
        uint8_t raw[7];
        err = i2c_read_regs(REG_SECONDS, raw, sizeof(raw));
        if (err == ESP_OK && (raw[0] & 0x80U)) {
            const esp_err_t e2 = clear_por_os_flag_locked(raw);
            if (e2 == ESP_OK) {
                ESP_LOGI(TAG, "Cleared default OS bit in Seconds (POR state per datasheet Table 8)");
            } else {
                ESP_LOGW(TAG, "OS bit set but clear rewrite failed: %s", esp_err_to_name(e2));
            }
        }
    }

    i2c_bus_share_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not detected (0x%02X): %s", PCF85063_I2C_ADDR_7BIT, esp_err_to_name(err));
        return err;
    }

    s_present = true;
    app_time_register_rtc_reader(rtc_reader_bridge);
    app_time_register_wall_sync_handler(on_wall_sync_hook);
    app_time_register_net_lost_handler(on_net_lost_hook);
    ESP_LOGI(TAG, "PCF85063 ready on I2C port %d", (int)i2c_port);
    return ESP_OK;
}

bool rtc_pcf85063_present(void)
{
    return s_present;
}

esp_err_t rtc_pcf85063_read_local_tm(struct tm *out)
{
    if (!out || !s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t raw[7];
    esp_err_t err = i2c_read_regs(REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        i2c_bus_share_unlock();
        return err;
    }

    i2c_bus_share_unlock();

    /*
     * Bit OS in Seconds is set at POR default (Table 8) until cleared by a write; it can also indicate
     * a real stop. We always decode masked seconds for the UI; init attempts to clear default OS once.
     */
    int sec, min, hour, mday, wday, mon, year2;
    if (!decode_raw_time(raw, &sec, &min, &hour, &mday, &wday, &mon, &year2)) {
        return ESP_FAIL;
    }

    out->tm_sec  = sec;
    out->tm_min  = min;
    out->tm_hour = hour;
    out->tm_mday = mday;
    out->tm_mon  = mon - 1;
    out->tm_year = year2 + 100; /* 2000–2099 */
    out->tm_wday = wday;
    return ESP_OK;
}

esp_err_t rtc_pcf85063_write_system_local_time(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t t = time(NULL);
    struct tm local;
    localtime_r(&t, &local);

    const int year_full = local.tm_year + 1900;
    if (year_full < 2000 || year_full > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t sec_b  = (uint8_t)(bin2bcd(local.tm_sec) & 0x7FU); /* OS = 0 */
    const uint8_t min_b  = bin2bcd(local.tm_min);
    const uint8_t hour_b = bin2bcd(local.tm_hour);
    const uint8_t day_b  = bin2bcd(local.tm_mday);
    const uint8_t wday_b = (uint8_t)(local.tm_wday & 0x07);
    const uint8_t mon_b  = bin2bcd(local.tm_mon + 1);
    const uint8_t year_b = bin2bcd(year_full % 100);

    if (!i2c_bus_share_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t block[7] = {sec_b, min_b, hour_b, day_b, wday_b, mon_b, year_b};
    const esp_err_t err = write_datetime_block_locked(block);

    i2c_bus_share_unlock();
    return err;
}
