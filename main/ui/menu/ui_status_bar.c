/**
 * @file ui_status_bar.c
 * @brief 右上角状态栏：WiFi、MQTT 服务器、PD 状态指示。
 */
#include "ui_status_bar.h"
#include "ui_power_config.h"
#include "ui_power_sheet.h"
#include "net_wifi.h"
#include "net_mqtt.h"
#include "pd_spoof.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lvgl.h"

#include <stdbool.h>

#define UI_STATUS_STRIP_Y   8
#define UI_STATUS_EDGE_PAD  28
#define UI_STATUS_ITEM_GAP  10

#define UI_STATUS_MQTT_COLOR  0x7DD3FC

static lv_obj_t *s_host;
static lv_obj_t *s_pd_lbl;
static lv_obj_t *s_mqtt_lbl;
static lv_obj_t *s_wifi_lbl;
static bool      s_events_registered;

static lv_obj_t *create_status_label(lv_obj_t *parent, const char *text, uint32_t color,
                                     const lv_font_t *font)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_remove_style_all(lb);
    lv_obj_add_flag(lb, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_color(lb, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, font, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(lb, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(lb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_side(lb, LV_BORDER_SIDE_NONE, LV_PART_MAIN);
    lv_obj_set_style_outline_width(lb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lb, 0, LV_PART_MAIN);
    lv_obj_remove_style(lb, NULL, LV_PART_SCROLLBAR | LV_STATE_ANY);
    return lb;
}

static void status_bar_invalidate_top_strip(void)
{
    if (!s_host || !lv_obj_is_valid(s_host)) {
        return;
    }
    const lv_coord_t w = lv_disp_get_hor_res(lv_disp_get_default());
    lv_area_t area = {
        .x1 = 0,
        .y1 = 0,
        .x2 = w - 1,
        .y2 = UI_STATUS_STRIP_Y + 36,
    };
    lv_obj_invalidate_area(s_host, &area);
}

static void status_bar_invalidate_area_padded(lv_obj_t *obj)
{
    if (!s_host || !obj || !lv_obj_is_valid(obj)) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    area.x1 -= 8;
    area.y1 -= 8;
    area.x2 += 8;
    area.y2 += 8;
    lv_obj_invalidate_area(s_host, &area);
}

void ui_status_bar_raise_to_front(void)
{
    if (s_wifi_lbl && lv_obj_is_valid(s_wifi_lbl)) {
        lv_obj_move_foreground(s_wifi_lbl);
    }
    if (s_mqtt_lbl && lv_obj_is_valid(s_mqtt_lbl)) {
        lv_obj_move_foreground(s_mqtt_lbl);
    }
    if (s_pd_lbl && lv_obj_is_valid(s_pd_lbl)) {
        lv_obj_move_foreground(s_pd_lbl);
    }
}

static void status_bar_relayout(void)
{
    if (!s_host) {
        return;
    }

    const bool wifi_vis = s_wifi_lbl && lv_obj_is_valid(s_wifi_lbl)
                          && !lv_obj_has_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
    const bool pd_vis = s_pd_lbl && lv_obj_is_valid(s_pd_lbl)
                        && !lv_obj_has_flag(s_pd_lbl, LV_OBJ_FLAG_HIDDEN);
    const bool mqtt_vis = s_mqtt_lbl && lv_obj_is_valid(s_mqtt_lbl)
                          && !lv_obj_has_flag(s_mqtt_lbl, LV_OBJ_FLAG_HIDDEN);

    if (wifi_vis) {
        lv_obj_align(s_wifi_lbl, LV_ALIGN_TOP_RIGHT, -UI_STATUS_EDGE_PAD, UI_STATUS_STRIP_Y);
    }

    if (pd_vis) {
        if (wifi_vis) {
            lv_obj_align_to(s_pd_lbl, s_wifi_lbl, LV_ALIGN_OUT_LEFT_MID, -UI_STATUS_ITEM_GAP, 0);
        } else {
            lv_obj_align(s_pd_lbl, LV_ALIGN_TOP_RIGHT, -UI_STATUS_EDGE_PAD, UI_STATUS_STRIP_Y);
        }
    }

    if (mqtt_vis) {
        if (pd_vis) {
            lv_obj_align_to(s_mqtt_lbl, s_pd_lbl, LV_ALIGN_OUT_LEFT_MID, -UI_STATUS_ITEM_GAP, 0);
        } else if (wifi_vis) {
            lv_obj_align_to(s_mqtt_lbl, s_wifi_lbl, LV_ALIGN_OUT_LEFT_MID, -UI_STATUS_ITEM_GAP, 0);
        } else {
            lv_obj_align(s_mqtt_lbl, LV_ALIGN_TOP_RIGHT, -UI_STATUS_EDGE_PAD, UI_STATUS_STRIP_Y);
        }
    }

    if (ui_power_sheet_bottom_is_open()) {
        ui_status_bar_raise_to_front();
    }
}

static void boot_anim_status_y(void *var, int32_t v)
{
    (void)var;
    const lv_coord_t y = (lv_coord_t)v;
    if (s_wifi_lbl && lv_obj_is_valid(s_wifi_lbl)) {
        lv_obj_set_y(s_wifi_lbl, y);
    }
    if (s_mqtt_lbl && lv_obj_is_valid(s_mqtt_lbl) && !lv_obj_has_flag(s_mqtt_lbl, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(s_mqtt_lbl, y);
    }
    if (s_pd_lbl && lv_obj_is_valid(s_pd_lbl) && !lv_obj_has_flag(s_pd_lbl, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(s_pd_lbl, y);
    }
}

static void (*s_boot_ready_cb)(void);

static void boot_anim_ready_cb(lv_anim_t *a)
{
    (void)a;
    status_bar_relayout();
    if (s_boot_ready_cb) {
        void (*cb)(void) = s_boot_ready_cb;
        s_boot_ready_cb  = NULL;
        cb();
    }
}

typedef struct {
    bool show;
} wifi_vis_msg_t;

typedef struct {
    bool show;
} mqtt_vis_msg_t;

typedef struct {
    bool show;
} pd_vis_msg_t;

static void pd_vis_apply(void *p)
{
    pd_vis_msg_t *m = (pd_vis_msg_t *)p;
    if (!m) {
        return;
    }
    if (m->show) {
        if (!s_pd_lbl || !lv_obj_is_valid(s_pd_lbl)) {
            s_pd_lbl = create_status_label(s_host, "PD", 0x5EEAD4, &lv_font_montserrat_18);
        } else {
            lv_label_set_text(s_pd_lbl, "PD");
            lv_obj_clear_flag(s_pd_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_pd_lbl && lv_obj_is_valid(s_pd_lbl)) {
        status_bar_invalidate_area_padded(s_pd_lbl);
        lv_obj_del(s_pd_lbl);
        s_pd_lbl = NULL;
    }
    status_bar_relayout();
    if (!m->show) {
        status_bar_invalidate_top_strip();
    }
    lv_mem_free(m);
}

static void post_pd_visible(bool show)
{
    pd_vis_msg_t *m = (pd_vis_msg_t *)lv_mem_alloc(sizeof(*m));
    if (!m) {
        return;
    }
    m->show = show;
    if (lv_async_call(pd_vis_apply, m) != LV_RES_OK) {
        lv_mem_free(m);
    }
}

static void mqtt_vis_apply(void *p)
{
    mqtt_vis_msg_t *m = (mqtt_vis_msg_t *)p;
    if (!m) {
        return;
    }
    /* 应用时读取实时状态，避免异步队列中过期的 show=true 覆盖断开后的隐藏 */
    const bool show = net_mqtt_is_connected() && net_wifi_sta_has_ip();
    if (s_mqtt_lbl && lv_obj_is_valid(s_mqtt_lbl)) {
        if (show) {
            lv_label_set_text(s_mqtt_lbl, LV_SYMBOL_DRIVE);
            lv_obj_clear_flag(s_mqtt_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_mqtt_lbl, "");
            lv_obj_add_flag(s_mqtt_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    status_bar_relayout();
    if (!show) {
        status_bar_invalidate_top_strip();
    }
    lv_mem_free(m);
}

static void post_mqtt_visible(bool show)
{
    mqtt_vis_msg_t *m = (mqtt_vis_msg_t *)lv_mem_alloc(sizeof(*m));
    if (!m) {
        return;
    }
    m->show = show;
    if (lv_async_call(mqtt_vis_apply, m) != LV_RES_OK) {
        lv_mem_free(m);
    }
}

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
            lv_label_set_text(s_wifi_lbl, "");
            lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    status_bar_relayout();
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
        post_mqtt_visible(false);
    }
}

void ui_status_bar_sync_wifi(void)
{
    post_wifi_visible(net_wifi_sta_has_ip());
}

void ui_status_bar_sync_mqtt(bool connected)
{
    post_mqtt_visible(connected);
}

void ui_status_bar_sync_pd(bool enabled)
{
    post_pd_visible(enabled);
}

void ui_status_bar_boot_slide_in(void (*on_ready)(void))
{
    if (!s_host) {
        if (on_ready) {
            on_ready();
        }
        return;
    }

    s_boot_ready_cb = on_ready;
    status_bar_relayout();

    const lv_coord_t y_target = UI_STATUS_STRIP_Y;
    const lv_coord_t y_from   = y_target - UI_POWER_BOOT_STATUS_SLIDE_PX;

    boot_anim_status_y(NULL, y_from);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_wifi_lbl ? s_wifi_lbl : s_host);
    lv_anim_set_exec_cb(&a, boot_anim_status_y);
    lv_anim_set_values(&a, y_from, y_target);
    lv_anim_set_time(&a, UI_POWER_BOOT_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, boot_anim_ready_cb);
    lv_anim_start(&a);
}

void ui_status_bar_init(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    if (s_host && lv_obj_is_valid(s_host)) {
        return;
    }

    s_host = screen;

    s_pd_lbl = NULL;

    s_wifi_lbl = create_status_label(screen, "", 0x94A3B8, &lv_font_montserrat_22);
    lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);

    s_mqtt_lbl = create_status_label(screen, "", UI_STATUS_MQTT_COLOR, &lv_font_montserrat_22);
    lv_obj_add_flag(s_mqtt_lbl, LV_OBJ_FLAG_HIDDEN);

    if (!s_events_registered) {
        esp_err_t e1 = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
        esp_err_t e2 = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL);
        if (e1 == ESP_OK && e2 == ESP_OK) {
            s_events_registered = true;
        }
    }

    ui_status_bar_sync_wifi();
    ui_status_bar_sync_mqtt(net_mqtt_is_connected());

    pd_spoof_status_t pd_st = {0};
    if (pd_spoof_get_status(&pd_st) == ESP_OK) {
        ui_status_bar_sync_pd(pd_st.enabled);
    }
}
