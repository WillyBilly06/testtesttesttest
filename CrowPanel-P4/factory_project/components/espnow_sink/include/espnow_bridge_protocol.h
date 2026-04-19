/*
 * ESP-NOW Bridge Protocol - Shared between C6 (slave) and P4 (host)
 * Defines message IDs and packet formats for tunneling ESP-NOW over SDIO
 */
#pragma once

#include <stdint.h>
#include <string.h>

/* Custom data message IDs for ESP-NOW bridge */
#define ESPNOW_BRIDGE_MSG_INIT          0x100
#define ESPNOW_BRIDGE_MSG_INIT_RESP     0x101
#define ESPNOW_BRIDGE_MSG_SEND          0x102
#define ESPNOW_BRIDGE_MSG_SEND_RESP     0x103
#define ESPNOW_BRIDGE_MSG_RECV          0x104
#define ESPNOW_BRIDGE_MSG_SET_CHANNEL   0x105
#define ESPNOW_BRIDGE_MSG_SET_CHANNEL_RESP 0x106
#define ESPNOW_BRIDGE_MSG_ADD_PEER      0x107
#define ESPNOW_BRIDGE_MSG_ADD_PEER_RESP 0x108
#define ESPNOW_BRIDGE_MSG_DEL_PEER      0x109
#define ESPNOW_BRIDGE_MSG_DEL_PEER_RESP 0x10A
#define ESPNOW_BRIDGE_MSG_DEINIT        0x10B
#define ESPNOW_BRIDGE_MSG_DEINIT_RESP   0x10C

/* Maximum ESP-NOW payload */
#define ESPNOW_BRIDGE_MAX_PAYLOAD       250

/* Result codes */
#define ESPNOW_BRIDGE_OK                0
#define ESPNOW_BRIDGE_FAIL              1

/* --- Packet structures (all little-endian, packed) --- */

typedef struct __attribute__((packed)) {
    uint8_t result;       /* 0=OK, 1=FAIL */
} espnow_bridge_resp_t;

/* SEND: P4->C6 - send an ESP-NOW packet */
typedef struct __attribute__((packed)) {
    uint8_t  dest_mac[6];
    uint16_t data_len;
    uint8_t  data[ESPNOW_BRIDGE_MAX_PAYLOAD];
} espnow_bridge_send_t;

/* RECV: C6->P4 - received an ESP-NOW packet */
typedef struct __attribute__((packed)) {
    uint8_t  src_mac[6];
    uint8_t  dest_mac[6];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t data_len;
    uint8_t  data[ESPNOW_BRIDGE_MAX_PAYLOAD];
} espnow_bridge_recv_t;

/* SET_CHANNEL: P4->C6 */
typedef struct __attribute__((packed)) {
    uint8_t channel;
    uint8_t secondary;  /* WIFI_SECOND_CHAN_NONE=0 */
} espnow_bridge_set_channel_t;

/* ADD_PEER: P4->C6 */
typedef struct __attribute__((packed)) {
    uint8_t peer_addr[6];
    uint8_t channel;
    uint8_t ifidx;      /* ESP_IF_WIFI_STA=0 */
    uint8_t encrypt;
} espnow_bridge_add_peer_t;

/* DEL_PEER: P4->C6 */
typedef struct __attribute__((packed)) {
    uint8_t peer_addr[6];
} espnow_bridge_del_peer_t;
