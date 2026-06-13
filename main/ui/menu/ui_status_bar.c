/**
 * @file ui_status_bar.c
 * @brief 右上角状态栏，仅显示 WiFi 连接状态。
 */
#include "ui_status_bar.h"
#include "ui_power_config.h"
#include "net_wifi.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lvgl.h"

#include <stdbool.h>

#define UI_STATUS_STRIP_Y   8
#define UI_STATUS_EDGE_PAD  28

static lv_obj_t *s_strip;
static lv_obj_t *s_wifi_lbl;
static bool      s_events_registered;

static void boot_anim_set_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

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

void ui_status_bar_boot_slide_in(void)
{
    if (!s_strip || !lv_obj_is_valid(s_strip)) {
        return;
    }

    lv_obj_update_layout(s_strip);
    const lv_coord_t y_target = lv_obj_get_y_aligned(s_strip);
    const lv_coord_t y_from   = y_target - UI_POWER_BOOT_STATUS_SLIDE_PX;
    lv_obj_set_y(s_strip, y_from);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_strip);
    lv_anim_set_exec_cb(&a, boot_anim_set_y_cb);
    lv_anim_set_values(&a, y_from, y_target);
    lv_anim_set_time(&a, UI_POWER_BOOT_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void ui_status_bar_init(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    if (s_strip && lv_obj_is_valid(s_strip)) {
        return;
    }

    s_strip = lv_obj_create(screen);
    lv_obj_add_flag(s_strip, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_strip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_strip, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_strip, 0, LV_PART_MAIN);
    lv_obj_align(s_strip, LV_ALIGN_TOP_RIGHT, -UI_STATUS_EDGE_PAD, UI_STATUS_STRIP_Y);

    s_wifi_lbl = lv_label_create(s_strip);
    lv_obj_clear_flag(s_wifi_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_lbl, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);

    if (!s_events_registered) {
        esp_err_t e1 = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
        esp_err_t e2 = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL);
        if (e1 == ESP_OK && e2 == ESP_OK) {
            s_events_registered = true;
        }
    }

    ui_status_bar_sync_wifi();
}
