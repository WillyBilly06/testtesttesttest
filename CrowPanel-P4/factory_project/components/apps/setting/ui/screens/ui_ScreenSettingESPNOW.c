// ESP-NOW Audio Settings Screen for CrowPanel P4
// Based on SquareLine Studio pattern

#include "../ui.h"

void ui_ScreenSettingESPNOW_screen_init(void)
{
    // Create main screen
    ui_ScreenSettingESPNOW = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_ScreenSettingESPNOW, lv_color_hex(0xE5F3FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenSettingESPNOW, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Enable/Disable switch panel — same styling as other settings items
    ui_PanelScreenSettingESPNOWSwitch = lv_obj_create(ui_ScreenSettingESPNOW);
    lv_obj_set_height(ui_PanelScreenSettingESPNOWSwitch, 83);
    lv_obj_set_width(ui_PanelScreenSettingESPNOWSwitch, lv_pct(90));
    lv_obj_set_x(ui_PanelScreenSettingESPNOWSwitch, 43);
    lv_obj_set_y(ui_PanelScreenSettingESPNOWSwitch, 77);
    lv_obj_clear_flag(ui_PanelScreenSettingESPNOWSwitch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_PanelScreenSettingESPNOWSwitch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ui_PanelScreenSettingESPNOWSwitch, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_PanelScreenSettingESPNOWSwitch, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_PanelScreenSettingESPNOWSwitch, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_PanelScreenSettingESPNOWSwitch, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_PanelScreenSettingESPNOWSwitch, lv_color_hex(0xCBCBCB), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ui_PanelScreenSettingESPNOWSwitch, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_PanelScreenSettingESPNOWSwitch, 16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Icon — positioned to match other items (left side at pct(-48))
    ui_ImagePanelScreenSettingESPNOWSwitch = lv_img_create(ui_PanelScreenSettingESPNOWSwitch);
    lv_img_set_src(ui_ImagePanelScreenSettingESPNOWSwitch, &ui_img_sound_png);
    lv_obj_set_width(ui_ImagePanelScreenSettingESPNOWSwitch, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ImagePanelScreenSettingESPNOWSwitch, LV_SIZE_CONTENT);
    lv_obj_set_y(ui_ImagePanelScreenSettingESPNOWSwitch, 3);
    lv_obj_set_x(ui_ImagePanelScreenSettingESPNOWSwitch, lv_pct(1));
    lv_obj_set_align(ui_ImagePanelScreenSettingESPNOWSwitch, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_ImagePanelScreenSettingESPNOWSwitch, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_ImagePanelScreenSettingESPNOWSwitch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Label — positioned same as other items (x = -183)
    ui_LabelPanelScreenSettingESPNOWSwitch = lv_label_create(ui_PanelScreenSettingESPNOWSwitch);
    lv_obj_set_width(ui_LabelPanelScreenSettingESPNOWSwitch, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_LabelPanelScreenSettingESPNOWSwitch, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_LabelPanelScreenSettingESPNOWSwitch, lv_pct(10));
    lv_obj_set_y(ui_LabelPanelScreenSettingESPNOWSwitch, 0);
    lv_obj_set_align(ui_LabelPanelScreenSettingESPNOWSwitch, LV_ALIGN_LEFT_MID);
    lv_label_set_text(ui_LabelPanelScreenSettingESPNOWSwitch, "Assistive Listening");
    lv_obj_set_style_text_align(ui_LabelPanelScreenSettingESPNOWSwitch, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_LabelPanelScreenSettingESPNOWSwitch, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ui_LabelPanelScreenSettingESPNOWSwitch, LV_OBJ_FLAG_CLICKABLE);

    // Enable switch — right side matching other items
    ui_SwitchPanelScreenSettingESPNOWSwitch = lv_switch_create(ui_PanelScreenSettingESPNOWSwitch);
    lv_obj_set_width(ui_SwitchPanelScreenSettingESPNOWSwitch, 96);
    lv_obj_set_height(ui_SwitchPanelScreenSettingESPNOWSwitch, 52);
    lv_obj_set_y(ui_SwitchPanelScreenSettingESPNOWSwitch, 0);
    lv_obj_set_x(ui_SwitchPanelScreenSettingESPNOWSwitch, -28);
    lv_obj_set_align(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_ALIGN_RIGHT_MID);
    lv_obj_set_style_bg_color(ui_SwitchPanelScreenSettingESPNOWSwitch, lv_color_hex(0xD9E6DB), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_SwitchPanelScreenSettingESPNOWSwitch, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_SwitchPanelScreenSettingESPNOWSwitch, lv_color_hex(0x2E7D32), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(ui_SwitchPanelScreenSettingESPNOWSwitch, 255, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ui_SwitchPanelScreenSettingESPNOWSwitch, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_SwitchPanelScreenSettingESPNOWSwitch, 255, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_SwitchPanelScreenSettingESPNOWSwitch, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_SwitchPanelScreenSettingESPNOWSwitch, 4, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_RADIUS_CIRCLE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_RADIUS_CIRCLE, LV_PART_KNOB | LV_STATE_DEFAULT);

    // Status label
    ui_LabelScreenSettingESPNOWStatus = lv_label_create(ui_ScreenSettingESPNOW);
    lv_obj_set_width(ui_LabelScreenSettingESPNOWStatus, lv_pct(80));
    lv_obj_set_height(ui_LabelScreenSettingESPNOWStatus, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_LabelScreenSettingESPNOWStatus, 50);
    lv_obj_set_y(ui_LabelScreenSettingESPNOWStatus, 178);
    lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Assistive Listening disabled");
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWStatus, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelScreenSettingESPNOWStatus, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Room list panel (scrollable)
    ui_PanelScreenSettingESPNOWList = lv_obj_create(ui_ScreenSettingESPNOW);
    lv_obj_set_width(ui_PanelScreenSettingESPNOWList, lv_pct(88));
    lv_obj_set_height(ui_PanelScreenSettingESPNOWList, 300);
    lv_obj_set_x(ui_PanelScreenSettingESPNOWList, 40);
    lv_obj_set_y(ui_PanelScreenSettingESPNOWList, 220);
    lv_obj_set_flex_flow(ui_PanelScreenSettingESPNOWList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_PanelScreenSettingESPNOWList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(ui_PanelScreenSettingESPNOWList, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_PanelScreenSettingESPNOWList, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Stats panel (shown when connected)
    ui_PanelScreenSettingESPNOWStats = lv_obj_create(ui_ScreenSettingESPNOW);
    lv_obj_set_width(ui_PanelScreenSettingESPNOWStats, lv_pct(88));
    lv_obj_set_height(ui_PanelScreenSettingESPNOWStats, 300);
    lv_obj_set_x(ui_PanelScreenSettingESPNOWStats, 40);
    lv_obj_set_y(ui_PanelScreenSettingESPNOWStats, 220);
    lv_obj_add_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_PanelScreenSettingESPNOWStats, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Stats: Packets received
    ui_LabelScreenSettingESPNOWPacketsRx = lv_label_create(ui_PanelScreenSettingESPNOWStats);
    lv_obj_set_x(ui_LabelScreenSettingESPNOWPacketsRx, 20);
    lv_obj_set_y(ui_LabelScreenSettingESPNOWPacketsRx, 20);
    lv_label_set_text(ui_LabelScreenSettingESPNOWPacketsRx, "Packets: 0");
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWPacketsRx, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Stats: Packets lost
    ui_LabelScreenSettingESPNOWPacketsLost = lv_label_create(ui_PanelScreenSettingESPNOWStats);
    lv_obj_set_x(ui_LabelScreenSettingESPNOWPacketsLost, 20);
    lv_obj_set_y(ui_LabelScreenSettingESPNOWPacketsLost, 50);
    lv_label_set_text(ui_LabelScreenSettingESPNOWPacketsLost, "Lost: 0");
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWPacketsLost, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Stats: RSSI
    ui_LabelScreenSettingESPNOWRSSI = lv_label_create(ui_PanelScreenSettingESPNOWStats);
    lv_obj_set_x(ui_LabelScreenSettingESPNOWRSSI, 20);
    lv_obj_set_y(ui_LabelScreenSettingESPNOWRSSI, 80);
    lv_label_set_text(ui_LabelScreenSettingESPNOWRSSI, "RSSI: -- dBm");
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWRSSI, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Disconnect button (shown when connected)
    ui_ButtonScreenSettingESPNOWDisconnect = lv_btn_create(ui_PanelScreenSettingESPNOWStats);
    lv_obj_set_width(ui_ButtonScreenSettingESPNOWDisconnect, 190);
    lv_obj_set_height(ui_ButtonScreenSettingESPNOWDisconnect, 52);
    lv_obj_set_x(ui_ButtonScreenSettingESPNOWDisconnect, -30);
    lv_obj_set_y(ui_ButtonScreenSettingESPNOWDisconnect, -25);
    lv_obj_set_align(ui_ButtonScreenSettingESPNOWDisconnect, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_style_bg_color(ui_ButtonScreenSettingESPNOWDisconnect, lv_color_hex(0xFF5252), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *disconnect_label = lv_label_create(ui_ButtonScreenSettingESPNOWDisconnect);
    lv_label_set_text(disconnect_label, "Disconnect");
    lv_obj_center(disconnect_label);
    lv_obj_set_style_text_font(disconnect_label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(disconnect_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Spinner (scanning indicator)
    ui_SpinnerScreenSettingESPNOW = lv_spinner_create(ui_ScreenSettingESPNOW, 1000, 90);
    lv_obj_set_width(ui_SpinnerScreenSettingESPNOW, lv_pct(16));
    lv_obj_set_height(ui_SpinnerScreenSettingESPNOW, lv_pct(16));
    lv_obj_set_align(ui_SpinnerScreenSettingESPNOW, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ui_SpinnerScreenSettingESPNOW, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(ui_SpinnerScreenSettingESPNOW, lv_color_hex(0xC2C2C2), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_SpinnerScreenSettingESPNOW, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(ui_SpinnerScreenSettingESPNOW, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Return button
    ui_ButtonScreenSettingESPNOWReturn = lv_btn_create(ui_ScreenSettingESPNOW);
    lv_obj_set_width(ui_ButtonScreenSettingESPNOWReturn, 60);
    lv_obj_set_height(ui_ButtonScreenSettingESPNOWReturn, 60);
    lv_obj_set_x(ui_ButtonScreenSettingESPNOWReturn, -460);
    lv_obj_set_y(ui_ButtonScreenSettingESPNOWReturn, -240);
    lv_obj_set_align(ui_ButtonScreenSettingESPNOWReturn, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ButtonScreenSettingESPNOWReturn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(ui_ButtonScreenSettingESPNOWReturn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_ButtonScreenSettingESPNOWReturn, lv_color_hex(0xF6F6F6), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ButtonScreenSettingESPNOWReturn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_ImageScreenSettingESPNOWReturn = lv_img_create(ui_ButtonScreenSettingESPNOWReturn);
    lv_img_set_src(ui_ImageScreenSettingESPNOWReturn, &ui_img_return_png);
    lv_obj_set_width(ui_ImageScreenSettingESPNOWReturn, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ImageScreenSettingESPNOWReturn, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_ImageScreenSettingESPNOWReturn, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_ImageScreenSettingESPNOWReturn, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_ImageScreenSettingESPNOWReturn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_img_recolor(ui_ImageScreenSettingESPNOWReturn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_img_recolor_opa(ui_ImageScreenSettingESPNOWReturn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Register event callbacks
    lv_obj_add_event_cb(ui_ButtonScreenSettingESPNOWReturn, ui_event_ButtonScreenSettingESPNOWReturn, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingESPNOWSwitch, ui_event_SwitchScreenSettingESPNOW, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_ButtonScreenSettingESPNOWDisconnect, ui_event_ButtonScreenSettingESPNOWDisconnect, LV_EVENT_ALL, NULL);
}
