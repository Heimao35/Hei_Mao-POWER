/**
 * @file net_mqtt.c
 */
#include "net_mqtt.h"

#include "net_wifi.h"
#include "power_meter.h"
#include "pd_spoof.h"
#include "ui_pd_panel.h"
#include "ui_status_bar.h"
#include "rtos_psram.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "net_mqtt";

#define NET_MQTT_TASK_STACK  6144U

static TaskHandle_t s_mqtt_task;

static esp_mqtt_client_handle_t s_client;
static char s_cmd_topic[64];
static char s_telem_topic[64];
static volatile bool s_mqtt_connected;
static volatile bool s_mqtt_suspended;

static void mqtt_ui_sync_async(void *user_data)
{
    (void)user_data;
    ui_pd_panel_sync_from_driver_ex(false);
    pd_spoof_status_t st = {0};
    if (pd_spoof_get_status(&st) == ESP_OK) {
        ui_status_bar_sync_pd(st.enabled);
    }
}

static pd_spoof_voltage_t voltage_from_int(int v)
{
    switch (v) {
    case 5:  return PD_SPOOF_VOLT_5V;
    case 9:  return PD_SPOOF_VOLT_9V;
    case 12: return PD_SPOOF_VOLT_12V;
    case 15: return PD_SPOOF_VOLT_15V;
    case 20: return PD_SPOOF_VOLT_20V;
    case 28: return PD_SPOOF_VOLT_28V;
    default: return PD_SPOOF_VOLT_5V;
    }
}

static bool voltage_is_valid(int v)
{
    return v == 5 || v == 9 || v == 12 || v == 15 || v == 20 || v == 28;
}

static bool json_as_bool(const cJSON *item, bool *out)
{
    if (!item || !out) {
        return false;
    }
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valueint != 0;
        return true;
    }
    return false;
}

static esp_err_t apply_remote_command(cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool        has_enabled = false;
    bool        has_voltage = false;
    bool        pd_enabled  = false;
    int         pd_voltage  = 0;
    esp_err_t   err         = ESP_OK;

    const cJSON *je = cJSON_GetObjectItem(root, "pd_enabled");
    if (je && json_as_bool(je, &pd_enabled)) {
        has_enabled = true;
    }

    const cJSON *jv = cJSON_GetObjectItem(root, "pd_voltage");
    if (jv && cJSON_IsNumber(jv)) {
        pd_voltage = jv->valueint;
        if (!voltage_is_valid(pd_voltage)) {
            ESP_LOGW(TAG, "忽略非法 PD 电压 %d", pd_voltage);
            return ESP_ERR_INVALID_ARG;
        }
        has_voltage = true;
    }

    if (!has_enabled && !has_voltage) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先关 → 改档位 → 再开，避免“改电压时 PD 已关但命令末尾又打开”的时序问题 */
    if (has_enabled && !pd_enabled) {
        err = pd_spoof_set_enabled(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "关闭 PD 失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (has_voltage) {
        err = pd_spoof_select_voltage(voltage_from_int(pd_voltage));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "设置 PD 电压失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (has_enabled && pd_enabled) {
        err = pd_spoof_set_enabled(true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "开启 PD 失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    lv_async_call(mqtt_ui_sync_async, NULL);
    ESP_LOGI(TAG, "已应用远程命令 enabled=%d voltage=%d",
             has_enabled ? (int)pd_enabled : -1,
             has_voltage ? pd_voltage : -1);
    return ESP_OK;
}

static void handle_command_payload(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }

    char *buf = (char *)malloc((size_t)len + 1U);
    if (!buf) {
        return;
    }
    memcpy(buf, data, (size_t)len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "收到命令: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "命令 JSON 解析失败");
        return;
    }

    (void)apply_remote_command(root);
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT 已连接，订阅 %s", s_cmd_topic);
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT 已断开");
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len <= 0 || event->data_len <= 0) {
            break;
        }
        if (event->topic_len == (int)strlen(s_cmd_topic) &&
            strncmp(event->topic, s_cmd_topic, (size_t)event->topic_len) == 0) {
            handle_command_payload(event->data, event->data_len);
        }
        break;
    default:
        break;
    }
}

static int publish_telemetry(void)
{
    if (!s_client || !s_mqtt_connected) {
        return -1;
    }

    power_meter_reading_t pm = {0};
    pd_spoof_status_t     pd = {0};

    if (power_meter_read(&pm) != ESP_OK) {
        ESP_LOGW(TAG, "读取功率计失败");
    }
    if (pd_spoof_get_status(&pd) != ESP_OK) {
        ESP_LOGW(TAG, "读取 PD 状态失败");
    }

    char payload[320];
    int  n = snprintf(payload, sizeof(payload),
                      "{"
                      "\"device_id\":\"%s\","
                      "\"voltage_v\":%.3f,"
                      "\"current_a\":%.6f,"
                      "\"power_w\":%.3f,"
                      "\"pd_enabled\":%s,"
                      "\"pd_voltage\":%d,"
                      "\"pd_pg_ok\":%s"
                      "}",
                      NET_MQTT_DEVICE_ID,
                      (double)pm.voltage_v,
                      (double)pm.current_a,
                      (double)pm.power_w,
                      pd.enabled ? "true" : "false",
                      (int)pd.selected_voltage,
                      pd.pg_ok ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(payload)) {
        return -1;
    }

    const int msg_id = esp_mqtt_client_publish(s_client, s_telem_topic, payload, 0, 0, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "遥测发布失败");
    }
    return msg_id;
}

static bool mqtt_client_start_once(void)
{
    if (s_client) {
        return true;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = NET_MQTT_BROKER_URI,
        .credentials.client_id = NET_MQTT_DEVICE_ID,
        .network.reconnect_timeout_ms = 5000,
        .session.keepalive = 30,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT 客户端创建失败");
        return false;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 事件注册失败: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return false;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return false;
    }

    ESP_LOGI(TAG, "MQTT 客户端已启动 -> %s", NET_MQTT_BROKER_URI);
    return true;
}

static void net_mqtt_task(void *arg)
{
    (void)arg;

    while (!net_wifi_sta_has_ip()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Wi-Fi 已就绪，连接 MQTT Broker");

    while (!mqtt_client_start_once()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!net_wifi_sta_has_ip()) {
            while (!net_wifi_sta_has_ip()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    for (;;) {
        if (s_mqtt_suspended) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!net_wifi_sta_has_ip()) {
            s_mqtt_connected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_client) {
            (void)mqtt_client_start_once();
        } else {
            (void)publish_telemetry();
        }
        vTaskDelay(pdMS_TO_TICKS(NET_MQTT_TELEMETRY_INTERVAL_MS));
    }
}

void net_mqtt_init(void)
{
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "power/%s/command", NET_MQTT_DEVICE_ID);
    snprintf(s_telem_topic, sizeof(s_telem_topic), "power/%s/telemetry", NET_MQTT_DEVICE_ID);

    s_mqtt_task = rtos_task_create_psram(net_mqtt_task, "net_mqtt", NET_MQTT_TASK_STACK, NULL, 1);
    if (!s_mqtt_task) {
        ESP_LOGE(TAG, "创建 MQTT 任务失败");
    }
}

void net_mqtt_suspend(void)
{
    s_mqtt_suspended  = true;
    s_mqtt_connected  = false;
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        ESP_LOGI(TAG, "MQTT 已暂停（Wi-Fi 操作）");
    }
}

void net_mqtt_resume(void)
{
    if (!s_mqtt_suspended) {
        return;
    }
    s_mqtt_suspended = false;
    if (s_client && net_wifi_sta_has_ip()) {
        esp_err_t err = esp_mqtt_client_start(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT 恢复失败: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "MQTT 已恢复");
        }
    }
}
