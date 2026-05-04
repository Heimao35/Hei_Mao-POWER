/**
 * @file app_time.c
 */
#include "app_time.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"

#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "app_time";

static app_time_rtc_read_fn s_rtc_read;
static bool                 s_sntp_started;
static bool                 s_wall_synced;

static void sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_wall_synced = true;
    ESP_LOGI(TAG, "SNTP time sync OK");
}

static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sync_notification_cb);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

static void stop_sntp(void)
{
    if (!s_sntp_started) {
        return;
    }
    esp_sntp_stop();
    s_sntp_started = false;
    s_wall_synced = false;
    ESP_LOGI(TAG, "SNTP stopped (STA lost)");
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        start_sntp();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        stop_sntp();
    }
}

void app_time_init(void)
{
    static bool s_inited;
    if (s_inited) {
        return;
    }
    s_inited = true;

    /* Default TZ for China (POSIX); change via NVS later if needed. */
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL));

    /* STA already up (e.g. fast reconnect) before we registered */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            start_sntp();
        }
    }
}

void app_time_register_rtc_reader(app_time_rtc_read_fn fn)
{
    s_rtc_read = fn;
}

bool app_time_wall_clock_synced(void)
{
    return s_wall_synced;
}

bool app_time_local_tm(struct tm *out)
{
    if (!out) {
        return false;
    }
    if (s_rtc_read && s_rtc_read(out)) {
        return true;
    }
    if (!s_wall_synced) {
        return false;
    }
    const time_t t = time(NULL);
    if (t < (time_t)1704067200) { /* 2024-01-01 UTC sanity */
        return false;
    }
    localtime_r(&t, out);
    return true;
}
