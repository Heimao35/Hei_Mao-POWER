/**
 * @file net_wifi.c
 */
#include "net_wifi.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "net_wifi";

#define NVS_NS   "wifi_user"
#define NVS_SSID "ssid"
#define NVS_PASS "pass"

static esp_netif_t      *s_sta_netif;
static bool              s_wifi_started;
static SemaphoreHandle_t s_wifi_conn_mx;

/** Defer STA so LCD/LVGL init are not contending with Wi-Fi on the same boot window. */
#define NET_WIFI_BOOT_RECONNECT_DELAY_MS  3000U
#define NET_WIFI_BOOT_TASK_STACK          4096U
#define NET_WIFI_BOOT_TASK_PRIO           1

typedef struct {
    net_wifi_scan_cb_t    cb;
    void                 *ud;
    net_wifi_ap_info_t    aps[NET_WIFI_SCAN_MAX_APS];
    int                   n;
    esp_err_t             err;
} scan_job_t;

typedef struct {
    net_wifi_connect_cb_t cb;
    void                 *ud;
    bool                  ok;
    char                  ssid[NET_WIFI_SSID_MAX_LEN + 1];
    char                  pass[NET_WIFI_PASS_MAX_LEN + 1];
} conn_work_t;

static esp_err_t nvs_flash_ensure(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_ERROR(nvs_flash_ensure(), TAG, "nvs");
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t creds_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    if (!ssid || ssid_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    if (pass && pass_sz) {
        pass[0] = '\0';
    }
    ESP_RETURN_ON_ERROR(nvs_flash_ensure(), TAG, "nvs");
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t req = ssid_sz;
    err = nvs_get_str(h, NVS_SSID, ssid, &req);
    if (err == ESP_OK && pass && pass_sz) {
        req = pass_sz;
        err = nvs_get_str(h, NVS_PASS, pass, &req);
    }
    nvs_close(h);
    return err;
}

static void scan_deliver_cb(void *p)
{
    scan_job_t *job = (scan_job_t *)p;
    if (!job) {
        return;
    }
    if (job->cb) {
        job->cb(job->err, job->aps, job->n, job->ud);
    }
    free(job);
}

static int cmp_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;
    if (ra->rssi > rb->rssi) {
        return -1;
    }
    if (ra->rssi < rb->rssi) {
        return 1;
    }
    return 0;
}

static void scan_task(void *arg)
{
    scan_job_t *job = (scan_job_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }
    job->err = ESP_FAIL;
    job->n   = 0;
    if (!s_wifi_started) {
        job->err = ESP_ERR_INVALID_STATE;
        goto finish;
    }

    wifi_scan_config_t sc = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        job->err = e;
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(e));
        goto finish;
    }

    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    uint16_t getn = total;
    if (getn > NET_WIFI_SCAN_MAX_APS) {
        getn = NET_WIFI_SCAN_MAX_APS;
    }
    if (getn == 0) {
        job->n   = 0;
        job->err = ESP_OK;
        ESP_LOGI(TAG, "scan: 0 APs");
        goto finish;
    }

    wifi_ap_record_t *rec = (wifi_ap_record_t *)calloc(getn, sizeof(wifi_ap_record_t));
    if (!rec) {
        job->err = ESP_ERR_NO_MEM;
        goto finish;
    }
    e = esp_wifi_scan_get_ap_records(&getn, rec);
    if (e != ESP_OK) {
        job->err = e;
        ESP_LOGW(TAG, "get_ap_records: %s", esp_err_to_name(e));
        free(rec);
        goto finish;
    }
    qsort(rec, getn, sizeof(rec[0]), cmp_rssi_desc);
    int n = (int)getn;
    if (n > NET_WIFI_SCAN_MAX_APS) {
        n = NET_WIFI_SCAN_MAX_APS;
    }
    job->n = n;
    for (int i = 0; i < n; i++) {
        memset(job->aps[i].ssid, 0, sizeof(job->aps[i].ssid));
        memcpy(job->aps[i].ssid, rec[i].ssid, sizeof(rec[i].ssid));
        job->aps[i].ssid[NET_WIFI_SSID_MAX_LEN] = '\0';
        job->aps[i].rssi                        = rec[i].rssi;
        job->aps[i].authmode                    = rec[i].authmode;
    }
    free(rec);
    job->err = ESP_OK;

finish:
    if (lv_async_call(scan_deliver_cb, job) != LV_RES_OK) {
        free(job);
    }
    vTaskDelete(NULL);
}

void net_wifi_scan_request(net_wifi_scan_cb_t cb, void *user_data)
{
    if (!cb || !s_wifi_started) {
        if (cb) {
            cb(ESP_ERR_INVALID_STATE, NULL, 0, user_data);
        }
        return;
    }
    scan_job_t *job = (scan_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        cb(ESP_ERR_NO_MEM, NULL, 0, user_data);
        return;
    }
    job->cb = cb;
    job->ud = user_data;
    if (xTaskCreate(scan_task, "wifi_scan", 6144, job, 5, NULL) != pdPASS) {
        free(job);
        cb(ESP_ERR_NO_MEM, NULL, 0, user_data);
    }
}

static bool sta_ip_ready(void)
{
    if (!s_sta_netif) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

static void conn_deliver_work_cb(void *p)
{
    conn_work_t *job = (conn_work_t *)p;
    if (!job) {
        return;
    }
    if (job->cb) {
        job->cb(job->ok, job->ud);
    }
    free(job);
}

static void connect_work_task(void *arg)
{
    conn_work_t *job = (conn_work_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }
    job->ok = false;
    bool mx_held = false;
    if (!s_wifi_started) {
        goto finish;
    }
    if (s_wifi_conn_mx) {
        xSemaphoreTake(s_wifi_conn_mx, portMAX_DELAY);
        mx_held = true;
    }

    esp_wifi_disconnect();
    for (int i = 0; i < 50; i++) {
        if (!sta_ip_ready()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, job->ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, job->pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "set_config: %s", esp_err_to_name(e));
        goto finish;
    }
    e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(e));
        goto finish;
    }

    for (int t = 0; t < 200; t++) {
        if (sta_ip_ready()) {
            job->ok = true;
            if (creds_save(job->ssid, job->pass) == ESP_OK) {
                ESP_LOGI(TAG, "saved STA to NVS: \"%s\"", job->ssid);
            } else {
                ESP_LOGW(TAG, "creds_save failed");
            }
            ESP_LOGI(TAG, "connected: %s", job->ssid);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

finish:
    if (mx_held && s_wifi_conn_mx) {
        xSemaphoreGive(s_wifi_conn_mx);
    }
    if (lv_async_call(conn_deliver_work_cb, job) != LV_RES_OK) {
        free(job);
    }
    vTaskDelete(NULL);
}

void net_wifi_connect_request(const char *ssid, const char *password, net_wifi_connect_cb_t cb, void *user_data)
{
    if (!ssid || !cb || !s_wifi_started) {
        if (cb) {
            cb(false, user_data);
        }
        return;
    }
    conn_work_t *job = (conn_work_t *)calloc(1, sizeof(*job));
    if (!job) {
        cb(false, user_data);
        return;
    }
    job->cb = cb;
    job->ud = user_data;
    strncpy(job->ssid, ssid, sizeof(job->ssid) - 1);
    job->ssid[sizeof(job->ssid) - 1] = '\0';
    if (password) {
        strncpy(job->pass, password, sizeof(job->pass) - 1);
        job->pass[sizeof(job->pass) - 1] = '\0';
    }
    if (xTaskCreate(connect_work_task, "wifi_conn", 8192, job, 5, NULL) != pdPASS) {
        free(job);
        cb(false, user_data);
    }
}

bool net_wifi_sta_has_ip(void)
{
    return sta_ip_ready();
}

void net_wifi_boot_try_saved(void)
{
    char ssid[NET_WIFI_SSID_MAX_LEN + 1];
    char pass[NET_WIFI_PASS_MAX_LEN + 1];
    if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK || ssid[0] == '\0') {
        return;
    }
    if (!s_wifi_started) {
        return;
    }
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (s_wifi_conn_mx && xSemaphoreTake(s_wifi_conn_mx, pdMS_TO_TICKS(12000)) != pdTRUE) {
        ESP_LOGW(TAG, "boot sta: radio busy, skip reconnect");
        return;
    }
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e == ESP_OK) {
        e = esp_wifi_connect();
    }
    if (s_wifi_conn_mx) {
        xSemaphoreGive(s_wifi_conn_mx);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "boot sta connect: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "boot sta: trying \"%s\"", ssid);
    }
}

static void wifi_boot_reconnect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(NET_WIFI_BOOT_RECONNECT_DELAY_MS));
    net_wifi_boot_try_saved();
    vTaskDelete(NULL);
}

void net_wifi_start_saved_reconnect_background(void)
{
    static bool s_boot_task_created;
    if (s_boot_task_created) {
        return;
    }
    if (xTaskCreate(wifi_boot_reconnect_task, "wifi_boot_sta", NET_WIFI_BOOT_TASK_STACK, NULL, NET_WIFI_BOOT_TASK_PRIO,
                    NULL) != pdPASS) {
        ESP_LOGW(TAG, "wifi_boot_sta task failed, trying saved STA inline");
        net_wifi_boot_try_saved();
        return;
    }
    s_boot_task_created = true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGD(TAG, "STA_START");
    }
}

void net_wifi_init(void)
{
    static bool inited;
    if (inited) {
        return;
    }
    inited = true;

    esp_err_t ne = esp_netif_init();
    if (ne != ESP_OK && ne != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ne);
    }
    ne = esp_event_loop_create_default();
    if (ne != ESP_OK && ne != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ne);
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    s_wifi_conn_mx = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "wifi sta ready");
}
