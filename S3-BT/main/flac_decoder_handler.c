#include "flac_decoder_handler.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "decoder/impl/esp_flac_dec.h"
#include "main.h"

static const char *TAG = "FLAC_DEC";

#define FLAC_PCM_OUT_BUF_SIZE 8192

static esp_audio_simple_dec_handle_t s_flac_dec = NULL;
static uint8_t *s_pcm_out_buf = NULL;
static size_t s_pcm_out_buf_size = 0;
static bool s_default_registered = false;
static bool s_flac_registered = false;
static bool s_info_applied = false;

static esp_err_t flac_apply_stream_info(void)
{
    if (!s_flac_dec || s_info_applied) {
        return ESP_OK;
    }

    esp_audio_simple_dec_info_t info = {0};
    esp_audio_err_t ret = esp_audio_simple_dec_get_info(s_flac_dec, &info);
    if (ret != ESP_AUDIO_ERR_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    if (info.bits_per_sample != 16) {
        ESP_LOGW(TAG,
                 "Unsupported FLAC bit depth: %u (only 16-bit output supported)",
                 (unsigned)info.bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info.sample_rate == 0 || info.channel == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t cfg_err = audio_reconfigure(info.sample_rate, info.channel);
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "audio_reconfigure failed for FLAC stream (%lu Hz, ch=%u): %s",
                 (unsigned long)info.sample_rate,
                 (unsigned)info.channel,
                 esp_err_to_name(cfg_err));
        return cfg_err;
    }

    s_info_applied = true;
    ESP_LOGI(TAG,
             "FLAC output configured: %lu Hz, %u ch, %u bits",
             (unsigned long)info.sample_rate,
             (unsigned)info.channel,
             (unsigned)info.bits_per_sample);
    return ESP_OK;
}

esp_err_t flac_decoder_init(void)
{
    if (!s_flac_registered) {
        esp_audio_err_t flac_reg_ret = esp_flac_dec_register();
        if (flac_reg_ret != ESP_AUDIO_ERR_OK && flac_reg_ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
            ESP_LOGE(TAG, "register FLAC decoder failed: %d", flac_reg_ret);
            return ESP_FAIL;
        }
        s_flac_registered = true;
    }

    if (!s_default_registered) {
        esp_audio_err_t reg_ret = esp_audio_simple_dec_register_default();
        if (reg_ret != ESP_AUDIO_ERR_OK && reg_ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
            ESP_LOGE(TAG, "register default decoders failed: %d", reg_ret);
            return ESP_FAIL;
        }
        s_default_registered = true;
    }

    if (!s_pcm_out_buf) {
        s_pcm_out_buf = malloc(FLAC_PCM_OUT_BUF_SIZE);
        if (!s_pcm_out_buf) {
            return ESP_ERR_NO_MEM;
        }
        s_pcm_out_buf_size = FLAC_PCM_OUT_BUF_SIZE;
    }

    return ESP_OK;
}

esp_err_t flac_decoder_start_stream(void)
{
    ESP_RETURN_ON_ERROR(flac_decoder_init(), TAG, "flac_decoder_init failed");

    if (s_flac_dec) {
        esp_audio_simple_dec_close(s_flac_dec);
        s_flac_dec = NULL;
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };

    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &s_flac_dec);
    if (ret != ESP_AUDIO_ERR_OK || !s_flac_dec) {
        ESP_LOGE(TAG, "open FLAC decoder failed: %d", ret);
        s_flac_dec = NULL;
        return ESP_FAIL;
    }

    s_info_applied = false;
    audio_reset_buffers();
    ESP_LOGI(TAG, "FLAC decoder stream started");
    return ESP_OK;
}

esp_err_t flac_decoder_feed(const uint8_t *data, uint32_t len, bool eos)
{
    if (!s_flac_dec) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t offset = 0;
    bool keep_processing = true;

    while (keep_processing) {
        esp_audio_simple_dec_raw_t raw = {
            .buffer = (uint8_t *)((data && offset < len) ? (data + offset) : NULL),
            .len = (data && offset < len) ? (len - offset) : 0,
            .eos = eos && (!data || offset >= len),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        esp_audio_simple_dec_out_t out = {
            .buffer = s_pcm_out_buf,
            .len = (uint32_t)s_pcm_out_buf_size,
            .needed_size = 0,
            .decoded_size = 0,
        };

        esp_audio_err_t ret = esp_audio_simple_dec_process(s_flac_dec, &raw, &out);

        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > s_pcm_out_buf_size) {
            uint8_t *new_buf = realloc(s_pcm_out_buf, out.needed_size);
            if (!new_buf) {
                ESP_LOGE(TAG, "FLAC PCM output realloc failed: need=%u", (unsigned)out.needed_size);
                return ESP_ERR_NO_MEM;
            }
            s_pcm_out_buf = new_buf;
            s_pcm_out_buf_size = out.needed_size;
            continue;
        }

        if (raw.consumed > 0) {
            offset += raw.consumed;
        }

        if (out.decoded_size > 0) {
            esp_err_t info_err = flac_apply_stream_info();
            if (info_err == ESP_OK) {
                audio_data_callback(out.buffer, out.decoded_size);
            } else if (info_err != ESP_ERR_NOT_FOUND) {
                return info_err;
            }
        }

        if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_DATA_LACK && ret != ESP_AUDIO_ERR_CONTINUE) {
            ESP_LOGW(TAG, "FLAC decode failed: %d", ret);
            return ESP_FAIL;
        }

        if (data && offset >= len && out.decoded_size == 0) {
            keep_processing = false;
        } else if (!data && out.decoded_size == 0) {
            keep_processing = false;
        }
    }

    return ESP_OK;
}

void flac_decoder_stop_stream(void)
{
    if (!s_flac_dec) {
        return;
    }

    (void)flac_decoder_feed(NULL, 0, true);
    esp_audio_simple_dec_close(s_flac_dec);
    s_flac_dec = NULL;
    s_info_applied = false;
    ESP_LOGI(TAG, "FLAC decoder stream stopped");
}
