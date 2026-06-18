/**
 * @file ui_boot_splash.c
 * @brief Hei_Mao-POWER 开机动画：圆环内 Logo、下滑退出。
 *
 * 与底部 PD 面板相同：overlay 直接挂在主 screen 上滑动，不切换 screen、
 * 不使用 transform 叠加全屏位移，主界面保持不透明，避免分块刷新拖影。
 */
#include "ui_boot_splash.h"
#include "ui_power_app.h"
#include "ui_power_config.h"
#include "ui_status_bar.h"

#include "lvgl.h"

#include <string.h>

#define BOOT_ARC_SIZE          196
#define BOOT_ARC_WIDTH         6
#define BOOT_LOGO_ZOOM_START   210   /* 256 = 100% */
#define BOOT_LOGO_ZOOM_END     256
#define BOOT_LOGO_SLIDE_PX     20
#define BOOT_BAR_W_MAX         96
#define BOOT_BAR_H             4
#define BOOT_SUBTITLE_GAP      12
#define BOOT_BAR_GAP           10

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *logo_col;
    lv_obj_t *arc;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *bar;
} boot_splash_ctx_t;

static boot_splash_ctx_t s_boot;

static void anim_set_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN);
}

static void anim_set_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_zoom(void *var, int32_t v)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)var, (lv_coord_t)v, LV_PART_MAIN);
}

static void anim_set_arc(void *var, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)var, (int32_t)v);
}

static void anim_set_bar_w_centered(void *var, int32_t v)
{
    lv_obj_t *bar = (lv_obj_t *)var;
    const lv_coord_t w = (lv_coord_t)v;
    lv_obj_set_width(bar, w);

    lv_obj_t *parent = lv_obj_get_parent(bar);
    if (parent) {
        lv_obj_set_x(bar, (lv_obj_get_width(parent) - w) / 2);
    }
}

static void boot_reset_transforms(void)
{
    if (s_boot.logo_col && lv_obj_is_valid(s_boot.logo_col)) {
        lv_obj_set_style_transform_zoom(s_boot.logo_col, 256, LV_PART_MAIN);
        lv_obj_update_layout(s_boot.logo_col);
    }
}

static void boot_cleanup_panel(void)
{
    if (s_boot.panel && lv_obj_is_valid(s_boot.panel)) {
        lv_anim_del(s_boot.panel, NULL);
        lv_obj_del(s_boot.panel);
        s_boot.panel = NULL;
    }
}

static void boot_after_slide(void)
{
    boot_cleanup_panel();
    ui_status_bar_boot_slide_in();
    memset(&s_boot, 0, sizeof(s_boot));
}

static void boot_begin_transition(void *p)
{
    (void)p;

    if (!s_boot.panel || !lv_obj_is_valid(s_boot.panel)) {
        ui_status_bar_boot_slide_in();
        memset(&s_boot, 0, sizeof(s_boot));
        return;
    }

    boot_reset_transforms();
    ui_power_app_slide_overlay_down(s_boot.panel, boot_after_slide);
}

static void start_slide_out(lv_anim_t *a)
{
    (void)a;

    if (lv_async_call(boot_begin_transition, NULL) != LV_RES_OK) {
        boot_begin_transition(NULL);
    }
}

static void start_hold(lv_anim_t *a)
{
    (void)a;

    lv_anim_t hold;
    lv_anim_init(&hold);
    lv_anim_set_var(&hold, s_boot.panel);
    lv_anim_set_exec_cb(&hold, anim_set_opa);
    lv_anim_set_values(&hold, LV_OPA_COVER, LV_OPA_COVER);
    lv_anim_set_time(&hold, UI_BOOT_SPLASH_HOLD_MS);
    lv_anim_set_ready_cb(&hold, start_slide_out);
    lv_anim_start(&hold);
}

static void start_bar_fill(lv_anim_t *a)
{
    (void)a;

    lv_anim_t bar;
    lv_anim_init(&bar);
    lv_anim_set_var(&bar, s_boot.bar);
    lv_anim_set_exec_cb(&bar, anim_set_bar_w_centered);
    lv_anim_set_values(&bar, 0, BOOT_BAR_W_MAX);
    lv_anim_set_time(&bar, UI_BOOT_SPLASH_BAR_MS);
    lv_anim_set_path_cb(&bar, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&bar, start_hold);
    lv_anim_start(&bar);
}

static void start_intro(lv_anim_t *a)
{
    (void)a;

    const lv_coord_t y_end   = lv_obj_get_y(s_boot.logo_col);
    const lv_coord_t y_start = y_end + BOOT_LOGO_SLIDE_PX;

    lv_obj_set_y(s_boot.logo_col, y_start);
    lv_obj_set_style_opa(s_boot.logo_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_transform_zoom(s_boot.logo_col, BOOT_LOGO_ZOOM_START, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(s_boot.logo_col, lv_obj_get_width(s_boot.logo_col) / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_boot.logo_col, lv_obj_get_height(s_boot.logo_col) / 2, LV_PART_MAIN);

    lv_anim_t move;
    lv_anim_init(&move);
    lv_anim_set_var(&move, s_boot.logo_col);
    lv_anim_set_exec_cb(&move, anim_set_y);
    lv_anim_set_values(&move, y_start, y_end);
    lv_anim_set_time(&move, UI_BOOT_SPLASH_FADE_IN_MS);
    lv_anim_set_path_cb(&move, lv_anim_path_ease_out);
    lv_anim_start(&move);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_boot.logo_col);
    lv_anim_set_exec_cb(&fade, anim_set_opa);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade, UI_BOOT_SPLASH_FADE_IN_MS);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_start(&fade);

    lv_anim_t zoom;
    lv_anim_init(&zoom);
    lv_anim_set_var(&zoom, s_boot.logo_col);
    lv_anim_set_exec_cb(&zoom, anim_set_zoom);
    lv_anim_set_values(&zoom, BOOT_LOGO_ZOOM_START, BOOT_LOGO_ZOOM_END);
    lv_anim_set_time(&zoom, UI_BOOT_SPLASH_FADE_IN_MS);
    lv_anim_set_path_cb(&zoom, lv_anim_path_ease_out);
    lv_anim_start(&zoom);

    lv_anim_t arc;
    lv_anim_init(&arc);
    lv_anim_set_var(&arc, s_boot.arc);
    lv_anim_set_exec_cb(&arc, anim_set_arc);
    lv_anim_set_values(&arc, 0, 100);
    lv_anim_set_time(&arc, UI_BOOT_SPLASH_ARC_MS);
    lv_anim_set_path_cb(&arc, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&arc, start_bar_fill);
    lv_anim_start(&arc);
}

static void logo_col_place_arc_center(lv_obj_t *logo_col, lv_obj_t *arc, lv_obj_t *subtitle,
                                      lv_obj_t *bar, lv_coord_t scr_w, lv_coord_t scr_h)
{
    lv_obj_update_layout(logo_col);

    lv_coord_t col_w = lv_obj_get_width(arc);
    if (subtitle && lv_obj_get_width(subtitle) > col_w) {
        col_w = lv_obj_get_width(subtitle);
    }
    if (BOOT_BAR_W_MAX > col_w) {
        col_w = BOOT_BAR_W_MAX;
    }

    lv_coord_t col_h = lv_obj_get_height(arc);
    if (subtitle) {
        col_h = lv_obj_get_y(subtitle) + lv_obj_get_height(subtitle);
    }
    if (bar) {
        const lv_coord_t bar_bottom = lv_obj_get_y(bar) + lv_obj_get_height(bar);
        if (bar_bottom > col_h) {
            col_h = bar_bottom;
        }
    }

    lv_obj_set_size(logo_col, col_w, col_h);

    if (subtitle) {
        lv_obj_align_to(subtitle, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, BOOT_SUBTITLE_GAP);
    }
    if (bar && subtitle) {
        lv_obj_align_to(bar, subtitle, LV_ALIGN_OUT_BOTTOM_MID, 0, BOOT_BAR_GAP);
        lv_obj_set_x(bar, (col_w - lv_obj_get_width(bar)) / 2);
    }

    const lv_coord_t arc_cy = lv_obj_get_y(arc) + lv_obj_get_height(arc) / 2;
    const lv_coord_t x      = (scr_w - col_w) / 2;
    const lv_coord_t y      = (scr_h / 2) - arc_cy;
    lv_obj_set_pos(logo_col, x, y);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lb, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_opa(lb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(lb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lb, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
    return lb;
}

void ui_boot_splash_play(void)
{
    lv_obj_t *scr = ui_power_app_get_screen();
    if (!scr || s_boot.panel) {
        return;
    }

    memset(&s_boot, 0, sizeof(s_boot));

    const lv_coord_t scr_w = lv_disp_get_hor_res(lv_disp_get_default());
    const lv_coord_t scr_h = lv_disp_get_ver_res(lv_disp_get_default());

    lv_obj_t *panel = lv_obj_create(scr);
    s_boot.panel = panel;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(panel, scr_w, scr_h);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_POWER_MAIN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_move_foreground(panel);

    lv_obj_t *logo_col = lv_obj_create(panel);
    s_boot.logo_col = logo_col;
    lv_obj_add_flag(logo_col, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(logo_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(logo_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(logo_col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(logo_col, 0, LV_PART_MAIN);
    lv_obj_clear_flag(logo_col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_opa(logo_col, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *arc = lv_arc_create(logo_col);
    s_boot.arc = arc;
    lv_obj_set_size(arc, BOOT_ARC_SIZE, BOOT_ARC_SIZE);
    lv_obj_set_pos(arc, 0, 0);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, BOOT_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, BOOT_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_BOOT_SPLASH_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);

    s_boot.title = make_label(arc, "Hei_Mao", &lv_font_montserrat_28, 0xF8FAFC);
    lv_obj_center(s_boot.title);

    s_boot.subtitle = make_label(logo_col, "-POWER", &lv_font_montserrat_22, UI_BOOT_SPLASH_ACCENT);
    lv_obj_align_to(s_boot.subtitle, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, BOOT_SUBTITLE_GAP);

    lv_obj_t *bar = lv_obj_create(logo_col);
    s_boot.bar = bar;
    lv_obj_set_size(bar, 0, BOOT_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BOOT_SPLASH_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, BOOT_BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(bar, s_boot.subtitle, LV_ALIGN_OUT_BOTTOM_MID, 0, BOOT_BAR_GAP);

    logo_col_place_arc_center(logo_col, arc, s_boot.subtitle, bar, scr_w, scr_h);
    lv_obj_update_layout(panel);

    lv_anim_t kick;
    lv_anim_init(&kick);
    lv_anim_set_var(&kick, panel);
    lv_anim_set_exec_cb(&kick, anim_set_opa);
    lv_anim_set_values(&kick, LV_OPA_COVER, LV_OPA_COVER);
    lv_anim_set_time(&kick, 1);
    lv_anim_set_ready_cb(&kick, start_intro);
    lv_anim_start(&kick);
}
