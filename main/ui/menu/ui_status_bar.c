/**
 * @file ui_status_bar.c
 */
#include "ui_status_bar.h"
#include "axp2101_battery.h"
#include "net_wifi.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>

#define UI_STATUS_STRIP_Y        8
/** Extra inset from screen right edge — larger = whole strip (Wi‑Fi + %) moves left, avoids corner overlap. */
#define UI_STATUS_EDGE_PAD       24
#define UI_STATUS_COL_GAP        10
#define BAT_TIMER_FIRST_MS       800
#define BAT_TIMER_PERIOD_MS      30000
#define BAT_BODY_W               28
#define BAT_BODY_H               15
#define BAT_INNER_PAD            3
#define BAT_ROOT_W               (BAT_BODY_W + 10)
#define BAT_ROOT_H               (BAT_BODY_H + 4)
#define BAT_TIP_W                4
#define BAT_TIP_H                9

static lv_obj_t *s_strip;
static lv_obj_t *s_wifi_lbl;
static lv_obj_t *s_pct_lbl;
static lv_obj_t *s_bat_root;
static lv_obj_t *s_bat_body;
static lv_obj_t *s_bat_fill;
static lv_obj_t *s_chg_lbl;
static lv_timer_t *s_bat_timer;
static bool        s_bat_first_period = true;
static bool        s_events_registered;

typedef struct {
    bool show;
} wifi_vis_msg_t;

static void apply_power_readings(void *p)
{
    (void)p;
    if (!s_pct_lbl || !lv_obj_is_valid(s_pct_lbl) || !s_bat_fill || !lv_obj_is_valid(s_bat_fill)) {
        return;
    }

    uint8_t soc = 0;
    const esp_err_t e = axp2101_battery_read_soc(&soc);
    if (e == ESP_OK) {
        char buf[8];
        (void)snprintf(buf, sizeof(buf), "%u%%", (unsigned)soc);
        lv_label_set_text(s_pct_lbl, buf);
        lv_obj_clear_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);

        const int inner = BAT_BODY_W - 2 * BAT_INNER_PAD;
        int         fw    = (int)soc * inner / 100;
        if (soc > 0 && fw < 3) {
            fw = 3;
        }
        if (fw > inner) {
            fw = inner;
        }
        lv_obj_set_width(s_bat_fill, (lv_coord_t)fw);
    } else {
        lv_label_set_text(s_pct_lbl, "--");
        lv_obj_clear_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_bat_fill, 0);
    }

    if (s_chg_lbl && lv_obj_is_valid(s_chg_lbl)) {
        if (axp2101_battery_vbus_good()) {
            lv_obj_clear_flag(s_chg_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chg_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void notify_from_axp(void)
{
    (void)lv_async_call(apply_power_readings, NULL);
}

static void bat_timer_cb(lv_timer_t *t)
{
    apply_power_readings(NULL);
    if (s_bat_first_period) {
        s_bat_first_period = false;
        lv_timer_set_period(t, BAT_TIMER_PERIOD_MS);
        lv_timer_reset(t);
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
    if (s_strip && lv_obj_is_valid(s_strip)) {
        return;
    }

    axp2101_battery_set_ui_notify(notify_from_axp);

    s_strip = lv_obj_create(screen);
    lv_obj_add_flag(s_strip, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_strip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_strip, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_strip, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_strip, UI_STATUS_COL_GAP, LV_PART_MAIN);
    lv_obj_align(s_strip, LV_ALIGN_TOP_RIGHT, -UI_STATUS_EDGE_PAD, UI_STATUS_STRIP_Y);

    s_wifi_lbl = lv_label_create(s_strip);
    lv_obj_clear_flag(s_wifi_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_lbl, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_add_flag(s_wifi_lbl, LV_OBJ_FLAG_HIDDEN);

    s_pct_lbl = lv_label_create(s_strip);
    lv_obj_clear_flag(s_pct_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_pct_lbl, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_pct_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(s_pct_lbl, "--");

    s_bat_root = lv_obj_create(s_strip);
    lv_obj_clear_flag(s_bat_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_bat_root, BAT_ROOT_W, BAT_ROOT_H);
    lv_obj_set_style_bg_opa(s_bat_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bat_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bat_root, 0, LV_PART_MAIN);

    s_bat_body = lv_obj_create(s_bat_root);
    lv_obj_clear_flag(s_bat_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_bat_body, BAT_BODY_W, BAT_BODY_H);
    lv_obj_set_style_radius(s_bat_body, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bat_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bat_body, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bat_body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bat_body, 0, LV_PART_MAIN);
    lv_obj_align(s_bat_body, LV_ALIGN_LEFT_MID, 0, 0);

    s_bat_fill = lv_obj_create(s_bat_body);
    lv_obj_clear_flag(s_bat_fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(0x64748B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bat_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bat_fill, 3, LV_PART_MAIN);
    lv_obj_set_height(s_bat_fill, BAT_BODY_H - 2 * BAT_INNER_PAD);
    lv_obj_set_width(s_bat_fill, 0);
    lv_obj_align(s_bat_fill, LV_ALIGN_LEFT_MID, BAT_INNER_PAD, 0);

    lv_obj_t *tip = lv_obj_create(s_bat_root);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tip, BAT_TIP_W, BAT_TIP_H);
    lv_obj_set_style_bg_color(tip, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 1, LV_PART_MAIN);
    lv_obj_align_to(tip, s_bat_body, LV_ALIGN_OUT_RIGHT_MID, 2, 0);

    s_chg_lbl = lv_label_create(s_bat_body);
    lv_obj_clear_flag(s_chg_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_chg_lbl, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(s_chg_lbl, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_chg_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(s_chg_lbl);
    lv_obj_add_flag(s_chg_lbl, LV_OBJ_FLAG_HIDDEN);

    if (!s_events_registered) {
        esp_err_t e1 = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
        esp_err_t e2 = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_event, NULL);
        if (e1 == ESP_OK && e2 == ESP_OK) {
            s_events_registered = true;
        }
    }

    ui_status_bar_sync_wifi();

    s_bat_first_period = true;
    s_bat_timer        = lv_timer_create(bat_timer_cb, BAT_TIMER_FIRST_MS, NULL);
    if (s_bat_timer) {
        lv_timer_set_repeat_count(s_bat_timer, -1);
    }
}
