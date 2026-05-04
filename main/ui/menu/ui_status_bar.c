/**
 * @file ui_status_bar.c
 */
#include "ui_status_bar.h"
#include "net_wifi.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include <stdbool.h>

/** Pixels kept free on the right for a future battery icon. */
#define UI_STATUS_RESERVE_RIGHT  96
#define UI_STATUS_WIFI_Y         8

static lv_obj_t *s_wifi_lbl;
static bool      s_events_registered;

typedef struct {
    bool show;
} wifi_vis_msg_t;

static void wifi_vis_apply(void *p)
{
    wifi_vis_msg_t *m = (wifi_vis_msg_t *)p;
    if (!m) {
        return;
    }
    if (s_wifi_lbl && lv_obj_is_valid(s_wifi_lbl)) {
        if (m->show) {
            lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI);
            lv_obj_clear_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_mem_free(m);
}

static void post_wifi_visible(bool show)
{
    wifi_vis_msg_t *m = (wifi_vis_msg_t *)lv_mem_alloc(sizeof(*m));
    if (!m) {
        return;
    }
    m->show = show;
    if (lv_async_call(wifi_vis_apply, m) != LV_RES_OK) {
        lv_mem_free(m);
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        post_wifi_visible(true);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        post_wifi_visible(false);
    }
}

void ui_status_bar_sync_wifi(void)
{
    post_wifi_visible(net_wifi_sta_has_ip());
}

void ui_status_bar_init(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    if (s_wifi_lbl && !lv_obj_is_valid(s_wifi_lbl)) {
        s_wifi_lbl = NULL;
    }
    if (s_wifi_lbl) {
        return;
    }

    s_wifi_lbl = lv_label_create(screen);
    lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wifi_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(s_wifi_lbl, LV_ALIGN_TOP_RIGHT, -UI_STATUS_RESERVE_RIGHT, UI_STATUS_WIFI_Y);

    if (!s_events_registered) {
        esp_err_t e1 = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
        esp_err_t e2 = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL);
        if (e1 == ESP_OK && e2 == ESP_OK) {
            s_events_registered = true;
        }
    }

    ui_status_bar_sync_wifi();
}
