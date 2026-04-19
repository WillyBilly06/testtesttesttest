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
static bsp_extra_audio_output_route_t s_output_route = BSP_EXTRA_AUDIO_OUTPUT_SPEAKER;
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

static i2s_data_bit_width_t aux_data_width_from_bits(uint32_t bits)
{
    if (bits <= 16) {
        return I2S_DATA_BIT_WIDTH_16BIT;
    }
    if (bits <= 24) {
        return I2S_DATA_BIT_WIDTH_24BIT;
    }
    return I2S_DATA_BIT_WIDTH_32BIT;
}

static esp_err_t aux_i2s_deinit(void)
{
    if (s_aux_tx_chan == NULL) {
        return ESP_OK;
    }
    i2s_channel_disable(s_aux_tx_chan);
    esp_err_t ret = i2s_del_channel(s_aux_tx_chan);
    s_aux_tx_chan = NULL;
    return ret;
}

static esp_err_t aux_i2s_init(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    ESP_RETURN_ON_ERROR(aux_i2s_deinit(), TAG, "deinit aux i2s failed");

    i2s_chan_config_t aux_chan_cfg = {
        .id = I2S_NUM_2,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 256,
        .auto_clear = true,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&aux_chan_cfg, &s_aux_tx_chan, NULL), TAG, "new aux i2s channel failed");

    i2s_data_bit_width_t data_width = aux_data_width_from_bits(bits_cfg);
    i2s_std_config_t aux_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = data_width,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = ch,
            .slot_mask = (ch == I2S_SLOT_MODE_MONO) ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH,
            .ws_width = data_width,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_8,
            .ws = GPIO_NUM_6,
            .dout = GPIO_NUM_7,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    esp_err_t ret = i2s_channel_init_std_mode(s_aux_tx_chan, &aux_std_cfg);
    if (ret != ESP_OK) {
        i2s_del_channel(s_aux_tx_chan);
        s_aux_tx_chan = NULL;
        return ret;
    }
    ret = i2s_channel_enable(s_aux_tx_chan);
    if (ret != ESP_OK) {
        i2s_del_channel(s_aux_tx_chan);
        s_aux_tx_chan = NULL;
        return ret;
    }

    s_aux_sample_rate = rate;
    s_aux_bits = bits_cfg;
    s_aux_slot_mode = ch;
    ESP_LOGI(TAG, "AUX I2S enabled on GPIO6/7/8 (%lu Hz, %lu-bit)",
             (unsigned long)rate, (unsigned long)bits_cfg);

    return ESP_OK;
}

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.

    bsp_extra_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity), TAG, "Set Codec volume failed");
    }

    return ESP_OK;
}

static void audio_callback(audio_player_cb_ctx_t *ctx)
{
    if (audio_idle_callback) {
        ctx->user_ctx = audio_idle_cb_user_data;
        audio_idle_callback(ctx);
    }
}

esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
    *bytes_read = len;
    return ret;
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if ((s_output_route == BSP_EXTRA_AUDIO_OUTPUT_AUX) && (s_aux_tx_chan != NULL)) {
        /* AUX uses direct I2S output; apply software volume so UI slider works on AUX too. */
        if ((audio_buffer != NULL) && (len >= sizeof(int16_t)) && (s_aux_bits <= 16)) {
            if (_vloume_intensity <= 0) {
                memset(audio_buffer, 0, len);
            } else if (_vloume_intensity < 100) {
                int16_t *samples = (int16_t *)audio_buffer;
                size_t sample_count = len / sizeof(int16_t);
                for (size_t i = 0; i < sample_count; i++) {
                    samples[i] = aux_scale_sample_i16(samples[i], _vloume_intensity);
                }
            }
        }

        size_t written = 0;
        TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        esp_err_t ret = i2s_channel_write(s_aux_tx_chan, audio_buffer, len, &written, timeout_ticks);
        if (bytes_written != NULL) {
            *bytes_written = written;
        }
        return ret;
    }

    esp_err_t ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    if (bytes_written != NULL) {
        *bytes_written = len;
    }
    return ret;
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_close(record_dev_handle);
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, CODEC_DEFAULT_ADC_VOLUME);
    }

    if (play_dev_handle) {
        ret |= esp_codec_dev_open(play_dev_handle, &fs);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_open(record_dev_handle, &fs);
    }

    s_aux_sample_rate = rate;
    s_aux_bits = bits_cfg;
    s_aux_slot_mode = ch;
    if (s_output_route == BSP_EXTRA_AUDIO_OUTPUT_AUX) {
        ret |= aux_i2s_init(rate, bits_cfg, ch);
    }

    return ret;
}

esp_err_t bsp_extra_output_route_set(bsp_extra_audio_output_route_t route)
{
    if (route == BSP_EXTRA_AUDIO_OUTPUT_AUX) {
        ESP_RETURN_ON_ERROR(aux_i2s_init(s_aux_sample_rate, s_aux_bits, s_aux_slot_mode), TAG, "init aux route failed");
    }
    s_output_route = route;
    ESP_LOGI(TAG, "Audio output route set to %s", (route == BSP_EXTRA_AUDIO_OUTPUT_AUX) ? "AUX" : "SPEAKER");
    return ESP_OK;
}

bsp_extra_audio_output_route_t bsp_extra_output_route_get(void)
{
    return s_output_route;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, volume), TAG, "Set Codec volume failed");
    _vloume_intensity = volume;

    ESP_LOGI(TAG, "Setting volume: %d", volume);

    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    return ret;
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }

    if (record_dev_handle) {
        ret = esp_codec_dev_close(record_dev_handle);
    }
    return ret;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t bsp_extra_codec_init()
{
    if (_is_audio_init) {
        return ESP_OK;
    }

    play_dev_handle = bsp_audio_codec_speaker_init();
    assert((play_dev_handle) && "play_dev_handle not initialized");

    record_dev_handle = bsp_audio_codec_microphone_init();
    assert((record_dev_handle) && "record_dev_handle not initialized");

    bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);

    _is_audio_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    audio_player_config_t config = { .mute_fn = audio_mute_function,
                                     .write_fn = bsp_extra_i2s_write,
                                     .clk_set_fn = bsp_extra_codec_set_fs,
                                     .priority = 5
                                   };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "audio_player_init failed");
    audio_player_callback_register(audio_callback, NULL);

    _is_player_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_del(void)
{
    _is_player_init = false;

    ESP_RETURN_ON_ERROR(audio_player_delete(), TAG, "audio_player_delete failed");

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
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128];
    int retval = file_iterator_get_full_path_from_index(instance, index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG, "file_iterator_get_full_path_from_index failed");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = fopen(filename, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, filename, sizeof(audio_file_path));

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, file_path, sizeof(audio_file_path));

    return ESP_OK;
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