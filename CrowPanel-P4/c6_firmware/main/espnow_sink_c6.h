/*
 * C6 room verification bridge - header
 *
 * Handles ESP-NOW room scanning and authenticated room joins on C6.
 * Sends verified nRF24 audio parameters to the P4 over ESP-Hosted SDIO.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the room verification bridge on C6
 *
 * Registers SDIO custom data handlers for P4 commands.
 * WiFi/ESP-NOW setup is deferred until ESP-Hosted starts WiFi or CMD_INIT is received.
 */
esp_err_t espnow_sink_c6_init(void);

#ifdef __cplusplus
}
#endif
