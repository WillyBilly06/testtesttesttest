/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <map>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "app_theme.h"

class AppSettings: public ESP_Brookesia_PhoneApp {
public:
    AppSettings();
    ~AppSettings();

    bool run(void);
    bool back(void);
    bool close(void);

    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    typedef enum {
        UI_MAIN_SETTING_INDEX = 0,
        UI_WIFI_SCAN_INDEX,
        UI_WIFI_CONNECT_INDEX,
        UI_BLUETOOTH_SETTING_INDEX,
        UI_VOLUME_SETTING_INDEX,
        UI_BRIGHTNESS_SETTING_INDEX,
        UI_ABOUT_SETTING_INDEX,
        UI_ESPNOW_SETTING_INDEX,
        UI_MAX_INDEX,
    } SettingScreenIndex_t;

    typedef enum {
        WIFI_SIGNAL_STRENGTH_NONE = 0,
        WIFI_SIGNAL_STRENGTH_WEAK = 1,
        WIFI_SIGNAL_STRENGTH_MODERATE = 2,
        WIFI_SIGNAL_STRENGTH_GOOD = 3,
    } WifiSignalStrengthLevel_t;

    typedef enum {
        WIFI_CONNECT_HIDE = 0,
        WIFI_CONNECT_RUNNING,
        WIFI_CONNECT_SUCCESS,
        WIFI_CONNECT_FAIL,
    } WifiConnectState_t;

    /* Operations */
    // UI
    void extraUiInit(void);
    void processWifiConnect(WifiConnectState_t state);
    void initWifiListButton(lv_obj_t* lv_panel_button, lv_obj_t* lv_label_ssid, lv_obj_t* lv_img_wifi_lock, lv_obj_t* lv_wifi_img,
                              lv_obj_t *lv_wifi_connect, uint8_t* ssid, bool psk, WifiSignalStrengthLevel_t signal_strength);
    void deinitWifiListButton(void);
    // NVS Parameters
    bool loadNvsParam(void);
    bool setNvsParam(std::string key, int value);
    bool loadNvsStringParam(const std::string &key, std::string &value, const char *default_value);
    bool setNvsStringParam(const std::string &key, const std::string &value);
    void updateUiByNvsParam(void);
    // WiFi
    esp_err_t initWifi(void);
    void startWifiScan(void);
    void stopWifiScan(void);
    void scanWifiAndUpdateUi(void);
    // Device info
    void refreshDeviceInfoUi(void);
    void showDeviceNameEditor(void);
    void hideDeviceNameEditor(void);
    void applyThemeToSettingScreens(void);
    void updateThemePreviewSelection(void);
    void restoreBuiltInIconSources(void);
    void resetUiHandles(void);
    bool isUiAlive(void) const;
    bool isUiObjectValid(lv_obj_t *obj) const;
    // Smart Gadget
    // void updateGadgetTime(struct tm timeinfo);

    /* Task */
    static void euiRefresTask(void *arg);
    static void wifiScanTask(void *arg);
    static void wifiConnectTask(void *arg);

    /* Event Handler */
    // WiFi
    static void wifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    /* UI Event Callback */
    // Main
    static void onScreenLoadEventCallback( lv_event_t * e);
    // WiFi
    static void onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback( lv_event_t * e);
    static void onButtonWifiListClickedEventCallback(lv_event_t * e);
    static void onKeyboardScreenSettingVerificationClickedEventCallback(lv_event_t *e);
    static void onVerificationBackButtonClickedEventCallback(lv_event_t *e);
    static void onHiddenNetworkBtnClickedEventCallback(lv_event_t *e);
    static void onHiddenNetworkConnectClickedEventCallback(lv_event_t *e);
    static void onHiddenNetworkCancelClickedEventCallback(lv_event_t *e);
    // Bluetooth
    static void onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback( lv_event_t * e);
    // Audio
    static void onSliderPanelVolumeSwitchValueChangeEventCallback( lv_event_t * e);
    static void onDropdownAudioOutputValueChangeEventCallback(lv_event_t *e);
    // Brightness
    static void onSliderPanelLightSwitchValueChangeEventCallback( lv_event_t * e);
    static void onDropdownScreenTimeoutValueChangeEventCallback(lv_event_t *e);
    static void onDeviceNamePanelClickedEventCallback(lv_event_t *e);
    static void onDeviceNameKeyboardEventCallback(lv_event_t *e);
    static void onThemePreviewCardClickedEventCallback(lv_event_t *e);
    // ESP-NOW Audio
    static void onESPNOWSwitchValueChangeEventCallback(lv_event_t *e);
    static void onESPNOWDisconnectButtonClickedEventCallback(lv_event_t *e);
    static void onESPNOWRoomButtonClickedEventCallback(lv_event_t *e);
    static void espnowScanTask(void *arg);
    static void espnowStatsTask(void *arg);

    bool _is_ui_resumed;
    bool _is_ui_del;
    SettingScreenIndex_t _screen_index;
    WifiSignalStrengthLevel_t _wifi_signal_strength_level;
    lv_obj_t *_panel_wifi_connect;
    lv_obj_t *_spinner_wifi_connect;
    lv_obj_t *_img_wifi_connect;
    lv_obj_t *_btn_back_verification;
    lv_obj_t *_audio_output_panel;
    lv_obj_t *_audio_output_dropdown;
    lv_obj_t *_display_timeout_panel;
    lv_obj_t *_display_timeout_dropdown;
    lv_obj_t *_display_theme_panel;
    lv_obj_t *_device_name_editor_overlay;
    lv_obj_t *_device_name_editor_textarea;
    lv_obj_t *_device_name_editor_keyboard;
    lv_obj_t *_hidden_network_btn;
    lv_obj_t *_hidden_network_overlay;
    lv_obj_t *_hidden_network_card;
    lv_obj_t *_hidden_network_ssid_textarea;
    lv_obj_t *_hidden_network_password_textarea;
    lv_obj_t *_hidden_network_keyboard;
    lv_obj_t *_hidden_network_connect_btn;
    lv_obj_t *_hidden_network_cancel_btn;
    lv_obj_t *_hidden_pwd_eye_btn;
    lv_obj_t *_hidden_pwd_eye_label;
    lv_obj_t *_verify_pwd_eye_btn;
    lv_obj_t *_verify_pwd_eye_label;
    lv_obj_t *_wifi_scan_dots_label;
    lv_timer_t *_wifi_scan_dots_timer;
    lv_obj_t *_timezone_panel;
    lv_obj_t *_timezone_label;
    std::array<lv_obj_t *, APP_THEME_MAX> _theme_preview_cards;
    std::array<lv_obj_t *, APP_THEME_MAX> _theme_preview_badges;
    std::array<lv_obj_t *, UI_MAX_INDEX> _screen_list;
    std::map<std::string, int32_t> _nvs_param_map;
    std::string _device_name;
    const ESP_Brookesia_StatusBar *status_bar = nullptr;
    const ESP_Brookesia_RecentsScreen *backstage = nullptr;
    
    // ESP-NOW Audio
    TaskHandle_t _espnow_scan_task = nullptr;
    TaskHandle_t _espnow_stats_task = nullptr;
};
