/*
 * OTA Slave Firmware Update for ESP32-C6 Coprocessor
 * Version-based OTA with actual firmware size detection
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Flash slave firmware from slave_fw partition if version differs
 * 
 * Extracts version from embedded firmware binary and compares with
 * last-flashed version stored in NVS. Parses ESP32 image header to
 * determine actual binary size (avoids writing 0xFF padding).
 * Does NOT call activate - that must be done separately.
 * 
 * @return ESP_OK on success (or skip), error code on failure
 */
esp_err_t ota_slave_flash_if_needed(void);

/**
 * @brief Activate new slave firmware if version supports it
 * 
 * Should be called after ota_slave_flash_if_needed() completes.
 * Only calls esp_hosted_slave_ota_activate() if slave FW >= v2.6.0.
 * Waits for C6 reboot and verifies it responds post-activation.
 * 
 * @return ESP_OK on success, error code on failure
 */
esp_err_t ota_slave_activate_if_supported(void);

/**
 * @brief Check if OTA transfer was performed
 * 
 * @return true if firmware was transferred this boot
 */
bool ota_slave_transfer_completed(void);

/**
 * @brief Check if slave firmware supports activation API
 * 
 * @return true if slave FW >= v2.6.0
 */
bool ota_slave_activate_supported(void);

/**
 * @brief Check if we just rebooted after OTA activation (early check)
 * 
 * This can be called very early after NVS init, before ESP-Hosted.
 * Used to add startup delay so C6 can validate its new firmware
 * before P4's GPIO reset interferes.
 * 
 * @return true if OTA pending flag is set
 */
bool ota_slave_is_pending(void);

/**
 * @brief Set ESP-NOW support status after verification
 * 
 * @param supported true if ESP-NOW is working on slave
 */
void ota_slave_set_espnow_supported(bool supported);

/**
 * @brief Check if ESP-NOW is supported on slave
 * 
 * @return true if ESP-NOW support was verified
 */
bool ota_slave_espnow_supported(void);

#ifdef __cplusplus
}
#endif
