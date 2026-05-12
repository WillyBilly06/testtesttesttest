#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "esp_brookesia.hpp"
#include "apps.h"
#include "../components/apps/app_theme.h"
#include "../components/espressif__esp32_p4_function_ev_board/elecrow_ui/include/elecrow_ui.h"
#include "../components/espressif__esp32_p4_function_ev_board/bsp_stc8h1kxx.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"

#include "ota_slave.h"
#include "espnow_sink.h"
#include "esp_hosted.h"

extern "C" uint8_t is_transport_tx_ready(void);

static const char *TAG = "main";
static const char *NVS_STORAGE_NAMESPACE = "storage";
static const char *NVS_KEY_DISPLAY_TIMEOUT = "disp_timeout";
static const char *NVS_KEY_UI_THEME = "ui_theme";
static constexpr int32_t SCREEN_TIMEOUT_DISABLED_S = 0;
static constexpr int32_t DEFAULT_UI_THEME = APP_THEME_CALPOLY;
static constexpr int STATUS_BAR_LOGO_ID = 0x4350;
static constexpr uint32_t CALPOLY_HOME_BG_COLOR = 0xF7FAF7;

LV_IMG_DECLARE(cp_logo_rev_home);
LV_IMG_DECLARE(theme_bg_topography_1024x600);
LV_IMG_DECLARE(theme_bg_vista_1024x600);

static TaskHandle_t battery_info_task_handle = NULL;     
static bool s_battery_monitor_ready = false;
uint32_t adc_voltage;
uint32_t bat_voltage;
uint32_t bat_level;
uint8_t bat_state;
uint8_t led_state;
static ESP_Brookesia_Phone *s_phone = nullptr;
static ESP_Brookesia_PhoneStylesheet_t *s_phone_stylesheet = nullptr;
static volatile int32_t s_ui_theme_id = DEFAULT_UI_THEME;
static bool s_home_logo_added = false;
static bool s_post_ota_c6_delay_required = false;
static bool s_c6_init_task_started = false;
static volatile bool s_c6_init_complete = false;

/*
 * Gate flag for the WiFi-Scan task in components/apps/setting.
 *
 * The factory C6 firmware (esp_hosted v2.3.0) has SDIO transport bugs:
 * concurrent WiFi RPCs (e.g. esp_wifi_init -> Req_GetMACAddress) and
 * slave-OTA RPCs (Req_OTAWrite) on the same SDIO bus deadlock the
 * transport, causing OTA write timeouts and a permanent WiFi failure
 * at ~4 minutes uptime.
 *
 * Returning false from this hook keeps wifiScanTask from calling
 * esp_wifi_init / esp_wifi_start until after the C6 OTA + activation
 * flow has finished and the C6 has rebooted into the new (fixed) image.
 */
extern "C" bool c6_init_wifi_allowed(void)
{
    return s_c6_init_complete;
}

static void start_c6_init_task(void)
{
    if (s_c6_init_task_started) {
        return;
    }
    s_c6_init_task_started = true;

    // Launch C6 initialization in background task so display isn't blocked.
    // IMPORTANT: Do not initialize espnow_sink before OTA slave checks, because both
    // paths use ESP-Hosted internals and early concurrent init can trip hosted memcpy asserts.
    xTaskCreate([](void *) {
        bool sink_ready = false;

        /* NOTE: the previous "fast-path" that called espnow_sink_init() and
         * espnow_sink_request_status() before the OTA flow has been removed.
         * It fired RPCs into the SDIO custom-data channel before the C6 had
         * sent its Coprocessor Boot-up event, leaving timed-out async-RPC
         * state behind. When the C6 later booted up, the host's RPC layer
         * tried to deliver the failure callback via a stale function
         * pointer and panic'd at MEPC=0x48 (NULL deref). Wait for the OTA
         * workflow to finish first. */

        if (s_post_ota_c6_delay_required) {
            ESP_LOGW("c6_init", "Waiting 8s for C6 OTA validation window before OTA checks...");
            vTaskDelay(pdMS_TO_TICKS(8000));
            ESP_LOGI("c6_init", "Post-OTA delay complete");
        }

        // Step 1: Check version & OTA if needed
#if CONFIG_OTA_SLAVE_FORCE_FLASH
        ESP_LOGW("c6_init", "CONFIG_OTA_SLAVE_FORCE_FLASH is set — forcing C6 re-flash");
        esp_err_t ota_ret = ota_slave_force_flash();
        if (ota_ret != ESP_OK) {
            ESP_LOGE("c6_init", "Force-flash failed: %s", esp_err_to_name(ota_ret));
        }
#else
        esp_err_t ota_ret = ota_slave_flash_if_needed();
        if (ota_ret != ESP_OK) {
            ESP_LOGW("c6_init", "Slave OTA check returned: %s (continuing with existing firmware)",
                     esp_err_to_name(ota_ret));
        }
#endif

        // Step 2: Activate new firmware if OTA was performed
        if (ota_ret == ESP_OK && ota_slave_transfer_completed()) {
            esp_err_t act_ret = ota_slave_activate_if_supported();
            if (act_ret != ESP_OK) {
                ESP_LOGE("c6_init", "Slave activation failed: %s - C6 may need manual restart",
                         esp_err_to_name(act_ret));
            }
        }

        // Ensure sink is initialized after OTA workflow if fast path did not run.
        if (ota_slave_espnow_supported()) {
            if (!sink_ready) {
                esp_err_t espnow_ret = espnow_sink_init(NULL);
                if (espnow_ret == ESP_OK) {
                    sink_ready = true;
                } else {
                    ESP_LOGE("c6_init", "ESP-NOW sink init failed: %s", esp_err_to_name(espnow_ret));
                }
            }

            if (sink_ready) {
                espnow_sink_request_status();
                espnow_sink_request_fw_version();
                /* NOTE: do NOT auto-start scanning at boot.
                 * The user controls ESP-NOW via Settings → Assistive
                 * Listening switch. Toggling it ON brings the C6 radio up
                 * and starts a scan; the room list is shown for the user
                 * to pick from. Toggling it OFF tears the radio down and
                 * stops all ESP-NOW activity. */
            }
        } else {
            ESP_LOGW("c6_init", "ESP-NOW disabled: C6 firmware version mismatch (OTA pending or failed)");
        }

        /* Release the WiFi-Scan task so it can finally call esp_wifi_init.
         * We do this regardless of OTA outcome — if OTA failed, the C6 is
         * still on the broken v2.3.0 firmware and WiFi will eventually die,
         * but at least the user can re-attempt OTA on the next boot
         * without the SDIO transport already being deadlocked.   */
        s_c6_init_complete = true;
        ESP_LOGI("c6_init", "C6 init flow complete — WiFi stack is now allowed to start");

        vTaskDelete(NULL);
    }, "c6_init", 8192, NULL, 5, NULL);
}

/*
    The task to get battery information from stc8h1kxx via i2c
*/
void battery_info_task(void *param)
{
    while (1)
    {
        Battery_info_t battery_info = {0};
        if (!s_battery_monitor_ready || (stc8_battery_info_get(&battery_info) != ESP_OK)) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        adc_voltage = battery_info.adc_voltage;
        bat_voltage = battery_info.bat_voltage;
        bat_level   = battery_info.bat_level;
        bat_state   = battery_info.bat_state;
        led_state   = battery_info.led_state;
        // ESP_LOGI(TAG, "adc_voltage = %lu mV", battery_info.adc_voltage);
        // ESP_LOGI(TAG, "bat_voltage = %lu mV", battery_info.bat_voltage);
        // ESP_LOGI(TAG, "bat_level = %d %%", battery_info.bat_level);
        // ESP_LOGI(TAG, "bat_state = %d", battery_info.bat_state);
        // ESP_LOGI(TAG, "led_state = %d", battery_info.led_state);
        if ((battery_info.bat_voltage > 0) && (battery_info.bat_voltage <= 3500)) {
            ESP_LOGI(TAG, "esp_deep_sleep_start()");
            vTaskDelay(100 / portTICK_PERIOD_MS);
            esp_deep_sleep_start();
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

extern esp_lcd_touch_handle_t tp;
static int s_prev_brightness = 0;
static bool s_enter_sleep_flag = false;
static bool s_display_ready = false;
static volatile uint32_t s_last_interaction_time_s = 0;
static volatile int32_t s_screen_timeout_s = SCREEN_TIMEOUT_DISABLED_S;

static uint32_t get_uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000 / 1000);
}

static int32_t sanitize_ui_theme_id(int32_t theme_id)
{
    if ((theme_id < APP_THEME_CALPOLY) || (theme_id >= APP_THEME_MAX)) {
        return DEFAULT_UI_THEME;
    }

    return theme_id;
}

static int32_t load_screen_timeout_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    int32_t timeout_s = SCREEN_TIMEOUT_DISABLED_S;

    esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open NVS for screen timeout: %s", esp_err_to_name(err));
        return SCREEN_TIMEOUT_DISABLED_S;
    }

    err = nvs_get_i32(nvs_handle, NVS_KEY_DISPLAY_TIMEOUT, &timeout_s);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        timeout_s = SCREEN_TIMEOUT_DISABLED_S;
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, NVS_KEY_DISPLAY_TIMEOUT, timeout_s));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to load screen timeout from NVS: %s", esp_err_to_name(err));
        timeout_s = SCREEN_TIMEOUT_DISABLED_S;
    }

    nvs_close(nvs_handle);

    return (timeout_s > 0) ? timeout_s : SCREEN_TIMEOUT_DISABLED_S;
}

static int32_t load_ui_theme_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    int32_t theme_id = DEFAULT_UI_THEME;

    esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open NVS for UI theme: %s", esp_err_to_name(err));
        return DEFAULT_UI_THEME;
    }

    err = nvs_get_i32(nvs_handle, NVS_KEY_UI_THEME, &theme_id);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        theme_id = DEFAULT_UI_THEME;
        ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, NVS_KEY_UI_THEME, theme_id));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to load UI theme from NVS: %s", esp_err_to_name(err));
        theme_id = DEFAULT_UI_THEME;
    }

    nvs_close(nvs_handle);

    return sanitize_ui_theme_id(theme_id);
}

static void save_ui_theme_to_nvs(int32_t theme_id)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open NVS to save UI theme: %s", esp_err_to_name(err));
        return;
    }

    theme_id = sanitize_ui_theme_id(theme_id);
    err = nvs_set_i32(nvs_handle, NVS_KEY_UI_THEME, theme_id);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to save UI theme to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

typedef struct {
    uint32_t background_color;
    uint32_t container_outline_colors[6];
    uint32_t indicator_active_color;
    uint32_t indicator_inactive_color;
    const lv_img_dsc_t *wallpaper;
} home_theme_palette_t;

static home_theme_palette_t get_home_theme_palette(int32_t theme_id)
{
    switch (sanitize_ui_theme_id(theme_id)) {
    case APP_THEME_TOPOGRAPHY:
        return {
            .background_color = 0xEEF5F0,
            .container_outline_colors = {0x154734, 0x3F6C57, 0x5F846F, 0x86A997, 0xB0CABC, 0xD5E6DC},
            .indicator_active_color = 0x154734,
            .indicator_inactive_color = 0x98B5A6,
            .wallpaper = &theme_bg_topography_1024x600,
        };
    case APP_THEME_VISTA:
        return {
            .background_color = 0xF3F8F4,
            .container_outline_colors = {0x154734, 0x497054, 0x64896A, 0x86A18A, 0xB5C7B8, 0xDCE8DE},
            .indicator_active_color = 0x154734,
            .indicator_inactive_color = 0xA5BBB0,
            .wallpaper = &theme_bg_vista_1024x600,
        };
    case APP_THEME_CALPOLY:
    default:
        return {
            .background_color = CALPOLY_HOME_BG_COLOR,
            .container_outline_colors = {0x154734, 0x2F6B57, 0x4F775E, 0x7AA087, 0xA9C4B5, 0xD3E2D8},
            .indicator_active_color = 0x154734,
            .indicator_inactive_color = 0x8FA998,
            .wallpaper = &cp_logo_rev_home,
        };
    }
}

static void configure_calpoly_stylesheet(ESP_Brookesia_PhoneStylesheet_t &stylesheet, int32_t theme_id)
{
    const home_theme_palette_t palette = get_home_theme_palette(theme_id);

    stylesheet.core.name = "1024x600 Cal Poly";
    stylesheet.core.home.background.color = ESP_BROOKESIA_STYLE_COLOR(palette.background_color);
    stylesheet.core.home.background.wallpaper_image_resource = ESP_BROOKESIA_STYLE_IMAGE(palette.wallpaper);
    for (int i = 0; i < 6; ++i) {
        stylesheet.core.home.container.styles[i].outline_color =
            ESP_BROOKESIA_STYLE_COLOR(palette.container_outline_colors[i]);
    }

    stylesheet.home.status_bar.data.main.size = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(100, 56);
    stylesheet.home.status_bar.data.main.background_color = ESP_BROOKESIA_STYLE_COLOR(0x154734);
    stylesheet.home.status_bar.data.main.text_color = ESP_BROOKESIA_STYLE_COLOR(0xFFFFFF);
    stylesheet.home.status_bar.data.area.data[0].layout_column_start_offset = 24;
    stylesheet.home.status_bar.data.area.data[2].layout_column_start_offset = 24;
    stylesheet.home.status_bar.data.area.data[2].layout_column_pad = 12;
    stylesheet.home.flags.enable_recents_screen = 1;

    stylesheet.home.app_launcher.data.icon.label.text_color = ESP_BROOKESIA_STYLE_COLOR(0x154734);
    stylesheet.home.app_launcher.data.table.default_num = 1;
    stylesheet.home.app_launcher.data.indicator.spot_active_background_color =
        ESP_BROOKESIA_STYLE_COLOR(palette.indicator_active_color);
    stylesheet.home.app_launcher.data.indicator.spot_inactive_background_color =
        ESP_BROOKESIA_STYLE_COLOR_WITH_OPACIRY(palette.indicator_inactive_color, 255);

    stylesheet.manager.flags.enable_gesture = 1;
    stylesheet.manager.flags.enable_gesture_navigation_back = 0;
}

static void apply_home_theme_locked(int32_t theme_id)
{
    if (s_phone == nullptr) {
        return;
    }

    lv_obj_t *main_screen_obj = s_phone->getHome().getMainScreenObject();
    if (main_screen_obj == nullptr) {
        return;
    }

    const home_theme_palette_t palette = get_home_theme_palette(theme_id);
    
    // Apply background color first
    lv_obj_set_style_bg_color(main_screen_obj, lv_color_hex(palette.background_color), 0);
    lv_obj_set_style_bg_opa(main_screen_obj, LV_OPA_COVER, 0);
    
    // Handle wallpaper image - properly clear if null
    if (palette.wallpaper != nullptr) {
        lv_obj_set_style_bg_img_src(main_screen_obj, palette.wallpaper, 0);
        lv_obj_set_style_bg_img_opa(main_screen_obj, 86, 0);
    } else {
        // Explicitly clear wallpaper for Cal Poly theme
        lv_obj_set_style_bg_img_opa(main_screen_obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_img_src(main_screen_obj, NULL, 0);
    }
    
    lv_obj_invalidate(main_screen_obj);
}

static void prepare_clean_home_screen_locked(void)
{
    lv_obj_t *fresh_screen = lv_obj_create(NULL);
    if (fresh_screen == nullptr) {
        ESP_LOGW(TAG, "Failed to create a clean home screen");
        return;
    }

    lv_obj_clear_flag(fresh_screen, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(fresh_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(fresh_screen, lv_color_hex(0xF7FAF7), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(fresh_screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_src(fresh_screen, nullptr, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_opa(fresh_screen, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_scr_load_anim(fresh_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    lv_refr_now(NULL);
}

static void add_home_logo_to_status_bar(void)
{
    if ((s_phone == nullptr) || s_home_logo_added) {
        return;
    }

    ESP_Brookesia_StatusBar *status_bar = s_phone->getHome().getStatusBar();
    const ESP_Brookesia_PhoneStylesheet_t *active_stylesheet = s_phone->getStylesheet();
    if ((status_bar == nullptr) || (active_stylesheet == nullptr)) {
        return;
    }

    ESP_Brookesia_StatusBarIconData_t logo_data = {
        .size = ESP_BROOKESIA_STYLE_SIZE_RECT(176, 45),
        .icon = {
            .image_num = 1,
            .images = {ESP_BROOKESIA_STYLE_IMAGE(&cp_logo_rev_home)},
        },
    };

    if (!ESP_Brookesia_StatusBar::calibrateIconData(active_stylesheet->home.status_bar.data, s_phone->getHome(), logo_data)) {
        ESP_LOGW(TAG, "Failed to calibrate home logo status bar data");
        return;
    }

    if (!status_bar->addIcon(logo_data, 0, STATUS_BAR_LOGO_ID)) {
        ESP_LOGW(TAG, "Failed to add home logo to status bar");
        return;
    }

    s_home_logo_added = true;
}

extern "C" int32_t app_get_screen_timeout_seconds(void)
{
    return s_screen_timeout_s;
}

extern "C" int32_t app_get_ui_theme_id(void)
{
    return sanitize_ui_theme_id(s_ui_theme_id);
}

extern "C" void app_note_user_interaction(void)
{
    s_last_interaction_time_s = get_uptime_seconds();
}

extern "C" void app_set_screen_timeout_seconds(int32_t timeout_seconds)
{
    s_screen_timeout_s = (timeout_seconds > 0) ? timeout_seconds : SCREEN_TIMEOUT_DISABLED_S;
    app_note_user_interaction();

    if (s_display_ready && (s_screen_timeout_s == SCREEN_TIMEOUT_DISABLED_S) && s_enter_sleep_flag) {
        s_enter_sleep_flag = false;
        bsp_display_brightness_set((s_prev_brightness > 0) ? s_prev_brightness : BSP_LCD_BACKLIGHT_BRIGHTNESS_MAX);
    }
}

extern "C" void app_set_ui_theme_id(int32_t theme_id)
{
    theme_id = sanitize_ui_theme_id(theme_id);
    if (theme_id == s_ui_theme_id) {
        return;
    }

    s_ui_theme_id = theme_id;
    save_ui_theme_to_nvs(theme_id);

    if (s_display_ready && (s_phone != nullptr)) {
        bsp_display_lock(0);
        apply_home_theme_locked(theme_id);
        bsp_display_unlock();
    }
}

void touch_detect_task(void *param)
{
    app_note_user_interaction();
    while (1)
    {
        uint32_t boot_time_s = get_uptime_seconds();

        uint16_t touch_x[1];
        uint16_t touch_y[1];
        uint8_t touch_cnt = 0;

        esp_lcd_touch_get_coordinates(tp, touch_x, touch_y, NULL, &touch_cnt, 1);
        /*There are clicks on the touchscreen.*/
        if (touch_cnt) {
            app_note_user_interaction();
            // If the screen was off before, restore the brightness
            if (s_enter_sleep_flag) {
                s_enter_sleep_flag = !s_enter_sleep_flag;
                bsp_display_brightness_set(s_prev_brightness);
            }
        }
        else {
            if (!s_enter_sleep_flag) {
                /* If there is no touch and it is in the non-screen-off state, and it 
                    has not been touched for more than a certain period of time, it enters the screen-off state*/
                if ((s_screen_timeout_s > SCREEN_TIMEOUT_DISABLED_S) &&
                    ((boot_time_s - s_last_interaction_time_s) >= (uint32_t)s_screen_timeout_s)) {
                    s_enter_sleep_flag = !s_enter_sleep_flag;
                    s_prev_brightness = bsp_display_brightness_get();
                    bsp_display_brightness_set(0);
                }
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}


extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Defer post-OTA C6 validation wait to background C6 init task so UI boot stays responsive.
    s_post_ota_c6_delay_required = ota_slave_is_pending();
    if (s_post_ota_c6_delay_required) {
        ESP_LOGW(TAG, "Post-OTA reboot detected - C6 init task will wait 8s for firmware validation");
    }

    /* Bring up ESP-Hosted SDIO transport explicitly *before* anything else
     * touches it. Without this, the transport only comes up lazily when
     * esp_wifi_init() runs (much later), causing:
     *   - ota_slave_flash_if_needed to time out for 10 s waiting on RPC,
     *   - any user-side custom-data send to leave a timed-out async-RPC
     *     entry that crashes the RPC layer when the C6 later replies.
     * Calling esp_hosted_init/connect early triggers the GPIO32 reset and
     * waits for the C6 INIT event, so by the time the c6_init task runs the
     * SDIO link is fully ready. */
    {
        int ret = esp_hosted_init();
        if (ret != 0) {
            ESP_LOGW(TAG, "esp_hosted_init returned %d (continuing)", ret);
        }
        ret = esp_hosted_connect_to_slave();
        if (ret != 0) {
            ESP_LOGW(TAG, "esp_hosted_connect_to_slave returned %d (continuing)", ret);
        }
    }

    s_screen_timeout_s = load_screen_timeout_from_nvs();
    s_ui_theme_id = load_ui_theme_from_nvs();

    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount failed: %s", esp_err_to_name(sd_err));
    }
    ESP_LOGI(TAG, "SD card mount successfully");
#endif

    ESP_LOGI(TAG, "Skipping BSP speaker/mic codec init; assistive AUX I2S owns the audio path");

    s_battery_monitor_ready = (stc8_i2c_init() == ESP_OK);
    Battery_info_t battery_info = {0};
    esp_err_t battery_info_err = s_battery_monitor_ready ? stc8_battery_info_get(&battery_info) : ESP_FAIL;
    ESP_LOGI(TAG, "adc_voltage = %lu mV", battery_info.adc_voltage);
    ESP_LOGI(TAG, "bat_voltage = %lu mV", battery_info.bat_voltage);
    ESP_LOGI(TAG, "bat_level = %d %%", battery_info.bat_level);
    ESP_LOGI(TAG, "bat_state = %d", battery_info.bat_state);
    ESP_LOGI(TAG, "led_state = %d", battery_info.led_state);
    if (battery_info_err != ESP_OK) {
        ESP_LOGW(TAG, "Battery monitor unavailable; continuing without low-battery sleep guard");
    } else if ((battery_info.bat_voltage > 0) && (battery_info.bat_voltage <= 3500)) {
        ESP_LOGI(TAG, "esp_deep_sleep_start()");
        vTaskDelay(100 / portTICK_PERIOD_MS);
        esp_deep_sleep_start();
    }
    xTaskCreatePinnedToCore(battery_info_task, "battery_info_task", 4096, NULL, 3, &battery_info_task_handle, 0);

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    /* Pin LVGL to CPU0 so it never contends with espnow_play (pinned CPU1).
     * Default ESP_LVGL_PORT_INIT_CONFIG() uses task_affinity = -1 (float), which
     * lets the scheduler land LVGL on the same core the audio task is using,
     * causing UI stutters during LC3 decode / blocking I2S writes. */
    cfg.lvgl_port_cfg.task_affinity = 0;
    cfg.lvgl_port_cfg.timer_period_ms = 10;
    cfg.lvgl_port_cfg.task_max_sleep_ms = 100;
    bsp_display_start_with_config(&cfg);
    s_display_ready = true;
    start_c6_init_task();

    bsp_display_backlight_on();

    xTaskCreatePinnedToCore(touch_detect_task, "touch_detect_task", 2048, NULL, 3, NULL, 0);

    bsp_display_lock(0);
    elecrow_screen();
    bsp_display_unlock();

    while (!elecrow_success)
    {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    bsp_display_lock(0);
    prepare_clean_home_screen_locked();

    s_phone = new ESP_Brookesia_Phone();
    assert(s_phone != nullptr && "Failed to create phone");

    s_phone_stylesheet = new ESP_Brookesia_PhoneStylesheet_t ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET();
    ESP_BROOKESIA_CHECK_NULL_EXIT(s_phone_stylesheet, "Create phone stylesheet failed");
    configure_calpoly_stylesheet(*s_phone_stylesheet, s_ui_theme_id);
    ESP_BROOKESIA_CHECK_FALSE_EXIT(s_phone->addStylesheet(*s_phone_stylesheet), "Add phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(s_phone->activateStylesheet(*s_phone_stylesheet), "Activate phone stylesheet failed");

    assert(s_phone->begin() && "Failed to begin phone");
    add_home_logo_to_status_bar();
    apply_home_theme_locked(s_ui_theme_id);

    /* Ensure the LVGL display background is opaque black so that any area
     * not covered by a screen object (e.g. DPI blanking gaps, transition
     * frames) does not show uninitialised framebuffer data as a green hue. */
    lv_disp_set_bg_color(lv_disp_get_default(), lv_color_black());
    lv_disp_set_bg_opa(lv_disp_get_default(), LV_OPA_COVER);

    /* Force a full-screen refresh to clear any uninitialised framebuffer areas
     * that might show as green/hue artifacts on the DPI panel. */
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    AppSettings *app_settings = new AppSettings();
    assert(app_settings != nullptr && "Failed to create app_settings");
    if (s_phone->installApp(app_settings) < 0) {
        ESP_LOGE(TAG, "Failed to install app_settings, continuing without it");
        delete app_settings;
    }

    bsp_display_unlock();
    
    start_c6_init_task();
}
