#include "source_udp_stream.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "source_config.h"
#include "source_udp_control.h"
#include "source_wifi.h"

typedef struct {
    source_udp_audio_packet_t pkt;
    size_t send_len;
} udp_queue_item_t;

static const char *TAG = "source_udp";
static QueueHandle_t s_udp_q;
static volatile uint16_t s_udp_port = SOURCE_DEFAULT_UDP_PORT;
static volatile uint32_t s_udp_sent;
static volatile uint32_t s_udp_queue_drops;
static volatile uint32_t s_udp_send_errors;

static void close_udp_socket(int *sock)
{
    if (sock && *sock >= 0) {
        close(*sock);
        *sock = -1;
    }
}

static int open_udp_socket(uint32_t local_ip, uint16_t local_port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return -1;
    }
    uint8_t ttl = 1;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(local_port);
    bind_addr.sin_addr.s_addr = local_ip;
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGW(TAG, "audio bind failed %u.%u.%u.%u:%u errno=%d",
                 (uint8_t)(ntohl(local_ip) >> 24),
                 (uint8_t)(ntohl(local_ip) >> 16),
                 (uint8_t)(ntohl(local_ip) >> 8),
                 (uint8_t)ntohl(local_ip),
                 (unsigned)local_port, errno);
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "UDP audio source bound to %u.%u.%u.%u:%u",
             (uint8_t)(ntohl(local_ip) >> 24),
             (uint8_t)(ntohl(local_ip) >> 16),
             (uint8_t)(ntohl(local_ip) >> 8),
             (uint8_t)ntohl(local_ip),
             (unsigned)local_port);
    return sock;
}

static void udp_stream_task(void *arg)
{
    (void)arg;
    int sock = -1;
    uint32_t bound_ip = 0;
    uint16_t bound_port = 0;
    udp_queue_item_t item;

    while (1) {
        if (xQueueReceive(s_udp_q, &item, pdMS_TO_TICKS(250)) != pdTRUE) {
            continue;
        }

        if (!source_wifi_is_sta_connected()) {
            close_udp_socket(&sock);
            bound_ip = 0;
            bound_port = 0;
            continue;
        }
        uint32_t sta_ip = source_wifi_sta_ip();
        uint16_t udp_port = s_udp_port;
        if (sta_ip == 0 || udp_port == 0) {
            continue;
        }
        if (sock < 0 || bound_ip != sta_ip || bound_port != udp_port) {
            close_udp_socket(&sock);
            sock = open_udp_socket(sta_ip, udp_port);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            bound_ip = sta_ip;
            bound_port = udp_port;
        }

        struct sockaddr_in dest = {0};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(udp_port);
        dest.sin_addr.s_addr = inet_addr(SOURCE_UDP_MULTICAST_ADDR);

        int sent = sendto(sock, &item.pkt, item.send_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
        if (sent < 0) {
            s_udp_send_errors++;
        } else {
            s_udp_sent++;
        }

        source_udp_audio_dest_t active[SOURCE_UDP_MAX_AUTH_CLIENTS];
        int active_count = source_udp_control_get_audio_dests(active, SOURCE_UDP_MAX_AUTH_CLIENTS);
        for (int i = 0; i < active_count; ++i) {
            struct sockaddr_in client = {0};
            client.sin_family = AF_INET;
            client.sin_port = htons(active[i].port);
            client.sin_addr.s_addr = active[i].addr;
            sent = sendto(sock, &item.pkt, item.send_len, 0,
                          (struct sockaddr *)&client, sizeof(client));
            if (sent < 0) {
                s_udp_send_errors++;
            } else {
                s_udp_sent++;
            }
        }

    }
}

esp_err_t source_udp_stream_start(void)
{
    if (s_udp_q) {
        return ESP_OK;
    }
    s_udp_q = xQueueCreate(SOURCE_UDP_QUEUE_DEPTH, sizeof(udp_queue_item_t));
    if (!s_udp_q) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(udp_stream_task, "udp_audio", 4096,
                                            NULL, 20, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void source_udp_stream_set_port(uint16_t port)
{
    if (port != 0) {
        s_udp_port = port;
    }
}

void source_udp_stream_submit(const source_udp_audio_packet_t *pkt, size_t payload_len)
{
    if (!s_udp_q || !pkt || payload_len > SOURCE_UDP_MAX_PAYLOAD) {
        return;
    }
    udp_queue_item_t item = {0};
    item.pkt = *pkt;
    item.send_len = offsetof(source_udp_audio_packet_t, payload) + payload_len;
    if (xQueueSend(s_udp_q, &item, 0) == pdTRUE) {
        return;
    }

    udp_queue_item_t old;
    if (xQueueReceive(s_udp_q, &old, 0) == pdTRUE) {
        s_udp_queue_drops++;
    }
    if (xQueueSend(s_udp_q, &item, 0) != pdTRUE) {
        s_udp_queue_drops++;
    }
}

void source_udp_stream_get_stats(source_udp_stats_t *out)
{
    if (!out) {
        return;
    }
    out->packets_sent = s_udp_sent;
    out->queue_level = s_udp_q ? uxQueueMessagesWaiting(s_udp_q) : 0;
    out->queue_drops = s_udp_queue_drops;
    out->send_errors = s_udp_send_errors;
}
