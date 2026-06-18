/**
 * @file net_mqtt.c
 */
#include "net_mqtt.h"

#include "net_wifi.h"
#include "power_meter.h"
#include "pd_spoof.h"
#include "rtos_psram.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "ui_status_bar.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "esp_random.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "net_mqtt";

#define NET_MQTT_TASK_STACK      6144U
#define NET_MQTT_TASK_PRIO       4
#define NET_MQTT_RECONNECT_MS    1000
#define NET_MQTT_NETWORK_MS      4000
#define NET_MQTT_IP_SETTLE_MS    200U
#define NET_MQTT_BROKER_POLL_MS  400U

static TaskHandle_t s_mqtt_task;

static esp_mqtt_client_handle_t s_client;
static char s_client_id[40];
static char s_cmd_topic[64];
static char s_telem_topic[64];
static volatile bool s_mqtt_connected;
static volatile bool s_mqtt_suspended;
static volatile bool s_mqtt_reset_pending;
static bool          s_mqtt_bar_on;

static void mqtt_sync_status_bar(void)
{
    const bool on = s_mqtt_connected;
    if (on == s_mqtt_bar_on) {
        return;
    }
    s_mqtt_bar_on = on;
    ui_status_bar_sync_mqtt(on);
}

bool net_mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

static bool parse_broker_endpoint(char *host, size_t host_sz, uint16_t *port)
{
    if (!host || !port) {
        return false;
    }
    const char *uri = NET_MQTT_BROKER_URI;
    if (strncmp(uri, "mqtt://", 7) != 0) {
        return false;
    }
    uri += 7;
    const char *colon = strrchr(uri, ':');
    if (!colon || colon == uri) {
        return false;
    }
    const size_t len = (size_t)(colon - uri);
    if (len >= host_sz) {
        return false;
    }
    memcpy(host, uri, len);
    host[len] = '\0';
    *port = (uint16_t)atoi(colon + 1);
    return (*port > 0);
}

static bool broker_tcp_port_open(const char *host, uint16_t port, int timeout_ms)
{
    if (!host || host[0] == '\0' || port == 0) {
        return false;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return false;
    }

    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const bool ok = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(sock);
    return ok;
}

static void mqtt_client_destroy(void)
{
    if (!s_client) {
        return;
    }
    esp_mqtt_client_handle_t client = s_client;
    s_client = NULL;
    s_mqtt_connected = false;
    mqtt_sync_status_bar();
    (void)esp_mqtt_client_stop(client);
    (void)esp_mqtt_client_destroy(client);
}

static void wait_for_broker_tcp(const char *host, uint16_t port)
{
    if (broker_tcp_port_open(host, port, 800)) {
        return;
    }
    ESP_LOGI(TAG, "等待 Broker %s:%u 就绪...", host, (unsigned)port);
    while (!s_mqtt_suspended) {
        if (!net_wifi_sta_has_ip()) {
            (void)net_wifi_wait_sta_ip(pdMS_TO_TICKS(200));
            continue;
        }
        if (broker_tcp_port_open(host, port, 800)) {
            ESP_LOGI(TAG, "Broker %s:%u 已就绪", host, (unsigned)port);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(NET_MQTT_BROKER_POLL_MS));
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

    pd_spoof_status_t cur = {0};
    if (pd_spoof_get_status(&cur) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool vol_change = has_voltage && (int)cur.selected_voltage != pd_voltage;
    const bool keep_on_change_v = has_enabled && pd_enabled && vol_change;

    if (keep_on_change_v) {
        /* PD 保持开启时切换档位：静默关→预选→再开，只发一次 UI 事件，避免状态栏被中间态清掉 */
        err = pd_spoof_set_enabled_quiet(false);
        if (err == ESP_OK) {
            err = pd_spoof_preset_voltage(voltage_from_int(pd_voltage));
        }
        if (err == ESP_OK) {
            err = pd_spoof_set_enabled_remote(true);
        }
    } else {
        if (has_enabled && !pd_enabled) {
            err = pd_spoof_set_enabled_remote(false);
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
            err = pd_spoof_set_enabled_remote(true);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "开启 PD 失败: %s", esp_err_to_name(err));
                return err;
            }
        }
    }

    if (err != ESP_OK) {
        return err;
    }

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
        mqtt_sync_status_bar();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT 已断开");
        mqtt_sync_status_bar();
        break;
    case MQTT_EVENT_ERROR:
        s_mqtt_connected = false;
        mqtt_sync_status_bar();
        if (event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "Broker 拒绝 MQTT 连接 (code=%d)，将重置客户端",
                     event->error_handle->connect_return_code);
            s_mqtt_reset_pending = true;
        }
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
        .credentials.client_id = s_client_id,
        .network.reconnect_timeout_ms = NET_MQTT_RECONNECT_MS,
        .network.timeout_ms = NET_MQTT_NETWORK_MS,
        .session.keepalive = 30,
        .session.disable_clean_session = false,
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

    char broker_host[64];
    uint16_t broker_port = 0;
    if (!parse_broker_endpoint(broker_host, sizeof(broker_host), &broker_port)) {
        ESP_LOGE(TAG, "Broker URI 无效: %s", NET_MQTT_BROKER_URI);
        vTaskDelete(NULL);
        return;
    }

    while (!net_wifi_sta_has_ip()) {
        (void)net_wifi_wait_sta_ip(pdMS_TO_TICKS(200));
    }
    vTaskDelay(pdMS_TO_TICKS(NET_MQTT_IP_SETTLE_MS));

    ESP_LOGI(TAG, "Wi-Fi 已就绪，连接 MQTT Broker");
    wait_for_broker_tcp(broker_host, broker_port);

    while (!mqtt_client_start_once()) {
        vTaskDelay(pdMS_TO_TICKS(NET_MQTT_BROKER_POLL_MS));
        if (!net_wifi_sta_has_ip()) {
            while (!net_wifi_sta_has_ip()) {
                (void)net_wifi_wait_sta_ip(pdMS_TO_TICKS(200));
            }
            vTaskDelay(pdMS_TO_TICKS(NET_MQTT_IP_SETTLE_MS));
        }
        wait_for_broker_tcp(broker_host, broker_port);
    }

    for (;;) {
        if (s_mqtt_reset_pending) {
            s_mqtt_reset_pending = false;
            mqtt_client_destroy();
            wait_for_broker_tcp(broker_host, broker_port);
            (void)mqtt_client_start_once();
        }
        if (s_mqtt_suspended) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!net_wifi_sta_has_ip()) {
            if (s_mqtt_connected) {
                s_mqtt_connected = false;
                mqtt_sync_status_bar();
            }
            (void)net_wifi_wait_sta_ip(pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_mqtt_connected && s_mqtt_bar_on) {
            mqtt_sync_status_bar();
        }
        if (!s_client) {
            wait_for_broker_tcp(broker_host, broker_port);
            (void)mqtt_client_start_once();
        } else if (s_mqtt_connected) {
            (void)publish_telemetry();
        }
        vTaskDelay(pdMS_TO_TICKS(NET_MQTT_TELEMETRY_INTERVAL_MS));
    }
}

void net_mqtt_init(void)
{
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "power/%s/command", NET_MQTT_DEVICE_ID);
    snprintf(s_telem_topic, sizeof(s_telem_topic), "power/%s/telemetry", NET_MQTT_DEVICE_ID);
    snprintf(s_client_id, sizeof(s_client_id), "%s-%04x",
             NET_MQTT_DEVICE_ID, (unsigned)(esp_random() & 0xFFFFU));
    ESP_LOGI(TAG, "MQTT client_id=%s", s_client_id);

    s_mqtt_task = rtos_task_create_psram(net_mqtt_task, "net_mqtt", NET_MQTT_TASK_STACK, NULL, NET_MQTT_TASK_PRIO);
    if (!s_mqtt_task) {
        ESP_LOGE(TAG, "创建 MQTT 任务失败");
    }
}

void net_mqtt_suspend(void)
{
    s_mqtt_suspended  = true;
    s_mqtt_connected  = false;
    mqtt_sync_status_bar();
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
