/**
 * @file ui_menu_wheel.c
 */
#include "ui_menu_wheel.h"
#include "ui_menu_config.h"
#include <string.h>

struct ui_menu_wheel {
    lv_obj_t *scroll;
    uint32_t count;
    uint32_t selected_index;
};

static void wheel_apply_row_style(lv_obj_t *row, lv_coord_t dist_from_center)
{
    const lv_coord_t d = dist_from_center < 0 ? 0 : dist_from_center;
    const bool       hi = d <= UI_MENU_WHEEL_HIGHLIGHT_PX;

    lv_obj_t *lbl = lv_obj_get_child(row, 0);
    if (lbl) {
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_26, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(hi ? 0x5EEAD4 : 0x64748B), LV_PART_MAIN);
        lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_invalidate(lbl);
    }

    lv_obj_set_style_bg_color(row, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, hi ? LV_OPA_40 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_invalidate(row);
}

static void wheel_refresh(ui_menu_wheel_t *w)
{
    if (!w || !w->scroll) {
        return;
    }

    /* Use the scroll widget's vertical center (matches snap target; works for partial-height wheels). */
    lv_area_t scroll_area;
    lv_obj_get_coords(w->scroll, &scroll_area);
    const lv_coord_t mid_scr_y = (scroll_area.y1 + scroll_area.y2) / 2;
    uint32_t best_i = 0;
    lv_coord_t best_d = LV_COORD_MAX;

    const uint32_t n = lv_obj_get_child_cnt(w->scroll);
    uint32_t menu_idx = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(w->scroll, i);
        if (!lv_obj_has_flag(row, LV_OBJ_FLAG_SNAPPABLE)) {
            continue;
        }

        lv_area_t a;
        lv_obj_get_coords(row, &a);
        const lv_coord_t cy = (a.y1 + a.y2) / 2;
        const lv_coord_t d = LV_ABS(cy - mid_scr_y);
        if (d < best_d) {
            best_d = d;
            best_i = menu_idx;
        }
        wheel_apply_row_style(row, d);
        menu_idx++;
    }

    w->selected_index = best_i;
}

static void wheel_on_scroll(lv_event_t *e)
{
    ui_menu_wheel_t *w = lv_event_get_user_data(e);
    wheel_refresh(w);
}

ui_menu_wheel_t *ui_menu_wheel_create(lv_obj_t *parent, const char *const *items, uint32_t count,
                                        lv_coord_t content_top_reserve)
{
    if (!parent || !items || count == 0) {
        return NULL;
    }

    if (content_top_reserve < 0) {
        content_top_reserve = 0;
    }

    lv_obj_update_layout(parent);

    ui_menu_wheel_t *w = (ui_menu_wheel_t *)lv_mem_alloc(sizeof(*w));
    if (!w) {
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->count = count;

    lv_obj_t *sc = lv_obj_create(parent);
    w->scroll = sc;
    lv_obj_set_user_data(sc, w);
    lv_obj_add_event_cb(sc, wheel_on_scroll, LV_EVENT_SCROLL, w);
    lv_obj_add_event_cb(sc, wheel_on_scroll, LV_EVENT_SCROLL_END, w);
    lv_obj_add_event_cb(sc, wheel_on_scroll, LV_EVENT_SIZE_CHANGED, w);

    {
        lv_coord_t pw = lv_obj_get_width(parent);
        if (pw < UI_MENU_WHEEL_COL_WIDTH) {
            pw = UI_MENU_WHEEL_COL_WIDTH;
        }
        lv_coord_t ph = lv_obj_get_height(parent);
        if (ph < UI_MENU_WHEEL_ROW_HEIGHT * 2) {
            ph = lv_disp_get_ver_res(lv_obj_get_disp(parent));
        }
        /* Full parent width: vertical drag anywhere hits this scroll area (content stays left via flex). */
        lv_obj_set_size(sc, pw, ph);
    }
    lv_obj_align(sc, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sc, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(sc, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    /* Column flow: track_place is horizontal — START keeps the column on the left (CENTER was centering the strip). */
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_layout(sc, LV_LAYOUT_FLEX);

    const lv_coord_t ver = lv_obj_get_height(parent);
    const lv_coord_t pad_edge = (ver > UI_MENU_WHEEL_ROW_HEIGHT)
                                    ? (ver - UI_MENU_WHEEL_ROW_HEIGHT) / 2
                                    : 0;
    /* Symmetric extra reserve keeps end-of-list snap symmetric when a header band is only visual (floating). */
    lv_obj_set_style_pad_top(sc, pad_edge + content_top_reserve, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(sc, pad_edge + content_top_reserve, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sc, UI_MENU_WHEEL_ROW_GAP, LV_PART_MAIN);

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *row = lv_obj_create(sc);
        lv_obj_set_size(row, UI_MENU_WHEEL_COL_WIDTH - 16, UI_MENU_WHEEL_ROW_HEIGHT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SNAPPABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(row, false, LV_PART_MAIN);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_label_set_text(lbl, items[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_26, LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, UI_MENU_WHEEL_COL_WIDTH - 40);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
    }

    lv_obj_update_layout(sc);
    wheel_refresh(w);
    /* scroll_y defaults to 0; snap is only applied after user scroll unless we run this once. */
    lv_obj_update_snap(sc, LV_ANIM_OFF);
    wheel_refresh(w);
    return w;
}

void ui_menu_wheel_delete(ui_menu_wheel_t *wheel)
{
    if (!wheel) {
        return;
    }
    if (wheel->scroll) {
        lv_obj_del(wheel->scroll);
    }
    lv_mem_free(wheel);
}

uint32_t ui_menu_wheel_get_selected_index(const ui_menu_wheel_t *wheel)
{
    if (!wheel) {
        return 0;
    }
    return wheel->selected_index;
}

lv_obj_t *ui_menu_wheel_get_root(const ui_menu_wheel_t *wheel)
{
    return wheel ? wheel->scroll : NULL;
}
