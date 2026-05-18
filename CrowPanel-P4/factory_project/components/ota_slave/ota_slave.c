/*
 * OTA Slave Firmware Update for ESP32-C6 Coprocessor
 * Version-based OTA: compares embedded firmware version with last-flashed version.
 * Parses ESP32 image header to determine actual binary size (avoids 0xFF padding).
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_hosted.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ota_slave.h"
#include "espnow_protocol.h"

/* From ESP-Hosted transport_drv.h — non-RPC transport state check */
extern uint8_t is_transport_tx_ready(void);

static const char *TAG = "ota_slave";

#define SLAVE_FW_PARTITION_LABEL "slave_fw"
#define OTA_CHUNK_SIZE           1024
#define NVS_NAMESPACE            "ota_slave"
#define NVS_KEY_FW_VERSION       "fw_ver"
#define NVS_KEY_OTA_PENDING      "ota_pending"
#define NVS_KEY_TARGET_VERSION   "target_ver"

// ESP32 image header magic
#define ESP_IMAGE_MAGIC          0xE9
// esp_app_desc_t magic word
#define APP_DESC_MAGIC           0xABCD5432

// State tracking
static bool s_transfer_completed = false;
static bool s_activate_supported = false;
static bool s_espnow_supported = false;
static char s_target_version[32] = {0};  // Version we're trying to OTA to

// C6 app version query state
static SemaphoreHandle_t s_ver_sem = NULL;
static char s_c6_app_version[32] = {0};
static char s_c6_app_project[32] = {0};

// Lightweight esp_app_desc_t (just the fields we need)
typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char     version[32];
    char     project_name[32];
} app_desc_lite_t;

// ESP32 image header (24 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  segment_count;
    uint8_t  spi_mode;
    uint8_t  spi_speed_size;   // speed:4, size:4
    uint32_t entry_addr;
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t  min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t  reserved[4];
    uint8_t  hash_appended;
} image_header_t;
_Static_assert(sizeof(image_header_t) == 24, "image_header_t must be 24 bytes");

// Segment header (8 bytes)
typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} segment_header_t;

// Check if version supports activate API (>= v2.6.0)
static bool version_supports_activate(int major, int minor)
{
    return (major > 2) || (major == 2 && minor >= 6);
}

bool ota_slave_transfer_completed(void)
{
    return s_transfer_completed;
}

bool ota_slave_activate_supported(void)
{
    return s_activate_supported;
}

void ota_slave_set_espnow_supported(bool supported)
{
    s_espnow_supported = supported;
}

bool ota_slave_espnow_supported(void)
{
    return s_espnow_supported;
}

// Public wrapper for early check before ESP-Hosted init
bool ota_slave_is_pending(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    
    uint8_t pending = 0;
    esp_err_t ret = nvs_get_u8(nvs, NVS_KEY_OTA_PENDING, &pending);
    nvs_close(nvs);
    return (ret == ESP_OK && pending == 1);
}

/**
 * Parse ESP32 image header to calculate actual firmware binary size.
 * Layout: image_header(24) + N×[seg_header(8) + seg_data] + checksum(1) + [sha256(32)]
 */
static esp_err_t get_firmware_size(const esp_partition_t *part, size_t *out_size)
{
    image_header_t hdr;
    esp_err_t ret = esp_partition_read(part, 0, &hdr, sizeof(hdr));
    if (ret != ESP_OK || hdr.magic != ESP_IMAGE_MAGIC) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = sizeof(image_header_t);
    for (int i = 0; i < hdr.segment_count; i++) {
        segment_header_t seg;
        ret = esp_partition_read(part, offset, &seg, sizeof(seg));
        if (ret != ESP_OK) return ret;
        offset += sizeof(segment_header_t) + seg.data_len;
        if (offset > part->size) {
            ESP_LOGE(TAG, "Firmware segment overflows partition at offset %u", (unsigned)offset);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    offset += 1; // checksum byte
    if (hdr.hash_appended) {
        offset += 32; // SHA256 digest
    }

    // Align up to 16 bytes (ESP-IDF pads binaries to 16-byte boundary)
    offset = (offset + 15) & ~15;

    *out_size = offset;
    return ESP_OK;
}

/**
 * Extract version string from esp_app_desc_t embedded in the firmware binary.
 * Searches first 4KB for the APP_DESC_MAGIC word.
 */
static esp_err_t get_firmware_version(const esp_partition_t *part, char *version, size_t version_len,
                                       char *project, size_t project_len)
{
    // esp_app_desc_t is typically within the first segment, after the image header
    // Search in 4-byte aligned positions within first 4KB
    uint8_t buf[sizeof(app_desc_lite_t)];

    for (size_t offset = sizeof(image_header_t) + sizeof(segment_header_t);
         offset < 4096;
         offset += 4) {
        uint32_t magic = 0;
        esp_err_t ret = esp_partition_read(part, offset, &magic, sizeof(magic));
        if (ret != ESP_OK) continue;

        if (magic == APP_DESC_MAGIC) {
            ret = esp_partition_read(part, offset, buf, sizeof(buf));
            if (ret != ESP_OK) return ret;

            app_desc_lite_t *desc = (app_desc_lite_t *)buf;
            if (version && version_len > 0) {
                strncpy(version, desc->version, version_len - 1);
                version[version_len - 1] = '\0';
            }
            if (project && project_len > 0) {
                strncpy(project, desc->project_name, project_len - 1);
                project[project_len - 1] = '\0';
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

// Load stored version from NVS
static esp_err_t load_stored_version(char *version, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(nvs, NVS_KEY_FW_VERSION, version, &len);
    nvs_close(nvs);
    return ret;
}

// Save version to NVS
static esp_err_t save_stored_version(const char *version)
{
    if (!version || version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    char current[32] = {0};
    size_t current_len = sizeof(current);
    esp_err_t get_ret = nvs_get_str(nvs, NVS_KEY_FW_VERSION, current, &current_len);
    if (get_ret == ESP_OK && strcmp(current, version) == 0) {
        nvs_close(nvs);
        return ESP_OK;
    }
    if (get_ret != ESP_OK && get_ret != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return get_ret;
    }

    ret = nvs_set_str(nvs, NVS_KEY_FW_VERSION, version);
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

// Check if OTA reboot is pending (we just activated and restarted)
static bool is_ota_pending(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    
    uint8_t pending = 0;
    esp_err_t ret = nvs_get_u8(nvs, NVS_KEY_OTA_PENDING, &pending);
    nvs_close(nvs);
    return (ret == ESP_OK && pending == 1);
}

// Set OTA pending flag and target version before restart
static esp_err_t set_ota_pending(const char *target_version)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_set_u8(nvs, NVS_KEY_OTA_PENDING, 1);
    nvs_set_str(nvs, NVS_KEY_TARGET_VERSION, target_version);
    ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

// Clear OTA pending flag
static esp_err_t clear_ota_pending(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_erase_key(nvs, NVS_KEY_OTA_PENDING);
    nvs_erase_key(nvs, NVS_KEY_TARGET_VERSION);
    ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

// Get the target version we were trying to OTA to
static esp_err_t get_ota_target_version(char *version, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(nvs, NVS_KEY_TARGET_VERSION, version, &len);
    nvs_close(nvs);
    return ret;
}

// Callback for C6 firmware version response
static void on_fw_ver_response(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len >= sizeof(espnow_evt_fw_ver_t)) {
        const espnow_evt_fw_ver_t *resp = (const espnow_evt_fw_ver_t *)data;
        strncpy(s_c6_app_version, resp->version, sizeof(s_c6_app_version) - 1);
        s_c6_app_version[sizeof(s_c6_app_version) - 1] = '\0';
        strncpy(s_c6_app_project, resp->project, sizeof(s_c6_app_project) - 1);
        s_c6_app_project[sizeof(s_c6_app_project) - 1] = '\0';
    }
    if (s_ver_sem) {
        xSemaphoreGive(s_ver_sem);
    }
}

/**
 * Query C6's application firmware version via custom data channel.
 * Returns ESP_OK and fills version/project if C6 responds within timeout.
 */
static esp_err_t query_c6_app_version(char *version, size_t ver_len,
                                       char *project, size_t proj_len,
                                       uint32_t timeout_ms)
{
    s_ver_sem = xSemaphoreCreateBinary();
    if (!s_ver_sem) return ESP_ERR_NO_MEM;

    memset(s_c6_app_version, 0, sizeof(s_c6_app_version));
    memset(s_c6_app_project, 0, sizeof(s_c6_app_project));

    // Register temporary callback for the version response
    esp_err_t ret = esp_hosted_register_custom_callback(ESPNOW_MSG_EVT_FW_VER, on_fw_ver_response, NULL);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_ver_sem);
        s_ver_sem = NULL;
        return ret;
    }

    // Send version query command to C6
    ret = esp_hosted_send_custom_data(ESPNOW_MSG_CMD_GET_FW_VER, NULL, 0);
    if (ret != ESP_OK) {
        esp_hosted_register_custom_callback(ESPNOW_MSG_EVT_FW_VER, NULL, NULL);
        vSemaphoreDelete(s_ver_sem);
        s_ver_sem = NULL;
        return ret;
    }

    // Wait for response
    bool got_response = (xSemaphoreTake(s_ver_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    // Deregister callback
    esp_hosted_register_custom_callback(ESPNOW_MSG_EVT_FW_VER, NULL, NULL);
    vSemaphoreDelete(s_ver_sem);
    s_ver_sem = NULL;

    if (!got_response) {
        ESP_LOGW(TAG, "C6 app version query timed out");
        return ESP_ERR_TIMEOUT;
    }

    if (version && ver_len > 0) {
        strncpy(version, s_c6_app_version, ver_len - 1);
        version[ver_len - 1] = '\0';
    }
    if (project && proj_len > 0) {
        strncpy(project, s_c6_app_project, proj_len - 1);
        project[proj_len - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t ota_slave_flash_if_needed(void)
{
    esp_err_t ret;
    esp_hosted_coprocessor_fwver_t ver_info = {0};
    bool post_ota_boot = is_ota_pending();

    /*
     * POST-OTA PATH: C6 just rebooted with new firmware.
     * CRITICAL: Do NOT call esp_hosted_get_coprocessor_fwversion() or any RPC here.
     * EUI_Setting's wifiScanTask runs concurrently and fires WiFi scan RPCs.
     * Concurrent RPCs on the SDIO channel cause timeouts that corrupt the RPC
     * state machine → hosted_memcpy NULL assert crash on the next RPC from any task.
     *
     * Instead: use non-RPC is_transport_tx_ready() to detect transport, then
     * wait for all concurrent WiFi RPCs to settle before returning.
     */
    if (post_ota_boot) {
        ESP_LOGI(TAG, "Post-OTA boot detected, waiting for transport (no RPC)...");

        /* Wait up to 30s for SDIO transport to come up (non-RPC check) */
        int wait = 0;
        while (!is_transport_tx_ready() && wait < 300) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait++;
            if (wait % 20 == 0) {
                ESP_LOGI(TAG, "POST-OTA: still waiting for transport... (%ds)", wait / 10);
            }
        }

        if (!is_transport_tx_ready()) {
            ESP_LOGE(TAG, "POST-OTA: transport not ready after 30s");
            clear_ota_pending();
            return ESP_ERR_TIMEOUT;
        }

        ESP_LOGI(TAG, "POST-OTA: transport active");

        char target_version[32] = {0};
        if (get_ota_target_version(target_version, sizeof(target_version)) == ESP_OK) {
            ESP_LOGI(TAG, "POST-OTA: trusting OTA target version '%s'", target_version);
            save_stored_version(target_version);
            s_espnow_supported = true;
        } else {
            ESP_LOGW(TAG, "POST-OTA: could not read target version, assuming success");
            s_espnow_supported = true;
        }
        s_activate_supported = true;  /* C6 firmware supports activate if OTA succeeded */

        clear_ota_pending();

        /* Wait 5s for EUI_Setting's concurrent WiFi scan/connect RPCs to finish.
         * Without this, espnow_sink_init fires its own RPCs while the SDIO path
         * is still handling WiFi scan responses → hosted_memcpy NULL crash. */
        ESP_LOGI(TAG, "POST-OTA: waiting 5s for concurrent RPCs to settle...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "POST-OTA: complete, ESP-NOW ready");
        return ESP_OK;
    }

    /* NORMAL BOOT PATH: safe to use RPC since no concurrent WiFi scan yet */

    // Wait for ESP-Hosted transport
    ESP_LOGI(TAG, "Waiting for ESP-Hosted transport...");
    int retry = 0;
    while (esp_hosted_get_coprocessor_fwversion(&ver_info) != ESP_OK && retry < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
        if (retry % 10 == 0) {
            ESP_LOGI(TAG, "Still waiting for transport... (%d)", retry);
        }
    }

    if (retry >= 100) {
        ESP_LOGE(TAG, "ESP-Hosted transport not ready after timeout");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "ESP-Hosted transport active (slave ESP-Hosted v%lu.%lu.%lu)",
             ver_info.major1, ver_info.minor1, ver_info.patch1);
    s_activate_supported = version_supports_activate(ver_info.major1, ver_info.minor1);

    // Normal boot: query C6's actual app firmware version via custom data channel
    // Small delay to let the custom data channel fully stabilize after transport init
    vTaskDelay(pdMS_TO_TICKS(200));

    char c6_app_version[32] = {0};
    char c6_app_project[32] = {0};
    ret = query_c6_app_version(c6_app_version, sizeof(c6_app_version),
                                c6_app_project, sizeof(c6_app_project), 3000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not query C6 app version: %s (falling back to ESP-Hosted version)",
                 esp_err_to_name(ret));
        // Fall back to ESP-Hosted RPC version if custom query fails
        snprintf(c6_app_version, sizeof(c6_app_version), "%lu.%lu.%lu",
                 ver_info.major1, ver_info.minor1, ver_info.patch1);
    } else {
        ESP_LOGI(TAG, "C6 app firmware: project='%s' version='%s'", c6_app_project, c6_app_version);
    }

    // Find the slave firmware partition
    const esp_partition_t *slave_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, SLAVE_FW_PARTITION_LABEL);

    if (!slave_part) {
        ESP_LOGW(TAG, "Slave firmware partition '%s' not found", SLAVE_FW_PARTITION_LABEL);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Found slave firmware partition at 0x%lx, size %lu",
             slave_part->address, slave_part->size);

    // Validate firmware header
    uint8_t magic = 0;
    ret = esp_partition_read(slave_part, 0, &magic, 1);
    if (ret != ESP_OK || magic != ESP_IMAGE_MAGIC) {
        ESP_LOGW(TAG, "No valid firmware in slave_fw partition (magic=0x%02x)", magic);
        /* No embedded image to OTA against. If the C6 is up and reporting
         * a version, trust whatever it is currently running and enable
         * ESP-NOW. This handles the case where the C6 was flashed via an
         * external tool (e.g. lboshuizen SDIO OTA) and the embedded image
         * was not compiled into the P4 binary. */
        if (c6_app_version[0] != '\0' && strcmp(c6_app_version, "unknown") != 0) {
            ESP_LOGI(TAG, "C6 running '%s' — no embedded image, trusting current firmware", c6_app_version);
            s_espnow_supported = true;
            save_stored_version(c6_app_version);
        }
        return ESP_OK;
    }

    // Extract version from embedded firmware binary
    char embedded_version[32] = {0};
    char embedded_project[32] = {0};
    ret = get_firmware_version(slave_part, embedded_version, sizeof(embedded_version),
                                embedded_project, sizeof(embedded_project));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not extract version from firmware (using 'unknown')");
        strncpy(embedded_version, "unknown", sizeof(embedded_version) - 1);
    } else {
        ESP_LOGI(TAG, "Embedded firmware: project='%s' version='%s'", embedded_project, embedded_version);
    }

    ESP_LOGI(TAG, "C6 running app version: '%s'", c6_app_version);

    // Compare embedded version against C6 app version
    bool need_ota = true;
    if (strcmp(embedded_version, c6_app_version) == 0) {
        ESP_LOGI(TAG, "Firmware version '%s' already running on C6, skipping OTA", embedded_version);
        need_ota = false;
        s_espnow_supported = true;
        // Update NVS to match (in case it was stale)
        save_stored_version(embedded_version);
    } else {
        ESP_LOGI(TAG, "Version mismatch: C6='%s' embedded='%s' -> OTA needed",
                 c6_app_version, embedded_version);
        s_espnow_supported = false;
    }

    if (!need_ota) {
        return ESP_OK;
    }

    // Calculate actual firmware binary size from image header
    size_t fw_size = 0;
    ret = get_firmware_size(slave_part, &fw_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse firmware image header: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Actual firmware size: %u bytes (partition: %lu bytes)",
             (unsigned)fw_size, slave_part->size);

    // Perform OTA transfer
    ESP_LOGI(TAG, "Starting slave firmware OTA transfer...");

    uint8_t *chunk = malloc(OTA_CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        free(chunk);
        ESP_LOGE(TAG, "esp_hosted_slave_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total_written = 0;
    int last_progress = -1;
    for (size_t offset = 0; offset < fw_size; offset += OTA_CHUNK_SIZE) {
        size_t len = (offset + OTA_CHUNK_SIZE > fw_size) ? (fw_size - offset) : OTA_CHUNK_SIZE;

        ret = esp_partition_read(slave_part, offset, chunk, len);
        if (ret != ESP_OK) {
            free(chunk);
            ESP_LOGE(TAG, "Partition read failed at offset %u: %s", (unsigned)offset, esp_err_to_name(ret));
            return ret;
        }

        ret = esp_hosted_slave_ota_write(chunk, len);
        if (ret != ESP_OK) {
            free(chunk);
            ESP_LOGE(TAG, "OTA write failed at offset %u: %s", (unsigned)offset, esp_err_to_name(ret));
            return ret;
        }

        total_written += len;
        int progress = (total_written * 100) / fw_size;
        if (progress / 10 != last_progress / 10) {
            ESP_LOGI(TAG, "OTA progress: %d%% (%u/%u bytes)", progress, (unsigned)total_written, (unsigned)fw_size);
            last_progress = progress;
        }
    }

    free(chunk);

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Save target version for activation
    strncpy(s_target_version, embedded_version, sizeof(s_target_version) - 1);
    s_target_version[sizeof(s_target_version) - 1] = '\0';

    s_transfer_completed = true;
    ESP_LOGI(TAG, "OTA transfer completed: %u bytes, version '%s'", (unsigned)total_written, embedded_version);

    return ESP_OK;
}

esp_err_t ota_slave_activate_if_supported(void)
{
    if (!s_transfer_completed) {
        ESP_LOGI(TAG, "No OTA transfer performed, activation not needed");
        return ESP_OK;
    }

    if (!s_activate_supported) {
        ESP_LOGW(TAG, "Slave firmware does not support activate API (< v2.6.0)");
        ESP_LOGW(TAG, "Device will need to restart for new firmware to take effect");
        return ESP_OK;
    }

    // Save OTA pending flag BEFORE activating (in case we get reset during reboot)
    ESP_LOGI(TAG, "Setting OTA pending flag for target version '%s'", s_target_version);
    set_ota_pending(s_target_version);

    ESP_LOGI(TAG, "Activating new slave firmware...");
    esp_err_t ret = esp_hosted_slave_ota_activate();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_activate failed: %s", esp_err_to_name(ret));
        clear_ota_pending();
        return ret;
    }

    // Following ESP-Hosted example: restart host cleanly to avoid SDIO sync issues
    // The C6 is rebooting - if we don't restart, the SDIO driver will crash trying to talk to it
    ESP_LOGI(TAG, "OTA activation sent, restarting host to sync with slave...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Brief delay to let C6 start its reboot
    esp_restart();
    
    // Should never reach here
    return ESP_OK;
}

/*
 * Force-flash path — recovery mode.
 *
 * Modeled after the "crowpanel-p4-c6-sdio-ota" reference tool: skip every
 * verify step and drive the ESP-Hosted slave_ota_* RPCs directly as soon as
 * the SDIO transport is ready. We deliberately do NOT call
 * esp_hosted_get_coprocessor_fwversion() here — if the C6 is bricked in a
 * reboot loop it may never answer that RPC, but the slave_ota_begin RPC is
 * served out of a dedicated path that comes up during C6 bootloader/early
 * init, so OTA can still succeed.
 */
esp_err_t ota_slave_force_flash(void)
{
    ESP_LOGW(TAG, "*** FORCE-FLASH MODE: skipping version verification ***");

    /* Wait for SDIO transport only — no RPC probes. */
    ESP_LOGI(TAG, "Waiting for ESP-Hosted SDIO transport (up to 30s)...");
    int wait = 0;
    while (!is_transport_tx_ready() && wait < 300) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
        if (wait % 20 == 0) {
            ESP_LOGI(TAG, "FORCE: still waiting for transport... (%ds)", wait / 10);
        }
    }
    if (!is_transport_tx_ready()) {
        ESP_LOGE(TAG, "FORCE: transport not ready after 30s, aborting");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "FORCE: transport active");

    /* Unconditionally assume activate is supported. The force path is only
     * useful with modern (>= v2.6.0) slave firmware anyway; if that isn't
     * true the activate RPC will just fail harmlessly. */
    s_activate_supported = true;

    /* Locate the embedded slave firmware partition. */
    const esp_partition_t *slave_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, SLAVE_FW_PARTITION_LABEL);
    if (!slave_part) {
        ESP_LOGE(TAG, "FORCE: slave_fw partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "FORCE: slave_fw partition at 0x%lx, size %lu",
             slave_part->address, slave_part->size);

    /* Minimal sanity check: the first byte must be the ESP32 image magic.
     * We do NOT compare versions. */
    uint8_t magic = 0;
    esp_err_t ret = esp_partition_read(slave_part, 0, &magic, 1);
    if (ret != ESP_OK || magic != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "FORCE: slave_fw has no valid image (magic=0x%02x)", magic);
        return ESP_ERR_INVALID_STATE;
    }

    /* Extract version purely for logging and for the post-OTA NVS bookkeeping
     * so the normal-path check on the next boot stays consistent. */
    char embedded_version[32] = {0};
    char embedded_project[32] = {0};
    if (get_firmware_version(slave_part, embedded_version, sizeof(embedded_version),
                             embedded_project, sizeof(embedded_project)) != ESP_OK) {
        strncpy(embedded_version, "unknown", sizeof(embedded_version) - 1);
    }
    ESP_LOGI(TAG, "FORCE: embedded firmware project='%s' version='%s'",
             embedded_project, embedded_version);

    /* Compute actual binary length (avoid writing 0xFF padding). */
    size_t fw_size = 0;
    ret = get_firmware_size(slave_part, &fw_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FORCE: image header parse failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "FORCE: image size=%u bytes", (unsigned)fw_size);

    uint8_t *chunk = malloc(OTA_CHUNK_SIZE);
    if (!chunk) return ESP_ERR_NO_MEM;

    ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        free(chunk);
        ESP_LOGE(TAG, "FORCE: esp_hosted_slave_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total_written = 0;
    int last_decile = -1;
    for (size_t offset = 0; offset < fw_size; offset += OTA_CHUNK_SIZE) {
        size_t len = (offset + OTA_CHUNK_SIZE > fw_size) ? (fw_size - offset) : OTA_CHUNK_SIZE;

        ret = esp_partition_read(slave_part, offset, chunk, len);
        if (ret != ESP_OK) {
            free(chunk);
            ESP_LOGE(TAG, "FORCE: partition read @%u failed: %s",
                     (unsigned)offset, esp_err_to_name(ret));
            return ret;
        }

        ret = esp_hosted_slave_ota_write(chunk, len);
        if (ret != ESP_OK) {
            free(chunk);
            ESP_LOGE(TAG, "FORCE: ota_write @%u failed: %s",
                     (unsigned)offset, esp_err_to_name(ret));
            return ret;
        }

        total_written += len;
        int decile = (total_written * 10) / fw_size;
        if (decile != last_decile) {
            ESP_LOGI(TAG, "FORCE: OTA %d%% (%u/%u)", decile * 10,
                     (unsigned)total_written, (unsigned)fw_size);
            last_decile = decile;
        }
    }

    free(chunk);

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FORCE: esp_hosted_slave_ota_end failed: %s", esp_err_to_name(ret));
        return ret;
    }

    strncpy(s_target_version, embedded_version, sizeof(s_target_version) - 1);
    s_target_version[sizeof(s_target_version) - 1] = '\0';
    s_transfer_completed = true;

    ESP_LOGI(TAG, "FORCE: transfer complete — %u bytes written, version '%s'",
             (unsigned)total_written, embedded_version);
    return ESP_OK;
}
