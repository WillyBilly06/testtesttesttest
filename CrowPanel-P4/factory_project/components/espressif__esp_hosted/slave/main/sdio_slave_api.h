/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SDIO_SLAVE_API_H
#define __SDIO_SLAVE_API_H

#if CONFIG_SOC_SDIO_SLAVE_SUPPORTED
#else
    #error "SDIO is not supported for this target. Please use SPI"
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cumulative counter of silent drops inside the SDIO slave TX path.
 * Surfaced to the host (P4) via the periodic stats event so audio
 * glitches caused by SDIO TX exhaustion become visible without UART
 * access to the C6. See definition in sdio_slave_api.c for full
 * details. */
uint32_t sdio_slave_get_tx_silent_drops(void);

#ifdef __cplusplus
}
#endif

#endif
