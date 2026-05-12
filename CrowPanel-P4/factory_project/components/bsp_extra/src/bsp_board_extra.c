/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "bsp_extra_board";

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;

static bool _is_audio_init = false;
static bool _is_player_init = false;
static int _vloume_intensity = CODEC_DEFAULT_VOLUME;
static bsp_extra_audio_output_route_t s_output_route = BSP_EXTRA_AUDIO_OUTPUT_AUX;
static i2s_chan_handle_t s_aux_tx_chan = NULL;
static uint32_t s_aux_sample_rate = CODEC_DEFAULT_SAMPLE_RATE;
static uint32_t s_aux_bits = CODEC_DEFAULT_BIT_WIDTH;
static i2s_slot_mode_t s_aux_slot_mode = I2S_SLOT_MODE_STEREO;

static audio_player_cb_t audio_idle_callback = NULL;
static void *audio_idle_cb_user_data = NULL;
static char audio_file_path[128];

static inline int16_t aux_scale_sample_i16(int16_t sample, int volume)
{
    int32_t scaled = ((int32_t)sample * volume) / 100;
    if (scaled > INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

/* Scale a 24-bit sample stored left-aligned in a 32-bit slot (s24<<8).
 * The codec/I2S peripheral uses the upper 24 bits and ignores the lower
 * 8, so we scale on int64 to avoid overflow then re-clamp into int32. */
static inline int32_t aux_scale_sample_s24_in_s32(int32_t sample, int volume)
{
    int64_t scaled = ((int64_t)sample * volume) / 100;
    if (scaled > INT32_MAX) {
        return INT32_MAX;
    }
    if (scaled < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)scaled;
}

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)audio_buffer;
    (void)len;
    (void)timeout_ms;
    if (bytes_read) {
        *bytes_read = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)audio_buffer;
    (void)len;
    (void)timeout_ms;
    if (bytes_written) {
        *bytes_written = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    s_aux_sample_rate = rate;
    s_aux_bits = bits_cfg;
    s_aux_slot_mode = ch;
    /* AUX I2S is owned exclusively by espnow_sink on I2S_NUM_0. */

    return ESP_OK;
}

esp_err_t bsp_extra_output_route_set(bsp_extra_audio_output_route_t route)
{
    s_output_route = route;
    return ESP_OK;
}

bsp_extra_audio_output_route_t bsp_extra_output_route_get(void)
{
    return s_output_route;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    _vloume_intensity = volume;
    if (volume_set) {
        *volume_set = volume;
    }

    ESP_LOGI(TAG, "AUX-only volume setting: %d", volume);

    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    (void)enable;
    return ESP_OK;
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    return ESP_OK;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t bsp_extra_codec_init()
{
    play_dev_handle = NULL;
    record_dev_handle = NULL;
    _is_audio_init = true;
    ESP_LOGI(TAG, "BSP speaker/mic codec disabled; ESP-NOW AUX owns I2S_NUM_0");

    return ESP_OK;
}

esp_err_t bsp_extra_player_init(void)
{
    _is_player_init = false;
    ESP_LOGW(TAG, "BSP audio player disabled while assistive AUX owns I2S_NUM_0");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_extra_player_del(void)
{
    _is_player_init = false;
    return ESP_OK;
}

esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG, "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    (void)instance;
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    (void)file_path;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    audio_idle_callback = cb;
    audio_idle_cb_user_data = user_data;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    return (strcmp(audio_file_path, file_path) == 0);
}

bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    return (index == file_iterator_get_index(instance));
}