/*
 * C6 Coprocessor app_main
 *
 * Initializes:
 * 1. ESP-Hosted coprocessor (WiFi/BT via SDIO to P4)
 * 2. ESP-NOW audio sink (room scanning, joining, LC3 decode)
 * 3. Custom data bridge (sends audio/events to P4 via SDIO)
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_hosted_coprocessor.h"
#include "espnow_sink_c6.h"

static const char *TAG = "c6_main";

void app_main(void)
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize ESP-Hosted coprocessor (WiFi, BT, SDIO transport) */
    ESP_LOGI(TAG, "Initializing ESP-Hosted coprocessor...");
    ESP_ERROR_CHECK(esp_hosted_coprocessor_init());
    ESP_LOGI(TAG, "ESP-Hosted coprocessor ready");

    /* Initialize ESP-NOW sink bridge (registers SDIO command handlers) */
    ESP_LOGI(TAG, "Initializing ESP-NOW sink bridge...");
    ESP_ERROR_CHECK(espnow_sink_c6_init());
    ESP_LOGI(TAG, "ESP-NOW sink bridge ready");
}
