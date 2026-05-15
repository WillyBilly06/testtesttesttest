#include "source_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "source_config.h"

#define SETUP_AP_SSID "CP-ROOM-SOURCE-SETUP"
#define SETUP_AP_PASS "configure123"
#define CONNECTED_BIT BIT0
#define FAILED_BIT BIT1
#define MAX_WIFI_TX_POWER_QDBM 84

static const char *TAG = "source_wifi";

static EventGroupHandle_t s_wifi_events;
static uint8_t s_setup_channel = 1;
static volatile bool s_sta_connected;
static volatile uint8_t s_current_channel;
static volatile bool s_audio_streaming;
static volatile uint32_t s_sta_ip;
static char s_status[128] = "starting";

static void set_status(const char *status)
{
    snprintf(s_status, sizeof(s_status), "%s", status ? status : "");
}

const char *source_wifi_status_text(void)
{
    return s_status;
}

bool source_wifi_is_sta_connected(void)
{
    return s_sta_connected;
}

uint32_t source_wifi_sta_ip(void)
{
    return s_sta_ip;
}

uint8_t source_wifi_current_channel(void)
{
    return s_current_channel;
}

void source_wifi_set_audio_streaming(bool streaming)
{
    s_audio_streaming = streaming;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA associated with AP (requesting DHCP address…)");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip = 0;
        set_status("STA disconnected; setup AP available");
        xEventGroupSetBits(s_wifi_events, FAILED_BIT);
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        s_sta_ip = event ? event->ip_info.ip.addr : 0;
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        (void)esp_wifi_get_channel(&ch, &sec);
        s_current_channel = ch;
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        set_status("connected STA-only");
        ESP_LOGI(TAG, "STA IP " IPSTR " — Web UI: http://" IPSTR "/",
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
    }
}

static void start_setup_ap(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", SETUP_AP_SSID);
    ap.ap.ssid_len = strlen(SETUP_AP_SSID);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", SETUP_AP_PASS);
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.channel = s_setup_channel;
    ap.ap.max_connection = 1;
    ap.ap.beacon_interval = 1000;
    ap.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "Setup AP active SSID=%s pass=%s channel=%u",
             SETUP_AP_SSID, SETUP_AP_PASS, s_setup_channel);
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
            ESP_LOGI(TAG, "Setup AP IP " IPSTR " — Web UI: http://" IPSTR "/",
                     IP2STR(&ip.ip), IP2STR(&ip.ip));
        }
    }
}

static esp_err_t find_ssid_channel(const char *ssid, uint8_t *channel)
{
    wifi_scan_config_t scan = {
        .ssid = (uint8_t *)ssid,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), TAG, "scan count");
    if (count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    wifi_ap_record_t best = {0};
    bool found = false;
    for (uint16_t i = 0; i < count; ++i) {
        wifi_ap_record_t rec = {0};
        if (esp_wifi_scan_get_ap_record(&rec) != ESP_OK) {
            continue;
        }
        if (strcmp((const char *)rec.ssid, ssid) == 0 &&
            (!found || rec.rssi > best.rssi)) {
            best = rec;
            found = true;
        }
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    *channel = best.primary;
    return ESP_OK;
}

static esp_err_t connect_sta(const source_config_t *cfg)
{
    if (!cfg || cfg->wifi_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t channel = 0;
    esp_err_t scan_err = find_ssid_channel(cfg->wifi_ssid, &channel);
    if (scan_err != ESP_OK) {
        set_status("saved Wi-Fi SSID not found; setup AP active");
        return scan_err;
    }
    wifi_config_t sta = {0};
    size_t ssid_len = strlen(cfg->wifi_ssid);
    if (ssid_len > sizeof(sta.sta.ssid)) {
        ssid_len = sizeof(sta.sta.ssid);
    }
    memcpy(sta.sta.ssid, cfg->wifi_ssid, ssid_len);
    size_t pass_len = strlen(cfg->wifi_pass);
    if (pass_len > sizeof(sta.sta.password)) {
        pass_len = sizeof(sta.sta.password);
    }
    memcpy(sta.sta.password, cfg->wifi_pass, pass_len);
    sta.sta.channel = channel;
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, CONNECTED_BIT | FAILED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s on channel %u", cfg->wifi_ssid, channel);
        return ESP_OK;
    }
    set_status("Wi-Fi connect timeout; setup AP active");
    return ESP_ERR_TIMEOUT;
}

esp_err_t source_wifi_start(uint8_t espnow_channel)
{
    s_setup_channel = espnow_channel;
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        return ev;
    }
    (void)esp_netif_create_default_wifi_sta();
    (void)esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    source_config_t app_cfg;
    source_config_get(&app_cfg);
    bool has_saved_wifi = app_cfg.wifi_ssid[0] != '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(has_saved_wifi ? WIFI_MODE_STA : WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    (void)esp_wifi_set_protocol(WIFI_IF_STA,
                                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    (void)esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    (void)esp_wifi_set_max_tx_power(MAX_WIFI_TX_POWER_QDBM);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (!has_saved_wifi) {
        ESP_ERROR_CHECK(esp_wifi_set_channel(s_setup_channel, WIFI_SECOND_CHAN_NONE));
    }

    bool setup_needed = true;
    if (has_saved_wifi) {
        if (connect_sta(&app_cfg) == ESP_OK) {
            setup_needed = false;
        }
    }
    if (setup_needed) {
        start_setup_ap();
        set_status(app_cfg.wifi_ssid[0] ? s_status : "setup AP active; no Wi-Fi configured");
    }

    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    (void)esp_wifi_get_channel(&ch, &sec);
    s_current_channel = ch;
    return ESP_OK;
}

esp_err_t source_wifi_get_sta_mac(uint8_t mac[6])
{
    return esp_wifi_get_mac(WIFI_IF_STA, mac);
}

esp_err_t source_wifi_save_credentials_if_channel_ok(const char *ssid, const char *pass,
                                                     char *err_buf, size_t err_buf_len)
{
    if (!ssid || ssid[0] == '\0') {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "SSID is required");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_streaming && s_sta_connected) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "busy streaming; Wi-Fi scan rejected");
        }
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t channel = 0;
    esp_err_t err = find_ssid_channel(ssid, &channel);
    if (err != ESP_OK) {
        if (err_buf && err_buf_len) {
            snprintf(err_buf, err_buf_len, "SSID not found");
        }
        return err;
    }
    err = source_config_set_wifi(ssid, pass);
    if (err == ESP_OK && err_buf && err_buf_len) {
        snprintf(err_buf, err_buf_len, "Saved channel %u. Reboot source to join %s.", channel, ssid);
    }
    return err;
}
