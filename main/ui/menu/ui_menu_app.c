/**
 * @file ui_menu_app.c
 */
#include "ui_menu_app.h"
#include "ui_menu_brightness.h"
#include "ui_menu_config.h"
#include "ui_menu_rgb.h"
#include "ui_menu_wifi.h"
#include "ui_status_bar.h"
#include "ui_main_clock.h"
#include "ui_menu_model.h"
#include "ui_menu_wheel.h"

#include "lvgl.h"
#include <string.h>

#define MENU_MAX_STACK 4

typedef struct {
    int pending;
    void (*on_done)(void *user_data);
    void *user_data;
} slide_sync_t;

typedef struct {
    int panel_index;
    lv_obj_t *panel;
    ui_menu_wheel_t *wheel;
} back_user_data_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *stage;
    lv_obj_t *panels[MENU_MAX_STACK];
    ui_menu_wheel_t *wheels[MENU_MAX_STACK];
    int stack_top;
    uint8_t submenu_main_idx;
    lv_point_t ptr_down;
    bool ptr_valid;
    /** After a right-swipe-back RELEASED, LVGL may still send SHORT_CLICKED; skip opening Brightness. */
    bool suppress_submenu_short_click;
} ui_menu_app_ctx_t;

static ui_menu_app_ctx_t s_ctx;

static void panel_apply_theme(lv_obj_t *panel)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0f172a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
}

static void anim_set_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void slide_anim_ready(lv_anim_t *a)
{
    slide_sync_t *s = (slide_sync_t *)a->user_data;
    if (!s) {
        return;
    }
    if (--s->pending > 0) {
        return;
    }
    if (s->on_done) {
        s->on_done(s->user_data);
    }
    lv_mem_free(s);
}

static void start_slide_pair(lv_obj_t *a_obj, lv_coord_t a_from, lv_coord_t a_to,
                             lv_obj_t *b_obj, lv_coord_t b_from, lv_coord_t b_to,
                             void (*on_done)(void *user_data), void *user_data)
{
    slide_sync_t *sync = (slide_sync_t *)lv_mem_alloc(sizeof(*sync));
    if (!sync) {
        return;
    }
    sync->pending = 2;
    sync->on_done = on_done;
    sync->user_data = user_data;

    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, a_obj);
    lv_anim_set_exec_cb(&ax, anim_set_x_cb);
    lv_anim_set_values(&ax, a_from, a_to);
    lv_anim_set_time(&ax, UI_MENU_SLIDE_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&ax, sync);
    lv_anim_set_ready_cb(&ax, slide_anim_ready);

    lv_anim_t bx;
    lv_anim_init(&bx);
    lv_anim_set_var(&bx, b_obj);
    lv_anim_set_exec_cb(&bx, anim_set_x_cb);
    lv_anim_set_values(&bx, b_from, b_to);
    lv_anim_set_time(&bx, UI_MENU_SLIDE_MS);
    lv_anim_set_path_cb(&bx, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&bx, sync);
    lv_anim_set_ready_cb(&bx, slide_anim_ready);

    lv_anim_start(&ax);
    lv_anim_start(&bx);
}

static void forward_done_cb(void *ud)
{
    (void)ud;
    s_ctx.stack_top++;
}

static void back_done_cb(void *ud)
{
    back_user_data_t *b = (back_user_data_t *)ud;
    if (!b) {
        return;
    }
    if (b->wheel) {
        ui_menu_wheel_delete(b->wheel);
    }
    if (b->panel) {
        lv_obj_del(b->panel);
    }
    if (b->panel_index >= 0 && b->panel_index < MENU_MAX_STACK) {
        s_ctx.panels[b->panel_index] = NULL;
        s_ctx.wheels[b->panel_index] = NULL;
    }
    if (b->panel_index > 0) {
        s_ctx.stack_top = b->panel_index - 1;
    }
    lv_mem_free(b);
}

static void submenu_touch_cb(lv_event_t *e);
static void submenu_short_click_cb(lv_event_t *e);
static lv_obj_t *brightness_panel_create(void);
static lv_obj_t *rgb_panel_create(void);
static lv_obj_t *wifi_panel_create(void);

static lv_obj_t *submenu_panel_create(const ui_menu_main_item_t *item)
{
    lv_obj_t *p = lv_obj_create(s_ctx.stage);
    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(p));
    const lv_coord_t h = lv_disp_get_ver_res(lv_obj_get_disp(p));
    lv_obj_set_size(p, w, h);
    panel_apply_theme(p);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(p, submenu_short_click_cb, LV_EVENT_SHORT_CLICKED, NULL);

    /* Full-screen wheel host first so snap uses the same vertical center as the main menu; header floats on top. */
    lv_obj_t *wheel_host = lv_obj_create(p);
    lv_obj_set_size(wheel_host, w, h);
    lv_obj_align(wheel_host, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(wheel_host, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wheel_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wheel_host, 0, LV_PART_MAIN);
    lv_obj_add_flag(wheel_host, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *title = lv_label_create(p);
    lv_obj_add_flag(title, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text_fmt(title, "%s", item->title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    return p;
}

static lv_obj_t *brightness_panel_create(void)
{
    lv_obj_t *p = lv_obj_create(s_ctx.stage);
    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(p));
    const lv_coord_t h = lv_disp_get_ver_res(lv_obj_get_disp(p));
    lv_obj_set_size(p, w, h);
    panel_apply_theme(p);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESS_LOST, NULL);
    ui_menu_brightness_populate(p);
    return p;
}

static lv_obj_t *rgb_panel_create(void)
{
    lv_obj_t *p = lv_obj_create(s_ctx.stage);
    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(p));
    const lv_coord_t h = lv_disp_get_ver_res(lv_obj_get_disp(p));
    lv_obj_set_size(p, w, h);
    panel_apply_theme(p);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESS_LOST, NULL);
    ui_menu_rgb_populate(p);
    return p;
}

static lv_obj_t *wifi_panel_create(void)
{
    lv_obj_t *p = lv_obj_create(s_ctx.stage);
    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(p));
    const lv_coord_t h = lv_disp_get_ver_res(lv_obj_get_disp(p));
    lv_obj_set_size(p, w, h);
    panel_apply_theme(p);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(p, submenu_touch_cb, LV_EVENT_PRESS_LOST, NULL);
    ui_menu_wifi_populate(p);
    return p;
}

static void submenu_short_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_ctx.suppress_submenu_short_click) {
        s_ctx.suppress_submenu_short_click = false;
        return;
    }
    if (s_ctx.stack_top != 1) {
        return;
    }
    ui_menu_wheel_t *sw = s_ctx.wheels[1];
    if (!sw) {
        return;
    }
    const uint32_t sub_idx = ui_menu_wheel_get_selected_index(sw);
    lv_obj_t *(*make_panel)(void) = NULL;
    if (s_ctx.submenu_main_idx == UI_MENU_MAIN_IDX_DISPLAY && sub_idx == UI_MENU_SUB_IDX_BRIGHTNESS) {
        make_panel = brightness_panel_create;
    } else if (s_ctx.submenu_main_idx == UI_MENU_MAIN_IDX_SYSTEM && sub_idx == UI_MENU_SUB_IDX_SYSTEM_WIFI) {
        make_panel = wifi_panel_create;
    } else if (s_ctx.submenu_main_idx == UI_MENU_MAIN_IDX_SYSTEM && sub_idx == UI_MENU_SUB_IDX_SYSTEM_RGB) {
        make_panel = rgb_panel_create;
    }
    if (!make_panel) {
        return;
    }

    lv_obj_t *sub_p = s_ctx.panels[1];
    lv_obj_t *bri_p = make_panel();
    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(bri_p));

    lv_obj_set_x(bri_p, w);
    s_ctx.panels[2] = bri_p;
    s_ctx.wheels[2] = NULL;

    start_slide_pair(sub_p, lv_obj_get_x(sub_p), -w, bri_p, w, 0, forward_done_cb, NULL);
}

static lv_obj_t *submenu_get_wheel_host(lv_obj_t *panel)
{
    if (lv_obj_get_child_cnt(panel) < 1) {
        return NULL;
    }
    return lv_obj_get_child(panel, 0);
}

static void submenu_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    if (code == LV_EVENT_PRESSED) {
        s_ctx.suppress_submenu_short_click = false;
        if (indev) {
            lv_indev_get_point(indev, &s_ctx.ptr_down);
            s_ctx.ptr_valid = true;
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        s_ctx.ptr_valid = false;
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (!s_ctx.ptr_valid || !indev) {
            s_ctx.ptr_valid = false;
            return;
        }
        s_ctx.ptr_valid = false;

        if (s_ctx.stack_top < 1) {
            return;
        }

        lv_point_t up;
        lv_indev_get_point(indev, &up);
        const lv_coord_t dx = up.x - s_ctx.ptr_down.x;
        const lv_coord_t dy = up.y - s_ctx.ptr_down.y;
        const lv_coord_t adx = LV_ABS(dx);
        const lv_coord_t ady = LV_ABS(dy);

        if (dx > UI_MENU_BACK_SWIPE_MIN_PX && adx * 100 > ady * UI_MENU_BACK_SWIPE_RATIO) {
            lv_obj_t *cur = s_ctx.panels[s_ctx.stack_top];
            lv_obj_t *parent_panel = s_ctx.panels[s_ctx.stack_top - 1];
            const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(cur));

            back_user_data_t *bud = (back_user_data_t *)lv_mem_alloc(sizeof(*bud));
            if (!bud) {
                return;
            }
            s_ctx.suppress_submenu_short_click = true;
            bud->panel_index = s_ctx.stack_top;
            bud->panel = cur;
            bud->wheel = s_ctx.wheels[s_ctx.stack_top];

            start_slide_pair(cur, lv_obj_get_x(cur), w,
                             parent_panel, lv_obj_get_x(parent_panel), 0,
                             back_done_cb, bud);
        }
    }
}

static void main_panel_short_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_ctx.stack_top != 0) {
        return;
    }

    const ui_menu_main_item_t *tbl = ui_menu_model_get_table();
    ui_menu_wheel_t *mw = s_ctx.wheels[0];
    if (!mw) {
        return;
    }

    const uint32_t idx = ui_menu_wheel_get_selected_index(mw);
    if (idx >= ui_menu_model_main_count()) {
        return;
    }

    const ui_menu_main_item_t *item = &tbl[idx];
    s_ctx.submenu_main_idx = (uint8_t)idx;
    lv_obj_t *main_p = s_ctx.panels[0];
    lv_obj_t *sub_p = submenu_panel_create(item);

    lv_obj_t *host = submenu_get_wheel_host(sub_p);
    if (!host) {
        lv_obj_del(sub_p);
        return;
    }

    const char *sub_items[UI_MENU_SUB_COUNT];
    for (int i = 0; i < UI_MENU_SUB_COUNT; i++) {
        sub_items[i] = item->sub[i];
    }

    ui_menu_wheel_t *sw = ui_menu_wheel_create(host, sub_items, UI_MENU_SUB_COUNT, UI_MENU_SUBMENU_HEADER_RESERVE_PX);
    const lv_coord_t w = lv_disp_get_hor_res(lv_obj_get_disp(sub_p));

    lv_obj_set_x(sub_p, w);
    s_ctx.panels[1] = sub_p;
    s_ctx.wheels[1] = sw;

    start_slide_pair(main_p, 0, -w, sub_p, w, 0, forward_done_cb, NULL);
}

void ui_menu_app_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.submenu_main_idx = 0xFF;

    lv_disp_t *disp = lv_disp_get_default();
    const lv_coord_t res_w = lv_disp_get_hor_res(disp);
    const lv_coord_t res_h = lv_disp_get_ver_res(disp);

    lv_obj_t *scr = lv_obj_create(NULL);
    s_ctx.screen = scr;
    lv_obj_set_size(scr, res_w, res_h);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020617), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    lv_obj_t *st = lv_obj_create(scr);
    s_ctx.stage = st;
    lv_obj_set_size(st, res_w, res_h);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(st, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(st, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(st, 0, LV_PART_MAIN);
    lv_obj_clear_flag(st, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_p = lv_obj_create(st);
    s_ctx.panels[0] = main_p;
    lv_obj_set_size(main_p, res_w, res_h);
    lv_obj_set_pos(main_p, 0, 0);
    panel_apply_theme(main_p);
    lv_obj_add_event_cb(main_p, main_panel_short_click_cb, LV_EVENT_SHORT_CLICKED, NULL);

    const ui_menu_main_item_t *tbl = ui_menu_model_get_table();
    const char *titles[UI_MENU_MAIN_COUNT];
    for (uint32_t i = 0; i < UI_MENU_MAIN_COUNT; i++) {
        titles[i] = tbl[i].title;
    }

    s_ctx.wheels[0] = ui_menu_wheel_create(main_p, titles, UI_MENU_MAIN_COUNT, 0);
    s_ctx.stack_top = 0;

    ui_status_bar_init(scr);
    lv_disp_load_scr(scr);
    /* Clock after active screen: layout coords and z-order refresh reliably. */
    ui_main_clock_create(main_p);
}
