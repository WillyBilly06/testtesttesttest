/* Header file declaration */
#include "include/elecrow_ui.h"
#include "stdio.h"
#include "esp_log.h"

LV_IMG_DECLARE(cp_logo_alt_rev_splash);

/* Variable declaration */
#define TAG "elecrow_ui"
#define BRAND_GREEN 0x154734
#define BRAND_GREEN_LIGHT 0x4F775E
#define BRAND_SURFACE 0xF7FAF7
#define BRAND_SURFACE_ALT 0xE7F0EB
#define BRAND_TEXT 0x193A2C
#define BRAND_TEXT_MUTED 0xD4E2DA

static lv_obj_t *Elecrow_logo_screen;
static lv_obj_t *Elecrow_P_bar_screen;
static lv_obj_t *Elecrow_touch_screen;
static lv_obj_t *Bp_bg_img;
static lv_obj_t *Bp_logo_img;
static lv_obj_t *Load_bg_img;
static lv_obj_t *Load_frame_img;
static lv_obj_t *Load_label;
static lv_obj_t *Load_brand_title;
static lv_obj_t *Progress_bar_frame;
static lv_obj_t *Progress_bar_img;
static lv_obj_t *ui_menu;
static lv_obj_t *ui_enter_touch;
static lv_obj_t *ui_exit_touch;
static lv_obj_t *ui_touch_xy;
lv_timer_t *move_down_logo_timer;
lv_timer_t *loading_bar_timer;
bool elecrow_success = false;
bool enter_touch_flag = false;
static int progress = 0;

static void apply_brand_screen_style(lv_obj_t *screen)
{
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(screen, LV_DIR_NONE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BRAND_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *top_orb = lv_obj_create(screen);
    lv_obj_set_size(top_orb, 420, 420);
    lv_obj_align(top_orb, LV_ALIGN_TOP_RIGHT, 160, -180);
    lv_obj_clear_flag(top_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(top_orb, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(top_orb, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(top_orb, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(top_orb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(top_orb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *left_band = lv_obj_create(screen);
    lv_obj_set_size(left_band, 460, 200);
    lv_obj_align(left_band, LV_ALIGN_BOTTOM_LEFT, -70, 80);
    lv_obj_clear_flag(left_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(left_band, 96, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(left_band, lv_color_hex(BRAND_GREEN_LIGHT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(left_band, 84, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(left_band, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(left_band, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *right_band = lv_obj_create(screen);
    lv_obj_set_size(right_band, 320, 120);
    lv_obj_align(right_band, LV_ALIGN_TOP_RIGHT, 92, 54);
    lv_obj_clear_flag(right_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(right_band, 60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(right_band, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(right_band, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(right_band, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(right_band, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void apply_card_style(lv_obj_t *obj, lv_coord_t radius, lv_opa_t bg_opa)
{
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, bg_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xC7DACE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(BRAND_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(obj, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 190, 58);
    lv_obj_align(button, align, x_ofs, y_ofs);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(button, 28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, 38, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(button, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(button, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(button, lv_color_hex(BRAND_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(label);

    return button;
}

static void set_action_button_pressed(lv_obj_t *button, bool pressed)
{
    lv_obj_set_style_bg_opa(button, pressed ? 72 : 38, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, pressed ? lv_color_hex(0xA7C6B7) : lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* Functional function */
static void loading_bar_time_cb(lv_timer_t *timer)
{
    char str[32];

    if (enter_touch_flag) {
        progress = 0;
        if (Load_label != NULL) {
            lv_label_set_text(Load_label, "Initializing 0%");
        }
        if (Progress_bar_img != NULL) {
            lv_bar_set_value(Progress_bar_img, 0, LV_ANIM_OFF);
        }
        return;
    }

    progress += 1;
    if (progress > 100) {
        progress = 100;
    }

    snprintf(str, sizeof(str), "Initializing %d%%", progress);
    lv_label_set_text(Load_label, str);
    lv_bar_set_value(Progress_bar_img, progress, LV_ANIM_OFF);

    if (progress >= 100) {
        lv_timer_del(timer);
        loading_bar_timer = NULL;
        elecrow_success = true;
    }
}

static void switch_to_loading_page_timer_cb(lv_timer_t *timer)
{
    if (timer != NULL) {
        lv_timer_del(timer);
    }
    move_down_logo_timer = NULL;
    elecrow_success = true;
}

void ui_enter_touch_event_cb(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if (event_code == LV_EVENT_PRESSED) {
        set_action_button_pressed(target, true);
    }
    if ((event_code == LV_EVENT_RELEASED) || (event_code == LV_EVENT_PRESS_LOST)) {
        set_action_button_pressed(target, false);
    }
    if (event_code == LV_EVENT_CLICKED) {
        enter_touch_flag = true;
        lv_scr_load_anim(Elecrow_touch_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
    }
}

void ui_menu_event_cb(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if (event_code == LV_EVENT_PRESSED) {
        set_action_button_pressed(target, true);
    }
    if ((event_code == LV_EVENT_RELEASED) || (event_code == LV_EVENT_PRESS_LOST)) {
        set_action_button_pressed(target, false);
    }
    if (event_code == LV_EVENT_CLICKED) {
        if (loading_bar_timer != NULL) {
            lv_timer_del(loading_bar_timer);
            loading_bar_timer = NULL;
        }
        elecrow_success = true;
    }
}

void ui_exit_touch_event_cb(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if (event_code == LV_EVENT_PRESSED) {
        set_action_button_pressed(target, true);
    }
    if ((event_code == LV_EVENT_RELEASED) || (event_code == LV_EVENT_PRESS_LOST)) {
        set_action_button_pressed(target, false);
    }
    if (event_code == LV_EVENT_CLICKED) {
        enter_touch_flag = false;
        lv_scr_load_anim(Elecrow_P_bar_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
    }
}

static void ui_touch_xy_event_cb(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) {
            return;
        }

        lv_point_t point;
        char str[64];
        lv_indev_get_point(indev, &point);
        snprintf(str, sizeof(str), "Touch Adjust: %4d %4d", point.x, point.y);
        ESP_LOGI(TAG, "%s", str);
        lv_label_set_text(ui_touch_xy, str);
    }
}

void elecrow_start_screen(void)
{
    Elecrow_logo_screen = lv_obj_create(NULL);
    apply_brand_screen_style(Elecrow_logo_screen);

    Bp_bg_img = lv_obj_create(Elecrow_logo_screen);
    lv_obj_set_size(Bp_bg_img, 560, 390);
    lv_obj_center(Bp_bg_img);
    lv_obj_clear_flag(Bp_bg_img, LV_OBJ_FLAG_SCROLLABLE);
    apply_card_style(Bp_bg_img, 36, 224);

    Bp_logo_img = lv_img_create(Bp_bg_img);
    lv_img_set_src(Bp_logo_img, &cp_logo_alt_rev_splash);
    lv_obj_align(Bp_logo_img, LV_ALIGN_CENTER, 0, -34);

    Load_brand_title = lv_label_create(Bp_bg_img);
    lv_label_set_text(Load_brand_title, "ALS HEARING DEVICE");
    lv_obj_set_style_text_font(Load_brand_title, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(Load_brand_title, lv_color_hex(BRAND_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(Load_brand_title, LV_ALIGN_CENTER, 0, 118);

    Load_label = lv_label_create(Bp_bg_img);
    lv_label_set_text(Load_label, "Initializing...");
    lv_obj_set_style_text_font(Load_label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(Load_label, lv_color_hex(BRAND_GREEN_LIGHT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(Load_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    move_down_logo_timer = lv_timer_create(switch_to_loading_page_timer_cb, 5000, NULL);
    lv_timer_set_repeat_count(move_down_logo_timer, 1);
}

void elecrow_loading_screen(void)
{
    Elecrow_P_bar_screen = lv_obj_create(NULL);
    apply_brand_screen_style(Elecrow_P_bar_screen);

    Load_bg_img = lv_obj_create(Elecrow_P_bar_screen);
    lv_obj_set_size(Load_bg_img, 760, 438);
    lv_obj_center(Load_bg_img);
    lv_obj_clear_flag(Load_bg_img, LV_OBJ_FLAG_SCROLLABLE);
    apply_card_style(Load_bg_img, 36, 232);

    Bp_logo_img = lv_img_create(Load_bg_img);
    lv_img_set_src(Bp_logo_img, &cp_logo_alt_rev_splash);
    lv_obj_align(Bp_logo_img, LV_ALIGN_TOP_MID, 0, 22);

    Load_brand_title = lv_label_create(Load_bg_img);
    lv_label_set_text(Load_brand_title, "ALS HEARING DEVICE");
    lv_obj_set_style_text_font(Load_brand_title, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(Load_brand_title, lv_color_hex(BRAND_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(Load_brand_title, LV_ALIGN_TOP_MID, 0, 284);

    Load_frame_img = lv_obj_create(Load_bg_img);
    lv_obj_set_size(Load_frame_img, 640, 96);
    lv_obj_align(Load_frame_img, LV_ALIGN_BOTTOM_MID, 0, -106);
    lv_obj_clear_flag(Load_frame_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(Load_frame_img, lv_color_hex(BRAND_SURFACE_ALT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(Load_frame_img, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(Load_frame_img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(Load_frame_img, 26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(Load_frame_img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    Load_label = lv_label_create(Load_frame_img);
    lv_label_set_text(Load_label, "Initializing 0%");
    lv_obj_set_style_text_font(Load_label, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(Load_label, lv_color_hex(BRAND_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(Load_label, LV_ALIGN_TOP_MID, 0, 12);

    Progress_bar_frame = lv_obj_create(Load_frame_img);
    lv_obj_set_size(Progress_bar_frame, 564, 18);
    lv_obj_align(Progress_bar_frame, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_clear_flag(Progress_bar_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(Progress_bar_frame, lv_color_hex(0xD5E2DA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(Progress_bar_frame, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(Progress_bar_frame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(Progress_bar_frame, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(Progress_bar_frame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(Progress_bar_frame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    Progress_bar_img = lv_bar_create(Load_frame_img);
    lv_obj_set_size(Progress_bar_img, 564, 18);
    lv_obj_align_to(Progress_bar_img, Progress_bar_frame, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(Progress_bar_img, 0, 100);
    lv_bar_set_value(Progress_bar_img, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(Progress_bar_img, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(Progress_bar_img, LV_RADIUS_CIRCLE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(Progress_bar_img, lv_color_hex(0xD5E2DA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(Progress_bar_img, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(Progress_bar_img, lv_color_hex(BRAND_GREEN), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(Progress_bar_img, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(Progress_bar_img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(Progress_bar_img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_enter_touch = create_action_button(Load_bg_img, "Touch Setup", LV_ALIGN_BOTTOM_LEFT, 28, -24);
    lv_obj_add_event_cb(ui_enter_touch, ui_enter_touch_event_cb, LV_EVENT_ALL, NULL);

    ui_menu = create_action_button(Load_bg_img, "Skip", LV_ALIGN_BOTTOM_RIGHT, -28, -24);
    lv_obj_add_event_cb(ui_menu, ui_menu_event_cb, LV_EVENT_ALL, NULL);
}

void elecrow_screen_init(void)
{
    Elecrow_touch_screen = lv_obj_create(NULL);
    apply_brand_screen_style(Elecrow_touch_screen);
    lv_obj_add_event_cb(Elecrow_touch_screen, ui_touch_xy_event_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *touch_card = lv_obj_create(Elecrow_touch_screen);
    lv_obj_set_size(touch_card, 760, 420);
    lv_obj_center(touch_card);
    lv_obj_clear_flag(touch_card, LV_OBJ_FLAG_SCROLLABLE);
    apply_card_style(touch_card, 34, 232);
    lv_obj_add_event_cb(touch_card, ui_touch_xy_event_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *touch_title = lv_label_create(touch_card);
    lv_label_set_text(touch_title, "Touch Setup");
    lv_obj_set_style_text_font(touch_title, &lv_font_montserrat_34, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(touch_title, lv_color_hex(BRAND_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(touch_title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *touch_hint = lv_label_create(touch_card);
    lv_label_set_text(touch_hint, "Press anywhere on the panel to preview touch coordinates.");
    lv_obj_set_style_text_font(touch_hint, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(touch_hint, lv_color_hex(BRAND_GREEN_LIGHT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(touch_hint, LV_ALIGN_TOP_MID, 0, 78);

    ui_touch_xy = lv_label_create(touch_card);
    lv_obj_set_width(ui_touch_xy, 520);
    lv_obj_set_style_text_align(ui_touch_xy, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_touch_xy, "Touch Adjust:    0    0");
    lv_obj_set_style_text_color(ui_touch_xy, lv_color_hex(BRAND_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_touch_xy, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(ui_touch_xy, LV_ALIGN_CENTER, 0, 18);

    lv_obj_t *touch_help = lv_label_create(touch_card);
    lv_label_set_text(touch_help, "Use Back when the touch response looks correct.");
    lv_obj_set_style_text_font(touch_help, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(touch_help, lv_color_hex(BRAND_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(touch_help, LV_ALIGN_BOTTOM_MID, 0, -96);

    ui_exit_touch = create_action_button(touch_card, "Back", LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(ui_exit_touch, ui_exit_touch_event_cb, LV_EVENT_ALL, NULL);
}

void elecrow_screen(void)
{
    elecrow_success = false;
    enter_touch_flag = false;
    progress = 0;
    move_down_logo_timer = NULL;
    loading_bar_timer = NULL;

    elecrow_start_screen();
    lv_scr_load(Elecrow_logo_screen);
}

/* Functional function end */
