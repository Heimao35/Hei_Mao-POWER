/**
 * @file net_wifi.h
 * @brief Station Wi-Fi: scan (bounded), connect worker, NVS credentials, boot reconnect.
 */
#ifndef NET_WIFI_H
#define NET_WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_WIFI_SSID_MAX_LEN   32
#define NET_WIFI_PASS_MAX_LEN   64
#define NET_WIFI_SCAN_MAX_APS   10

typedef struct {
    char     ssid[NET_WIFI_SSID_MAX_LEN + 1];
    int8_t   rssi;
    uint8_t  authmode; /* wifi_auth_mode_t */
} net_wifi_ap_info_t;

/** Init netif + default STA + Wi-Fi driver. Safe to call once. */
void net_wifi_init(void);

/**
 * After boot, wait then try saved STA in a low-priority task (does not compete with display bring-up).
 * Call once from app_main after @ref net_wifi_init; do not call @ref net_wifi_boot_try_saved directly at boot unless you need immediate connect.
 */
void net_wifi_start_saved_reconnect_background(void);

/** Load saved STA from NVS and call esp_wifi_connect (use from worker or tests). */
void net_wifi_boot_try_saved(void);

/**
 * Start background scan (does not block caller). Results delivered on LVGL thread via @p cb.
 * At most @ref NET_WIFI_SCAN_MAX_APS entries, RSSI-sorted strongest first.
 */
typedef void (*net_wifi_scan_cb_t)(esp_err_t err, const net_wifi_ap_info_t *aps, int n, void *user_data);
void net_wifi_scan_request(net_wifi_scan_cb_t cb, void *user_data);

typedef void (*net_wifi_connect_cb_t)(bool success, void *user_data);

/** Queue STA connect attempt (disconnect first if needed). Callback runs on LVGL thread. */
void net_wifi_connect_request(const char *ssid, const char *password, net_wifi_connect_cb_t cb, void *user_data);

bool net_wifi_sta_has_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_WIFI_H */
