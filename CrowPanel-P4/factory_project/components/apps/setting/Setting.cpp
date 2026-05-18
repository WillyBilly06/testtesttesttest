/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_mac.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "nvs.h"

#include "ui/ui.h"
#include "Setting.hpp"
#include "app_sntp.h"
#include "espnow_sink.h"

#include "esp_brookesia_versions.h"

#define ENABLE_DEBUG_LOG                (0)

#define HOME_REFRESH_TASK_STACK_SIZE    (1024 * 4)
#define HOME_REFRESH_TASK_PRIORITY      (1)
#define HOME_REFRESH_TASK_PERIOD_MS     (2000)

#define WIFI_SCAN_TASK_STACK_SIZE       (1024 * 16)
#define WIFI_SCAN_TASK_PRIORITY         (1)
#define WIFI_SCAN_TASK_PERIOD_MS        (7 * 1000)

#define WIFI_CONNECT_TASK_STACK_SIZE    (1024 * 4)
#define WIFI_CONNECT_TASK_PRIORITY      (4)
#define WIFI_CONNECT_TASK_STACK_CORE    (0)
#define WIFI_CONNECT_UI_WAIT_TIME_MS    (1 * 1000)
#define WIFI_CONNECT_UI_PANEL_SIZE      (1 * 1000)
#define WIFI_CONNECT_RET_WAIT_TIME_MS   (10 * 1000)
#define WIFI_AUTO_CONNECT_MAX_RETRIES   (5)
#define WIFI_AUTO_CONNECT_RETRY_MS      (1000)

#define SCREEN_BRIGHTNESS_MIN           (20)
#define SCREEN_BRIGHTNESS_MAX           (BSP_LCD_BACKLIGHT_BRIGHTNESS_MAX)

#define SPEAKER_VOLUME_MIN              (0)
#define SPEAKER_VOLUME_MAX              (100)

#define NVS_STORAGE_NAMESPACE           "storage"
#define NVS_WIFI_CRED_NAMESPACE         "wifi_cred"
#define NVS_KEY_WIFI_ENABLE             "wifi_en"
#define NVS_KEY_BLE_ENABLE              "ble_en"
#define NVS_KEY_AUDIO_VOLUME            "volume"
#define NVS_KEY_AUDIO_OUTPUT            "audio_out"
#define NVS_KEY_DISPLAY_BRIGHTNESS      "brightness"
#define NVS_KEY_DISPLAY_TIMEOUT         "disp_timeout"
#define NVS_KEY_DEVICE_NAME             "device_name"

#define AUDIO_OUTPUT_SPEAKER            (0)
#define AUDIO_OUTPUT_AUX                (1)

#define UI_MAIN_ITEM_LEFT_OFFSET        (20)
#define UI_WIFI_LIST_UP_OFFSET          (20)
#define UI_WIFI_LIST_UP_PAD             (20)
#define UI_WIFI_LIST_DOWN_PAD           (40)  // Extra padding at bottom for Hidden Network and scroll distance
#define UI_WIFI_LIST_H_PERCENT          (75)
#define UI_WIFI_LIST_ITEM_H             (60)
#define UI_WIFI_LIST_ITEM_FONT          (&lv_font_montserrat_26)
#define UI_WIFI_KEYBOARD_H_PERCENT      (30)
#define UI_WIFI_ICON_LOCK_RIGHT_OFFSET       (-10)
#define UI_WIFI_ICON_SIGNAL_RIGHT_OFFSET     (-50)
#define UI_WIFI_ICON_CONNECT_RIGHT_OFFSET    (-90)
#define DEVICE_NAME_MAX_LEN             (32)
#define DEVICE_NAME_DEFAULT             "ESP-ALS"

using namespace std;

#define SCAN_LIST_SIZE      25
#define WIFI_SCAN_FETCH_SIZE 64

extern "C" int32_t app_get_screen_timeout_seconds(void);
extern "C" void app_set_screen_timeout_seconds(int32_t timeout_seconds);
extern "C" int32_t app_get_ui_theme_id(void);
extern "C" void app_set_ui_theme_id(int32_t theme_id);
extern "C" int app_sntp_get_timezone_index(void);
extern "C" void app_sntp_set_timezone_by_index(int index);
extern "C" void app_set_ui_theme_id(int32_t theme_id);

LV_IMG_DECLARE(theme_bg_topography_1024x600);
LV_IMG_DECLARE(theme_bg_vista_1024x600);
LV_IMG_DECLARE(theme_preview_calpoly_256x144);
LV_IMG_DECLARE(theme_preview_topography_256x144);
LV_IMG_DECLARE(theme_preview_vista_256x144);

namespace {

constexpr int kScreenTimeoutOptionsSeconds[] = {0, 15, 30, 60, 120, 300, 600};
constexpr uint32_t kCalPolyGreen = 0x154734;
constexpr uint32_t kCalPolyGreenLight = 0x4F775E;
constexpr uint32_t kCalPolySurface = 0xF7FAF7;
constexpr uint32_t kCalPolySurfaceAlt = 0xE8F1ED;
constexpr uint32_t kCalPolyBorder = 0xD6E2DB;
constexpr uint32_t kCalPolyText = 0x193A2C;
constexpr lv_opa_t kWallpaperOpa = 86;
constexpr lv_coord_t kDisplayDetailsListHeight = 372;
constexpr lv_coord_t kDisplayThemePanelHeight = 356;
constexpr lv_coord_t kDisplayThemeRowTopOffset = 90;
constexpr lv_coord_t kThemePreviewCardWidth = 272;
constexpr lv_coord_t kThemePreviewCardHeight = 196;
constexpr size_t kWifiSsidMaxLen = 32;
constexpr size_t kWifiPasswordMaxLen = 64;

app_theme_id_t sanitize_theme_id(int32_t theme_id)
{
    if ((theme_id < APP_THEME_CALPOLY) || (theme_id >= APP_THEME_MAX)) {
        return APP_THEME_CALPOLY;
    }

    return static_cast<app_theme_id_t>(theme_id);
}

int screen_timeout_to_index(int timeout_seconds)
{
    for (size_t i = 0; i < (sizeof(kScreenTimeoutOptionsSeconds) / sizeof(kScreenTimeoutOptionsSeconds[0])); ++i) {
        if (timeout_seconds <= kScreenTimeoutOptionsSeconds[i]) {
            return static_cast<int>(i);
        }
    }

    return static_cast<int>((sizeof(kScreenTimeoutOptionsSeconds) / sizeof(kScreenTimeoutOptionsSeconds[0])) - 1);
}

int index_to_screen_timeout(uint16_t index)
{
    if (index >= (sizeof(kScreenTimeoutOptionsSeconds) / sizeof(kScreenTimeoutOptionsSeconds[0]))) {
        index = static_cast<uint16_t>((sizeof(kScreenTimeoutOptionsSeconds) / sizeof(kScreenTimeoutOptionsSeconds[0])) - 1);
    }

    return kScreenTimeoutOptionsSeconds[index];
}

void copy_cstr_to_buffer(char *dst, size_t dst_size, const char *src)
{
    if ((dst == nullptr) || (dst_size == 0)) {
        return;
    }

    if (src == nullptr) {
        src = "";
    }

    snprintf(dst, dst_size, "%s", src);
}

void copy_cstr_to_wifi_field(uint8_t *dst, size_t dst_size, const char *src)
{
    if ((dst == nullptr) || (dst_size == 0)) {
        return;
    }

    memset(dst, 0, dst_size);
    if (src == nullptr) {
        return;
    }

    const size_t copy_len = strnlen(src, dst_size);
    memcpy(dst, src, copy_len);
}

bool wifi_ssid_matches(const uint8_t *lhs, const uint8_t *rhs)
{
    return (lhs != nullptr) && (rhs != nullptr) &&
           (strncmp(reinterpret_cast<const char *>(lhs), reinterpret_cast<const char *>(rhs), kWifiSsidMaxLen) == 0);
}

std::string sanitize_device_name(const char *value)
{
    std::string name = (value != nullptr) ? value : "";

    while (!name.empty() && isspace(static_cast<unsigned char>(name.front()))) {
        name.erase(name.begin());
    }
    while (!name.empty() && isspace(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }

    if (name.empty()) {
        name = DEVICE_NAME_DEFAULT;
    }

    if (name.size() >= DEVICE_NAME_MAX_LEN) {
        name.resize(DEVICE_NAME_MAX_LEN - 1);
    }

    return name;
}

const char *get_running_firmware_version()
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if ((app_desc == nullptr) || (app_desc->version[0] == '\0')) {
        return "unknown";
    }

    return app_desc->version;
}

const char *get_c6_firmware_version_text()
{
    static char c6_version[40] = "Loading...";
    if (espnow_sink_get_c6_fw_version(c6_version, sizeof(c6_version))) {
        return c6_version;
    }
    return "Loading...";
}

const char *get_combined_firmware_version_text()
{
    static char combined[96] = {0};
    snprintf(combined, sizeof(combined), "%s Display || %s Wireless",
             get_running_firmware_version(), get_c6_firmware_version_text());
    return combined;
}

void align_info_value_label(lv_obj_t *label)
{
    if (label == ui_LabelPanelPanelScreenSettingAbout5) {
        lv_obj_set_width(label, 420);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    } else {
        lv_obj_set_width(label, 300);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -24, 0);
}

const lv_img_dsc_t *get_theme_wallpaper_asset(app_theme_id_t theme_id)
{
    switch (theme_id) {
    case APP_THEME_TOPOGRAPHY:
        return &theme_bg_topography_1024x600;
    case APP_THEME_VISTA:
        return &theme_bg_vista_1024x600;
    case APP_THEME_CALPOLY:
    default:
        return nullptr;
    }
}

const lv_img_dsc_t *get_theme_preview_asset(app_theme_id_t theme_id)
{
    switch (theme_id) {
    case APP_THEME_TOPOGRAPHY:
        return &theme_preview_topography_256x144;
    case APP_THEME_VISTA:
        return &theme_preview_vista_256x144;
    case APP_THEME_CALPOLY:
    default:
        return &theme_preview_calpoly_256x144;
    }
}

const char *get_theme_name(app_theme_id_t theme_id)
{
    switch (theme_id) {
    case APP_THEME_TOPOGRAPHY:
        return "Topography";
    case APP_THEME_VISTA:
        return "Vista";
    case APP_THEME_CALPOLY:
    default:
        return "Cal Poly";
    }
}

const char *get_theme_description(app_theme_id_t theme_id)
{
    switch (theme_id) {
    case APP_THEME_TOPOGRAPHY:
        return "Subtle topo texture";
    case APP_THEME_VISTA:
        return "Photo backdrop";
    case APP_THEME_CALPOLY:
    default:
        return "Clean green and white";
    }
}

uint32_t get_theme_surface_color(app_theme_id_t theme_id)
{
    switch (theme_id) {
    case APP_THEME_TOPOGRAPHY:
        return 0xEEF5F0;
    case APP_THEME_VISTA:
        return 0xF3F8F4;
    case APP_THEME_CALPOLY:
    default:
        return kCalPolySurface;
    }
}

uint32_t get_theme_surface_alt_color(app_theme_id_t theme_id)
{
    switch (theme_id) {
    case APP_THEME_TOPOGRAPHY:
        return 0xE3EEE7;
    case APP_THEME_VISTA:
        return 0xEAF3EE;
    case APP_THEME_CALPOLY:
    default:
        return kCalPolySurfaceAlt;
    }
}

void apply_screen_background_style(lv_obj_t *screen, const lv_img_dsc_t *wallpaper, uint32_t surface_color = kCalPolySurface)
{
    if (screen == nullptr) {
        return;
    }

    const lv_opa_t wallpaper_opa = (wallpaper != nullptr) ? kWallpaperOpa : static_cast<lv_opa_t>(LV_OPA_TRANSP);

    lv_obj_set_style_bg_color(screen, lv_color_hex(surface_color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (wallpaper != nullptr) {
        lv_obj_set_style_bg_img_src(screen, wallpaper, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_img_opa(screen, wallpaper_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        // Explicitly clear wallpaper
        lv_obj_set_style_bg_img_opa(screen, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_img_src(screen, NULL, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void apply_panel_card_style(lv_obj_t *panel, bool translucent = false)
{
    if (panel == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, translucent ? 230 : LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(panel, lv_color_hex(kCalPolyBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(panel, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(panel, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(panel, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void apply_text_accent_style(lv_obj_t *label)
{
    if (label == nullptr) {
        return;
    }

    lv_obj_set_style_text_color(label, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
}

void apply_icon_accent_style(lv_obj_t *image)
{
    if (image == nullptr) {
        return;
    }

    lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_img_recolor_opa(image, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_img_opa(image, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void apply_return_button_style(lv_obj_t *button, lv_obj_t *image)
{
    if (button == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(button, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(button, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(button, 26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(button, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);

    if (image != nullptr) {
        lv_obj_set_style_img_recolor(image, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void apply_toggle_style(lv_obj_t *toggle)
{
    if (toggle == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(toggle, lv_color_hex(0xD7E4DC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(kCalPolyGreen), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(toggle, lv_color_white(), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DEFAULT);
}

void apply_slider_style(lv_obj_t *slider)
{
    if (slider == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(slider, lv_color_hex(0xDCE7E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kCalPolyGreen), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(slider, lv_color_hex(kCalPolyGreenLight), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(slider, 16, LV_PART_KNOB | LV_STATE_DEFAULT);
}

void apply_dropdown_style(lv_obj_t *dropdown)
{
    if (dropdown == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(dropdown, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(kCalPolyBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dropdown, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(kCalPolyText), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(kCalPolySurfaceAlt), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(dropdown, 13, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(dropdown, 9, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(dropdown, 13, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(dropdown, 9, LV_PART_SELECTED | LV_STATE_DEFAULT);
}

// WiFi credential storage helper functions
// NVS keys are limited to 15 chars, so we use a hash of the SSID
void makeWifiCredKey(const char *ssid, char *key_out, size_t key_len)
{
    // Simple hash using first characters and a checksum
    uint32_t hash = 0;
    const char *p = ssid;
    while (*p) {
        hash = hash * 31 + (uint8_t)*p++;
    }
    snprintf(key_out, key_len, "w%08X", (unsigned int)hash);
}

bool saveWifiCredential(const char *ssid, const char *password)
{
    if (ssid == nullptr || password == nullptr || ssid[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_CRED_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE("WiFiCred", "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    char key[16];
    makeWifiCredKey(ssid, key, sizeof(key));

    err = nvs_set_str(handle, key, password);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI("WiFiCred", "Saved credential for SSID: %s (key: %s)", ssid, key);
    } else {
        ESP_LOGE("WiFiCred", "Failed to save credential: %s", esp_err_to_name(err));
    }
    return (err == ESP_OK);
}

bool loadWifiCredential(const char *ssid, char *password_out, size_t max_len)
{
    if (ssid == nullptr || password_out == nullptr || ssid[0] == '\0' || max_len == 0) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_CRED_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Namespace doesn't exist yet or other error
        return false;
    }

    char key[16];
    makeWifiCredKey(ssid, key, sizeof(key));

    size_t required_size = 0;
    err = nvs_get_str(handle, key, nullptr, &required_size);
    if (err != ESP_OK || required_size == 0 || required_size > max_len) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, key, password_out, &required_size);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI("WiFiCred", "Loaded saved credential for SSID: %s", ssid);
        return true;
    }
    return false;
}

} // namespace

static const char TAG[] = "EUI_Setting";

/* Implemented in main/main.cpp.
 * Returns false until the C6 OTA + activation flow has finished, so the
 * WiFi-Scan task does not race RPCs against an in-flight slave OTA on the
 * factory v2.3.0 SDIO transport (which deadlocks the bus). */
extern "C" bool c6_init_wifi_allowed(void);

static void formatEspnowRoomCode(uint32_t room_code, char *out, size_t out_len)
{
    uint16_t building = 0;
    uint16_t room = 0;
    uint8_t suffix = 0;
    espnow_decode_room_code(room_code, &building, &room, &suffix);

    if (suffix == 0) {
        snprintf(out, out_len, "%03u-%03u", building, room);
    } else if (suffix <= 26) {
        snprintf(out, out_len, "%03u-%03u%c", building, room, (char)('A' + suffix - 1));
    } else {
        snprintf(out, out_len, "%03u-%03u?", building, room);
    }
}

static void formatEspnowRoomName(const espnow_room_info_t &room, char *out, size_t out_len)
{
    if ((out == nullptr) || (out_len == 0)) {
        return;
    }
    if (room.name[0] != '\0') {
        snprintf(out, out_len, "%.*s", (int)(sizeof(room.name) - 1), room.name);
    } else {
        formatEspnowRoomCode(room.room_code, out, out_len);
    }
}

TaskHandle_t wifi_scan_handle_task;

static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_stack_initialized = false;
static bool s_wifi_handlers_registered = false;
static esp_event_handler_instance_t s_wifi_event_handler_any = nullptr;
static esp_event_handler_instance_t s_ip_event_handler_got_ip = nullptr;

static char st_wifi_ssid[kWifiSsidMaxLen + 1];
static char st_wifi_password[kWifiPasswordMaxLen + 1];
static char st_pending_ssid[kWifiSsidMaxLen + 1];
static AppSettings *st_wifi_list_app = nullptr;  // Store app pointer for WiFi list click callbacks
static volatile bool st_auto_reconnect_in_progress = false;  // Prevent multiple auto-reconnect attempts
static volatile bool st_startup_auto_reconnect_done = false;  // Track if startup auto-reconnect has been attempted

struct WifiConnectParams {
    AppSettings *app;
    char ssid[kWifiSsidMaxLen + 1];
    char password[kWifiPasswordMaxLen + 1];
    bool is_auto_connect;      // True if using saved password
    int max_retries;           // Number of retries for auto-connect
};

static uint8_t base_mac_addr[6] = {0};
static char mac_str[18] = {0};

static lv_obj_t* panel_wifi_btn[SCAN_LIST_SIZE];
static lv_obj_t* label_wifi_ssid[SCAN_LIST_SIZE];
static lv_obj_t* img_img_wifi_lock[SCAN_LIST_SIZE];
static lv_obj_t* wifi_image[SCAN_LIST_SIZE];
static lv_obj_t* wifi_connect[SCAN_LIST_SIZE];

static int brightness;

LV_IMG_DECLARE(img_wifisignal_absent);
LV_IMG_DECLARE(img_wifisignal_wake);
LV_IMG_DECLARE(img_wifisignal_moderate);
LV_IMG_DECLARE(img_wifisignal_good);
LV_IMG_DECLARE(img_wifi_lock);
LV_IMG_DECLARE(img_wifi_connect_success);
LV_IMG_DECLARE(img_wifi_connect_fail);

typedef enum {
    WIFI_EVENT_CONNECTED = BIT(0),
    WIFI_EVENT_INIT_DONE = BIT(1),
    WIFI_EVENT_UI_INIT_DONE = BIT(2),
    WIFI_EVENT_SCANING = BIT(3)
} wifi_event_id_t;

static bool ensureWifiEventGroup(void)
{
    if (s_wifi_event_group == nullptr) {
        s_wifi_event_group = xEventGroupCreate();
    }

    if (s_wifi_event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    return true;
}

static EventBits_t getWifiEventBits(void)
{
    if (s_wifi_event_group == nullptr) {
        return 0;
    }

    return xEventGroupGetBits(s_wifi_event_group);
}

LV_IMG_DECLARE(img_app_setting);
extern lv_obj_t *ui_Min;
extern lv_obj_t *ui_Hour;
extern lv_obj_t *ui_Sec;
extern lv_obj_t *ui_Date;
extern lv_obj_t *ui_Clock_Number;

AppSettings::AppSettings():
    ESP_Brookesia_PhoneApp("Settings", &img_app_setting, false),                  // auto_resize_visual_area
    _is_ui_resumed(false),
    _is_ui_del(true),
    _screen_index(UI_MAIN_SETTING_INDEX),
    _wifi_signal_strength_level(WIFI_SIGNAL_STRENGTH_NONE),
    _panel_wifi_connect(nullptr),
    _spinner_wifi_connect(nullptr),
    _img_wifi_connect(nullptr),
    _btn_back_verification(nullptr),
    _audio_output_panel(nullptr),
    _audio_output_dropdown(nullptr),
    _display_timeout_panel(nullptr),
    _display_timeout_dropdown(nullptr),
    _display_theme_panel(nullptr),
    _device_name_editor_overlay(nullptr),
    _device_name_editor_textarea(nullptr),
    _device_name_editor_keyboard(nullptr),
    _hidden_network_btn(nullptr),
    _hidden_network_overlay(nullptr),
    _hidden_network_ssid_textarea(nullptr),
    _hidden_network_password_textarea(nullptr),
    _hidden_network_keyboard(nullptr),
    _hidden_network_connect_btn(nullptr),
    _hidden_network_cancel_btn(nullptr),
    _espnow_volume_slider(nullptr),
    _espnow_output_dropdown(nullptr),
    _espnow_volume_value_label(nullptr),
    _espnow_selected_room_label(nullptr),
    _theme_preview_cards({nullptr}),
    _theme_preview_badges({nullptr}),
    _screen_list({nullptr}),
    _device_name(DEVICE_NAME_DEFAULT)
{
}

AppSettings::~AppSettings()
{
}

bool AppSettings::isUiAlive(void) const
{
    return !_is_ui_del && (ui_ScreenSettingMain != nullptr) && lv_obj_is_valid(ui_ScreenSettingMain);
}

bool AppSettings::isUiObjectValid(lv_obj_t *obj) const
{
    return isUiAlive() && (obj != nullptr) && lv_obj_is_valid(obj);
}

void AppSettings::resetUiHandles(void)
{
    _panel_wifi_connect = nullptr;
    _spinner_wifi_connect = nullptr;
    _img_wifi_connect = nullptr;
    _audio_output_panel = nullptr;
    _audio_output_dropdown = nullptr;
    _display_timeout_panel = nullptr;
    _display_timeout_dropdown = nullptr;
    _display_theme_panel = nullptr;
    _device_name_editor_overlay = nullptr;
    _device_name_editor_textarea = nullptr;
    _device_name_editor_keyboard = nullptr;
    _espnow_volume_slider = nullptr;
    _espnow_output_dropdown = nullptr;
    _espnow_volume_value_label = nullptr;
    _espnow_selected_room_label = nullptr;
    _theme_preview_cards.fill(nullptr);
    _theme_preview_badges.fill(nullptr);
    _screen_list.fill(nullptr);
}

void AppSettings::restoreBuiltInIconSources(void)
{
    if (!isUiAlive()) {
        return;
    }

    lv_img_set_src(ui_ImagePanelSettingMainContainer1WiFi, &ui_img_wifi_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer1Arrow, &ui_img_arrow_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer2Blue, &ui_img_bluetooth_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer2Arrow, &ui_img_arrow_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer3Volume, &ui_img_sound_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer3Arrow, &ui_img_arrow_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer4Light, &ui_img_light_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer4Arrow, &ui_img_arrow_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer5About, &ui_img_about_png);
    lv_img_set_src(ui_ImagePanelSettingMainContainer5Arrow, &ui_img_arrow_png);

    lv_img_set_src(ui_ImagePanelScreenSettingWiFiSwitch, &ui_img_wifi_png);
    lv_img_set_src(ui_ImagePanelScreenSettingBLESwitch, &ui_img_bluetooth_png);
    lv_img_set_src(ui_ImagePanelScreenSettingVolumeSwitch, &ui_img_sound_png);
    lv_img_set_src(ui_ImagePanelScreenSettingLightSwitch, &ui_img_light_png);

    lv_img_set_src(ui_ImageScreenSettingWiFiReturn, &ui_img_return_png);
    lv_img_set_src(ui_ImageScreenSettingBLEReturn, &ui_img_return_png);
    lv_img_set_src(ui_ImageScreenSettingVolumeReturn, &ui_img_return_png);
    lv_img_set_src(ui_ImageScreenSettingLightReturn, &ui_img_return_png);
    lv_img_set_src(ui_ImageScreenSettingAboutReturn, &ui_img_return_png);

    lv_obj_t *icons[] = {
        ui_ImagePanelSettingMainContainer1WiFi,
        ui_ImagePanelSettingMainContainer1Arrow,
        ui_ImagePanelSettingMainContainer2Blue,
        ui_ImagePanelSettingMainContainer2Arrow,
        ui_ImagePanelSettingMainContainer3Volume,
        ui_ImagePanelSettingMainContainer3Arrow,
        ui_ImagePanelSettingMainContainer4Light,
        ui_ImagePanelSettingMainContainer4Arrow,
        ui_ImagePanelSettingMainContainer5About,
        ui_ImagePanelSettingMainContainer5Arrow,
        ui_ImagePanelScreenSettingWiFiSwitch,
        ui_ImagePanelScreenSettingBLESwitch,
        ui_ImagePanelScreenSettingVolumeSwitch,
        ui_ImagePanelScreenSettingLightSwitch,
        ui_ImageScreenSettingWiFiReturn,
        ui_ImageScreenSettingBLEReturn,
        ui_ImageScreenSettingVolumeReturn,
        ui_ImageScreenSettingLightReturn,
        ui_ImageScreenSettingAboutReturn,
    };
    for (lv_obj_t *icon : icons) {
        if (icon == nullptr) {
            continue;
        }
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_img_opa(icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

bool AppSettings::run(void)
{
    _is_ui_del = false;
    resetUiHandles();

    // Initialize Squareline UI
    ui_setting_init();

    // Get MAC
    esp_read_mac(base_mac_addr, ESP_MAC_EFUSE_FACTORY);
    snprintf(mac_str, sizeof(mac_str), "%02X-%02X-%02X-%02X-%02X-%02X",
             base_mac_addr[0], base_mac_addr[1], base_mac_addr[2],
             base_mac_addr[3], base_mac_addr[4], base_mac_addr[5]);


    // Initialize custom UI
    extraUiInit();

    // Upate UI by NVS parametres
    updateUiByNvsParam();

    if (ensureWifiEventGroup()) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_UI_INIT_DONE);
    }

    return true;
}

bool AppSettings::back(void)
{
    _is_ui_resumed = false;

    if (_screen_index == UI_WIFI_CONNECT_INDEX) {
        lv_scr_load(ui_ScreenSettingWiFi);
    } else if (_screen_index != UI_MAIN_SETTING_INDEX) {
        lv_scr_load(ui_ScreenSettingMain);
    } else {
        while (getWifiEventBits() & WIFI_EVENT_SCANING) {
            ESP_LOGI(TAG, "WiFi is scanning, please wait");
            vTaskDelay(pdMS_TO_TICKS(100));
            stopWifiScan();
        } 
        notifyCoreClosed();
    }

    return true;
}

bool AppSettings::close(void)
{
    while (getWifiEventBits() & WIFI_EVENT_SCANING) {
        ESP_LOGI(TAG, "WiFi is scanning, please wait");
        vTaskDelay(pdMS_TO_TICKS(100));
        stopWifiScan();
    } 
    
    _is_ui_del = true;

    /* Kill ESP-NOW background tasks before UI objects are destroyed */
    if (_espnow_scan_task != nullptr) {
        vTaskDelete(_espnow_scan_task);
        _espnow_scan_task = nullptr;
    }
    if (_espnow_stats_task != nullptr) {
        vTaskDelete(_espnow_stats_task);
        _espnow_stats_task = nullptr;
    }

    resetUiHandles();
    
    return true;
}

bool AppSettings::init(void)
{
    ESP_Brookesia_Phone *phone = getPhone();
    ESP_Brookesia_PhoneHome& home = phone->getHome();
    status_bar = home.getStatusBar();
    backstage = home.getRecentsScreen();

    // Initialize NVS parameters
    _nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
    _nvs_param_map[NVS_KEY_BLE_ENABLE] = false;
    _nvs_param_map[NVS_KEY_AUDIO_VOLUME] = bsp_extra_codec_volume_get();
    _nvs_param_map[NVS_KEY_AUDIO_VOLUME] = max(min((int)_nvs_param_map[NVS_KEY_AUDIO_VOLUME], SPEAKER_VOLUME_MAX), SPEAKER_VOLUME_MIN);
    _nvs_param_map[NVS_KEY_AUDIO_OUTPUT] = AUDIO_OUTPUT_AUX;
    // _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = bsp_display_brightness_get();
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = max(min((int)_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], SCREEN_BRIGHTNESS_MAX), SCREEN_BRIGHTNESS_MIN);
    _nvs_param_map[NVS_KEY_DISPLAY_TIMEOUT] = max(0, (int)app_get_screen_timeout_seconds());
    // Load NVS parameters if exist
    loadNvsParam();
    loadNvsStringParam(NVS_KEY_DEVICE_NAME, _device_name, DEVICE_NAME_DEFAULT);
    // Update System parameters
    bsp_extra_codec_volume_set(_nvs_param_map[NVS_KEY_AUDIO_VOLUME], (int *)&_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    espnow_sink_set_output_volume(_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    _nvs_param_map[NVS_KEY_AUDIO_OUTPUT] =
        (_nvs_param_map[NVS_KEY_AUDIO_OUTPUT] == AUDIO_OUTPUT_SPEAKER) ? AUDIO_OUTPUT_SPEAKER : AUDIO_OUTPUT_AUX;
    bsp_extra_output_route_set((_nvs_param_map[NVS_KEY_AUDIO_OUTPUT] == AUDIO_OUTPUT_AUX)
        ? BSP_EXTRA_AUDIO_OUTPUT_AUX
        : BSP_EXTRA_AUDIO_OUTPUT_SPEAKER);
    bsp_display_brightness_set(_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS]);
    app_set_screen_timeout_seconds(_nvs_param_map[NVS_KEY_DISPLAY_TIMEOUT]);

    if (!ensureWifiEventGroup()) {
        return false;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_CONNECTED | WIFI_EVENT_INIT_DONE |
                                             WIFI_EVENT_UI_INIT_DONE | WIFI_EVENT_SCANING);

    xTaskCreate(euiRefresTask, "Home Refresh", HOME_REFRESH_TASK_STACK_SIZE, this, HOME_REFRESH_TASK_PRIORITY, NULL);
    xTaskCreate(wifiScanTask, "WiFi Scan", WIFI_SCAN_TASK_STACK_SIZE, this, WIFI_SCAN_TASK_PRIORITY, NULL);

    return true;
}

bool AppSettings::pause(void)
{
    _is_ui_resumed = true;

    return true;
}

bool AppSettings::resume(void)
{
    _is_ui_resumed = false;

    return true;
}

void AppSettings::extraUiInit(void)
{
    const bool wifi_is_scanning = ((getWifiEventBits() & WIFI_EVENT_SCANING) != 0);

    lv_obj_clear_flag(ui_ScreenSettingMain, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_ScreenSettingWiFi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_ScreenSettingBLE, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_ScreenSettingVolume, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_ScreenSettingLight, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ui_ScreenSettingAbout, LV_OBJ_FLAG_SCROLLABLE);

    restoreBuiltInIconSources();

    /* Main */
    lv_label_set_text(ui_LabelPanelSettingMainContainer3Volume, "Audio");
    lv_label_set_text(ui_LabelPanelSettingMainContainer4Light, "Display");
    lv_label_set_text(ui_LabelPanelSettingMainContainer5About, "Device Info");
    lv_obj_align_to(ui_LabelPanelSettingMainContainer1WiFi, ui_ImagePanelSettingMainContainer1WiFi, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer2Blue, ui_ImagePanelSettingMainContainer2Blue, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer3Volume, ui_ImagePanelSettingMainContainer3Volume, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer4Light, ui_ImagePanelSettingMainContainer4Light, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer5About, ui_ImagePanelSettingMainContainer5About, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_MAIN_SETTING_INDEX] = ui_ScreenSettingMain;
    lv_obj_add_event_cb(ui_ScreenSettingMain, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* WiFi */
    // Switch
    lv_obj_set_size(ui_SwitchPanelScreenSettingWiFiSwitch, 96, 52);
    lv_obj_align(ui_SwitchPanelScreenSettingWiFiSwitch, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingWiFiSwitch, onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    
    // Hidden Network button is now created inside the WiFi list after the WiFi items (see below)
    
    // Hidden Network overlay (starts hidden)
    _hidden_network_overlay = lv_obj_create(ui_ScreenSettingWiFi);
    lv_obj_set_size(_hidden_network_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(_hidden_network_overlay);
    lv_obj_set_style_bg_color(_hidden_network_overlay, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(_hidden_network_overlay, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(_hidden_network_overlay, LV_OBJ_FLAG_HIDDEN);
    
    // Hidden network dialog card
    _hidden_network_card = lv_obj_create(_hidden_network_overlay);
    lv_obj_t *hidden_card = _hidden_network_card;
    lv_obj_set_size(hidden_card, 500, 400);
    lv_obj_center(hidden_card);
    lv_obj_set_style_radius(hidden_card, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hidden_card, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(hidden_card, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(hidden_card, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(hidden_card);
    lv_label_set_text(title, "Connect to Hidden Network");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    
    // SSID label and text area
    lv_obj_t *ssid_label = lv_label_create(hidden_card);
    lv_label_set_text(ssid_label, "Network Name (SSID):");
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 0, 45);
    
    _hidden_network_ssid_textarea = lv_textarea_create(hidden_card);
    lv_obj_set_size(_hidden_network_ssid_textarea, 440, 50);
    lv_obj_align(_hidden_network_ssid_textarea, LV_ALIGN_TOP_LEFT, 0, 75);
    lv_textarea_set_placeholder_text(_hidden_network_ssid_textarea, "Enter network name");
    lv_textarea_set_one_line(_hidden_network_ssid_textarea, true);
    lv_obj_set_style_border_color(_hidden_network_ssid_textarea, lv_color_hex(kCalPolyBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Password label and text area
    lv_obj_t *pwd_label = lv_label_create(hidden_card);
    lv_label_set_text(pwd_label, "Password:");
    lv_obj_set_style_text_font(pwd_label, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 0, 140);
    
    _hidden_network_password_textarea = lv_textarea_create(hidden_card);
    lv_obj_set_size(_hidden_network_password_textarea, 390, 50);
    lv_obj_align(_hidden_network_password_textarea, LV_ALIGN_TOP_LEFT, 0, 170);
    lv_textarea_set_placeholder_text(_hidden_network_password_textarea, "Enter password");
    lv_textarea_set_one_line(_hidden_network_password_textarea, true);
    lv_textarea_set_password_mode(_hidden_network_password_textarea, true);
    lv_obj_set_style_border_color(_hidden_network_password_textarea, lv_color_hex(kCalPolyBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Eye toggle for hidden network password
    _hidden_pwd_eye_btn = lv_btn_create(hidden_card);
    lv_obj_set_size(_hidden_pwd_eye_btn, 40, 50);
    lv_obj_align_to(_hidden_pwd_eye_btn, _hidden_network_password_textarea, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
    lv_obj_set_style_radius(_hidden_pwd_eye_btn, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_hidden_pwd_eye_btn, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(_hidden_pwd_eye_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    _hidden_pwd_eye_label = lv_label_create(_hidden_pwd_eye_btn);
    lv_label_set_text(_hidden_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(_hidden_pwd_eye_label);
    lv_obj_set_style_text_color(_hidden_pwd_eye_label, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(_hidden_pwd_eye_btn, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        bool is_pwd = lv_textarea_get_password_mode(app->_hidden_network_password_textarea);
        lv_textarea_set_password_mode(app->_hidden_network_password_textarea, !is_pwd);
        lv_label_set_text(app->_hidden_pwd_eye_label, is_pwd ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }, LV_EVENT_CLICKED, this);
    
    // Connect button
    _hidden_network_connect_btn = lv_btn_create(hidden_card);
    lv_obj_set_size(_hidden_network_connect_btn, 130, 50);
    lv_obj_align(_hidden_network_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(_hidden_network_connect_btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_hidden_network_connect_btn, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *connect_label = lv_label_create(_hidden_network_connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_set_style_text_color(connect_label, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(_hidden_network_connect_btn, onHiddenNetworkConnectClickedEventCallback, LV_EVENT_CLICKED, this);
    
    // Cancel button
    _hidden_network_cancel_btn = lv_btn_create(hidden_card);
    lv_obj_set_size(_hidden_network_cancel_btn, 110, 50);
    lv_obj_align(_hidden_network_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(_hidden_network_cancel_btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_hidden_network_cancel_btn, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *cancel_label = lv_label_create(_hidden_network_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(_hidden_network_cancel_btn, onHiddenNetworkCancelClickedEventCallback, LV_EVENT_CLICKED, this);
    
    // Keyboard for hidden network (starts hidden)
    _hidden_network_keyboard = lv_keyboard_create(_hidden_network_overlay);
    lv_obj_set_size(_hidden_network_keyboard, lv_pct(100), 250);
    lv_obj_align(_hidden_network_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
    
    // Focus callbacks for text areas
    lv_obj_add_event_cb(_hidden_network_ssid_textarea, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        lv_keyboard_set_textarea(app->_hidden_network_keyboard, app->_hidden_network_ssid_textarea);
        lv_obj_clear_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
        // Shrink card and shift up when editing SSID
        if (app->_hidden_network_card != nullptr) {
            lv_obj_set_height(app->_hidden_network_card, 310);
            lv_obj_align(app->_hidden_network_card, LV_ALIGN_TOP_MID, 0, 10);
        }
        if (app->_hidden_network_connect_btn != nullptr) {
            lv_obj_align(app->_hidden_network_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 8);
        }
        if (app->_hidden_network_cancel_btn != nullptr) {
            lv_obj_align(app->_hidden_network_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 8);
        }
    }, LV_EVENT_FOCUSED, this);
    
    // SSID Enter/Ready: close keyboard and focus password field
    lv_obj_add_event_cb(_hidden_network_ssid_textarea, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        // Move focus to password textarea
        lv_obj_clear_state(app->_hidden_network_ssid_textarea, LV_STATE_FOCUSED);
        lv_obj_add_state(app->_hidden_network_password_textarea, LV_STATE_FOCUSED);
        lv_keyboard_set_textarea(app->_hidden_network_keyboard, app->_hidden_network_password_textarea);
        // Shrink card and shift up so password is visible above keyboard
        if (app->_hidden_network_card != nullptr) {
            lv_obj_set_height(app->_hidden_network_card, 310);
            lv_obj_align(app->_hidden_network_card, LV_ALIGN_TOP_MID, 0, -100);
        }
        if (app->_hidden_network_connect_btn != nullptr) {
            lv_obj_align(app->_hidden_network_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 8);
        }
        if (app->_hidden_network_cancel_btn != nullptr) {
            lv_obj_align(app->_hidden_network_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 8);
        }
    }, LV_EVENT_READY, this);
    
    lv_obj_add_event_cb(_hidden_network_password_textarea, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        lv_keyboard_set_textarea(app->_hidden_network_keyboard, app->_hidden_network_password_textarea);
        lv_obj_clear_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
        // Shrink card and shift up so password field is visible above keyboard
        if (app->_hidden_network_card != nullptr) {
            lv_obj_set_height(app->_hidden_network_card, 310);
            lv_obj_align(app->_hidden_network_card, LV_ALIGN_TOP_MID, 0, -100);
        }
        if (app->_hidden_network_connect_btn != nullptr) {
            lv_obj_align(app->_hidden_network_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 8);
        }
        if (app->_hidden_network_cancel_btn != nullptr) {
            lv_obj_align(app->_hidden_network_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 8);
        }
    }, LV_EVENT_FOCUSED, this);

    // Password Enter/Ready: close keyboard
    lv_obj_add_event_cb(_hidden_network_password_textarea, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        lv_obj_add_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
        // Restore card size and center
        if (app->_hidden_network_card != nullptr) {
            lv_obj_set_height(app->_hidden_network_card, 400);
            lv_obj_center(app->_hidden_network_card);
        }
        if (app->_hidden_network_connect_btn != nullptr) {
            lv_obj_align(app->_hidden_network_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        }
        if (app->_hidden_network_cancel_btn != nullptr) {
            lv_obj_align(app->_hidden_network_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
    }, LV_EVENT_READY, this);

    // Tap on overlay background (outside card and keyboard) dismisses keyboard
    lv_obj_add_event_cb(_hidden_network_overlay, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        lv_obj_t *target = lv_event_get_target(e);
        // Only react when the overlay itself is tapped (not its children)
        if (target != app->_hidden_network_overlay) return;
        if (!lv_obj_has_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
            // Defocus textareas
            lv_obj_clear_state(app->_hidden_network_ssid_textarea, LV_STATE_FOCUSED);
            lv_obj_clear_state(app->_hidden_network_password_textarea, LV_STATE_FOCUSED);
            // Restore card size and center
            if (app->_hidden_network_card != nullptr) {
                lv_obj_set_height(app->_hidden_network_card, 400);
                lv_obj_center(app->_hidden_network_card);
            }
            if (app->_hidden_network_connect_btn != nullptr) {
                lv_obj_align(app->_hidden_network_connect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            }
            if (app->_hidden_network_cancel_btn != nullptr) {
                lv_obj_align(app->_hidden_network_cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            }
        }
    }, LV_EVENT_CLICKED, this);
    
    // List
    lv_obj_set_scroll_dir(ui_PanelScreenSettingWiFiList, LV_DIR_VER);
    lv_obj_set_height(ui_PanelScreenSettingWiFiList, 280);
    lv_obj_set_width(ui_PanelScreenSettingWiFiList, lv_pct(90));
    lv_obj_align_to(ui_PanelScreenSettingWiFiList, ui_PanelScreenSettingWiFiSwitch, LV_ALIGN_OUT_BOTTOM_MID, 0,
                    16);
    lv_obj_set_style_pad_all(ui_PanelScreenSettingWiFiList, 0, 0);
    lv_obj_set_style_pad_top(ui_PanelScreenSettingWiFiList, UI_WIFI_LIST_UP_PAD, 0);
    lv_obj_set_style_pad_bottom(ui_PanelScreenSettingWiFiList, UI_WIFI_LIST_DOWN_PAD, 0);
    lv_obj_set_style_pad_row(ui_PanelScreenSettingWiFiList, 0, 0);
    lv_obj_set_scrollbar_mode(ui_PanelScreenSettingWiFiList, LV_SCROLLBAR_MODE_OFF);
    // Enhanced scroll smoothness settings
    lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_anim_time(ui_PanelScreenSettingWiFiList, 300, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_snap_y(ui_PanelScreenSettingWiFiList, LV_SCROLL_SNAP_NONE);
    st_wifi_list_app = this;  // Store app pointer for WiFi list click callbacks
    for(int i = 0; i < SCAN_LIST_SIZE; i++) {
        panel_wifi_btn[i] = lv_obj_create(ui_PanelScreenSettingWiFiList);
        lv_obj_set_size(panel_wifi_btn[i], lv_pct(100), UI_WIFI_LIST_ITEM_H);
        lv_obj_set_style_radius(panel_wifi_btn[i], 0, 0);
        lv_obj_set_style_border_width(panel_wifi_btn[i], 0, 0);
        lv_obj_set_style_text_font(panel_wifi_btn[i], UI_WIFI_LIST_ITEM_FONT, 0);
        lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag( panel_wifi_btn[i], LV_OBJ_FLAG_SCROLLABLE );
        lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_left(panel_wifi_btn[i], 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(panel_wifi_btn[i], 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(panel_wifi_btn[i], lv_color_hex(0xCBCBCB), LV_PART_MAIN | LV_STATE_PRESSED );
        lv_obj_set_style_bg_opa(panel_wifi_btn[i], 255, LV_PART_MAIN| LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(panel_wifi_btn[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT );
        lv_obj_set_style_border_opa(panel_wifi_btn[i], 255, LV_PART_MAIN| LV_STATE_DEFAULT);

        label_wifi_ssid[i] = lv_label_create(panel_wifi_btn[i]);
        lv_obj_set_align(label_wifi_ssid[i], LV_ALIGN_LEFT_MID);
        lv_obj_set_width(label_wifi_ssid[i], 500);
        lv_label_set_long_mode(label_wifi_ssid[i], LV_LABEL_LONG_DOT);

        img_img_wifi_lock[i] = lv_img_create(panel_wifi_btn[i]);
        lv_obj_align(img_img_wifi_lock[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_LOCK_RIGHT_OFFSET, 0);
        lv_obj_add_flag(img_img_wifi_lock[i], LV_OBJ_FLAG_HIDDEN);

        wifi_image[i] = lv_img_create(panel_wifi_btn[i]);
        lv_obj_align(wifi_image[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_SIGNAL_RIGHT_OFFSET, 0);

        wifi_connect[i] = lv_label_create(panel_wifi_btn[i]);
        lv_label_set_text(wifi_connect[i], LV_SYMBOL_OK);
        lv_obj_align(wifi_connect[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_CONNECT_RIGHT_OFFSET, 0);
        lv_obj_add_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(panel_wifi_btn[i], onButtonWifiListClickedEventCallback, LV_EVENT_CLICKED, (void*)label_wifi_ssid[i]);
    }
    
    // Hidden Network button - at the bottom of the WiFi list (iPad/tablet style)
    _hidden_network_btn = lv_obj_create(ui_PanelScreenSettingWiFiList);
    lv_obj_set_size(_hidden_network_btn, lv_pct(100), UI_WIFI_LIST_ITEM_H);
    lv_obj_set_style_radius(_hidden_network_btn, 0, 0);
    lv_obj_set_style_border_width(_hidden_network_btn, 0, 0);
    lv_obj_set_style_bg_color(_hidden_network_btn, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(_hidden_network_btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_hidden_network_btn, lv_color_hex(0xCBCBCB), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(_hidden_network_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_hidden_network_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(_hidden_network_btn, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *hidden_btn_label = lv_label_create(_hidden_network_btn);
    lv_label_set_text(hidden_btn_label, "Hidden Network");
    lv_obj_align(hidden_btn_label, LV_ALIGN_LEFT_MID, 0, 0);  // Left-aligned text
    lv_obj_set_style_text_color(hidden_btn_label, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(hidden_btn_label, UI_WIFI_LIST_ITEM_FONT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(_hidden_network_btn, onHiddenNetworkBtnClickedEventCallback, LV_EVENT_CLICKED, this);
    if (!_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        lv_obj_add_flag(_hidden_network_btn, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 3-dot scanning indicator (replaces spinner)
    _wifi_scan_dots_label = lv_label_create(ui_ScreenSettingWiFi);
    lv_label_set_text(_wifi_scan_dots_label, "Scanning");
    lv_obj_set_style_text_font(_wifi_scan_dots_label, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(_wifi_scan_dots_label, lv_color_hex(0x888888), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(_wifi_scan_dots_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_wifi_scan_dots_label, LV_OBJ_FLAG_HIDDEN);
    _wifi_scan_dots_timer = lv_timer_create([](lv_timer_t *timer) {
        lv_obj_t *label = (lv_obj_t *)timer->user_data;
        if (label == nullptr || lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) return;
        const char *txt = lv_label_get_text(label);
        if (strcmp(txt, "Scanning.") == 0) lv_label_set_text(label, "Scanning..");
        else if (strcmp(txt, "Scanning..") == 0) lv_label_set_text(label, "Scanning...");
        else lv_label_set_text(label, "Scanning.");
    }, 400, _wifi_scan_dots_label);
    lv_timer_pause(_wifi_scan_dots_timer);
    // Always hide the SquareLine spinner - we use dots now
    lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    
    if (!wifi_is_scanning) {
        // Keep WiFi list visible (but items hidden) so Hidden Network button is always accessible
        lv_obj_clear_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    }
    // lv_obj_add_flag(ui_ButtonScreenSettingWiFiReturn, LV_OBJ_FLAG_HIDDEN);
    // Connect
    lv_obj_add_flag(ui_SpinnerScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
    _panel_wifi_connect = lv_obj_create(ui_ScreenSettingVerification);
    lv_obj_set_size(_panel_wifi_connect, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(_panel_wifi_connect, 0, 0);
    lv_obj_set_style_bg_color(_panel_wifi_connect, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(_panel_wifi_connect, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(_panel_wifi_connect, 0, 0);
    lv_obj_set_style_border_width(_panel_wifi_connect, 0, 0);
    lv_obj_set_align(_panel_wifi_connect, LV_ALIGN_TOP_LEFT);
    lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_SCROLLABLE);
    _img_wifi_connect = lv_img_create(_panel_wifi_connect);
    lv_obj_center(_img_wifi_connect);
    _spinner_wifi_connect = lv_spinner_create(_panel_wifi_connect, 1000, 600);
    lv_obj_set_size(_spinner_wifi_connect, lv_pct(20), lv_pct(20));
    lv_obj_center(_spinner_wifi_connect);
    processWifiConnect(WIFI_CONNECT_HIDE);

    // Shift SSID label and Password textarea down to avoid overlap with back button
    // SquareLine sets SSID label y=-197, password y=-95. Add 50px offset.
    lv_obj_set_y(ui_LabelScreenSettingVerification, -147);       // "SSID:" label
    lv_obj_set_y(ui_LabelScreenSettingVerificationSSID, -147);   // SSID value label
    lv_obj_set_y(ui_TextAreaScreenSettingVerificationPassword, -45); // Password textarea

    // Back button for verification screen - styled with green background like main return buttons
    _btn_back_verification = lv_btn_create(ui_ScreenSettingVerification);
    lv_obj_set_size(_btn_back_verification, 60, 60);
    lv_obj_set_x(_btn_back_verification, -348);
    lv_obj_set_y(_btn_back_verification, -180);
    lv_obj_set_align(_btn_back_verification, LV_ALIGN_CENTER);
    lv_obj_add_flag(_btn_back_verification, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(_btn_back_verification, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *back_img = lv_img_create(_btn_back_verification);
        lv_img_set_src(back_img, &ui_img_return_png);
        lv_obj_set_size(back_img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_align(back_img, LV_ALIGN_CENTER);
        lv_obj_add_flag(back_img, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_clear_flag(back_img, LV_OBJ_FLAG_SCROLLABLE);
        // Apply green button style with white icon
        apply_return_button_style(_btn_back_verification, back_img);
    }
    lv_obj_add_event_cb(_btn_back_verification, onVerificationBackButtonClickedEventCallback,
                        LV_EVENT_CLICKED, this);
    // Keyboard
    lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
    // lv_obj_set_size(ui_KeyboardScreenSettingVerification, lv_pct(100), lv_pct(UI_WIFI_KEYBOARD_H_PERCENT));
    // lv_obj_align(ui_KeyboardScreenSettingVerification, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(ui_KeyboardScreenSettingVerification, onKeyboardScreenSettingVerificationClickedEventCallback,
                        LV_EVENT_CLICKED, this);
    
    // Eye toggle for WiFi verification password
    _verify_pwd_eye_btn = lv_btn_create(ui_ScreenSettingVerification);
    lv_obj_set_size(_verify_pwd_eye_btn, 50, 50);
    lv_obj_align_to(_verify_pwd_eye_btn, ui_TextAreaScreenSettingVerificationPassword, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_set_style_radius(_verify_pwd_eye_btn, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_verify_pwd_eye_btn, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(_verify_pwd_eye_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    _verify_pwd_eye_label = lv_label_create(_verify_pwd_eye_btn);
    lv_label_set_text(_verify_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(_verify_pwd_eye_label);
    lv_obj_set_style_text_color(_verify_pwd_eye_label, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(_verify_pwd_eye_btn, [](lv_event_t *e) {
        AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
        bool is_pwd = lv_textarea_get_password_mode(ui_TextAreaScreenSettingVerificationPassword);
        lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, !is_pwd);
        lv_label_set_text(app->_verify_pwd_eye_label, is_pwd ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }, LV_EVENT_CLICKED, this);
    // Record the screen index and install the screen loaded event callback
    // lv_obj_add_flag(ui_ButtonScreenSettingBLEReturn, LV_OBJ_FLAG_HIDDEN);
    _screen_list[UI_WIFI_SCAN_INDEX] = ui_ScreenSettingWiFi;
    lv_obj_add_event_cb(ui_ScreenSettingWiFi, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);
    _screen_list[UI_WIFI_CONNECT_INDEX] = ui_ScreenSettingVerification;
    lv_obj_add_event_cb(ui_ScreenSettingVerification, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* Bluetooth */
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingBLESwitch, onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_BLUETOOTH_SETTING_INDEX] = ui_ScreenSettingBLE;
    lv_obj_add_event_cb(ui_ScreenSettingBLE, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* Display */
    lv_slider_set_range(ui_SliderPanelScreenSettingLightSwitch1, SCREEN_BRIGHTNESS_MIN, SCREEN_BRIGHTNESS_MAX);
    lv_obj_set_height(ui_SliderPanelScreenSettingLightSwitch1, 28);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingLightSwitch1, onSliderPanelLightSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingLightSwitch1, onSliderPanelLightSwitchValueChangeEventCallback,
                        LV_EVENT_RELEASED, this);
    lv_obj_clear_flag(ui_PanelScreenSettingLightList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelScreenSettingLightList, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                                     LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(ui_PanelScreenSettingLightList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_PanelScreenSettingLightList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_time(ui_PanelScreenSettingLightList, 200, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_PanelScreenSettingLightList, lv_pct(90));
    lv_obj_set_height(ui_PanelScreenSettingLightList, kDisplayDetailsListHeight);
    lv_obj_align_to(ui_PanelScreenSettingLightList, ui_PanelScreenSettingLightSwitch, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);
    lv_obj_set_style_bg_opa(ui_PanelScreenSettingLightList, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_PanelScreenSettingLightList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_PanelScreenSettingLightList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_PanelScreenSettingLightList, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_PanelScreenSettingLightList, 26, LV_PART_MAIN | LV_STATE_DEFAULT);

    _display_timeout_panel = lv_obj_create(ui_PanelScreenSettingLightList);
    lv_obj_set_size(_display_timeout_panel, lv_pct(100), 83);
    lv_obj_clear_flag(_display_timeout_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(_display_timeout_panel, 20, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *timeout_label = lv_label_create(_display_timeout_panel);
    lv_label_set_text(timeout_label, "Screen Timeout");
    lv_obj_set_style_text_font(timeout_label, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(timeout_label, LV_ALIGN_LEFT_MID, 30, 0);

    _display_timeout_dropdown = lv_dropdown_create(_display_timeout_panel);
    lv_dropdown_set_options_static(_display_timeout_dropdown, "Off\n15 sec\n30 sec\n1 min\n2 min\n5 min\n10 min");
    lv_obj_set_width(_display_timeout_dropdown, 220);
    lv_obj_align(_display_timeout_dropdown, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_add_event_cb(_display_timeout_dropdown, onDropdownScreenTimeoutValueChangeEventCallback, LV_EVENT_VALUE_CHANGED, this);

    _display_theme_panel = lv_obj_create(ui_PanelScreenSettingLightList);
    lv_obj_set_size(_display_theme_panel, lv_pct(100), kDisplayThemePanelHeight);
    lv_obj_clear_flag(_display_theme_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(_display_theme_panel, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(_display_theme_panel, 16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *theme_title = lv_label_create(_display_theme_panel);
    lv_label_set_text(theme_title, "Theme");
    lv_obj_set_style_text_font(theme_title, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    apply_text_accent_style(theme_title);
    lv_obj_align(theme_title, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *theme_subtitle = lv_label_create(_display_theme_panel);
    lv_label_set_text(theme_subtitle, "Choose a preloaded look");
    lv_obj_set_style_text_font(theme_subtitle, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(theme_subtitle, lv_color_hex(kCalPolyGreenLight), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(theme_subtitle, LV_ALIGN_TOP_LEFT, 16, 46);

    lv_obj_t *theme_row = lv_obj_create(_display_theme_panel);
    lv_obj_set_size(theme_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(theme_row, LV_ALIGN_TOP_MID, 0, kDisplayThemeRowTopOffset);
    lv_obj_clear_flag(theme_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(theme_row, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(theme_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(theme_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(theme_row, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(theme_row, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(theme_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(theme_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (int theme_index = APP_THEME_CALPOLY; theme_index < APP_THEME_MAX; ++theme_index) {
        app_theme_id_t theme_id = static_cast<app_theme_id_t>(theme_index);

        _theme_preview_cards[theme_index] = lv_btn_create(theme_row);
        lv_obj_set_size(_theme_preview_cards[theme_index], kThemePreviewCardWidth, kThemePreviewCardHeight);
        lv_obj_clear_flag(_theme_preview_cards[theme_index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(_theme_preview_cards[theme_index], 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(_theme_preview_cards[theme_index], 22, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(_theme_preview_cards[theme_index], onThemePreviewCardClickedEventCallback, LV_EVENT_CLICKED, this);

        lv_obj_t *preview_frame = lv_obj_create(_theme_preview_cards[theme_index]);
        lv_obj_set_size(preview_frame, lv_pct(100), 118);
        lv_obj_align(preview_frame, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(preview_frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(preview_frame, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_radius(preview_frame, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_clip_corner(preview_frame, true, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(preview_frame, lv_color_hex(0xEAF2ED), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(preview_frame, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(preview_frame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(preview_frame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(preview_frame, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *preview = lv_img_create(preview_frame);
        lv_img_set_src(preview, get_theme_preview_asset(theme_id));
        lv_img_set_zoom(preview, 176);
        lv_obj_center(preview);

        lv_obj_t *footer = lv_obj_create(_theme_preview_cards[theme_index]);
        lv_obj_set_size(footer, lv_pct(100), 52);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(footer, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_radius(footer, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(footer, lv_color_hex(kCalPolySurfaceAlt), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(footer, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(footer, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(footer, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(footer, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(footer, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(footer, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *theme_name = lv_label_create(footer);
        lv_label_set_text(theme_name, get_theme_name(theme_id));
        lv_obj_set_style_text_font(theme_name, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
        apply_text_accent_style(theme_name);
        lv_obj_align(theme_name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *theme_description = lv_label_create(footer);
        lv_label_set_text(theme_description, get_theme_description(theme_id));
        lv_obj_set_style_text_font(theme_description, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(theme_description, lv_color_hex(kCalPolyGreenLight), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(theme_description, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        _theme_preview_badges[theme_index] = lv_label_create(_theme_preview_cards[theme_index]);
        lv_label_set_text(_theme_preview_badges[theme_index], "Selected");
        lv_obj_add_flag(_theme_preview_badges[theme_index], LV_OBJ_FLAG_FLOATING);
        lv_obj_align(_theme_preview_badges[theme_index], LV_ALIGN_TOP_RIGHT, -10, 10);
        lv_obj_add_flag(_theme_preview_badges[theme_index], LV_OBJ_FLAG_HIDDEN);
    }
    // lv_obj_add_flag(ui_ButtonScreenSettingLightReturn, LV_OBJ_FLAG_HIDDEN);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_BRIGHTNESS_SETTING_INDEX] = ui_ScreenSettingLight;
    lv_obj_add_event_cb(ui_ScreenSettingLight, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* Audio */
    lv_slider_set_range(ui_SliderPanelScreenSettingVolumeSwitch, SPEAKER_VOLUME_MIN, SPEAKER_VOLUME_MAX);
    lv_obj_set_height(ui_SliderPanelScreenSettingVolumeSwitch, 28);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingVolumeSwitch, onSliderPanelVolumeSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingVolumeSwitch, onSliderPanelVolumeSwitchValueChangeEventCallback,
                        LV_EVENT_RELEASED, this);
    lv_obj_clear_flag(ui_PanelScreenSettingVolumeList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelScreenSettingVolumeList, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                                   LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(ui_PanelScreenSettingVolumeList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_PanelScreenSettingVolumeList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_time(ui_PanelScreenSettingVolumeList, 200, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_PanelScreenSettingVolumeList, lv_pct(90));
    lv_obj_set_height(ui_PanelScreenSettingVolumeList, 190);
    lv_obj_align_to(ui_PanelScreenSettingVolumeList, ui_PanelScreenSettingVolumeSwitch, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);
    lv_obj_set_style_bg_opa(ui_PanelScreenSettingVolumeList, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_PanelScreenSettingVolumeList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_PanelScreenSettingVolumeList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ui_PanelScreenSettingVolumeList, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui_PanelScreenSettingVolumeList, 24, LV_PART_MAIN | LV_STATE_DEFAULT);

    _audio_output_panel = lv_obj_create(ui_PanelScreenSettingVolumeList);
    lv_obj_set_size(_audio_output_panel, lv_pct(100), 83);
    lv_obj_clear_flag(_audio_output_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(_audio_output_panel, 20, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *audio_output_label = lv_label_create(_audio_output_panel);
    lv_label_set_text(audio_output_label, "Output");
    lv_obj_set_style_text_font(audio_output_label, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(audio_output_label, LV_ALIGN_LEFT_MID, 30, 0);

    _audio_output_dropdown = lv_dropdown_create(_audio_output_panel);
    lv_dropdown_set_options_static(_audio_output_dropdown, "SPEAKER\nAUX");
    lv_obj_set_width(_audio_output_dropdown, 360);
    lv_obj_align(_audio_output_dropdown, LV_ALIGN_RIGHT_MID, -24, 0);
    apply_dropdown_style(_audio_output_dropdown);
    /* Add extra vertical breathing room between options to reduce accidental taps. */
    lv_obj_set_style_text_line_space(_audio_output_dropdown, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(_audio_output_dropdown, 12, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(_audio_output_dropdown, 12, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(_audio_output_dropdown, onDropdownAudioOutputValueChangeEventCallback, LV_EVENT_VALUE_CHANGED, this);

    // lv_obj_add_flag(ui_ButtonScreenSettingVolumeReturn, LV_OBJ_FLAG_HIDDEN);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_VOLUME_SETTING_INDEX] = ui_ScreenSettingVolume;
    lv_obj_add_event_cb(ui_ScreenSettingVolume, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* About */
    // lv_obj_add_flag(ui_ButtonScreenSettingAboutReturn, LV_OBJ_FLAG_HIDDEN);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_ABOUT_SETTING_INDEX] = ui_ScreenSettingAbout;
    lv_obj_add_event_cb(ui_ScreenSettingAbout, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    lv_obj_add_flag(ui_PanelSettingMainContainerItem1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelSettingMainContainerItem2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelSettingMainContainerItem3, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAboutManufacturer, "Manufacturer");
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout1, "Cal Poly");
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout3, mac_str);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAboutSoftwareVersion, "Firmware Version");
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout5, get_combined_firmware_version_text());
    lv_obj_add_flag(ui_PanelPanelScreenSettingAbout3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelPanelScreenSettingAbout5, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelScreenSettingAbout, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(ui_PanelScreenSettingAbout, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_PanelScreenSettingAbout, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ui_PanelPanelScreenSettingAbout, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_PanelPanelScreenSettingAbout, onDeviceNamePanelClickedEventCallback, LV_EVENT_CLICKED, this);

    align_info_value_label(ui_LabelPanelPanelScreenSettingAbout2);
    align_info_value_label(ui_LabelPanelPanelScreenSettingAbout1);
    align_info_value_label(ui_LabelPanelPanelScreenSettingAbout3);
    align_info_value_label(ui_LabelPanelPanelScreenSettingAbout5);
    refreshDeviceInfoUi();
    espnow_sink_request_fw_version();

    // Timezone dropdown - create panel below device info (matching style of other Device Info panels)
    _timezone_panel = lv_obj_create(ui_PanelScreenSettingAbout);
    lv_obj_set_width(_timezone_panel, 716);
    lv_obj_set_height(_timezone_panel, 82);
    lv_obj_set_x(_timezone_panel, 1);
    lv_obj_set_y(_timezone_panel, -15);
    lv_obj_set_align(_timezone_panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(_timezone_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_color(_timezone_panel, lv_color_hex(0xF6F6F6), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(_timezone_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    _timezone_label = lv_label_create(_timezone_panel);
    lv_obj_set_width(_timezone_label, LV_SIZE_CONTENT);
    lv_obj_set_height(_timezone_label, LV_SIZE_CONTENT);
    lv_obj_set_x(_timezone_label, -280);
    lv_obj_set_y(_timezone_label, 1);
    lv_obj_set_align(_timezone_label, LV_ALIGN_CENTER);
    lv_label_set_text(_timezone_label, "Timezone");
    lv_obj_set_style_text_font(_timezone_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *timezone_dropdown = lv_dropdown_create(_timezone_panel);
    lv_dropdown_set_options_static(timezone_dropdown,
        "Eastern\n"
        "Central\n"
        "Mountain\n"
        "Arizona\n"
        "Pacific\n"
        "Alaska\n"
        "Hawaii");
    lv_obj_set_width(timezone_dropdown, 280);
    lv_obj_set_height(timezone_dropdown, 50);
    lv_obj_set_x(timezone_dropdown, 180);
    lv_obj_set_y(timezone_dropdown, 0);
    lv_obj_set_align(timezone_dropdown, LV_ALIGN_CENTER);
    lv_dropdown_set_selected(timezone_dropdown, app_sntp_get_timezone_index());
    apply_dropdown_style(timezone_dropdown);
    // Make dropdown list taller for better visibility
    lv_obj_t *dropdown_list = lv_dropdown_get_list(timezone_dropdown);
    if (dropdown_list) {
        lv_obj_set_style_max_height(dropdown_list, 350, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_obj_add_event_cb(timezone_dropdown, [](lv_event_t *e) {
        lv_obj_t *dropdown = lv_event_get_target(e);
        int selected = lv_dropdown_get_selected(dropdown);
        app_sntp_set_timezone_by_index(selected);
        ESP_LOGI("Settings", "Timezone changed to index %d", selected);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *device_name_edit_hint = lv_label_create(ui_PanelPanelScreenSettingAbout);
    lv_label_set_text(device_name_edit_hint, LV_SYMBOL_EDIT);
    lv_obj_align(device_name_edit_hint, LV_ALIGN_RIGHT_MID, 12, 0);

    _device_name_editor_overlay = lv_obj_create(ui_ScreenSettingAbout);
    lv_obj_set_size(_device_name_editor_overlay, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(_device_name_editor_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_device_name_editor_overlay, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(_device_name_editor_overlay, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(_device_name_editor_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *device_name_dialog = lv_obj_create(_device_name_editor_overlay);
    lv_obj_set_size(device_name_dialog, 860, 480);
    lv_obj_center(device_name_dialog);
    lv_obj_clear_flag(device_name_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(device_name_dialog, 24, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *device_name_title = lv_label_create(device_name_dialog);
    lv_label_set_text(device_name_title, "Device Name");
    lv_obj_set_style_text_font(device_name_title, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(device_name_title, LV_ALIGN_TOP_LEFT, 24, 18);

    _device_name_editor_textarea = lv_textarea_create(device_name_dialog);
    lv_obj_set_size(_device_name_editor_textarea, 812, 64);
    lv_obj_align(_device_name_editor_textarea, LV_ALIGN_TOP_MID, 0, 70);
    lv_textarea_set_one_line(_device_name_editor_textarea, true);
    lv_textarea_set_max_length(_device_name_editor_textarea, DEVICE_NAME_MAX_LEN - 1);
    lv_textarea_set_placeholder_text(_device_name_editor_textarea, DEVICE_NAME_DEFAULT);
    lv_obj_set_style_text_font(_device_name_editor_textarea, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);

    _device_name_editor_keyboard = lv_keyboard_create(device_name_dialog);
    lv_obj_set_size(_device_name_editor_keyboard, 812, 280);
    lv_obj_align(_device_name_editor_keyboard, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_keyboard_set_textarea(_device_name_editor_keyboard, _device_name_editor_textarea);
    lv_obj_add_event_cb(_device_name_editor_keyboard, onDeviceNameKeyboardEventCallback, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_device_name_editor_keyboard, onDeviceNameKeyboardEventCallback, LV_EVENT_CANCEL, this);

    /* ESP-NOW Audio */
    _screen_list[UI_ESPNOW_SETTING_INDEX] = ui_ScreenSettingESPNOW;
    lv_obj_add_event_cb(ui_ScreenSettingESPNOW, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingESPNOWSwitch, onESPNOWSwitchValueChangeEventCallback, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(ui_ButtonScreenSettingESPNOWDisconnect, onESPNOWDisconnectButtonClickedEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_set_height(ui_PanelScreenSettingESPNOWList, 300);
    lv_obj_set_y(ui_PanelScreenSettingESPNOWList, 220);
    lv_obj_set_height(ui_PanelScreenSettingESPNOWStats, 300);
    lv_obj_set_y(ui_PanelScreenSettingESPNOWStats, 220);

    lv_label_set_text(ui_LabelScreenSettingESPNOWPacketsRx, "Room: --");
    lv_obj_set_pos(ui_LabelScreenSettingESPNOWPacketsRx, 24, 20);
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWPacketsRx, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
    _espnow_selected_room_label = ui_LabelScreenSettingESPNOWPacketsRx;

    lv_label_set_text(ui_LabelScreenSettingESPNOWPacketsLost, "Volume");
    lv_obj_set_pos(ui_LabelScreenSettingESPNOWPacketsLost, 24, 86);
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWPacketsLost, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);

    _espnow_volume_slider = lv_slider_create(ui_PanelScreenSettingESPNOWStats);
    lv_obj_set_size(_espnow_volume_slider, 350, 28);
    lv_obj_set_pos(_espnow_volume_slider, 130, 90);
    lv_slider_set_range(_espnow_volume_slider, SPEAKER_VOLUME_MIN, SPEAKER_VOLUME_MAX);
    apply_slider_style(_espnow_volume_slider);
    lv_obj_add_event_cb(_espnow_volume_slider, onSliderPanelVolumeSwitchValueChangeEventCallback, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_espnow_volume_slider, onSliderPanelVolumeSwitchValueChangeEventCallback, LV_EVENT_RELEASED, this);

    _espnow_volume_value_label = lv_label_create(ui_PanelScreenSettingESPNOWStats);
    lv_obj_set_width(_espnow_volume_value_label, 90);
    lv_obj_set_pos(_espnow_volume_value_label, 500, 86);
    lv_obj_set_style_text_font(_espnow_volume_value_label, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(_espnow_volume_value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_label_set_text(ui_LabelScreenSettingESPNOWRSSI, "Output: AUX");
    lv_obj_set_pos(ui_LabelScreenSettingESPNOWRSSI, 24, 154);
    lv_obj_set_style_text_font(ui_LabelScreenSettingESPNOWRSSI, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);

    _espnow_output_dropdown = lv_dropdown_create(ui_PanelScreenSettingESPNOWStats);
    lv_dropdown_set_options_static(_espnow_output_dropdown, "AUX");
    lv_obj_set_size(_espnow_output_dropdown, 300, 56);
    lv_obj_set_pos(_espnow_output_dropdown, 150, 138);
    apply_dropdown_style(_espnow_output_dropdown);
    lv_obj_add_event_cb(_espnow_output_dropdown, onDropdownAudioOutputValueChangeEventCallback, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_flag(_espnow_output_dropdown, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_size(ui_ButtonScreenSettingESPNOWDisconnect, 190, 52);
    lv_obj_align(ui_ButtonScreenSettingESPNOWDisconnect, LV_ALIGN_BOTTOM_RIGHT, -24, -22);

    applyThemeToSettingScreens();
    updateThemePreviewSelection();
}

void AppSettings::processWifiConnect(WifiConnectState_t state)
{
    if ((_panel_wifi_connect == nullptr) || (_img_wifi_connect == nullptr) || (_spinner_wifi_connect == nullptr)) {
        return;
    }

    switch (state) {
    case WIFI_CONNECT_HIDE:
        lv_obj_add_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_RUNNING:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_SUCCESS:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(_img_wifi_connect, &img_wifi_connect_success);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_FAIL:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(_img_wifi_connect, &img_wifi_connect_fail);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

bool AppSettings::loadNvsParam(void)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    for (auto& key_value : _nvs_param_map) {
        err = nvs_get_i32(nvs_handle, key_value.first.c_str(), &key_value.second);
        switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG, "Load %s: %d", key_value.first.c_str(), key_value.second);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            err = nvs_set_i32(nvs_handle, key_value.first.c_str(), key_value.second);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) setting %s", esp_err_to_name(err), key_value.first.c_str());
            }
            ESP_LOGW(TAG, "The value of %s is not initialized yet, set it to default value: %d", key_value.first.c_str(),
                     key_value.second);
            break;
        default:
            break;
        }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing NVS changes", esp_err_to_name(err));
        return false;
    }
    nvs_close(nvs_handle);

    return true;
}

bool AppSettings::setNvsParam(std::string key, int value)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_i32(nvs_handle, key.c_str(), value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting %s", esp_err_to_name(err), key.c_str());
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing NVS changes", esp_err_to_name(err));
        return false;
    }
    nvs_close(nvs_handle);

    return true;
}

bool AppSettings::loadNvsStringParam(const std::string &key, std::string &value, const char *default_value)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, key.c_str(), nullptr, &required_size);
    if (err == ESP_OK && required_size > 0) {
        std::string loaded_value(required_size, '\0');
        err = nvs_get_str(nvs_handle, key.c_str(), loaded_value.data(), &required_size);
        if (err == ESP_OK) {
            loaded_value.resize(required_size > 0 ? required_size - 1 : 0);
            value = sanitize_device_name(loaded_value.c_str());
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        value = sanitize_device_name(default_value);
        err = nvs_set_str(nvs_handle, key.c_str(), value.c_str());
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) reading %s", esp_err_to_name(err), key.c_str());
    }

    nvs_close(nvs_handle);
    return (err == ESP_OK);
}

bool AppSettings::setNvsStringParam(const std::string &key, const std::string &value)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs_handle, key.c_str(), value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting %s", esp_err_to_name(err), key.c_str());
    }

    nvs_close(nvs_handle);
    return (err == ESP_OK);
}

void AppSettings::updateUiByNvsParam(void)
{
    if (_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        lv_obj_add_state(ui_SwitchPanelScreenSettingWiFiSwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingWiFiSwitch, LV_STATE_CHECKED);
    }

    if (_nvs_param_map[NVS_KEY_BLE_ENABLE]) {
        lv_obj_add_state(ui_SwitchPanelScreenSettingBLESwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingBLESwitch, LV_STATE_CHECKED);
    }

    if (_hidden_network_btn != nullptr) {
        if (_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
            lv_obj_clear_flag(_hidden_network_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_hidden_network_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderPanelScreenSettingVolumeSwitch, _nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
    espnow_sink_set_output_volume(_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    if (_espnow_volume_slider != nullptr) {
        lv_slider_set_value(_espnow_volume_slider, _nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
    }
    if (_espnow_volume_value_label != nullptr) {
        lv_label_set_text_fmt(_espnow_volume_value_label, "%ld%%", (long)_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    }
    if (_audio_output_dropdown != nullptr) {
        const uint16_t selected = (_nvs_param_map[NVS_KEY_AUDIO_OUTPUT] == AUDIO_OUTPUT_AUX) ? AUDIO_OUTPUT_AUX : AUDIO_OUTPUT_SPEAKER;
        lv_dropdown_set_selected(_audio_output_dropdown, selected);
    }
    _nvs_param_map[NVS_KEY_AUDIO_OUTPUT] = AUDIO_OUTPUT_AUX;
    if (_display_timeout_dropdown != nullptr) {
        lv_dropdown_set_selected(_display_timeout_dropdown, screen_timeout_to_index(_nvs_param_map[NVS_KEY_DISPLAY_TIMEOUT]));
    }
    applyThemeToSettingScreens();
    updateThemePreviewSelection();
    updateEspnowUiState();
}

void AppSettings::refreshDeviceInfoUi(void)
{
    _device_name = sanitize_device_name(_device_name.c_str());
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout2, _device_name.c_str());
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout1, "Cal Poly");
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout3, mac_str);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout5, get_combined_firmware_version_text());
}

void AppSettings::showEspnowScanningUi(void)
{
    if (!isUiObjectValid(ui_LabelScreenSettingESPNOWStatus)) {
        return;
    }
    lv_obj_add_state(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_STATE_CHECKED);
    lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Scanning For Room...");
    lv_obj_clear_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
}

void AppSettings::showEspnowConnectedUi(const char *room_name)
{
    char status_buf[64];
    snprintf(status_buf, sizeof(status_buf), "Connected Room: %s", (room_name && room_name[0]) ? room_name : "Unknown");
    lv_obj_add_state(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_STATE_CHECKED);
    if (strcmp(lv_label_get_text(ui_LabelScreenSettingESPNOWStatus), status_buf) != 0) {
        lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, status_buf);
    }
    if (_espnow_selected_room_label != nullptr) {
        char room_buf[48];
        snprintf(room_buf, sizeof(room_buf), "Room: %s", (room_name && room_name[0]) ? room_name : "Unknown");
        if (strcmp(lv_label_get_text(_espnow_selected_room_label), room_buf) != 0) {
            lv_label_set_text(_espnow_selected_room_label, room_buf);
        }
    }
    if (!lv_obj_has_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(ui_PanelScreenSettingESPNOWList);
    }
    if (lv_obj_has_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
    }
    if (!lv_obj_has_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
    }
    if (_espnow_volume_slider != nullptr) {
        lv_slider_set_value(_espnow_volume_slider, _nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
    }
    if (_espnow_volume_value_label != nullptr) {
        lv_label_set_text_fmt(_espnow_volume_value_label, "%ld%%", (long)_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    }
}

void AppSettings::updateEspnowUiState(void)
{
    if (!isUiObjectValid(ui_SwitchPanelScreenSettingESPNOWSwitch)) {
        return;
    }
    if (espnow_sink_is_connected()) {
        espnow_room_info_t room = {};
        char room_name[32] = "Unknown";
        if (espnow_sink_get_connected_room(&room)) {
            formatEspnowRoomName(room, room_name, sizeof(room_name));
        }
        showEspnowConnectedUi(room_name);
        return;
    }

    if (espnow_sink_get_autoscan()) {
        showEspnowScanningUi();
        if (_espnow_scan_task == nullptr) {
            xTaskCreatePinnedToCore(espnowScanTask, "espnow_scan", 4096, this, 2, &_espnow_scan_task, 0);
        }
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingESPNOWSwitch, LV_STATE_CHECKED);
        lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Assistive Listening Off");
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::applyThemeToSettingScreens(void)
{
    const app_theme_id_t current_theme = sanitize_theme_id(app_get_ui_theme_id());
    const lv_img_dsc_t *wallpaper = get_theme_wallpaper_asset(current_theme);
    const uint32_t surface_color = get_theme_surface_color(current_theme);
    const uint32_t surface_alt_color = get_theme_surface_alt_color(current_theme);

    restoreBuiltInIconSources();

    lv_obj_t *screen_list[] = {
        ui_ScreenSettingMain,
        ui_ScreenSettingWiFi,
        ui_ScreenSettingBLE,
        ui_ScreenSettingVolume,
        ui_ScreenSettingLight,
        ui_ScreenSettingAbout,
        ui_ScreenSettingESPNOW,
        ui_ScreenSettingVerification,
    };
    for (lv_obj_t *screen : screen_list) {
        apply_screen_background_style(screen, wallpaper, surface_color);
    }

    lv_obj_t *transparent_panels[] = {
        ui_PanelSettingMainContainer,
        ui_PanelScreenSettingWiFiList,
        ui_PanelScreenSettingLightList,
        ui_PanelScreenSettingAboutList,
        ui_PanelScreenSettingAbout,
    };
    for (lv_obj_t *panel : transparent_panels) {
        if (panel == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t *content_cards[] = {
        ui_PanelSettingMainContainerItem1,
        ui_PanelSettingMainContainerItem2,
        ui_PanelSettingMainContainerItem3,
        ui_PanelSettingMainContainerItem4,
        ui_PanelSettingMainContainerItem5,
        ui_PanelSettingMainContainerItem6,
        ui_PanelScreenSettingWiFiSwitch,
        ui_PanelScreenSettingBLESwitch,
        ui_PanelScreenSettingVolumeSwitch,
        ui_PanelScreenSettingLightSwitch,
        ui_PanelScreenSettingESPNOWSwitch,
        ui_PanelScreenSettingESPNOWStats,
        _display_timeout_panel,
        _display_theme_panel,
        ui_PanelPanelScreenSettingAbout,
        ui_PanelPanelScreenSettingAbout1,
        ui_PanelPanelScreenSettingAbout2,
        ui_PanelPanelScreenSettingAbout4,
        ui_PanelPanelScreenSettingAbout5,
        _timezone_panel,
    };
    for (lv_obj_t *card : content_cards) {
        apply_panel_card_style(card, false);
    }

    for (int i = 0; i < SCAN_LIST_SIZE; ++i) {
        if (panel_wifi_btn[i] == nullptr) {
            continue;
        }
        apply_panel_card_style(panel_wifi_btn[i], true);
        lv_obj_set_style_radius(panel_wifi_btn[i], 16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(panel_wifi_btn[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(panel_wifi_btn[i], lv_color_hex(surface_alt_color), LV_PART_MAIN | LV_STATE_PRESSED);
        apply_text_accent_style(label_wifi_ssid[i]);
    }

    lv_obj_t *main_labels[] = {
        ui_LabelPanelSettingMainContainer1WiFi,
        ui_LabelPanelSettingMainContainer2Blue,
        ui_LabelPanelSettingMainContainer3Volume,
        ui_LabelPanelSettingMainContainer4Light,
        ui_LabelPanelSettingMainContainer5About,
        ui_LabelPanelSettingMainContainer6ESPNOW,
        ui_LabelPanelScreenSettingWiFiSwitch,
        ui_LabelPanelScreenSettingBLESwitch,
        ui_LabelPanelScreenSettingVolumeSwitch,
        ui_LabelPanelScreenSettingLightSwitch,
        ui_LabelPanelScreenSettingESPNOWSwitch,
        ui_LabelScreenSettingESPNOWStatus,
        ui_LabelScreenSettingESPNOWPacketsRx,
        ui_LabelScreenSettingESPNOWPacketsLost,
        ui_LabelScreenSettingESPNOWRSSI,
        ui_LabelPanelPanelScreenSettingAboutDevice,
        ui_LabelPanelPanelScreenSettingAboutManufacturer,
        ui_LabelPanelPanelScreenSettingAboutMAC,
        ui_LabelPanelPanelScreenSettingAboutSoftwareVersion,
        ui_LabelPanelPanelScreenSettingAboutUIFrameworkVersion,
        ui_LabelPanelPanelScreenSettingAbout1,
        ui_LabelPanelPanelScreenSettingAbout2,
        ui_LabelPanelPanelScreenSettingAbout3,
        ui_LabelPanelPanelScreenSettingAbout5,
        ui_LabelPanelPanelScreenSettingAbout6,
        _timezone_label,
    };
    for (lv_obj_t *label : main_labels) {
        apply_text_accent_style(label);
    }

    lv_obj_t *accent_images[] = {
        ui_ImagePanelSettingMainContainer1WiFi,
        ui_ImagePanelSettingMainContainer1Arrow,
        ui_ImagePanelSettingMainContainer2Blue,
        ui_ImagePanelSettingMainContainer2Arrow,
        ui_ImagePanelSettingMainContainer6ESPNOW,
        ui_ImagePanelSettingMainContainer6Arrow,
        ui_ImagePanelScreenSettingWiFiSwitch,
        ui_ImagePanelScreenSettingBLESwitch,
        ui_ImagePanelScreenSettingVolumeSwitch,
        ui_ImagePanelScreenSettingLightSwitch,
        ui_ImagePanelScreenSettingESPNOWSwitch,
        ui_ImagePanelSettingMainContainer3Volume,
        ui_ImagePanelSettingMainContainer3Arrow,
        ui_ImagePanelSettingMainContainer4Light,
        ui_ImagePanelSettingMainContainer4Arrow,
        ui_ImagePanelSettingMainContainer5About,
        ui_ImagePanelSettingMainContainer5Arrow,
        ui_ImageScreenSettingWiFiReturn,
        ui_ImageScreenSettingBLEReturn,
        ui_ImageScreenSettingVolumeReturn,
        ui_ImageScreenSettingLightReturn,
        ui_ImageScreenSettingAboutReturn,
        ui_ImageScreenSettingESPNOWReturn,
    };
    for (lv_obj_t *image : accent_images) {
        apply_icon_accent_style(image);
    }

    apply_return_button_style(ui_ButtonScreenSettingWiFiReturn, ui_ImageScreenSettingWiFiReturn);
    apply_return_button_style(ui_ButtonScreenSettingBLEReturn, ui_ImageScreenSettingBLEReturn);
    apply_return_button_style(ui_ButtonScreenSettingVolumeReturn, ui_ImageScreenSettingVolumeReturn);
    apply_return_button_style(ui_ButtonScreenSettingLightReturn, ui_ImageScreenSettingLightReturn);
    apply_return_button_style(ui_ButtonScreenSettingAboutReturn, ui_ImageScreenSettingAboutReturn);
    apply_return_button_style(ui_ButtonScreenSettingESPNOWReturn, ui_ImageScreenSettingESPNOWReturn);

    apply_toggle_style(ui_SwitchPanelScreenSettingWiFiSwitch);
    apply_toggle_style(ui_SwitchPanelScreenSettingBLESwitch);
    apply_toggle_style(ui_SwitchPanelScreenSettingESPNOWSwitch);
    apply_slider_style(ui_SliderPanelScreenSettingLightSwitch1);
    apply_slider_style(ui_SliderPanelScreenSettingVolumeSwitch);
    apply_dropdown_style(_display_timeout_dropdown);

    if (_display_theme_panel != nullptr) {
        lv_obj_set_style_bg_color(_display_theme_panel, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(_display_theme_panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (ui_TextAreaScreenSettingVerificationPassword != nullptr) {
        lv_obj_set_style_bg_color(ui_TextAreaScreenSettingVerificationPassword, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_TextAreaScreenSettingVerificationPassword, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_TextAreaScreenSettingVerificationPassword, lv_color_hex(kCalPolyBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_TextAreaScreenSettingVerificationPassword, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ui_TextAreaScreenSettingVerificationPassword, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (_panel_wifi_connect != nullptr) {
        lv_obj_set_style_bg_color(_panel_wifi_connect, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(_panel_wifi_connect, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_panel_wifi_connect, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_t *spinners[] = {
        ui_SpinnerScreenSettingWiFi,
        ui_SpinnerScreenSettingBLE,
        ui_SpinnerScreenSettingVolume,
        ui_SpinnerScreenSettingLight,
        ui_SpinnerScreenAboutList,
        ui_SpinnerScreenSettingVerification,
        _spinner_wifi_connect,
    };
    for (lv_obj_t *spinner : spinners) {
        if (spinner == nullptr) {
            continue;
        }
        lv_obj_set_style_arc_color(spinner, lv_color_hex(0xB7CCC1), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(kCalPolyGreen), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_opa(spinner, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }

    if (_device_name_editor_overlay != nullptr) {
        lv_obj_set_style_bg_color(_device_name_editor_overlay, lv_color_hex(kCalPolyText), LV_PART_MAIN | LV_STATE_DEFAULT);
        if (lv_obj_get_child_cnt(_device_name_editor_overlay) > 0) {
            lv_obj_t *dialog = lv_obj_get_child(_device_name_editor_overlay, 0);
            apply_panel_card_style(dialog, false);
        }
    }
}

void AppSettings::updateThemePreviewSelection(void)
{
    const app_theme_id_t current_theme = sanitize_theme_id(app_get_ui_theme_id());

    for (int theme_index = APP_THEME_CALPOLY; theme_index < APP_THEME_MAX; ++theme_index) {
        lv_obj_t *card = _theme_preview_cards[theme_index];
        lv_obj_t *badge = _theme_preview_badges[theme_index];
        if (card == nullptr) {
            continue;
        }

        const bool is_selected = (theme_index == current_theme);
        lv_obj_set_style_bg_color(card, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(card, lv_color_hex(is_selected ? kCalPolyGreen : kCalPolyBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(card, is_selected ? 4 : 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(card, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(card, is_selected ? 20 : 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(card, is_selected ? 10 : 4, LV_PART_MAIN | LV_STATE_DEFAULT);

        if (badge != nullptr) {
            lv_obj_set_style_bg_color(badge, lv_color_hex(kCalPolyGreen), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(badge, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_left(badge, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_right(badge, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_top(badge, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_bottom(badge, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(badge, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
            if (is_selected) {
                lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void AppSettings::showDeviceNameEditor(void)
{
    if ((_device_name_editor_overlay == nullptr) || (_device_name_editor_textarea == nullptr) || (_device_name_editor_keyboard == nullptr)) {
        return;
    }

    lv_textarea_set_text(_device_name_editor_textarea, _device_name.c_str());
    lv_textarea_set_cursor_pos(_device_name_editor_textarea, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_state(_device_name_editor_textarea, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(_device_name_editor_keyboard, _device_name_editor_textarea);
    lv_obj_clear_flag(_device_name_editor_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_device_name_editor_overlay);
}

void AppSettings::hideDeviceNameEditor(void)
{
    if (_device_name_editor_overlay != nullptr) {
        lv_obj_add_flag(_device_name_editor_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t AppSettings::initWifi()
{
    if (!ensureWifiEventGroup()) {
        return ESP_FAIL;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_CONNECTED | WIFI_EVENT_SCANING);

    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == nullptr) {
            return ESP_FAIL;
        }
    }

    wifi_mode_t current_mode = WIFI_MODE_NULL;
    err = esp_wifi_get_mode(&current_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_get_mode failed (0x%x), calling esp_wifi_init", err);
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_init failed (0x%x)", err);
            return err;
        }
    }

    if (!s_wifi_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &wifiEventHandler,
                                                  this,
                                                  &s_wifi_event_handler_any);
        if (err != ESP_OK) {
            return err;
        }

        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &wifiEventHandler,
                                                  this,
                                                  &s_ip_event_handler_got_ip);
        if (err != ESP_OK) {
            return err;
        }

        s_wifi_handlers_registered = true;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_stack_initialized = true;

    return ESP_OK;
}

void AppSettings::startWifiScan(void)
{
    if (!ensureWifiEventGroup()) {
        return;
    }

    ESP_LOGI(TAG, "Start Wi-Fi scan");
    xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_SCANING);
    // Clear the WiFi list immediately so UI resets on rescan
    deinitWifiListButton();
    if (isUiObjectValid(ui_PanelScreenSettingWiFiList)) {
        lv_obj_scroll_to_y(ui_PanelScreenSettingWiFiList, 0, LV_ANIM_OFF);
    }
    if (isUiObjectValid(_wifi_scan_dots_label)) {
        lv_label_set_text(_wifi_scan_dots_label, "Scanning.");
        lv_obj_clear_flag(_wifi_scan_dots_label, LV_OBJ_FLAG_HIDDEN);
        if (_wifi_scan_dots_timer != nullptr) lv_timer_resume(_wifi_scan_dots_timer);
    }
    if (isUiObjectValid(ui_SpinnerScreenSettingWiFi)) {
        lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    }
    if (isUiObjectValid(ui_SwitchPanelScreenSettingWiFiSwitch)) {
        lv_obj_add_flag(ui_SwitchPanelScreenSettingWiFiSwitch, LV_OBJ_FLAG_CLICKABLE);
    }
}

void AppSettings::stopWifiScan(void)
{
    if (s_wifi_event_group == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stop Wi-Fi scan");
    xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_SCANING);
    if (isUiObjectValid(ui_PanelScreenSettingWiFiList)) {
        // Keep list visible so Hidden Network is always accessible
        // lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    }
    if (isUiObjectValid(ui_SpinnerScreenSettingWiFi)) {
        lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    }
    if (isUiObjectValid(_wifi_scan_dots_label)) {
        lv_obj_add_flag(_wifi_scan_dots_label, LV_OBJ_FLAG_HIDDEN);
        if (_wifi_scan_dots_timer != nullptr) lv_timer_pause(_wifi_scan_dots_timer);
    }
    deinitWifiListButton();
}

void AppSettings::scanWifiAndUpdateUi(void)
{
    uint16_t number = WIFI_SCAN_FETCH_SIZE;
    static wifi_ap_record_t ap_info[WIFI_SCAN_FETCH_SIZE];
    static wifi_ap_record_t unique_ap_info[SCAN_LIST_SIZE];
    uint16_t ap_count = 0;
    int visible_index = 0;
    memset(ap_info, 0, sizeof(ap_info));
    memset(unique_ap_info, 0, sizeof(unique_ap_info));

    if (!s_wifi_stack_initialized || _is_ui_del || (ui_PanelScreenSettingWiFiList == nullptr) || (s_wifi_event_group == nullptr)) {
        return;
    }

    /* Block WiFi scan when ESP-NOW is connected — scan RPC causes SDIO congestion and crashes */
    if (espnow_sink_is_connected()) {
        ESP_LOGW(TAG, "WiFi scan skipped: ESP-NOW is connected (would crash SDIO)");
        stopWifiScan();
        return;
    }

    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return;
    }
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        return;
    }
    if (ap_count < number) {
        number = ap_count;
    }
    if (esp_wifi_scan_get_ap_records(&number, ap_info) != ESP_OK) {
        return;
    }
#if ENABLE_DEBUG_LOG
    ESP_LOGI(TAG, "Total APs scanned = %u", ap_count);
#endif

    bsp_display_lock(0);
    if (isUiObjectValid(ui_PanelScreenSettingWiFiList) && (getWifiEventBits() & WIFI_EVENT_SCANING)) {
        deinitWifiListButton();
    } else {
        bsp_display_unlock();
        return;
    }

    for (int i = 0; i < number; i++) {
#if ENABLE_DEBUG_LOG
        ESP_LOGI(TAG, "SSID \t\t%s", ap_info[i].ssid);
        ESP_LOGI(TAG, "RSSI \t\t%d", ap_info[i].rssi);
        ESP_LOGI(TAG, "Channel \t\t%d", ap_info[i].primary);
#endif

        if (ap_info[i].ssid[0] == '\0') {
            continue;
        }

        int duplicate_index = -1;
        for (int j = 0; j < visible_index; ++j) {
            if (wifi_ssid_matches(unique_ap_info[j].ssid, ap_info[i].ssid)) {
                duplicate_index = j;
                break;
            }
        }

        if (duplicate_index >= 0) {
            if (ap_info[i].rssi > unique_ap_info[duplicate_index].rssi) {
                unique_ap_info[duplicate_index] = ap_info[i];
            }
            continue;
        }

        if (visible_index >= SCAN_LIST_SIZE) {
            break;
        }

        unique_ap_info[visible_index] = ap_info[i];
        visible_index++;
    }

    for (int i = 0; i < visible_index; ++i) {
        const wifi_ap_record_t &record = unique_ap_info[i];
        bool psk_flag = (record.authmode != WIFI_AUTH_OPEN && record.authmode != WIFI_AUTH_OWE);
#if ENABLE_DEBUG_LOG
        ESP_LOGI(TAG, "psk_flag: %d", psk_flag);
#endif

        if(record.rssi > -100 && record.rssi <= -80) {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_WEAK;
        } else if(record.rssi > -80 && record.rssi <= -60) {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_MODERATE;
        } else if(record.rssi > -60) {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_GOOD;
        } else {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_NONE;
        }
#if ENABLE_DEBUG_LOG
        ESP_LOGI(TAG, "signal_strength: %d", _wifi_signal_strength_level);
#endif

        if (getWifiEventBits() & WIFI_EVENT_SCANING) {
            initWifiListButton(panel_wifi_btn[i], label_wifi_ssid[i], img_img_wifi_lock[i],
                                wifi_image[i], wifi_connect[i],
                                const_cast<uint8_t *>(record.ssid), psk_flag, _wifi_signal_strength_level);
        }
    }

    bsp_display_unlock();

    // Auto-reconnect to saved networks if not currently connected
    if (!(getWifiEventBits() & WIFI_EVENT_CONNECTED) && !st_auto_reconnect_in_progress) {
        // Look for a saved network among scanned networks
        for (int i = 0; i < visible_index; ++i) {
            const wifi_ap_record_t &record = unique_ap_info[i];
            char saved_password[kWifiPasswordMaxLen + 1] = {0};
            const char *ssid_str = (const char *)record.ssid;

            if (loadWifiCredential(ssid_str, saved_password, sizeof(saved_password))) {
                ESP_LOGI(TAG, "Auto-reconnect: Found saved network '%s', connecting...", ssid_str);
                st_auto_reconnect_in_progress = true;

                // Create params for background connect task
                WifiConnectParams *params = new (std::nothrow) WifiConnectParams;
                if (params != nullptr) {
                    params->app = this;
                    copy_cstr_to_buffer(params->ssid, sizeof(params->ssid), ssid_str);
                    copy_cstr_to_buffer(params->password, sizeof(params->password), saved_password);
                    params->is_auto_connect = true;
                    params->max_retries = WIFI_AUTO_CONNECT_MAX_RETRIES;

                    copy_cstr_to_buffer(st_wifi_ssid, sizeof(st_wifi_ssid), ssid_str);
                    copy_cstr_to_buffer(st_wifi_password, sizeof(st_wifi_password), saved_password);

                    // Stop scanning during connect
                    if (s_wifi_event_group != nullptr) {
                        xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_SCANING);
                    }
                    esp_wifi_scan_stop();

                    xTaskCreatePinnedToCore(wifiConnectTask, "wifi AutoConn", WIFI_CONNECT_TASK_STACK_SIZE, params,
                                            WIFI_CONNECT_TASK_PRIORITY, NULL, WIFI_CONNECT_TASK_STACK_CORE);
                } else {
                    st_auto_reconnect_in_progress = false;
                }
                break;  // Only try one network at a time
            }
        }
    }
}

void AppSettings::initWifiListButton(lv_obj_t* lv_panel_button, lv_obj_t* lv_label_ssid, lv_obj_t* lv_img_wifi_lock, lv_obj_t* lv_wifi_img,
                                     lv_obj_t *lv_wifi_connect, uint8_t* ssid, bool psk, WifiSignalStrengthLevel_t signal_strength)
{
    if (!isUiObjectValid(lv_panel_button) || !isUiObjectValid(lv_label_ssid) || !isUiObjectValid(lv_img_wifi_lock) ||
        !isUiObjectValid(lv_wifi_img) || !isUiObjectValid(lv_wifi_connect)) {
        return;
    }

    lv_obj_clear_flag(lv_panel_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(lv_label_ssid, "%s", (const char*)ssid);
    lv_obj_add_flag(lv_img_wifi_lock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_wifi_connect, LV_OBJ_FLAG_HIDDEN);

    if (strncmp((const char*)ssid, st_wifi_ssid, kWifiSsidMaxLen) == 0) {
        lv_obj_clear_flag(lv_wifi_connect, LV_OBJ_FLAG_HIDDEN);
    }

    if(psk) {
        lv_img_set_src(lv_img_wifi_lock, &img_wifi_lock);
        lv_obj_clear_flag(lv_img_wifi_lock, LV_OBJ_FLAG_HIDDEN);
    }

    if (signal_strength == WIFI_SIGNAL_STRENGTH_GOOD) {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_good);
    } else if (signal_strength == WIFI_SIGNAL_STRENGTH_MODERATE) {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_moderate);
    } else if (signal_strength == WIFI_SIGNAL_STRENGTH_WEAK) {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_wake);
    } else {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_absent);
    }
}

void AppSettings::deinitWifiListButton(void)
{
    for (int i = 0; i < SCAN_LIST_SIZE; i++) {
        if (isUiObjectValid(panel_wifi_btn[i])) {
            lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (isUiObjectValid(label_wifi_ssid[i])) {
            lv_label_set_text(label_wifi_ssid[i], "");
        }
        if (isUiObjectValid(img_img_wifi_lock[i])) {
            lv_obj_add_flag(img_img_wifi_lock[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (isUiObjectValid(wifi_connect[i])) {
            lv_obj_add_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AppSettings::euiRefresTask(void *arg)
{
    AppSettings *app = (AppSettings *)arg;
    time_t now;
    struct tm timeinfo;
    bool is_time_pm = false;
    int last_displayed_hour = -1;
    int last_displayed_min = -1;
    int last_wifi_icon_state = -1;
    uint32_t last_battery_level = UINT32_MAX;
    int last_battery_state = -1;
    uint16_t free_sram_size_kb = 0;
    uint16_t total_sram_size_kb = 0;
    uint16_t free_psram_size_kb = 0;
    uint16_t total_psram_size_kb = 0;
    bool sntp_initialized_for_connection = false;

    if (app == NULL) {
        ESP_LOGE(TAG, "App instance is NULL");
        goto err;
    }

    while (1) {
        /* Update status bar */
        // time
        time(&now);
        localtime_r(&now, &timeinfo);
        is_time_pm = (timeinfo.tm_hour >= 12);

        if ((app->status_bar != nullptr) &&
            ((timeinfo.tm_hour != last_displayed_hour) || (timeinfo.tm_min != last_displayed_min))) {
            bsp_display_lock(0);
            if(!app->status_bar->setClock(timeinfo.tm_hour, timeinfo.tm_min, is_time_pm)) {
                ESP_LOGE(TAG, "Set clock failed");
            }
            bsp_display_unlock();
            last_displayed_hour = timeinfo.tm_hour;
            last_displayed_min = timeinfo.tm_min;
        }

        extern uint32_t adc_voltage;
        extern uint32_t bat_voltage;
        extern uint32_t bat_level;
        extern uint8_t bat_state;
        extern uint8_t led_state;
        if ((app->status_bar != nullptr) &&
            ((bat_level != last_battery_level) || ((int)bat_state != last_battery_state))) {
            bsp_display_lock(0);
            if (1==bat_state) {
                if(!app->status_bar->setBatteryPercent(1, bat_level)) {
                    ESP_LOGE(TAG, "Set battery failed");
                }
            }
            else {
                if(!app->status_bar->setBatteryPercent(0, bat_level)) {
                    ESP_LOGE(TAG, "Set battery failed");
                }
            }
            bsp_display_unlock();
            last_battery_level = bat_level;
            last_battery_state = bat_state;
        }

        // Update WiFi icon state. Do not issue Wi-Fi RPCs while ESP-NOW audio
        // is connected; those share the C6/SDIO path and can disturb audio.
        int wifi_icon_state = 0;
        bool espnow_audio_active = espnow_sink_is_connected();
        if ((getWifiEventBits() & WIFI_EVENT_CONNECTED) && !espnow_audio_active) {
            if (!sntp_initialized_for_connection) {
                app_sntp_init();
                sntp_initialized_for_connection = true;
            }

            // Query actual WiFi signal strength from connected AP
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                int8_t rssi = ap_info.rssi;
                if (rssi > -60) {
                    app->_wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_GOOD;
                    wifi_icon_state = 3;
                } else if (rssi > -80) {
                    app->_wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_MODERATE;
                    wifi_icon_state = 2;
                } else if (rssi > -100) {
                    app->_wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_WEAK;
                    wifi_icon_state = 1;
                } else {
                    app->_wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_NONE;
                    wifi_icon_state = 0;
                }
            } else {
                // Fallback to stored signal strength if query fails
                if(app->_wifi_signal_strength_level == WIFI_SIGNAL_STRENGTH_GOOD) {
                    wifi_icon_state = 3;
                } else if(app->_wifi_signal_strength_level == WIFI_SIGNAL_STRENGTH_MODERATE) {
                    wifi_icon_state = 2;
                } else if(app->_wifi_signal_strength_level == WIFI_SIGNAL_STRENGTH_WEAK) {
                    wifi_icon_state = 1;
                } else {
                    wifi_icon_state = 0;
                }
            }
        } else {
            sntp_initialized_for_connection = false;
            app->_wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_NONE;
        }

        if ((app->status_bar != nullptr) && (wifi_icon_state != last_wifi_icon_state)) {
            bsp_display_lock(0);
            app->status_bar->setWifiIconState(wifi_icon_state);
            bsp_display_unlock();
            last_wifi_icon_state = wifi_icon_state;
        }

        /* Updte Smart Gadget app */
        // app->updateGadgetTime(timeinfo);

        // Update memory in backstage
        if ((app->backstage != nullptr) && app->backstage->checkVisible()) {
            free_sram_size_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
            total_sram_size_kb = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;
            free_psram_size_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
            total_psram_size_kb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;

            bsp_display_lock(0);
            if(!app->backstage->setMemoryLabel(free_sram_size_kb, total_sram_size_kb, free_psram_size_kb, total_psram_size_kb)) {
                ESP_LOGE(TAG, "Update memory usage failed");
            }
            bsp_display_unlock();
        }

        if (app->_screen_index == UI_ABOUT_SETTING_INDEX) {
            char fw_text[40] = {0};
            if (!espnow_sink_get_c6_fw_version(fw_text, sizeof(fw_text))) {
                espnow_sink_request_fw_version();
            }
            bsp_display_lock(0);
            app->refreshDeviceInfoUi();
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(HOME_REFRESH_TASK_PERIOD_MS));
    }

err:
    vTaskDelete(NULL);
}

void AppSettings::wifiScanTask(void *arg)
{
    AppSettings *app = (AppSettings *)arg;
    TickType_t last_init_retry_tick = 0;

    if (app == NULL) {
        ESP_LOGE(TAG, "App instance is NULL");
        goto err;
    }

    while (true) {
        if (!s_wifi_stack_initialized) {
            /* Hold off on esp_wifi_init until the C6 OTA + activation flow
             * has completed. The factory v2.3.0 C6 firmware deadlocks the
             * SDIO transport when WiFi RPCs and slave-OTA RPCs run
             * concurrently, which previously caused the OTA write to time
             * out at chunk #2 and left the C6 stuck on broken firmware. */
            if (!c6_init_wifi_allowed()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            const TickType_t now_tick = xTaskGetTickCount();
            if ((last_init_retry_tick == 0) || ((now_tick - last_init_retry_tick) >= pdMS_TO_TICKS(1000))) {
                esp_err_t ret = app->initWifi();
                last_init_retry_tick = now_tick;
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Wi-Fi init completed");
                    xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_INIT_DONE);
                } else if (ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "Wi-Fi init deferred: %s", esp_err_to_name(ret));
                }
            }
        }

        if (((getWifiEventBits() & WIFI_EVENT_INIT_DONE) != 0) &&
            ((getWifiEventBits() & WIFI_EVENT_UI_INIT_DONE) != 0)) {
            if (!app->_is_ui_del && (ui_SwitchPanelScreenSettingWiFiSwitch != nullptr)) {
                bsp_display_lock(0);
                if (app->isUiObjectValid(ui_SwitchPanelScreenSettingWiFiSwitch)) {
                    lv_obj_clear_flag(ui_SwitchPanelScreenSettingWiFiSwitch, LV_OBJ_FLAG_CLICKABLE);
                }
                bsp_display_unlock();
            }
            if (s_wifi_event_group != nullptr) {
                xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_INIT_DONE | WIFI_EVENT_UI_INIT_DONE);
            }
        }

        // Auto-reconnect disabled: Wi-Fi scanning changes the C6's channel
        // and breaks ESP-NOW beacon reception. ESP-NOW has radio priority.
        if (s_wifi_stack_initialized && !st_startup_auto_reconnect_done) {
            st_startup_auto_reconnect_done = true;
            ESP_LOGW(TAG, "WiFi scan/auto-reconnect skipped (ESP-NOW radio priority)");
            vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_TASK_PERIOD_MS));
            continue;
        }

        if (false) {
            // Dead code — original auto-reconnect scan kept for future OTA use
            uint16_t number = WIFI_SCAN_FETCH_SIZE;
            wifi_ap_record_t ap_info[WIFI_SCAN_FETCH_SIZE];
            uint16_t ap_count = 0;
            memset(ap_info, 0, sizeof(ap_info));

            if (espnow_sink_is_connected()) {
                ESP_LOGW(TAG, "Startup WiFi scan skipped: ESP-NOW is connected");
                st_startup_auto_reconnect_done = true;
                vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_TASK_PERIOD_MS));
                continue;
            }

            if (esp_wifi_scan_start(NULL, true) == ESP_OK &&
                esp_wifi_scan_get_ap_num(&ap_count) == ESP_OK) {
                if (ap_count > number) ap_count = number;
                if (esp_wifi_scan_get_ap_records(&number, ap_info) == ESP_OK) {
                    // Look for saved networks
                    for (int i = 0; i < number; ++i) {
                        if (ap_info[i].ssid[0] == '\0') continue;
                        char saved_password[kWifiPasswordMaxLen + 1] = {0};
                        const char *ssid_str = (const char *)ap_info[i].ssid;
                        
                        if (loadWifiCredential(ssid_str, saved_password, sizeof(saved_password))) {
                            ESP_LOGI(TAG, "Startup auto-reconnect: Found saved network '%s', connecting...", ssid_str);
                            st_auto_reconnect_in_progress = true;
                            
                            WifiConnectParams *params = new (std::nothrow) WifiConnectParams;
                            if (params != nullptr) {
                                params->app = app;
                                copy_cstr_to_buffer(params->ssid, sizeof(params->ssid), ssid_str);
                                copy_cstr_to_buffer(params->password, sizeof(params->password), saved_password);
                                params->is_auto_connect = true;
                                params->max_retries = WIFI_AUTO_CONNECT_MAX_RETRIES;
                                
                                copy_cstr_to_buffer(st_wifi_ssid, sizeof(st_wifi_ssid), ssid_str);
                                copy_cstr_to_buffer(st_wifi_password, sizeof(st_wifi_password), saved_password);
                                
                                xTaskCreatePinnedToCore(wifiConnectTask, "wifi StartupConn", WIFI_CONNECT_TASK_STACK_SIZE, params,
                                                        WIFI_CONNECT_TASK_PRIORITY, NULL, WIFI_CONNECT_TASK_STACK_CORE);
                            } else {
                                st_auto_reconnect_in_progress = false;
                            }
                            break;  // Only try one network
                        }
                    }
                }
            }
        }

        // Wi-Fi scanning disabled — ESP-NOW has radio priority
        vTaskDelay(pdMS_TO_TICKS(500));
    }

err:
    vTaskDelete(NULL);
}

void AppSettings::wifiConnectTask(void *arg)
{
    WifiConnectParams *params = (WifiConnectParams *)arg;
    AppSettings *app = params->app;
    wifi_config_t wifi_config = { 0 };
    esp_err_t ret = ESP_OK;
    bool connect_success = false;

    /* Use the credentials from the heap-allocated params struct, which are
     * immune to the disconnect event handler clearing the statics. */
    copy_cstr_to_wifi_field(wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), params->ssid);
    copy_cstr_to_wifi_field(wifi_config.sta.password, sizeof(wifi_config.sta.password), params->password);
    
    // Store auto-connect info before deleting params
    bool is_auto_connect = params->is_auto_connect;
    int max_retries = params->max_retries;
    char ssid_for_save[kWifiSsidMaxLen + 1];
    char password_for_save[kWifiPasswordMaxLen + 1];
    copy_cstr_to_buffer(ssid_for_save, sizeof(ssid_for_save), params->ssid);
    copy_cstr_to_buffer(password_for_save, sizeof(password_for_save), params->password);
    
    delete params;
    params = nullptr;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    /* Dual-band router compatibility: Use ALL_CHANNEL_SCAN to find all BSSIDs
     * (both 2.4GHz and 5GHz) before connecting. This helps when the router
     * broadcasts the same SSID on both bands. The ESP32-C6 via ESP-Hosted
     * may have issues with 5GHz, so scanning all channels first allows the
     * connection logic to try multiple BSSIDs if the first one fails. */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.channel = 0;  /* Auto-select channel */

    if (wifi_config.sta.ssid[0] == 0) {
        ESP_LOGE(TAG, "SSID is empty, aborting connect");
        if (!app->_is_ui_del) {
            bsp_display_lock(0);
            app->processWifiConnect(WIFI_CONNECT_FAIL);
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_UI_WAIT_TIME_MS));

        if (!app->_is_ui_del) {
            bsp_display_lock(0);
            app->processWifiConnect(WIFI_CONNECT_HIDE);
            if (app->isUiObjectValid(ui_TextAreaScreenSettingVerificationPassword)) {
                lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
            }
            bsp_display_unlock();
        }
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Connecting to SSID:%s (password length: %u, auto_connect: %s, max_retries: %d)",
             (const char *)wifi_config.sta.ssid,
             static_cast<unsigned>(strnlen((const char *)wifi_config.sta.password,
                                           sizeof(wifi_config.sta.password))),
             is_auto_connect ? "true" : "false",
             max_retries);

    if (!s_wifi_stack_initialized) {
        ret = app->initWifi();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to prepare Wi-Fi before connect: %s", esp_err_to_name(ret));
            if (!app->_is_ui_del) {
                bsp_display_lock(0);
                app->processWifiConnect(WIFI_CONNECT_FAIL);
                bsp_display_unlock();
            }
            vTaskDelete(NULL);
            return;
        }
    }

    if (!ensureWifiEventGroup()) {
        vTaskDelete(NULL);
        return;
    }

    // Retry loop for auto-connect
    int retry_count = is_auto_connect ? max_retries : 1;
    for (int attempt = 1; attempt <= retry_count && !connect_success; attempt++) {
        if (is_auto_connect && attempt > 1) {
            ESP_LOGI(TAG, "Auto-connect retry attempt %d/%d", attempt, retry_count);
            vTaskDelay(pdMS_TO_TICKS(WIFI_AUTO_CONNECT_RETRY_MS));
        }

        /* Use the stop → set_config → start → connect sequence required by
         * the older C6 slave firmware (ESP-Hosted Co-proc v2.3.0).
         * WARNING: esp_wifi_stop()/start() are RPCs that destroy ESP-NOW on C6.
         * The C6 bridge handler detects WIFI_EVENT_STA_STOP/START and auto-reinits
         * ESP-NOW, but there will be a brief audio interruption. */
        if (espnow_sink_is_connected()) {
            ESP_LOGW(TAG, "ESP-NOW is active — WiFi stop/start will briefly disrupt audio");
        }
        esp_wifi_disconnect();
        if (app->status_bar != nullptr) {
            app->status_bar->setWifiIconState(0);
        }
        esp_wifi_stop();
        xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_CONNECTED);

        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set Wi-Fi config: %s", esp_err_to_name(ret));
            continue;
        }
        
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(ret));
            continue;
        }
        
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to connect to Wi-Fi: %s", esp_err_to_name(ret));
            continue;
        }
        
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_EVENT_CONNECTED,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(WIFI_CONNECT_RET_WAIT_TIME_MS));
        connect_success = ((bits & WIFI_EVENT_CONNECTED) != 0);
    }

    if (connect_success) {
        ESP_LOGI(TAG, "Connected successfully");
        
        // Save the password on successful connection (for future auto-connect)
        saveWifiCredential(ssid_for_save, password_for_save);

        if (!app->_is_ui_del) {
            bsp_display_lock(0);
            app->processWifiConnect(WIFI_CONNECT_SUCCESS);
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_UI_WAIT_TIME_MS));

        if (!app->_is_ui_del) {
            bsp_display_lock(0);
            app->processWifiConnect(WIFI_CONNECT_HIDE);
            // lv_obj_clear_flag(ui_KeyboardScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
            if (app->isUiObjectValid(ui_TextAreaScreenSettingVerificationPassword)) {
                lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
            }
            app->back();
            bsp_display_unlock();
        }

        // app->updateGadgetTime(timeinfo);
    } else {
        ESP_LOGI(TAG, "Connect failed%s", is_auto_connect ? " (auto-connect exhausted, prompting for password)" : "");

        if (!app->_is_ui_del) {
            bsp_display_lock(0);
            app->processWifiConnect(WIFI_CONNECT_FAIL);
            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_UI_WAIT_TIME_MS));

        if (!app->_is_ui_del) {
            bsp_display_lock(0);
            app->processWifiConnect(WIFI_CONNECT_HIDE);
            // lv_obj_clear_flag(ui_KeyboardScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
            if (app->isUiObjectValid(ui_TextAreaScreenSettingVerificationPassword)) {
                lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
            }
            // For auto-connect failure, stay on password screen so user can enter correct password
            // app->back();
            bsp_display_unlock();
        }
    }

    // if (!app->_is_ui_del) {
    //     xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_SCANING);
    //     app->startWifiScan();
    // }

    // Reset auto-reconnect flag so scanning can trigger another attempt later
    st_auto_reconnect_in_progress = false;

    vTaskDelete(NULL);
}

void AppSettings::wifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    AppSettings *app = (AppSettings *)arg;
    if ((app == nullptr) || (s_wifi_event_group == nullptr)) {
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_CONNECTED);
        ESP_LOGI(TAG, "Got IP for SSID:%s", st_wifi_ssid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_CONNECTED);
        ESP_LOGW(TAG, "Disconnected from SSID:%s, reason:%d", st_wifi_ssid, disconnected != nullptr ? disconnected->reason : -1);
        memset(st_wifi_ssid, 0, sizeof(st_wifi_ssid));
        memset(st_wifi_password, 0, sizeof(st_wifi_password));

        // app->back();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (!app->_is_ui_del && (ui_PanelScreenSettingWiFiList != nullptr) &&
            (ui_SpinnerScreenSettingWiFi != nullptr) && (ui_SwitchPanelScreenSettingWiFiSwitch != nullptr) &&
            (getWifiEventBits() & WIFI_EVENT_SCANING)) {
            bsp_display_lock(0);
            if (app->isUiObjectValid(ui_PanelScreenSettingWiFiList) &&
                lv_obj_has_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
                if (app->isUiObjectValid(ui_SpinnerScreenSettingWiFi)) {
                    lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
                }
                if (app->isUiObjectValid(ui_SwitchPanelScreenSettingWiFiSwitch)) {
                    lv_obj_clear_flag(ui_SwitchPanelScreenSettingWiFiSwitch, LV_OBJ_FLAG_CLICKABLE);
                }
                if (app->status_bar != nullptr) {
                    app->status_bar->setWifiIconState(0);
                }
            }
            // Hide 3-dot scanning animation
            if (app->_wifi_scan_dots_label != nullptr && app->isUiObjectValid(app->_wifi_scan_dots_label)) {
                lv_obj_add_flag(app->_wifi_scan_dots_label, LV_OBJ_FLAG_HIDDEN);
            }
            if (app->_wifi_scan_dots_timer != nullptr) {
                lv_timer_pause(app->_wifi_scan_dots_timer);
            }
            bsp_display_unlock();
        }
    }
}

void AppSettings::onVerificationBackButtonClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    /* Clear pending state and navigate back to WiFi scan screen */
    memset(st_pending_ssid, 0, sizeof(st_pending_ssid));
    if (app->isUiObjectValid(ui_TextAreaScreenSettingVerificationPassword)) {
        lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
    }
    app->processWifiConnect(WIFI_CONNECT_HIDE);
    app->back();

end:
    return;
}

void AppSettings::onKeyboardScreenSettingVerificationClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);

    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    lv_keyboard_set_textarea(target, ui_TextAreaScreenSettingVerificationPassword);

    if(lv_keyboard_get_selected_btn(target) == 39) {
        /* Use st_pending_ssid (set when user tapped a WiFi network) as the
         * authoritative SSID source. Fall back to the label only if the
         * pending buffer is somehow empty. */
        const char *ssid_text = (st_pending_ssid[0] != '\0')
            ? st_pending_ssid
            : lv_label_get_text(ui_LabelScreenSettingVerificationSSID);
        const char *pwd_text = lv_textarea_get_text(ui_TextAreaScreenSettingVerificationPassword);

        ESP_LOGI(TAG, "OK pressed: pending_ssid='%s', label_ssid='%s', ssid_text='%s', pwd_len=%u",
                 st_pending_ssid,
                 lv_label_get_text(ui_LabelScreenSettingVerificationSSID),
                 ssid_text ? ssid_text : "(null)",
                 pwd_text ? (unsigned)strlen(pwd_text) : 0);

        WifiConnectParams *params = new (std::nothrow) WifiConnectParams;
        if (params == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate WifiConnectParams");
            app->processWifiConnect(WIFI_CONNECT_FAIL);
            goto end;
        }
        params->app = app;
        copy_cstr_to_buffer(params->ssid, sizeof(params->ssid), ssid_text);
        copy_cstr_to_buffer(params->password, sizeof(params->password), pwd_text);
        params->is_auto_connect = false;  // Manual password entry, no retries
        params->max_retries = 1;

        /* Update statics for UI display purposes (may be cleared by
         * disconnect handler later, but params already has the copy). */
        copy_cstr_to_buffer(st_wifi_ssid, sizeof(st_wifi_ssid), ssid_text);
        copy_cstr_to_buffer(st_wifi_password, sizeof(st_wifi_password), pwd_text);
        app->processWifiConnect(WIFI_CONNECT_RUNNING);
        // lv_obj_add_flag(ui_KeyboardScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);

        app->stopWifiScan();

        xTaskCreatePinnedToCore(wifiConnectTask, "wifi Connect", WIFI_CONNECT_TASK_STACK_SIZE, params,
                                WIFI_CONNECT_TASK_PRIORITY, NULL, WIFI_CONNECT_TASK_STACK_CORE);
    }

end:
    return;
}

void AppSettings::onScreenLoadEventCallback( lv_event_t * e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    SettingScreenIndex_t last_scr_index = UI_MAIN_SETTING_INDEX;
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");
    last_scr_index = app->_screen_index;

    for (int i = 0; i < UI_MAX_INDEX; i++) {
        if (app->_screen_list[i] == lv_event_get_target(e)) {
            app->_screen_index = (SettingScreenIndex_t)i;
            break;
        }
    }

    if (last_scr_index == UI_WIFI_SCAN_INDEX) {
        app->stopWifiScan();
    }

    if ((app->_screen_index == UI_WIFI_SCAN_INDEX) && (app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] == true)) {
        app->startWifiScan();
    }

    if (app->_screen_index == UI_ABOUT_SETTING_INDEX) {
        espnow_sink_request_fw_version();
        app->refreshDeviceInfoUi();
    }

    if (app->_screen_index == UI_ESPNOW_SETTING_INDEX) {
        app->updateEspnowUiState();
    }

end:
    return;
}

void AppSettings::onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback( lv_event_t * e) {
    lv_state_t state = lv_obj_get_state(ui_SwitchPanelScreenSettingWiFiSwitch);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (state & LV_STATE_CHECKED) {
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = true;
        app->setNvsParam(NVS_KEY_WIFI_ENABLE, 1);
        if (app->_hidden_network_btn != nullptr) {
            lv_obj_clear_flag(app->_hidden_network_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (app->_screen_index == UI_WIFI_SCAN_INDEX) {
            app->startWifiScan();
        }
    } else {
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
        app->setNvsParam(NVS_KEY_WIFI_ENABLE, 0);
        if (app->_hidden_network_btn != nullptr) {
            lv_obj_add_flag(app->_hidden_network_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (app->_screen_index == UI_WIFI_SCAN_INDEX) {
            app->stopWifiScan();
            if (getWifiEventBits() & WIFI_EVENT_CONNECTED) {
                ESP_ERROR_CHECK(esp_wifi_disconnect());
                if (app->status_bar != nullptr) {
                    app->status_bar->setWifiIconState(0);
                }
            }
        }
    }

end:
    return;
}

void AppSettings::onButtonWifiListClickedEventCallback(lv_event_t * e)
{
    lv_obj_t *label_wifi_ssid = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    lv_area_t btn_click_area;
    lv_point_t point;
    lv_indev_t *active_indev = nullptr;

    if ((label_wifi_ssid == nullptr) || (ui_PanelScreenSettingWiFiList == nullptr) ||
        lv_obj_is_scrolling(ui_PanelScreenSettingWiFiList)) {
        return;
    }

    active_indev = lv_indev_get_act();
    if (active_indev == nullptr) {
        return;
    }

    lv_obj_get_click_area(btn, &btn_click_area);
    lv_indev_get_point(active_indev, &point);
    if ((point.x < btn_click_area.x1) || (point.x > btn_click_area.x2) ||
        (point.y < btn_click_area.y1) || (point.y > btn_click_area.y2)) {
        return;
    }

    /* Capture the SSID text BEFORE lv_scr_load, because loading the
     * verification screen fires LV_EVENT_SCREEN_LOADED which triggers
     * stopWifiScan() -> deinitWifiListButton() that clears every label. */
    copy_cstr_to_buffer(st_pending_ssid, sizeof(st_pending_ssid), lv_label_get_text(label_wifi_ssid));
    ESP_LOGI(TAG, "WiFi selected: SSID='%s'", st_pending_ssid);

    if (s_wifi_event_group != nullptr) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_SCANING);
    }
    esp_wifi_scan_stop();

    // Check for saved password
    char saved_password[kWifiPasswordMaxLen + 1] = {0};
    if (loadWifiCredential(st_pending_ssid, saved_password, sizeof(saved_password)) && st_wifi_list_app != nullptr) {
        // Saved password found - attempt auto-connect
        ESP_LOGI(TAG, "Found saved password for '%s', attempting auto-connect", st_pending_ssid);

        // Load verification screen to show connection status
        lv_scr_load(ui_ScreenSettingVerification);
        lv_label_set_text_fmt(ui_LabelScreenSettingVerificationSSID, "%s", st_pending_ssid);
        if (ui_TextAreaScreenSettingVerificationPassword != nullptr) {
            lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
        }
        if (st_wifi_list_app != nullptr && st_wifi_list_app->_verify_pwd_eye_label != nullptr) {
            lv_label_set_text(st_wifi_list_app->_verify_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
        }

        // Start auto-connect task with saved password
        WifiConnectParams *params = new (std::nothrow) WifiConnectParams;
        if (params != nullptr) {
            params->app = st_wifi_list_app;
            copy_cstr_to_buffer(params->ssid, sizeof(params->ssid), st_pending_ssid);
            copy_cstr_to_buffer(params->password, sizeof(params->password), saved_password);
            params->is_auto_connect = true;
            params->max_retries = WIFI_AUTO_CONNECT_MAX_RETRIES;

            copy_cstr_to_buffer(st_wifi_ssid, sizeof(st_wifi_ssid), st_pending_ssid);
            copy_cstr_to_buffer(st_wifi_password, sizeof(st_wifi_password), saved_password);
            st_wifi_list_app->processWifiConnect(WIFI_CONNECT_RUNNING);
            st_wifi_list_app->stopWifiScan();

            xTaskCreatePinnedToCore(wifiConnectTask, "wifi Connect", WIFI_CONNECT_TASK_STACK_SIZE, params,
                                    WIFI_CONNECT_TASK_PRIORITY, NULL, WIFI_CONNECT_TASK_STACK_CORE);
        }
    } else {
        // No saved password - show password entry screen
        lv_scr_load(ui_ScreenSettingVerification);
        lv_label_set_text_fmt(ui_LabelScreenSettingVerificationSSID, "%s", st_pending_ssid);
        if (ui_TextAreaScreenSettingVerificationPassword != nullptr) {
            lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
            lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
        }
        if (st_wifi_list_app != nullptr && st_wifi_list_app->_verify_pwd_eye_label != nullptr) {
            lv_label_set_text(st_wifi_list_app->_verify_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
        }
    }
}

void AppSettings::onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback( lv_event_t * e) {
    lv_state_t state = lv_obj_get_state(ui_SwitchPanelScreenSettingBLESwitch);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (state & LV_STATE_CHECKED) {
        app->_nvs_param_map[NVS_KEY_BLE_ENABLE] = true;
        app->setNvsParam(NVS_KEY_BLE_ENABLE, 1);
    } else {
        app->_nvs_param_map[NVS_KEY_BLE_ENABLE] = false;
        app->setNvsParam(NVS_KEY_BLE_ENABLE, 0);
    }

end:
    return;
}

void AppSettings::onSliderPanelVolumeSwitchValueChangeEventCallback( lv_event_t * e) {
    lv_obj_t *slider = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    int volume = lv_slider_get_value(slider);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (volume != app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME]) {
        if ((bsp_extra_codec_volume_set(volume, NULL) != ESP_OK) && (bsp_extra_codec_volume_get() != volume)) {
            ESP_LOGE(TAG, "Set volume failed");
            lv_slider_set_value(ui_SliderPanelScreenSettingVolumeSwitch, app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
            return;
        }
        espnow_sink_set_output_volume(volume);
        app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME] = volume;
        if (app->_espnow_volume_value_label != nullptr && app->isUiObjectValid(app->_espnow_volume_value_label)) {
            lv_label_set_text_fmt(app->_espnow_volume_value_label, "%d%%", volume);
        }
        if (slider != ui_SliderPanelScreenSettingVolumeSwitch &&
            app->isUiObjectValid(ui_SliderPanelScreenSettingVolumeSwitch)) {
            lv_slider_set_value(ui_SliderPanelScreenSettingVolumeSwitch, volume, LV_ANIM_OFF);
        }
        if (slider == ui_SliderPanelScreenSettingVolumeSwitch &&
            app->_espnow_volume_slider != nullptr && app->isUiObjectValid(app->_espnow_volume_slider)) {
            lv_slider_set_value(app->_espnow_volume_slider, volume, LV_ANIM_OFF);
        }
    }
    if (code == LV_EVENT_RELEASED) {
        app->setNvsParam(NVS_KEY_AUDIO_VOLUME, app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    }

end:
    return;
}

void AppSettings::onDropdownAudioOutputValueChangeEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    {
        app->_nvs_param_map[NVS_KEY_AUDIO_OUTPUT] = AUDIO_OUTPUT_AUX;
        bsp_extra_output_route_set(BSP_EXTRA_AUDIO_OUTPUT_AUX);
        app->setNvsParam(NVS_KEY_AUDIO_OUTPUT, AUDIO_OUTPUT_AUX);

        if (app->_audio_output_dropdown != nullptr && app->isUiObjectValid(app->_audio_output_dropdown)) {
            lv_dropdown_set_selected(app->_audio_output_dropdown, AUDIO_OUTPUT_AUX);
        }
        if (app->_espnow_output_dropdown != nullptr && app->isUiObjectValid(app->_espnow_output_dropdown)) {
            lv_dropdown_set_selected(app->_espnow_output_dropdown, 0);
        }
    }

end:
    return;
}

void AppSettings::onSliderPanelLightSwitchValueChangeEventCallback( lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    brightness = lv_slider_get_value(ui_SliderPanelScreenSettingLightSwitch1);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (brightness != app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS]) {
        // if ((bsp_display_brightness_set(brightness) != ESP_OK) && (bsp_display_brightness_get() != brightness)) {
        if (bsp_display_brightness_set(brightness) != ESP_OK) {
            ESP_LOGE(TAG, "Set brightness failed");
            lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], LV_ANIM_OFF);
            return;
        }
        app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
    }
    if (code == LV_EVENT_RELEASED) {
        app->setNvsParam(NVS_KEY_DISPLAY_BRIGHTNESS, app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS]);
    }

end:
    return;
}

void AppSettings::onDropdownScreenTimeoutValueChangeEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    int timeout_seconds = index_to_screen_timeout(lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e)));
    if (timeout_seconds != app->_nvs_param_map[NVS_KEY_DISPLAY_TIMEOUT]) {
        app->_nvs_param_map[NVS_KEY_DISPLAY_TIMEOUT] = timeout_seconds;
        app->setNvsParam(NVS_KEY_DISPLAY_TIMEOUT, timeout_seconds);
        app_set_screen_timeout_seconds(timeout_seconds);
    }
}

void AppSettings::onThemePreviewCardClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    lv_obj_t *target = lv_event_get_current_target(e);
    for (int theme_index = APP_THEME_CALPOLY; theme_index < APP_THEME_MAX; ++theme_index) {
        if (app->_theme_preview_cards[theme_index] == target) {
            app_set_ui_theme_id(theme_index);
            app->applyThemeToSettingScreens();
            app->updateThemePreviewSelection();
            return;
        }
    }
}

void AppSettings::onDeviceNamePanelClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    app->showDeviceNameEditor();
}

void AppSettings::onDeviceNameKeyboardEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        app->_device_name = sanitize_device_name(lv_textarea_get_text(app->_device_name_editor_textarea));
        app->setNvsStringParam(NVS_KEY_DEVICE_NAME, app->_device_name);
        app->refreshDeviceInfoUi();
    }

    app->hideDeviceNameEditor();
}

void AppSettings::onHiddenNetworkBtnClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr || app->_hidden_network_overlay == nullptr) {
        return;
    }

    // Show hidden network dialog
    lv_obj_clear_flag(app->_hidden_network_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(app->_hidden_network_overlay);
    // Clear previous input
    if (app->_hidden_network_ssid_textarea != nullptr) {
        lv_textarea_set_text(app->_hidden_network_ssid_textarea, "");
    }
    if (app->_hidden_network_password_textarea != nullptr) {
        lv_textarea_set_text(app->_hidden_network_password_textarea, "");
        lv_textarea_set_password_mode(app->_hidden_network_password_textarea, true);
    }
    // Reset eye icon to closed
    if (app->_hidden_pwd_eye_label != nullptr) {
        lv_label_set_text(app->_hidden_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    }
    // Reset card size and position
    if (app->_hidden_network_card != nullptr) {
        lv_obj_set_height(app->_hidden_network_card, 400);
        lv_obj_align(app->_hidden_network_card, LV_ALIGN_TOP_MID, 0, 10);
    }
    // Hide keyboard initially
    if (app->_hidden_network_keyboard != nullptr) {
        lv_obj_add_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::onHiddenNetworkConnectClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }

    const char *ssid = lv_textarea_get_text(app->_hidden_network_ssid_textarea);
    const char *password = lv_textarea_get_text(app->_hidden_network_password_textarea);

    if (ssid == nullptr || ssid[0] == '\0') {
        ESP_LOGW(TAG, "Hidden network SSID is empty");
        return;
    }

    ESP_LOGI(TAG, "Connecting to hidden network: %s", ssid);

    // Hide keyboard and dialog
    if (app->_hidden_network_keyboard != nullptr) {
        lv_obj_add_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (app->_hidden_network_card != nullptr) {
        lv_obj_set_height(app->_hidden_network_card, 400);
        lv_obj_center(app->_hidden_network_card);
    }
    if (app->_hidden_network_overlay != nullptr) {
        lv_obj_add_flag(app->_hidden_network_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    // Store SSID and password
    copy_cstr_to_buffer(st_wifi_ssid, sizeof(st_wifi_ssid), ssid);
    copy_cstr_to_buffer(st_wifi_password, sizeof(st_wifi_password), password);
    copy_cstr_to_buffer(st_pending_ssid, sizeof(st_pending_ssid), ssid);

    // Navigate to verification screen to show connection progress
    lv_scr_load(ui_ScreenSettingVerification);
    lv_label_set_text_fmt(ui_LabelScreenSettingVerificationSSID, "%s", ssid);
    if (ui_TextAreaScreenSettingVerificationPassword != nullptr) {
        lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
    }
    if (app->_verify_pwd_eye_label != nullptr) {
        lv_label_set_text(app->_verify_pwd_eye_label, LV_SYMBOL_EYE_CLOSE);
    }

    // Stop WiFi scanning
    if (s_wifi_event_group != nullptr) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_SCANING);
    }
    esp_wifi_scan_stop();

    // Create connection task
    WifiConnectParams *params = new (std::nothrow) WifiConnectParams;
    if (params != nullptr) {
        params->app = app;
        copy_cstr_to_buffer(params->ssid, sizeof(params->ssid), ssid);
        copy_cstr_to_buffer(params->password, sizeof(params->password), password);
        params->is_auto_connect = false;
        params->max_retries = 1;

        app->processWifiConnect(WIFI_CONNECT_RUNNING);
        app->stopWifiScan();

        xTaskCreatePinnedToCore(wifiConnectTask, "wifi HiddenNet", WIFI_CONNECT_TASK_STACK_SIZE, params,
                                WIFI_CONNECT_TASK_PRIORITY, NULL, WIFI_CONNECT_TASK_STACK_CORE);
    }
}

void AppSettings::onHiddenNetworkCancelClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr || app->_hidden_network_overlay == nullptr) {
        return;
    }

    // Hide dialog and reset state
    if (app->_hidden_network_keyboard != nullptr) {
        lv_obj_add_flag(app->_hidden_network_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (app->_hidden_network_card != nullptr) {
        lv_obj_set_height(app->_hidden_network_card, 400);
        lv_obj_center(app->_hidden_network_card);
    }
    lv_obj_add_flag(app->_hidden_network_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ==================== ESP-NOW Audio Callbacks ====================

void AppSettings::onESPNOWSwitchValueChangeEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    lv_state_t state = lv_obj_get_state(ui_SwitchPanelScreenSettingESPNOWSwitch);
    bool enable = (state & LV_STATE_CHECKED);

    if (enable) {
        /* User just turned ON Assistive Listening. Bring the C6 radio up
         * and start a continuous scan. We deliberately disable the
         * autoscan auto-join behaviour: the user must pick a room from
         * the list. Drop-recovery for an explicitly-picked room still
         * works because join_room() seeds the autoscan target cache. */
        espnow_sink_set_autoscan_autojoin(false);
        espnow_sink_set_autoscan(true);
        app->showEspnowScanningUi();
        /* Create UI-update task (room list + scan animation). It does not
         * call any espnow_sink_* lifecycle functions — only read-only
         * queries (get_rooms, is_connected, get_state). */
        if (app->_espnow_scan_task == nullptr) {
            xTaskCreatePinnedToCore(espnowScanTask, "espnow_scan", 4096, app, 2, &app->_espnow_scan_task, 0);
        }
    } else {
        /* User turned OFF Assistive Listening. Tear down BOTH UI tasks
         * before disabling the sink: otherwise the stats task races and
         * overwrites our "Assistive Audio Off" label with the spurious
         * "Connection lost. Searching..." message. */
        if (app->_espnow_scan_task != nullptr) {
            vTaskDelete(app->_espnow_scan_task);
            app->_espnow_scan_task = nullptr;
        }
        if (app->_espnow_stats_task != nullptr) {
            vTaskDelete(app->_espnow_stats_task);
            app->_espnow_stats_task = nullptr;
        }
        espnow_sink_set_autoscan(false);
        espnow_sink_set_autoscan_autojoin(false);
        espnow_sink_leave_room();
        espnow_sink_disable();
        app->_espnow_selected_room = -1;
        lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Assistive Listening Off");
        lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(ui_PanelScreenSettingESPNOWList);
    }
}

void AppSettings::onESPNOWDisconnectButtonClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    bool c6_alive = espnow_sink_leave_room();
    app->_espnow_selected_room = -1;
    lv_obj_add_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(ui_PanelScreenSettingESPNOWList);
    
    if (c6_alive) {
        lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Scanning For Room...");
        lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
        espnow_sink_start_scan();
        if (app->_espnow_scan_task == nullptr) {
            xTaskCreatePinnedToCore(espnowScanTask, "espnow_scan", 4096, app, 2, &app->_espnow_scan_task, 0);
        }
    } else {
        lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "C6 unresponsive - please restart device");
        lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::onESPNOWRoomButtonClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }

    // Get room index from user data (cast from int via void*)
    int room_index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    
    espnow_sink_stop_scan();
    esp_err_t ret = espnow_sink_join_room(room_index);
    if (ret == ESP_OK) {
        if (app->_espnow_scan_task != nullptr) {
            vTaskDelete(app->_espnow_scan_task);
            app->_espnow_scan_task = nullptr;
        }
        app->_espnow_selected_room = room_index;

        espnow_room_info_t rooms[ESPNOW_SINK_MAX_ROOMS];
        int count = espnow_sink_get_rooms(rooms, ESPNOW_SINK_MAX_ROOMS);
        char room_text[32] = "Unknown";
        if (room_index >= 0 && room_index < count) {
            formatEspnowRoomName(rooms[room_index], room_text, sizeof(room_text));
        }
        lv_label_set_text_fmt(ui_LabelScreenSettingESPNOWStatus, "Connecting To Room: %s", room_text);
        lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
        if (app->_espnow_selected_room_label != nullptr) {
            lv_label_set_text_fmt(app->_espnow_selected_room_label, "Room: %s", room_text);
        }

    } else {
        if (ret == ESP_ERR_INVALID_ARG) {
            lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Room no longer available. Refreshing...");
        } else {
            lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Failed to connect");
        }
        if (!espnow_sink_is_connected()) {
            espnow_sink_start_scan();
        }
    }
}

void AppSettings::espnowScanTask(void *arg)
{
    AppSettings *app = (AppSettings *)arg;
    espnow_room_info_t rooms[ESPNOW_SINK_MAX_ROOMS];
    int scan_dots_phase = 0;
    char last_room_signature[192] = "";
    int last_count = -1;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // Update every second
        
        if (!app->isUiAlive()) {
            ESP_LOGW(TAG, "espnowScanTask: UI destroyed, exiting");
            app->_espnow_scan_task = nullptr;
            vTaskDelete(NULL);
            return;
        }

        if (!espnow_sink_is_connected()) {
            int count = espnow_sink_get_rooms(rooms, ESPNOW_SINK_MAX_ROOMS);
            
            bsp_display_lock(0);
            if (!app->isUiAlive()) {
                bsp_display_unlock();
                app->_espnow_scan_task = nullptr;
                vTaskDelete(NULL);
                return;
            }

            if (count > 0) {
                char room_signature[192] = "";
                size_t used = 0;
                for (int i = 0; i < count && used < sizeof(room_signature); i++) {
                    char room_text[32];
                    formatEspnowRoomName(rooms[i], room_text, sizeof(room_text));
                    int written = snprintf(room_signature + used, sizeof(room_signature) - used,
                                           "%s:%d|", room_text, rooms[i].rssi);
                    if (written < 0) {
                        break;
                    }
                    used += (size_t)written;
                }
                if (count == last_count && strcmp(room_signature, last_room_signature) == 0) {
                    bsp_display_unlock();
                    continue;
                }
                last_count = count;
                snprintf(last_room_signature, sizeof(last_room_signature), "%s", room_signature);

                lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text_fmt(ui_LabelScreenSettingESPNOWStatus, "Found %d Room%s", count, (count == 1) ? "" : "s");
                
                // Update room list
                lv_obj_clean(ui_PanelScreenSettingESPNOWList);
                for (int i = 0; i < count; i++) {
                    // Create room button
                    lv_obj_t *btn = lv_btn_create(ui_PanelScreenSettingESPNOWList);
                    lv_obj_set_width(btn, lv_pct(95));
                    lv_obj_set_height(btn, 70);
                    lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                    lv_obj_add_event_cb(btn, onESPNOWRoomButtonClickedEventCallback, LV_EVENT_CLICKED, app);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0xCBCBCB), LV_PART_MAIN | LV_STATE_PRESSED);
                    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
                    
                    // Room label with name and RSSI. Prefer the broadcaster's
                    // human-readable name (ECast beacon) when available; fall
                    // back to the room code for legacy / pre-ECast sources.
                    lv_obj_t *label = lv_label_create(btn);
                    char room_text[32];
                    formatEspnowRoomName(rooms[i], room_text, sizeof(room_text));
                    lv_label_set_text_fmt(label, "%s  (RSSI: %d dBm)", room_text, rooms[i].rssi);
                    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_text_color(label, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_center(label);
                }
            } else {
                last_count = 0;
                last_room_signature[0] = '\0';
                lv_obj_add_flag(ui_SpinnerScreenSettingESPNOW, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
                if (scan_dots_phase == 0) {
                    lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Scanning For Room.");
                } else if (scan_dots_phase == 1) {
                    lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Scanning For Room..");
                } else {
                    lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Scanning For Room...");
                }
                scan_dots_phase = (scan_dots_phase + 1) % 3;
            }
            bsp_display_unlock();
        }
    }
}

void AppSettings::espnowStatsTask(void *arg)
{
    AppSettings *app = (AppSettings *)arg;
    int not_connected_ticks = 0;
    bool connected_ui_shown = false;
    char last_room_name[32] = "";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (!app->isUiAlive()) {
            app->_espnow_stats_task = nullptr;
            vTaskDelete(NULL);
            return;
        }

        if (espnow_sink_is_connected()) {
            not_connected_ticks = 0;
            espnow_room_info_t room = {};
            char room_name[32] = "Unknown";
            if (espnow_sink_get_connected_room(&room)) {
                formatEspnowRoomName(room, room_name, sizeof(room_name));
            }
            if (connected_ui_shown && strcmp(room_name, last_room_name) == 0) {
                continue;
            }
            bsp_display_lock(0);
            if (app->isUiAlive()) {
                app->showEspnowConnectedUi(room_name);
            }
            bsp_display_unlock();
            connected_ui_shown = true;
            snprintf(last_room_name, sizeof(last_room_name), "%s", room_name);
            continue;
        }

        if (app->_espnow_selected_room < 0) {
            app->_espnow_stats_task = nullptr;
            vTaskDelete(NULL);
            return;
        }

        not_connected_ticks++;
        if (not_connected_ticks >= 3) {
            connected_ui_shown = false;
            last_room_name[0] = '\0';
            bsp_display_lock(0);
            if (!app->isUiAlive()) {
                bsp_display_unlock();
                app->_espnow_stats_task = nullptr;
                vTaskDelete(NULL);
                return;
            }
            lv_obj_add_flag(ui_PanelScreenSettingESPNOWStats, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui_PanelScreenSettingESPNOWList, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(ui_LabelScreenSettingESPNOWStatus, "Reconnecting To Room...");
            bsp_display_unlock();
            espnow_sink_start_scan();
            if (app->_espnow_scan_task == nullptr) {
                xTaskCreatePinnedToCore(espnowScanTask, "espnow_scan", 4096, app, 2, &app->_espnow_scan_task, 0);
            }
            not_connected_ticks = 0;
        }
    }
}
