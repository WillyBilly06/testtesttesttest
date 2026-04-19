/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"

// Default timezone: Pacific Time (California)
#define DEFAULT_TIMEZONE        "PST8PDT,M3.2.0,M11.1.0"
#define NVS_NAMESPACE_TZ        "timezone"
#define NVS_KEY_TZ              "tz_str"

#define SERVER_NAME_0   "pool.ntp.org"
#define SERVER_NAME_1   "time.nist.gov"
#define SERVER_NAME_2   "time.google.com"

static const char *TAG = "sntp";
static char current_timezone[64] = DEFAULT_TIMEZONE;

static void obtain_time(void);
static void initialize_sntp(void);

// Load timezone from NVS
static void load_timezone_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_TZ, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t required_size = sizeof(current_timezone);
        err = nvs_get_str(handle, NVS_KEY_TZ, current_timezone, &required_size);
        if (err != ESP_OK) {
            strncpy(current_timezone, DEFAULT_TIMEZONE, sizeof(current_timezone) - 1);
        }
        nvs_close(handle);
    } else {
        strncpy(current_timezone, DEFAULT_TIMEZONE, sizeof(current_timezone) - 1);
    }
    current_timezone[sizeof(current_timezone) - 1] = '\0';
    ESP_LOGI(TAG, "Loaded timezone: %s", current_timezone);
}

// Save timezone to NVS and apply it
void app_sntp_set_timezone(const char *tz_str)
{
    if (tz_str == NULL || tz_str[0] == '\0') {
        return;
    }

    strncpy(current_timezone, tz_str, sizeof(current_timezone) - 1);
    current_timezone[sizeof(current_timezone) - 1] = '\0';

    // Apply timezone
    setenv("TZ", current_timezone, 1);
    tzset();

    // Save to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_TZ, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_TZ, current_timezone);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved timezone: %s", current_timezone);
    }
}

// Get current timezone string
const char* app_sntp_get_timezone(void)
{
    return current_timezone;
}

// Get timezone index for UI dropdown
int app_sntp_get_timezone_index(void)
{
    // Common timezone options (matching index order in dropdown)
    static const char *tz_options[] = {
        "PST8PDT,M3.2.0,M11.1.0",    // 0: Pacific Time (California)
        "MST7MDT,M3.2.0,M11.1.0",    // 1: Mountain Time
        "CST6CDT,M3.2.0,M11.1.0",    // 2: Central Time
        "EST5EDT,M3.2.0,M11.1.0",    // 3: Eastern Time
        "UTC0",                       // 4: UTC
        "GMT0BST,M3.5.0/1,M10.5.0",  // 5: London
        "CET-1CEST,M3.5.0,M10.5.0/3", // 6: Central Europe
        "CST-8",                      // 7: China Standard Time
        "JST-9",                      // 8: Japan Standard Time
    };
    const int num_options = sizeof(tz_options) / sizeof(tz_options[0]);

    for (int i = 0; i < num_options; i++) {
        if (strcmp(current_timezone, tz_options[i]) == 0) {
            return i;
        }
    }
    return 0; // Default to Pacific Time
}

// Set timezone by index
void app_sntp_set_timezone_by_index(int index)
{
    static const char *tz_options[] = {
        "PST8PDT,M3.2.0,M11.1.0",    // 0: Pacific Time (California)
        "MST7MDT,M3.2.0,M11.1.0",    // 1: Mountain Time
        "CST6CDT,M3.2.0,M11.1.0",    // 2: Central Time
        "EST5EDT,M3.2.0,M11.1.0",    // 3: Eastern Time
        "UTC0",                       // 4: UTC
        "GMT0BST,M3.5.0/1,M10.5.0",  // 5: London
        "CET-1CEST,M3.5.0,M10.5.0/3", // 6: Central Europe
        "CST-8",                      // 7: China Standard Time
        "JST-9",                      // 8: Japan Standard Time
    };
    const int num_options = sizeof(tz_options) / sizeof(tz_options[0]);

    if (index >= 0 && index < num_options) {
        app_sntp_set_timezone(tz_options[index]);
    }
}

#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_CUSTOM
void sntp_sync_time(struct timeval *tv)
{
    settimeofday(tv, NULL);
    ESP_LOGI(TAG, "Time is synchronized from custom code");
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}
#endif

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event, sec=%lu", tv->tv_sec);
    settimeofday(tv, NULL);
}

void app_sntp_init(void)
{
    static bool sntp_initialized = false;
    time_t now;
    struct tm timeinfo;

    if (sntp_initialized) {
        return;
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    // Load and set timezone from NVS (defaults to Pacific Time)
    load_timezone_from_nvs();
    setenv("TZ", current_timezone, 1);
    tzset();
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    else {
        // add 500 ms error to the current system time.
        // Only to demonstrate a work of adjusting method!
        {
            ESP_LOGI(TAG, "Add a error for test adjtime");
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            int64_t cpu_time = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
            int64_t error_time = cpu_time + 500 * 1000L;
            struct timeval tv_error = {.tv_sec = error_time / 1000000L, .tv_usec = error_time % 1000000L};
            settimeofday(&tv_error, NULL);
        }

        ESP_LOGI(TAG, "Time was set, now just adjusting it. Use SMOOTH SYNC method.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
#endif

    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s (TZ: %s)", strftime_buf, current_timezone);

    if (sntp_get_sync_mode() == SNTP_SYNC_MODE_SMOOTH) {
        struct timeval outdelta;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS) {
            adjtime(NULL, &outdelta);
            ESP_LOGI(TAG, "Waiting for adjusting time ... outdelta = %li sec: %li ms: %li us",
                     (long)outdelta.tv_sec,
                     outdelta.tv_usec / 1000,
                     outdelta.tv_usec % 1000);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    sntp_initialized = true;
}

static void obtain_time(void)
{
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SERVER_NAME_0);
    esp_sntp_setservername(1, SERVER_NAME_1);
    esp_sntp_setservername(2, SERVER_NAME_2);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    esp_sntp_init();
}
