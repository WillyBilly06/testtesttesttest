 /*
 * C6 EspCastBR receive bridge — pure RX.
 *
 * Responsibilities:
 *   1. Init WiFi (STA mode, channel 6, no power save) and ESP-NOW.
 *   2. Register an ESP-NOW receive callback that filters ECast frames:
 *        - Beacons: verify AES-CMAC auth tag with the shared Broadcast_Code.
 *          Drop any beacon from a source that doesn't know our secret.
 *          Forward authenticated beacons verbatim to P4 over SDIO.
 *        - Audio:   de-duplicate by (stream_id16, psn). Forward the FIRST
 *          copy of each psn verbatim to P4 — later copies are redundant.
 *   3. Never TX. No JOIN, no keepalive, no peer management.
 *
 * This decoupling means: C6 only needs the Broadcast_Code to authenticate
 * beacons; the P4 does AES-CCM payload decryption + LC3 + playback.
 */

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "esp_app_desc.h"
#include "esp_hosted_peer_data.h"
#include "espnow_protocol.h"
#include "espnow_sink_c6.h"
#include "ecast_protocol.h"
#include "ecast_crypto.h"

static const char *TAG = "ecast_c6";

/* Pinning: channel 6 is the convention shared with the source. Source
 * dynamically adjusts to its STA channel, and advertises it in the beacon;
 * once the P4 picks a room it can CMD_ECAST_SET_CH us to match. Until
 * then we camp on 6. */
#define ECAST_INITIAL_CHANNEL 6

/* ─── State ───────────────────────────────────────────────────────── */

static uint8_t s_broadcast_code[16] = ECAST_BROADCAST_CODE_BYTES;
static volatile uint32_t s_last_fwd_psn = 0;   /* dedupe: last audio psn we forwarded */
static volatile uint16_t s_last_fwd_sid = 0;   /* paired stream_id16 */
static volatile int8_t   s_last_rssi    = 0;

/* Stats (periodically shipped to P4 via EVT_ECAST_RSSI) */
static volatile uint32_t s_beacons_ok   = 0;
static volatile uint32_t s_beacons_bad  = 0;
static volatile uint32_t s_audio_rx     = 0;
static volatile uint32_t s_audio_dupe   = 0;
static volatile uint32_t s_sdio_errors  = 0;

/* ─── Helpers ─────────────────────────────────────────────────────── */

static void sdio_send(uint32_t msg_id, const void *data, size_t len)
{
    esp_err_t err = esp_hosted_send_custom_data(msg_id, (const uint8_t *)data, len);
    if (err != ESP_OK) {
        s_sdio_errors++;
    }
}

static void send_status(uint8_t state)
{
    espnow_evt_status_t s = {
        .state = state,
        .wifi_init = 1,
        .espnow_init = 1,
        .reserved = 0,
    };
    sdio_send(ESPNOW_MSG_EVT_STATUS, &s, sizeof(s));
}

static void send_rx_stats(void)
{
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &sec);

    espnow_evt_stats_t st = {
        .packets_rx       = s_audio_rx,
        .packets_lost     = s_audio_dupe,      /* reuse — interpreted as "dupes" on P4 */
        .plc_frames       = s_beacons_ok,      /* reuse — "authenticated beacons" */
        .rssi_last        = s_last_rssi,
        .wifi_channel     = ch,
        .sdio_send_errors = (uint8_t)(s_sdio_errors > 0xFF ? 0xFF : s_sdio_errors),
        .reserved         = 0,
    };
    sdio_send(ESPNOW_MSG_EVT_ECAST_RSSI, &st, sizeof(st));
}

/* ─── ESP-NOW receive callback ───────────────────────────────────── */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (!info || !data || len < (int)sizeof(ecast_hdr_t)) return;

    const ecast_hdr_t *h = (const ecast_hdr_t *)data;
    if (h->magic != ECAST_MAGIC) return;
    if (ECAST_VER_OF(h->ver_type) != ECAST_VERSION) return;

    uint8_t type = ECAST_TYPE_OF(h->ver_type);
    if (info->rx_ctrl) s_last_rssi = info->rx_ctrl->rssi;

    if (type == ECAST_TYPE_BEACON) {
        if (len != (int)ECAST_BEACON_FRAME_SIZE) return;

        /* Verify CMAC tag: only authentic source beacons pass through. */
        size_t body_len = sizeof(ecast_hdr_t) + sizeof(ecast_beacon_payload_t);
        const uint8_t *tag = data + body_len;
        if (!ecast_beacon_verify(s_broadcast_code, data, body_len, tag)) {
            s_beacons_bad++;
            return;
        }

        s_beacons_ok++;
        sdio_send(ESPNOW_MSG_EVT_ECAST_BEACON, data, len);
        return;
    }

    if (type == ECAST_TYPE_AUDIO) {
        if (len != (int)ECAST_AUDIO_FRAME_SIZE) return;

        /* Dedupe: forward only the first copy of each psn. Later copies are
         * the BIS-style scheduled redundancy and would just waste SDIO. */
        if (h->stream_id16 == s_last_fwd_sid && h->psn == s_last_fwd_psn) {
            s_audio_dupe++;
            return;
        }
        s_last_fwd_sid = h->stream_id16;
        s_last_fwd_psn = h->psn;

        s_audio_rx++;
        sdio_send(ESPNOW_MSG_EVT_ECAST_AUDIO, data, len);
        return;
    }
}

/* ─── P4 → C6 command handler ─────────────────────────────────────── */

static void cmd_fw_ver_cb(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id; (void)data; (void)len;
    espnow_evt_fw_ver_t fw = {0};
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) {
        strncpy(fw.version, d->version,      sizeof(fw.version) - 1);
        strncpy(fw.project, d->project_name, sizeof(fw.project) - 1);
    }
    sdio_send(ESPNOW_MSG_EVT_FW_VER, &fw, sizeof(fw));
}

static void cmd_get_status_cb(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id; (void)data; (void)len;
    send_status(ESPNOW_STATE_IDLE);
}

static void cmd_set_channel_cb(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id;
    if (len < 1 || !data) return;
    uint8_t ch = data[0];
    if (ch < 1 || ch > 14) return;
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "set_channel(%u) -> %s", (unsigned)ch, esp_err_to_name(err));
}

static void cmd_legacy_noop_cb(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id; (void)data; (void)len;
    /* Legacy init/deinit/scan/join/leave commands — acknowledged with a
     * status event so older P4 UIs keep working, but we're always in
     * "IDLE and always-listening" mode now. */
    send_status(ESPNOW_STATE_IDLE);
}

/* ─── Stats task ──────────────────────────────────────────────────── */

static void stats_task_fn(void *arg)
{
    (void)arg;
    while (1) {
        send_rx_stats();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ─── Public init (called from app_main) ──────────────────────────── */

/* Deferred ECast bring-up: runs only after the host RPC has called
 * esp_wifi_init + esp_wifi_start on the slave. Calling esp_now_init()
 * (or any esp_wifi_* setter) at boot — before the host's RPC has
 * inited WiFi — fails with ESP_ERR_ESPNOW_NOT_INIT / WIFI_NOT_INIT and
 * aborts app_main, which is why the ESP-Hosted "slave ready" handshake
 * would never complete. */
static void ecast_bringup_once(void)
{
    static bool s_up = false;
    if (s_up) return;
    s_up = true;

    /* Best-effort tuning knobs; ignore errors (we don't control the host's
     * mode transitions here). */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(84);
    esp_wifi_set_channel(ECAST_INITIAL_CHANNEL, WIFI_SECOND_CHAN_NONE);

    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_INTERNAL) {
        ESP_LOGE(TAG, "esp_now_init deferred: %s", esp_err_to_name(err));
        return;
    }

    esp_now_register_recv_cb(espnow_recv_cb);

    /* Add broadcast peer so the driver accepts FF:FF:FF:FF:FF:FF traffic */
    esp_now_peer_info_t p = {0};
    memset(p.peer_addr, 0xFF, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);

    xTaskCreate(stats_task_fn, "ecast_stats", 3072, NULL, 2, NULL);

    send_status(ESPNOW_STATE_IDLE);
    ESP_LOGI(TAG, "ECast C6 bridge up (ch=%d)", ECAST_INITIAL_CHANNEL);
}

static void wifi_evt_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    /* STA_START is the first event we get after host's RPC esp_wifi_start.
     * WIFI_READY also works but may fire before STA mode is selected. */
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_AP_START) {
        ecast_bringup_once();
    }
}

esp_err_t espnow_sink_c6_init(void)
{
    /* Register SDIO command handlers up-front — these don't depend on
     * WiFi and the host may send CMD_GET_FW_VER as soon as the RPC
     * transport is ready. */
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_GET_FW_VER,  cmd_fw_ver_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_GET_STATUS,  cmd_get_status_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_ECAST_SET_CH,cmd_set_channel_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_INIT,        cmd_legacy_noop_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_DEINIT,      cmd_legacy_noop_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_SCAN,        cmd_legacy_noop_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_STOP_SCAN,   cmd_legacy_noop_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_JOIN,        cmd_legacy_noop_cb);
    esp_hosted_register_custom_callback(ESPNOW_MSG_CMD_LEAVE,       cmd_legacy_noop_cb);

    /* Defer ESP-NOW / WiFi calls until the host has inited+started WiFi. */
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_evt_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event handler register: %s", esp_err_to_name(err));
        /* Non-fatal: don't abort app_main, hosted transport must come up. */
    }

    ESP_LOGI(TAG, "ECast C6 bridge: waiting for host to start WiFi…");
    return ESP_OK;
}
