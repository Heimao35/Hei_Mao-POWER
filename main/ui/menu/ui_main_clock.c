/**
 * @file ui_main_clock.c
 *
 * Parent: main menu panel so the clock translates with the main page slide.
 * Digit strip: symbols ordered with '-' near bottom of strip geometry so that
 * when the shown slot index increases, strip Y moves downward (ease-out).
 */
#include "ui_main_clock.h"
#include "ui_power_config.h"
#include "app_time.h"

#include "lvgl.h"

#include <string.h>

#define CLOCK_FONT               (&lv_font_montserrat_48)
#define CLOCK_DIGIT_COL_W        40
#define CLOCK_SLOT_PAD           4
#define CLOCK_ROW_GAP            10
#define CLOCK_FROM_RIGHT         32
#define CLOCK_ANIM_BASE_MS       380
#define CLOCK_ANIM_PER_SLOT_MS   95
#define CLOCK_ANIM_MAX_MS        2200
/** After UI init, wait this long before the first clock digit update (RTC/SNTP read). */
#define CLOCK_BOOT_DELAY_MS      1000

/** Strip row index 0 = '-', 1..10 = '0'..'9' (logical slot in digit_to_slot). */
#define CLOCK_STRIP_LAST_ROW     10

typedef struct {
    lv_obj_t *viewport;
    lv_obj_t *strip;
    int8_t    shown_digit;
    int8_t    target_digit;
} roll_digit_t;

typedef struct {
    lv_obj_t    *root;
    lv_timer_t  *tick;
    roll_digit_t h10;
    roll_digit_t h01;
    roll_digit_t m10;
    roll_digit_t m01;
    int          last_h;
    int          last_m;
    bool         had_valid_time;
} ui_clock_ctx_t;

static ui_clock_ctx_t s_clk;
static bool         s_clock_boot_wait = true;

static uint8_t digit_to_slot(int8_t d)
{
    if (d < 0) {
        return 0;
    }
    if (d > 9) {
        d = 9;
    }
    return (uint8_t)(1u + (uint8_t)d);
}

static lv_coord_t slot_height(void)
{
    return lv_font_get_line_height(CLOCK_FONT) + CLOCK_SLOT_PAD * 2;
}

/** Strip Y so row @p s (0=dash..10='9') is aligned to the viewport top. */
static lv_coord_t strip_y_for_slot(uint8_t s)
{
    const lv_coord_t sh = slot_height();
    return (lv_coord_t)(-((int32_t)CLOCK_STRIP_LAST_ROW - (int32_t)s) * (int32_t)sh);
}

static void roll_digit_sync_shown_from_geometry(roll_digit_t *r)
{
    if (!r->strip) {
        return;
    }
    const lv_coord_t sh = slot_height();
    const lv_coord_t y  = lv_obj_get_y(r->strip);
    int32_t          s;
    if (y <= 0) {
        s = CLOCK_STRIP_LAST_ROW - (int32_t)(((int32_t)(-y) + (int32_t)sh / 2) / (int32_t)sh);
    } else {
        s = CLOCK_STRIP_LAST_ROW - (int32_t)(((int32_t)y + (int32_t)sh / 2) / (int32_t)sh);
    }
    if (s < 0) {
        s = 0;
    }
    if (s > CLOCK_STRIP_LAST_ROW) {
        s = CLOCK_STRIP_LAST_ROW;
    }
    r->shown_digit = (s == 0) ? (int8_t)-1 : (int8_t)(s - 1);
}

static void anim_strip_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void boot_root_set_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

static void roll_anim_ready_cb(lv_anim_t *anim)
{
    roll_digit_t *rd = (roll_digit_t *)lv_anim_get_user_data(anim);
    if (rd) {
        rd->shown_digit = rd->target_digit;
    }
}

static uint32_t anim_duration_ms(uint8_t from_slot, uint8_t to_slot)
{
    const int steps = (int)to_slot - (int)from_slot;
    const int dist  = steps >= 0 ? steps : -steps;
    uint32_t ms     = CLOCK_ANIM_BASE_MS + (uint32_t)dist * CLOCK_ANIM_PER_SLOT_MS;
    if (ms > CLOCK_ANIM_MAX_MS) {
        ms = CLOCK_ANIM_MAX_MS;
    }
    return ms;
}

static lv_coord_t digit_col_width(void)
{
    const uint16_t gw = lv_font_get_glyph_width(CLOCK_FONT, '0', '0');
    lv_coord_t     w  = (lv_coord_t)gw + 12;
    if (w < CLOCK_DIGIT_COL_W) {
        w = CLOCK_DIGIT_COL_W;
    }
    return w;
}

static void roll_digit_build(roll_digit_t *r, lv_obj_t *parent, lv_coord_t col_w)
{
    memset(r, 0, sizeof(*r));
    r->shown_digit  = -1;
    r->target_digit = -1;

    const lv_coord_t sh = slot_height();

    r->viewport = lv_obj_create(parent);
    lv_obj_clear_flag(r->viewport, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r->viewport, col_w, sh);
    lv_obj_set_style_bg_opa(r->viewport, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(r->viewport, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(r->viewport, 0, LV_PART_MAIN);

    r->strip = lv_obj_create(r->viewport);
    lv_obj_clear_flag(r->strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r->strip, col_w, sh * 11);
    lv_obj_set_style_bg_opa(r->strip, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(r->strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(r->strip, 0, LV_PART_MAIN);
    lv_obj_set_pos(r->strip, 0, strip_y_for_slot(0));

    static const char *const sym[] = {
        "-", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    };
    for (int i = 0; i < 11; i++) {
        lv_obj_t *lb = lv_label_create(r->strip);
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(lb, sym[i]);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lb, CLOCK_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(lb, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
        lv_obj_set_style_text_opa(lb, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(lb, 1, LV_PART_MAIN);
        lv_obj_set_size(lb, col_w, sh);
        /* Row i (slot i): place '-' at bottom (large Y), '9' at top — increasing slot scrolls downward. */
        lv_obj_set_pos(lb, 0, (CLOCK_STRIP_LAST_ROW - i) * sh);
    }
}

static void roll_digit_animate_to(roll_digit_t *r, int8_t new_digit)
{
    if (!r->strip || !lv_obj_is_valid(r->strip)) {
        return;
    }
    r->target_digit = new_digit;
    lv_anim_del(r->strip, anim_strip_y);
    roll_digit_sync_shown_from_geometry(r);

    const uint8_t from_slot = digit_to_slot(r->shown_digit);
    const uint8_t to_slot   = digit_to_slot(new_digit);
    if (from_slot == to_slot) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r->strip);
    lv_anim_set_exec_cb(&a, anim_strip_y);
    lv_anim_set_values(&a, strip_y_for_slot(from_slot), strip_y_for_slot(to_slot));
    lv_anim_set_time(&a, anim_duration_ms(from_slot, to_slot));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, roll_anim_ready_cb);
    lv_anim_set_user_data(&a, r);
    lv_anim_start(&a);
}

static void apply_time_digits(int h24, int minute, bool valid)
{
    if (!valid) {
        roll_digit_animate_to(&s_clk.h10, -1);
        roll_digit_animate_to(&s_clk.h01, -1);
        roll_digit_animate_to(&s_clk.m10, -1);
        roll_digit_animate_to(&s_clk.m01, -1);
        s_clk.last_h         = -1;
        s_clk.last_m         = -1;
        s_clk.had_valid_time = false;
        return;
    }

    const int h10d = (h24 / 10) % 10;
    const int h01d = h24 % 10;
    const int m10d = (minute / 10) % 10;
    const int m01d = minute % 10;

    if (!s_clk.had_valid_time || h24 != s_clk.last_h || minute != s_clk.last_m) {
        roll_digit_animate_to(&s_clk.h10, (int8_t)h10d);
        roll_digit_animate_to(&s_clk.h01, (int8_t)h01d);
        roll_digit_animate_to(&s_clk.m10, (int8_t)m10d);
        roll_digit_animate_to(&s_clk.m01, (int8_t)m01d);
    }

    s_clk.last_h         = h24;
    s_clk.last_m         = minute;
    s_clk.had_valid_time = true;
}

static void clock_timer_cb(lv_timer_t *t)
{
    ui_clock_ctx_t *c = (ui_clock_ctx_t *)t->user_data;
    if (!c || !c->root || !lv_obj_is_valid(c->root)) {
        return;
    }

    if (s_clock_boot_wait) {
        s_clock_boot_wait = false;
        lv_timer_set_period(t, 250);
    }

    struct tm tmv;
    const bool ok = app_time_local_tm(&tmv);

    if (!ok) {
        apply_time_digits(0, 0, false);
        lv_timer_set_period(t, 250);
        return;
    }
    lv_timer_set_period(t, 1000);
    apply_time_digits(tmv.tm_hour, tmv.tm_min, true);
}

void ui_main_clock_create(lv_obj_t *main_panel)
{
    if (!main_panel || s_clk.root) {
        return;
    }

    memset(&s_clk, 0, sizeof(s_clk));
    s_clock_boot_wait = true;

    lv_obj_update_layout(main_panel);

    lv_disp_t *disp = lv_obj_get_disp(main_panel);

    const lv_coord_t col_w  = digit_col_width();
    const lv_coord_t sh     = slot_height();
    const lv_coord_t row_w  = col_w * 2 + 6;
    const lv_coord_t root_w = row_w;
    const lv_coord_t root_h = sh * 2 + CLOCK_ROW_GAP;
    const lv_coord_t res_h  = lv_disp_get_ver_res(disp);

    s_clk.root = lv_obj_create(main_panel);
    lv_obj_add_flag(s_clk.root, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_clk.root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_clk.root, root_w, root_h);
    lv_obj_set_style_bg_opa(s_clk.root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_clk.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_clk.root, 0, LV_PART_MAIN);

    lv_obj_t *hh = lv_obj_create(s_clk.root);
    lv_obj_clear_flag(hh, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(hh, 0, 0);
    lv_obj_set_size(hh, row_w, sh);
    lv_obj_set_style_bg_opa(hh, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hh, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hh, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(hh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hh, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hh, 4, LV_PART_MAIN);

    lv_obj_t *mm = lv_obj_create(s_clk.root);
    lv_obj_clear_flag(mm, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(mm, 0, sh + CLOCK_ROW_GAP);
    lv_obj_set_size(mm, row_w, sh);
    lv_obj_set_style_bg_opa(mm, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mm, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mm, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(mm, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mm, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mm, 4, LV_PART_MAIN);

    roll_digit_build(&s_clk.h10, hh, col_w);
    roll_digit_build(&s_clk.h01, hh, col_w);
    roll_digit_build(&s_clk.m10, mm, col_w);
    roll_digit_build(&s_clk.m01, mm, col_w);

    lv_obj_update_layout(hh);
    lv_obj_update_layout(mm);
    lv_obj_update_layout(s_clk.root);

    lv_obj_align(s_clk.root, LV_ALIGN_TOP_RIGHT, -CLOCK_FROM_RIGHT, (res_h - root_h) / 2);
    lv_obj_move_foreground(s_clk.root);
    lv_obj_clear_flag(s_clk.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_clk.root);

    s_clk.tick = lv_timer_create(clock_timer_cb, CLOCK_BOOT_DELAY_MS, &s_clk);
    if (s_clk.tick) {
        lv_timer_set_repeat_count(s_clk.tick, -1);
    }
}

void ui_main_clock_boot_slide_in(void)
{
    if (!s_clk.root || !lv_obj_is_valid(s_clk.root)) {
        return;
    }

    lv_obj_update_layout(s_clk.root);
    /*
     * Must use style X (lv_obj_get_x_aligned), not lv_obj_get_x(): for LV_ALIGN_TOP_RIGHT,
     * lv_obj_get_x() is the laid-out coordinate while lv_obj_set_x() writes LV_STYLE_X.
     * Mixing them doubles the TOP_RIGHT (pw - w) offset and moves the clock off-screen.
     */
    const lv_coord_t x_target = lv_obj_get_x_aligned(s_clk.root);
    const lv_coord_t x_from = x_target + UI_POWER_BOOT_CLOCK_SLIDE_PX;
    lv_obj_set_x(s_clk.root, x_from);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_clk.root);
    lv_anim_set_exec_cb(&a, boot_root_set_x_cb);
    lv_anim_set_values(&a, x_from, x_target);
    lv_anim_set_time(&a, UI_POWER_BOOT_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
