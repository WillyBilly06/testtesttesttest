/*
 * EspCastBR source-side broadcaster
 *
 * Replaces the old unicast-fanout model with a BIS-style broadcast
 * transmitter. Each audio frame is emitted RTN times at SUB_INTERVAL_US
 * spacing via ESP-NOW broadcast. No ACKs, no peers, no JOIN.
 *
 * See ecast_protocol.h for the full wire format.
 */

#include "ecast_source.h"
#include "ecast_crypto.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *ETAG = "ecast_src";

/* ─── Module state ────────────────────────────────────────────────── */

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint64_t        s_broadcast_id   = 0;
static uint32_t        s_stream_id_full = 0;
static uint16_t        s_stream_id16    = 0;
static char            s_room_name[24]  = {0};
static uint8_t         s_enc_iv[8]      = {0};
static uint8_t         s_key_diversifier[8] = {0};
static uint8_t         s_session_key[16] = {0};
static uint8_t         s_broadcast_code[16] = {0};  /* kept for beacon CMAC */
static bool            s_encrypted      = false;
static volatile uint8_t s_wifi_channel  = 6;

static QueueHandle_t   s_audio_q        = NULL;
static TaskHandle_t    s_scheduler_task = NULL;
static TaskHandle_t    s_beacon_task    = NULL;

volatile uint32_t ecast_tx_ok      = 0;
volatile uint32_t ecast_tx_fail    = 0;
volatile uint32_t ecast_tx_frames  = 0;
volatile uint32_t ecast_tx_dropped = 0;

/* One item pending in the scheduler = one audio frame to transmit RTN times */
typedef struct {
    uint32_t psn;
    uint32_t pres_time_us;
    uint8_t  lc3[ECAST_AUDIO_BYTES];
} ecast_pending_frame_t;

#define ECAST_TX_QUEUE_DEPTH   6   /* small: scheduler drains fast */

static void apply_broadcast_peer_rate(void)
{
    esp_now_rate_config_t rcfg = {
        .phymode = WIFI_PHY_MODE_HT20,
        .rate = WIFI_PHY_RATE_MCS1_LGI,
        .ersu = false,
        .dcm = false,
    };
    esp_err_t err = esp_now_set_peer_rate_config(BROADCAST_MAC, &rcfg);
    if (err != ESP_OK) {
        ESP_LOGW(ETAG, "broadcast peer MCS1 HT20 rate set failed: %s",
                 esp_err_to_name(err));
    }
}

/* ─── Helpers ─────────────────────────────────────────────────────── */

static void ensure_broadcast_peer(void)
{
    if (esp_now_is_peer_exist(BROADCAST_MAC)) {
        apply_broadcast_peer_rate();
        return;
    }

    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, BROADCAST_MAC, 6);
    p.channel = 0;              /* use current */
    p.ifidx   = WIFI_IF_AP;     /* AP iface — matches source_wifi.c */
    p.encrypt = false;          /* our own AES-CCM handles payload */
    esp_err_t err = esp_now_add_peer(&p);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(ETAG, "add broadcast peer: %d", err);
        return;
    }

    apply_broadcast_peer_rate();
}

/* Build a single on-air copy of an audio frame and TX it.
 * The same payload is encrypted RTN times (one per copy_idx) because
 * CCM nonces must differ per encryption, but the plaintext is identical. */
static void tx_one_audio_copy(const ecast_pending_frame_t *pf, uint8_t copy_idx)
{
    uint8_t frame[ECAST_AUDIO_FRAME_SIZE];
    ecast_hdr_t *h = (ecast_hdr_t *)frame;

    h->magic        = ECAST_MAGIC;
    h->ver_type     = ECAST_VER_TYPE(ECAST_TYPE_AUDIO);
    h->stream_id16  = s_stream_id16;
    h->psn          = pf->psn;
    h->pres_time_us = pf->pres_time_us;
    h->copy_idx     = copy_idx;
    h->rtn          = ECAST_RTN;
    h->payload_len  = (uint16_t)(sizeof(ecast_audio_plain_t) + ECAST_MIC_LEN);

    /* Build plaintext audio payload */
    ecast_audio_plain_t plain;
    plain.flags = 0;
    memcpy(plain.lc3, pf->lc3, ECAST_AUDIO_BYTES);

    uint8_t *cipher_out = frame + sizeof(ecast_hdr_t);

    if (s_encrypted) {
        uint8_t nonce[ECAST_NONCE_LEN];
        ecast_make_nonce(nonce, s_enc_iv, pf->psn, copy_idx);
        if (!ecast_ccm_encrypt(s_session_key, nonce,
                               (const uint8_t *)h, sizeof(ecast_hdr_t),
                               (const uint8_t *)&plain, sizeof(plain),
                               cipher_out)) {
            ecast_tx_fail++;
            return;
        }
    } else {
        memcpy(cipher_out, &plain, sizeof(plain));
        /* MIC field is still reserved in the frame size; zero it so the
         * receiver-side MIC presence check is unambiguous. */
        memset(cipher_out + sizeof(plain), 0, ECAST_MIC_LEN);
    }

    esp_err_t err = esp_now_send(BROADCAST_MAC, frame, sizeof(frame));
    if (err != ESP_OK) {
        ecast_tx_fail++;
    } else {
        ecast_tx_ok++;
    }
}

/* Scheduler task: dequeue a pending frame, emit RTN copies at
 * SUB_INTERVAL_US spacing using esp_timer_get_time() for precise pacing. */
static void scheduler_task_fn(void *arg)
{
    (void)arg;
    ecast_pending_frame_t pf;

    while (1) {
        if (xQueueReceive(s_audio_q, &pf, portMAX_DELAY) != pdTRUE) continue;

        int64_t copy_due = esp_timer_get_time();
        for (uint8_t i = 0; i < ECAST_RTN; i++) {
            int64_t now = esp_timer_get_time();
            int64_t wait = copy_due - now;
            if (wait > 1000) {
                /* Sleep in ms; precision to ~1 ms is plenty. */
                vTaskDelay(pdMS_TO_TICKS(wait / 1000));
            } else if (wait > 0) {
                /* Sub-ms: busy-wait (yields not needed — scheduler is pinned). */
                int64_t end = now + wait;
                while (esp_timer_get_time() < end) { /* spin */ }
            }

            tx_one_audio_copy(&pf, i);
            copy_due += ECAST_SUB_INTERVAL_US;
        }
    }
}

/* Beacon task: once per ECAST_BEACON_PERIOD_MS broadcast the discovery info */
static void beacon_task_fn(void *arg)
{
    (void)arg;

    ensure_broadcast_peer();

    /* Beacon frame is plaintext (no AAD needed). Use plain magic + psn=0 in
     * header; receivers only process audio psns for dedupe. */
    uint8_t buf[ECAST_BEACON_FRAME_SIZE];
    ecast_hdr_t *h = (ecast_hdr_t *)buf;
    ecast_beacon_payload_t *b = (ecast_beacon_payload_t *)(buf + sizeof(ecast_hdr_t));
    uint8_t *tag = buf + sizeof(ecast_hdr_t) + sizeof(ecast_beacon_payload_t);

    uint32_t beacon_seq = 0;

    while (1) {
        /* Header (signed by CMAC tag below, not encrypted) */
        h->magic        = ECAST_MAGIC;
        h->ver_type     = ECAST_VER_TYPE(ECAST_TYPE_BEACON);
        h->stream_id16  = s_stream_id16;
        h->psn          = beacon_seq++;
        h->pres_time_us = (uint32_t)esp_timer_get_time();
        h->copy_idx     = 0;
        h->rtn          = 1;
        h->payload_len  = sizeof(ecast_beacon_payload_t) + ECAST_BEACON_TAG_LEN;

        /* Beacon payload */
        b->broadcast_id         = s_broadcast_id;
        b->stream_id_full       = s_stream_id_full;
        b->pres_delay_us        = ECAST_PRES_DELAY_US;
        b->frame_us             = ECAST_LC3_FRAME_US;
        b->sample_rate_hz       = ECAST_SAMPLE_RATE_HZ;
        b->channels             = ECAST_CHANNELS;
        b->lc3_bytes_per_ch     = ECAST_LC3_BYTES_PER_CH;
        b->wifi_channel         = s_wifi_channel;
        b->rtn                  = ECAST_RTN;
        b->sub_interval_us_x100 = (uint8_t)(ECAST_SUB_INTERVAL_US / 100);
        b->is_encrypted         = s_encrypted ? 1 : 0;
        memcpy(b->enc_iv,          s_enc_iv,          8);
        memcpy(b->key_diversifier, s_key_diversifier, 8);
        memset(b->name, 0, sizeof(b->name));
        strncpy(b->name, s_room_name, sizeof(b->name) - 1);

        /* Authentication tag — only holders of the shared Broadcast_Code
         * can produce a valid tag, so only our sink will accept this room. */
        size_t body_len = sizeof(ecast_hdr_t) + sizeof(ecast_beacon_payload_t);
        if (!ecast_beacon_sign(s_broadcast_code, buf, body_len, tag)) {
            ESP_LOGE(ETAG, "beacon_sign failed");
            vTaskDelay(pdMS_TO_TICKS(ECAST_BEACON_PERIOD_MS));
            continue;
        }

        esp_err_t err = esp_now_send(BROADCAST_MAC, buf, sizeof(buf));
        if (err != ESP_OK) {
            ESP_LOGW(ETAG, "beacon send: %d", err);
        }

        vTaskDelay(pdMS_TO_TICKS(ECAST_BEACON_PERIOD_MS));
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */

bool ecast_source_init(uint64_t broadcast_id,
                       uint32_t stream_id_full,
                       const char *room_name,
                       const uint8_t broadcast_code[16],
                       bool enable_encryption)
{
    s_broadcast_id   = broadcast_id;
    s_stream_id_full = stream_id_full;
    s_stream_id16    = (uint16_t)(stream_id_full & 0xFFFFU);
    s_encrypted      = enable_encryption;
    memcpy(s_broadcast_code, broadcast_code, 16);

    memset(s_room_name, 0, sizeof(s_room_name));
    if (room_name) strncpy(s_room_name, room_name, sizeof(s_room_name) - 1);

    /* Randomize per-boot crypto material (like BIS GSKD/GIV) */
    for (int i = 0; i < 8; i++) {
        s_enc_iv[i]          = (uint8_t)(esp_random() & 0xFF);
        s_key_diversifier[i] = (uint8_t)(esp_random() & 0xFF);
    }

    if (enable_encryption) {
        if (!ecast_derive_session_key(broadcast_code, s_key_diversifier, s_session_key)) {
            ESP_LOGE(ETAG, "session key derivation failed");
            return false;
        }
    }

    s_audio_q = xQueueCreate(ECAST_TX_QUEUE_DEPTH, sizeof(ecast_pending_frame_t));
    if (!s_audio_q) {
        ESP_LOGE(ETAG, "queue alloc failed");
        return false;
    }

    /* Scheduler at high priority so RTN copies aren't preempted.
     * Pinned to core 0 (same as old audio_send_task). */
    BaseType_t rc = xTaskCreatePinnedToCore(scheduler_task_fn, "ecast_sched",
                                            4096, NULL, 10, &s_scheduler_task, 0);
    if (rc != pdPASS) {
        ESP_LOGE(ETAG, "scheduler task create failed");
        return false;
    }

    /* Priority 12: above ecast_sched (10) and the audio decoder/UDP tasks
     * but still well below lc3_emit (22), so beacons get out reliably on
     * single-core targets without disturbing audio cadence. The beacon work
     * itself (build + CMAC + esp_now_send) is ~1 ms once per 100 ms. */
    rc = xTaskCreatePinnedToCore(beacon_task_fn, "ecast_beacon",
                                 3072, NULL, 12, &s_beacon_task, 0);
    if (rc != pdPASS) {
        ESP_LOGE(ETAG, "beacon task create failed");
        return false;
    }

    /* Track current WiFi channel so beacons advertise it accurately. */
    uint8_t primary = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &sec);
    if (primary >= 1 && primary <= 14) s_wifi_channel = primary;

    ESP_LOGI(ETAG,
        "broadcaster up: room='%s' broadcast_id=%016llX stream=%08lX "
        "encrypted=%d RTN=%d sub=%dus pres_delay=%dus ch=%u",
        s_room_name, (unsigned long long)s_broadcast_id,
        (unsigned long)s_stream_id_full, (int)s_encrypted,
        ECAST_RTN, ECAST_SUB_INTERVAL_US, ECAST_PRES_DELAY_US,
        (unsigned)s_wifi_channel);

    return true;
}

void ecast_source_publish_audio(const uint8_t *lc3_payload, uint32_t capture_us)
{
    if (!s_audio_q) return;

    static uint32_t s_next_psn = 1;   /* 0 reserved for beacon / init */

    ecast_pending_frame_t pf;
    pf.psn          = s_next_psn++;
    pf.pres_time_us = capture_us + ECAST_PRES_DELAY_US;
    memcpy(pf.lc3, lc3_payload, ECAST_AUDIO_BYTES);

    ecast_tx_frames++;

    /* Non-blocking; drop-oldest policy so steady-state latency stays
     * bounded even if scheduler transiently lags. */
    if (xQueueSend(s_audio_q, &pf, 0) != pdTRUE) {
        ecast_pending_frame_t drop;
        (void)xQueueReceive(s_audio_q, &drop, 0);
        (void)xQueueSend(s_audio_q, &pf, 0);
        ecast_tx_dropped++;
    }
}

void ecast_source_set_channel(uint8_t channel)
{
    if (channel >= 1 && channel <= 14) s_wifi_channel = channel;
}
