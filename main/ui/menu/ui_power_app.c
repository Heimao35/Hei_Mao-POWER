/**
 * @file ui_power_app.c
 * @brief 功率计主应用：主界面手势、设置子页、上下滑面板。
 */
#include "ui_power_app.h"
#include "ui_power_config.h"
#include "ui_power_main.h"
#include "ui_power_sheet.h"
#include "ui_menu_brightness.h"
#include "ui_menu_wifi.h"
#include "ui_menu_volume.h"
#include "ui_status_bar.h"
#include "ui_pd_panel.h"
#include "pd_spoof.h"
#include "buzzer.h"

#include "lvgl.h"
#include <string.h>

#define UI_POWER_REFRESH_MS 500

typedef struct {
    int pending;
    void (*on_done)(void *user_data);
    void *user_data;
} slide_sync_t;

typedef struct {
    lv_obj_t *panel;
} sub_back_ud_t;

typedef enum {
    UI_POWER_SUB_NONE = 0,
    UI_POWER_SUB_WIFI,
    UI_POWER_SUB_BRIGHTNESS,
    UI_POWER_SUB_VOLUME,
} ui_power_sub_page_t;

typedef struct {
    lv_obj_t           *screen;
    lv_obj_t           *stage;
    lv_obj_t           *main_panel;
    lv_obj_t           *sub_panel;
    ui_power_sub_page_t sub_page;
    ui_power_view_mode_t view_mode;
    lv_point_t          ptr_down;
    bool                ptr_valid;
    bool                suppress_click;
    lv_timer_t         *refresh_timer;
    lv_coord_t          scr_w;
    lv_coord_t          scr_h;
} ui_power_ctx_t;

static ui_power_ctx_t s_ctx;

static void sub_touch_cb(lv_event_t *e);
static void main_touch_cb(lv_event_t *e);
static void bottom_touch_cb(lv_event_t *e);
static void toggle_top_sheet(bool open);
static void toggle_bottom_sheet(bool open);
static void switch_view_mode(lv_coord_t dx);
static void screen_redraw(void);

static void anim_set_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void anim_set_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void anim_set_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void panel_apply_theme(lv_obj_t *panel)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_POWER_MAIN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
}

static void screen_redraw(void)
{
    ui_power_main_reset_views();
    if (s_ctx.main_panel && lv_obj_is_valid(s_ctx.main_panel)) {
        lv_obj_invalidate(s_ctx.main_panel);
    }
    if (s_ctx.stage && lv_obj_is_valid(s_ctx.stage)) {
        lv_obj_invalidate(s_ctx.stage);
    }
    if (s_ctx.screen && lv_obj_is_valid(s_ctx.screen)) {
        lv_obj_invalidate(s_ctx.screen);
    }
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

static void start_slide_x(lv_obj_t *obj, lv_coord_t from, lv_coord_t to,
                          void (*on_done)(void *), void *ud)
{
    slide_sync_t *sync = (slide_sync_t *)lv_mem_alloc(sizeof(*sync));
    if (!sync) {
        return;
    }
    sync->pending = 1;
    sync->on_done = on_done;
    sync->user_data = ud;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_set_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, UI_POWER_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, sync);
    lv_anim_set_ready_cb(&a, slide_anim_ready);
    lv_anim_start(&a);
}

static void start_slide_y(lv_obj_t *obj, lv_coord_t from, lv_coord_t to,
                          void (*on_done)(void *), void *ud,
                          lv_anim_path_cb_t path_cb)
{
    slide_sync_t *sync = (slide_sync_t *)lv_mem_alloc(sizeof(*sync));
    if (!sync) {
        return;
    }
    sync->pending = 1;
    sync->on_done = on_done;
    sync->user_data = ud;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, UI_POWER_SLIDE_MS);
    lv_anim_set_path_cb(&a, path_cb ? path_cb : lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, sync);
    lv_anim_set_ready_cb(&a, slide_anim_ready);
    lv_anim_start(&a);
}

static void start_slide_pair(lv_obj_t *a_obj, lv_coord_t a_from, lv_coord_t a_to,
                             lv_obj_t *b_obj, lv_coord_t b_from, lv_coord_t b_to,
                             void (*on_done)(void *), void *ud)
{
    slide_sync_t *sync = (slide_sync_t *)lv_mem_alloc(sizeof(*sync));
    if (!sync) {
        return;
    }
    sync->pending = 2;
    sync->on_done = on_done;
    sync->user_data = ud;

    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, a_obj);
    lv_anim_set_exec_cb(&ax, anim_set_x_cb);
    lv_anim_set_values(&ax, a_from, a_to);
    lv_anim_set_time(&ax, UI_POWER_SLIDE_MS);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&ax, sync);
    lv_anim_set_ready_cb(&ax, slide_anim_ready);

    lv_anim_t bx;
    lv_anim_init(&bx);
    lv_anim_set_var(&bx, b_obj);
    lv_anim_set_exec_cb(&bx, anim_set_x_cb);
    lv_anim_set_values(&bx, b_from, b_to);
    lv_anim_set_time(&bx, UI_POWER_SLIDE_MS);
    lv_anim_set_path_cb(&bx, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&bx, sync);
    lv_anim_set_ready_cb(&bx, slide_anim_ready);

    lv_anim_start(&ax);
    lv_anim_start(&bx);
}

static void sub_back_done(void *ud)
{
    sub_back_ud_t *b = (sub_back_ud_t *)ud;
    if (b) {
        if (b->panel) {
            lv_obj_del(b->panel);
        }
        lv_mem_free(b);
    }
    s_ctx.sub_panel = NULL;
    s_ctx.sub_page  = UI_POWER_SUB_NONE;
    if (s_ctx.main_panel && lv_obj_is_valid(s_ctx.main_panel)) {
        lv_obj_set_x(s_ctx.main_panel, 0);
    }
    ui_power_main_reset_views();
}

static void open_sub_page(ui_power_sub_page_t page)
{
    if (s_ctx.sub_panel) {
        return;
    }

    if (ui_power_sheet_top_is_open()) {
        toggle_top_sheet(false);
    }

    lv_obj_t *p = lv_obj_create(s_ctx.stage);
    s_ctx.sub_panel = p;
    s_ctx.sub_page  = page;
    lv_obj_set_size(p, s_ctx.scr_w, s_ctx.scr_h);
    panel_apply_theme(p);
    lv_obj_move_foreground(p);

    switch (page) {
    case UI_POWER_SUB_WIFI:
        ui_menu_wifi_populate(p);
        break;
    case UI_POWER_SUB_BRIGHTNESS:
        ui_menu_brightness_populate(p);
        break;
    case UI_POWER_SUB_VOLUME:
        ui_menu_volume_populate(p);
        break;
    default:
        lv_obj_del(p);
        s_ctx.sub_panel = NULL;
        s_ctx.sub_page  = UI_POWER_SUB_NONE;
        return;
    }

    lv_obj_set_x(s_ctx.main_panel, 0);
    lv_obj_set_x(p, s_ctx.scr_w);
    /* 进入：主界面左移、子页从右侧滑入，全程无空白暴露区 */
    start_slide_pair(s_ctx.main_panel, 0, -s_ctx.scr_w, p, s_ctx.scr_w, 0, NULL, NULL);

    /* 子页右滑返回 */
    lv_obj_add_event_cb(p, sub_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(p, sub_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(p, sub_touch_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void sheet_action_cb(ui_power_sheet_action_t action, void *user_data)
{
    (void)user_data;
    switch (action) {
    case UI_POWER_SHEET_WIFI:
        open_sub_page(UI_POWER_SUB_WIFI);
        break;
    case UI_POWER_SHEET_BRIGHTNESS:
        open_sub_page(UI_POWER_SUB_BRIGHTNESS);
        break;
    case UI_POWER_SHEET_VOLUME:
        open_sub_page(UI_POWER_SUB_VOLUME);
        break;
    default:
        break;
    }
}

static void toggle_top_sheet(bool open)
{
    lv_obj_t *top = ui_power_sheet_get_top();
    if (!top) {
        return;
    }
    ui_power_sheet_set_top_open(open);
    const lv_coord_t y_target = open ? 0 : -UI_POWER_TOP_SHEET_H;
    const lv_coord_t y_from   = lv_obj_get_y(top);
    start_slide_y(top, y_from, y_target, NULL, NULL, lv_anim_path_ease_in_out);
}

static void toggle_bottom_sheet(bool open)
{
    lv_obj_t *bot = ui_power_sheet_get_bottom();
    if (!bot) {
        return;
    }

    const lv_coord_t scr_h = ui_power_sheet_screen_h();
    const lv_coord_t y_hidden = scr_h;
    const lv_coord_t y_shown  = 0;

    lv_anim_del(bot, anim_set_y_cb);

    if (open) {
        lv_obj_clear_flag(bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(bot, y_hidden);
        lv_obj_move_foreground(bot);
    }

    ui_power_sheet_set_bottom_open(open);

    if (open) {
        ui_status_bar_raise_to_front();
    }
    const lv_coord_t y_target = open ? y_shown : y_hidden;
    const lv_coord_t y_from   = open ? y_hidden : lv_obj_get_y(bot);
    const lv_anim_path_cb_t path = open ? lv_anim_path_ease_out : lv_anim_path_ease_in;
    start_slide_y(bot, y_from, y_target, NULL, NULL, path);
}

typedef struct {
    ui_power_view_mode_t mode;
} view_switch_ud_t;

static void view_switch_done(void *ud)
{
    view_switch_ud_t *v = (view_switch_ud_t *)ud;
    if (v) {
        ui_power_main_set_mode(v->mode);
        lv_mem_free(v);
    }
    screen_redraw();
}

static void switch_view_mode(lv_coord_t dx)
{
    const ui_power_view_mode_t next =
        (s_ctx.view_mode == UI_POWER_VIEW_NUMERIC) ? UI_POWER_VIEW_CHART : UI_POWER_VIEW_NUMERIC;

    lv_obj_t *num = ui_power_main_get_numeric_view();
    lv_obj_t *chart = ui_power_main_get_chart_view();
    if (!num || !chart) {
        return;
    }

    const lv_obj_t *cur      = (s_ctx.view_mode == UI_POWER_VIEW_NUMERIC) ? num : chart;
    const lv_obj_t *incoming = (next == UI_POWER_VIEW_NUMERIC) ? num : chart;
    const bool      swipe_right = dx > 0;

    s_ctx.view_mode = next;

    view_switch_ud_t *vud = (view_switch_ud_t *)lv_mem_alloc(sizeof(*vud));
    if (!vud) {
        ui_power_main_set_mode(next);
        return;
    }
    vud->mode = next;

    lv_obj_clear_flag(num, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);

    if (swipe_right) {
        /* 手指右滑：当前页向右退出，新页从左侧进入 */
        lv_obj_set_x((lv_obj_t *)cur, 0);
        lv_obj_set_x((lv_obj_t *)incoming, -s_ctx.scr_w);
        start_slide_pair((lv_obj_t *)cur, 0, s_ctx.scr_w,
                         (lv_obj_t *)incoming, -s_ctx.scr_w, 0,
                         view_switch_done, vud);
    } else {
        /* 手指左滑：当前页向左退出，新页从右侧进入 */
        lv_obj_set_x((lv_obj_t *)cur, 0);
        lv_obj_set_x((lv_obj_t *)incoming, s_ctx.scr_w);
        start_slide_pair((lv_obj_t *)cur, 0, -s_ctx.scr_w,
                         (lv_obj_t *)incoming, s_ctx.scr_w, 0,
                         view_switch_done, vud);
    }
}

static void sub_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t     *indev = lv_indev_get_act();

    if (code == LV_EVENT_PRESSED) {
        s_ctx.suppress_click = false;
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
    if (code != LV_EVENT_RELEASED || !s_ctx.ptr_valid || !indev || !s_ctx.sub_panel) {
        s_ctx.ptr_valid = false;
        return;
    }
    s_ctx.ptr_valid = false;

    lv_point_t up;
    lv_indev_get_point(indev, &up);
    const lv_coord_t dx = up.x - s_ctx.ptr_down.x;
    const lv_coord_t dy = up.y - s_ctx.ptr_down.y;
    const lv_coord_t adx = LV_ABS(dx);
    const lv_coord_t ady = LV_ABS(dy);

    if (dx > UI_POWER_SWIPE_MIN_PX && adx * 100 > ady * UI_POWER_SWIPE_RATIO) {
        lv_obj_t *cur = s_ctx.sub_panel;
        sub_back_ud_t *bud = (sub_back_ud_t *)lv_mem_alloc(sizeof(*bud));
        if (!bud) {
            return;
        }
        bud->panel = cur;
        s_ctx.sub_panel = NULL;
        s_ctx.sub_page  = UI_POWER_SUB_NONE;

        /* 返回：子页右移、主界面从左同步滑入，避免逐步暴露主界面导致刷新条纹 */
        lv_obj_set_x(s_ctx.main_panel, -s_ctx.scr_w);
        lv_obj_set_x(cur, 0);
        start_slide_pair(cur, 0, s_ctx.scr_w,
                         s_ctx.main_panel, -s_ctx.scr_w, 0,
                         sub_back_done, bud);
    }
}

static void bottom_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t     *indev = lv_indev_get_act();

    if (!ui_power_sheet_bottom_is_open()) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
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
    if (code != LV_EVENT_RELEASED || !s_ctx.ptr_valid || !indev) {
        s_ctx.ptr_valid = false;
        return;
    }
    s_ctx.ptr_valid = false;

    lv_point_t up;
    lv_indev_get_point(indev, &up);
    const lv_coord_t dx = up.x - s_ctx.ptr_down.x;
    const lv_coord_t dy = up.y - s_ctx.ptr_down.y;
    const lv_coord_t adx = LV_ABS(dx);
    const lv_coord_t ady = LV_ABS(dy);

    if (dy > UI_POWER_SWIPE_MIN_PX && ady * 100 > adx * UI_POWER_SWIPE_RATIO) {
        toggle_bottom_sheet(false);
    }
}

static void main_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t     *indev = lv_indev_get_act();

    if (s_ctx.sub_panel) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
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
    if (code != LV_EVENT_RELEASED || !s_ctx.ptr_valid || !indev) {
        s_ctx.ptr_valid = false;
        return;
    }
    s_ctx.ptr_valid = false;

    lv_point_t up;
    lv_indev_get_point(indev, &up);
    const lv_coord_t dx = up.x - s_ctx.ptr_down.x;
    const lv_coord_t dy = up.y - s_ctx.ptr_down.y;
    const lv_coord_t adx = LV_ABS(dx);
    const lv_coord_t ady = LV_ABS(dy);

    /* 顶部菜单已展开：上滑或点击菜单外区域收起 */
    if (ui_power_sheet_top_is_open()) {
        const bool swipe_up = dy < -UI_POWER_SWIPE_MIN_PX && ady * 100 > adx * UI_POWER_SWIPE_RATIO;
        const bool tap_out  = adx < UI_POWER_SWIPE_MIN_PX && ady < UI_POWER_SWIPE_MIN_PX
                              && up.y > UI_POWER_TOP_SHEET_H;
        if (swipe_up || tap_out) {
            toggle_top_sheet(false);
        }
        return;
    }

    /* 底部 PD 全屏已展开时由 bottom_touch_cb 处理 */
    if (ui_power_sheet_bottom_is_open()) {
        return;
    }

    /* 水平：任意方向切换显示模式，动画跟随手势 */
    if (adx > UI_POWER_SWIPE_MIN_PX && adx * 100 > ady * UI_POWER_SWIPE_RATIO) {
        switch_view_mode(dx);
        return;
    }

    /* 垂直：展开/收起上下面板 */
    if (ady > UI_POWER_SWIPE_MIN_PX && ady * 100 > adx * UI_POWER_SWIPE_RATIO) {
        if (dy > 0) {
            toggle_top_sheet(true);
        } else {
            toggle_bottom_sheet(true);
        }
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_power_main_refresh();
    if (ui_power_sheet_bottom_is_open()) {
        ui_pd_panel_refresh();
    }
}

typedef struct {
    pd_spoof_event_t evt;
} pd_async_msg_t;

static void pd_async_handler(void *p)
{
    pd_async_msg_t *msg = (pd_async_msg_t *)p;
    if (!msg) {
        return;
    }
    bool animate_sw = false;
    const bool notify_ui = msg->evt.from_button || msg->evt.from_remote;
    if (msg->evt.id == PD_SPOOF_EVT_TOGGLED) {
        animate_sw = notify_ui || !msg->evt.enabled;
    } else if (msg->evt.id == PD_SPOOF_EVT_VOLTAGE && !msg->evt.enabled) {
        animate_sw = true;
    }
    ui_pd_panel_sync_from_driver_ex(animate_sw);
    if (msg->evt.id == PD_SPOOF_EVT_TOGGLED) {
        pd_spoof_status_t st = {0};
        if (pd_spoof_get_status(&st) == ESP_OK) {
            ui_status_bar_sync_pd(st.enabled);
        } else {
            ui_status_bar_sync_pd(msg->evt.enabled);
        }
        if (notify_ui && !ui_power_sheet_bottom_is_open()) {
            ui_pd_panel_show_toggle_toast(msg->evt.enabled);
        }
    }
    lv_mem_free(msg);
}

static void pd_event_cb(const pd_spoof_event_t *evt, void *user_data)
{
    (void)user_data;
    if (!evt) {
        return;
    }
    if (evt->id == PD_SPOOF_EVT_TOGGLED) {
        if (evt->enabled) {
            buzzer_play_pattern(BUZZER_PATTERN_PD_ON);
        } else {
            buzzer_play_pattern(BUZZER_PATTERN_PD_OFF);
        }
    }
    pd_async_msg_t *msg = (pd_async_msg_t *)lv_mem_alloc(sizeof(*msg));
    if (!msg) {
        return;
    }
    msg->evt = *evt;
    lv_async_call(pd_async_handler, msg);
}

void ui_power_app_init(void)
{
    ui_power_app_prepare_hidden();
    lv_obj_set_style_opa(s_ctx.main_panel, LV_OPA_COVER, LV_PART_MAIN);
    ui_status_bar_boot_slide_in(NULL);
}

lv_obj_t *ui_power_app_get_screen(void)
{
    if (s_ctx.screen && lv_obj_is_valid(s_ctx.screen)) {
        return s_ctx.screen;
    }
    return NULL;
}

lv_obj_t *ui_power_app_get_stage(void)
{
    if (s_ctx.stage && lv_obj_is_valid(s_ctx.stage)) {
        return s_ctx.stage;
    }
    return NULL;
}

typedef struct {
    void (*cb)(void);
} overlay_slide_ud_t;

static void overlay_slide_done(void *ud)
{
    overlay_slide_ud_t *msg = (overlay_slide_ud_t *)ud;
    if (msg) {
        if (msg->cb) {
            msg->cb();
        }
        lv_mem_free(msg);
    }
}

void ui_power_app_slide_overlay_down(lv_obj_t *overlay, void (*on_done)(void))
{
    if (!overlay || !lv_obj_is_valid(overlay) || !s_ctx.screen) {
        if (on_done) {
            on_done();
        }
        return;
    }

    lv_anim_del(overlay, anim_set_y_cb);

    if (lv_obj_get_parent(overlay) != s_ctx.screen) {
        lv_obj_set_parent(overlay, s_ctx.screen);
    }

    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(overlay, s_ctx.scr_w, s_ctx.scr_h);
    lv_obj_set_pos(overlay, 0, lv_obj_get_y(overlay));
    lv_obj_move_foreground(overlay);

    overlay_slide_ud_t *msg = (overlay_slide_ud_t *)lv_mem_alloc(sizeof(*msg));
    if (!msg) {
        if (on_done) {
            on_done();
        }
        return;
    }
    msg->cb = on_done;

    const lv_coord_t y_from = lv_obj_get_y(overlay);
    /* 与 toggle_bottom_sheet(false) 相同：全屏 overlay 向下滑出，ease_in。 */
    start_slide_y(overlay, y_from, s_ctx.scr_h, overlay_slide_done, msg, lv_anim_path_ease_in);
}

void ui_power_app_prepare_hidden(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.view_mode = UI_POWER_VIEW_NUMERIC;
    s_ctx.sub_page  = UI_POWER_SUB_NONE;

    lv_disp_t *disp = lv_disp_get_default();
    s_ctx.scr_w = lv_disp_get_hor_res(disp);
    s_ctx.scr_h = lv_disp_get_ver_res(disp);

    lv_obj_t *scr = lv_obj_create(NULL);
    s_ctx.screen = scr;
    lv_obj_set_size(scr, s_ctx.scr_w, s_ctx.scr_h);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_POWER_MAIN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    lv_obj_t *st = lv_obj_create(scr);
    s_ctx.stage = st;
    lv_obj_set_size(st, s_ctx.scr_w, s_ctx.scr_h);
    lv_obj_set_style_bg_color(st, lv_color_hex(UI_POWER_MAIN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(st, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(st, 0, LV_PART_MAIN);
    lv_obj_add_flag(st, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(st, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(st, main_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(st, main_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(st, main_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *main_p = lv_obj_create(st);
    s_ctx.main_panel = main_p;
    lv_obj_set_size(main_p, s_ctx.scr_w, s_ctx.scr_h);
    lv_obj_set_pos(main_p, 0, 0);
    panel_apply_theme(main_p);
    lv_obj_clear_flag(main_p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(main_p, main_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(main_p, main_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(main_p, main_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    ui_power_main_create(main_p);

    ui_power_sheet_create(scr, sheet_action_cb, NULL);
    pd_spoof_set_event_cb(pd_event_cb, NULL);

    lv_obj_t *bottom = ui_power_sheet_get_bottom();
    if (bottom) {
        lv_obj_add_flag(bottom, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bottom, bottom_touch_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(bottom, bottom_touch_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(bottom, bottom_touch_cb, LV_EVENT_PRESS_LOST, NULL);
    }

    ui_status_bar_init(scr);

    lv_disp_load_scr(scr);

    s_ctx.refresh_timer = lv_timer_create(refresh_timer_cb, UI_POWER_REFRESH_MS, NULL);
    if (s_ctx.refresh_timer) {
        lv_timer_set_repeat_count(s_ctx.refresh_timer, -1);
    }
}

void ui_power_app_fade_in(void)
{
    if (!s_ctx.main_panel || !lv_obj_is_valid(s_ctx.main_panel)) {
        return;
    }

    lv_obj_t *scr = s_ctx.screen;
    if (scr && lv_obj_is_valid(scr)) {
        lv_obj_invalidate(scr);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ctx.main_panel);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, UI_BOOT_SPLASH_MAIN_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    ui_status_bar_boot_slide_in(NULL);
}
