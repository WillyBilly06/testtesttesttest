/*
 * C6 ESP-NOW Sink - header
 *
 * Handles ESP-NOW room scanning, joining, LC3 decoding on C6.
 * Communicates with P4 host via ESP-Hosted custom data channel.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the ESP-NOW sink on C6
 *
 * Registers SDIO custom data handlers for P4 commands.
 * Does NOT start WiFi/ESP-NOW until CMD_INIT is received from P4.
 */
esp_err_t espnow_sink_c6_init(void);

#ifdef __cplusplus
}
#endif
