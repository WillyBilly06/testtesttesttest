#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAUD_MAGIC 0x44554152u
#define RAUD_VERSION 1

#define RAUD_DISCOVERY_PORT 45600
#define RAUD_CONTROL_PORT 46000

#define RAUD_SOURCE_ID_LEN 16
#define RAUD_CLIENT_ID_LEN 16
#define RAUD_KEY_LEN 32
#define RAUD_NONCE_LEN 16
#define RAUD_TAG_LEN 16
#define RAUD_ROOM_CODE_LEN 9
#define RAUD_ROOM_NAME_LEN 32
#define RAUD_CLIENT_NAME_LEN 32

#define RAUD_CODEC_SBC 1

typedef enum {
    RAUD_MSG_ROOM_ADVERTISE = 1,
    RAUD_MSG_PAIR_REQUEST = 10,
    RAUD_MSG_PAIR_ACCEPT = 11,
    RAUD_MSG_PAIR_REJECT = 12,
    RAUD_MSG_JOIN_HELLO = 20,
    RAUD_MSG_JOIN_ACCEPT = 21,
    RAUD_MSG_JOIN_REJECT = 22,
    RAUD_MSG_HEARTBEAT = 30,
    RAUD_MSG_LEAVE = 31,
    RAUD_MSG_IDENTIFY = 32,
} raud_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t msg_type;
    uint16_t length;
} raud_msg_hdr_t;

typedef struct __attribute__((packed)) {
    raud_msg_hdr_t hdr;
    uint8_t source_id[RAUD_SOURCE_ID_LEN];
    char room_code[RAUD_ROOM_CODE_LEN];
    char room_name[RAUD_ROOM_NAME_LEN];
    uint32_t stream_id;
    uint32_t source_ip;
    uint16_t control_port;
    uint16_t sample_rate_hz;
    uint16_t frame_us;
    uint16_t frame_bytes;
    uint8_t codec;
    uint8_t channels;
    uint8_t bit_depth;
    uint8_t auth_required;
    uint8_t pairing_available;
    char firmware_version[16];
    char device_model[16];
} raud_room_advertise_t;

typedef struct __attribute__((packed)) {
    raud_msg_hdr_t hdr;
    uint8_t source_id[RAUD_SOURCE_ID_LEN];
    char room_code[RAUD_ROOM_CODE_LEN];
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    char client_name[RAUD_CLIENT_NAME_LEN];
    uint8_t client_nonce[RAUD_NONCE_LEN];
    uint8_t pin_proof[RAUD_TAG_LEN];
} raud_pair_request_t;

typedef struct __attribute__((packed)) {
    raud_msg_hdr_t hdr;
    uint8_t source_id[RAUD_SOURCE_ID_LEN];
    char room_code[RAUD_ROOM_CODE_LEN];
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    uint8_t client_nonce[RAUD_NONCE_LEN];
    uint8_t source_nonce[RAUD_NONCE_LEN];
    uint8_t key_wrap_nonce[RAUD_NONCE_LEN];
    uint8_t encrypted_client_auth_key[RAUD_KEY_LEN];
    uint8_t auth_tag[RAUD_TAG_LEN];
} raud_pair_accept_t;

typedef struct __attribute__((packed)) {
    raud_msg_hdr_t hdr;
    uint8_t source_id[RAUD_SOURCE_ID_LEN];
    char room_code[RAUD_ROOM_CODE_LEN];
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    uint8_t client_nonce[RAUD_NONCE_LEN];
    uint16_t audio_port;
    uint8_t requested_codec;
    uint16_t requested_sample_rate_hz;
    uint8_t requested_channels;
    uint8_t requested_bit_depth;
    uint8_t auth_tag[RAUD_TAG_LEN];
} raud_join_hello_t;

typedef struct __attribute__((packed)) {
    raud_msg_hdr_t hdr;
    uint8_t source_id[RAUD_SOURCE_ID_LEN];
    char room_code[RAUD_ROOM_CODE_LEN];
    uint32_t stream_id;
    uint32_t session_id;
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    uint8_t client_nonce[RAUD_NONCE_LEN];
    uint8_t source_nonce[RAUD_NONCE_LEN];
    uint8_t key_wrap_nonce[RAUD_NONCE_LEN];
    uint8_t encrypted_audio_session_key[RAUD_KEY_LEN];
    uint16_t audio_key_len;
    uint8_t codec;
    uint16_t sample_rate_hz;
    uint8_t channels;
    uint8_t bit_depth;
    uint16_t frame_us;
    uint16_t frame_bytes;
    uint32_t audio_start_sequence;
    uint8_t auth_tag[RAUD_TAG_LEN];
} raud_join_accept_t;

typedef struct __attribute__((packed)) {
    raud_msg_hdr_t hdr;
    uint8_t source_id[RAUD_SOURCE_ID_LEN];
    char room_code[RAUD_ROOM_CODE_LEN];
    uint32_t session_id;
    uint8_t client_id[RAUD_CLIENT_ID_LEN];
    uint32_t counter;
    uint8_t auth_tag[RAUD_TAG_LEN];
} raud_client_control_t;

#ifdef __cplusplus
}
#endif
