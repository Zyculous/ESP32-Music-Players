#include "main.h"
#include "driver/i2s.h"
#include "freertos/queue.h"
#include <limits.h>
#include <string.h>

static const char *TAG = "AUDIO";

#define AUDIO_QUEUE_SIZE 24
#define AUDIO_BUFFER_SIZE 1024
#define AUDIO_TASK_STACK_SIZE 3072
#define AUDIO_TASK_PRIORITY (configMAX_PRIORITIES - 1)

typedef struct {
    uint16_t samples[AUDIO_BUFFER_SIZE / sizeof(uint16_t)];
    uint32_t len;
} audio_buffer_t;

static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static bool s_audio_initialized = false;
static uint32_t s_current_sample_rate = 44100;
static uint8_t s_current_channels = 2;

void audio_set_volume(uint8_t volume)
{
    if (volume > 0x7F) {
        volume = 0x7F;
    }

    if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_player_state.volume = volume;
        xSemaphoreGive(g_player_state_mutex);
    }
}

static void audio_task(void *params)
{
    (void)params;

    audio_buffer_t buffer;
    while (1) {
        if (xQueueReceive(s_audio_queue, &buffer, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_audio_initialized) {
            continue;
        }

        uint8_t volume = g_player_state.volume;
        if (volume > 0x7F) {
            volume = 0x7F;
        }

        size_t sample_count = buffer.len / sizeof(int16_t);
        for (size_t i = 0; i < sample_count; i++) {
            int16_t pcm_sample = (int16_t)buffer.samples[i];
            int32_t scaled = ((int32_t)pcm_sample * (int32_t)volume) / 0x7F;
            if (scaled > INT16_MAX) {
                scaled = INT16_MAX;
            } else if (scaled < INT16_MIN) {
                scaled = INT16_MIN;
            }

            // Internal ESP32 DAC expects unsigned sample range (0..65535),
            // with audio centered at mid-scale.
            buffer.samples[i] = (uint16_t)(scaled + 32768);
        }

        size_t offset = 0;
        while (offset < buffer.len) {
            size_t bytes_written = 0;
            esp_err_t err = i2s_write(I2S_NUM_CH,
                                      ((uint8_t *)buffer.samples) + offset,
                                      buffer.len - offset,
                                      &bytes_written,
                                      pdMS_TO_TICKS(20));
            if (err != ESP_OK || bytes_written == 0) {
                static uint32_t write_errors = 0;
                write_errors++;
                if ((write_errors % 100) == 0) {
                    ESP_LOGW(TAG, "i2s_write errors: %lu", (unsigned long)write_errors);
                }
                break;
            }
            offset += bytes_written;
        }
    }
}

esp_err_t audio_init(void)
{
    if (s_audio_initialized) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    err = i2s_driver_install(I2S_NUM_CH, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_dac_mode failed: %s", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_NUM_CH);
        return err;
    }

    s_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_buffer_t));
    if (s_audio_queue == NULL) {
        ESP_LOGE(TAG, "audio queue create failed");
        i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
        i2s_driver_uninstall(I2S_NUM_CH);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreatePinnedToCore(audio_task,
                                                  "audio_task",
                                                  AUDIO_TASK_STACK_SIZE,
                                                  NULL,
                                                  AUDIO_TASK_PRIORITY,
                                                  &s_audio_task_handle,
                                                  1);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
        i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
        i2s_driver_uninstall(I2S_NUM_CH);
        return ESP_ERR_NO_MEM;
    }

    s_current_sample_rate = 44100;
    s_current_channels = 2;
    s_audio_initialized = true;
    ESP_LOGI(TAG, "audio init ok: %lu Hz stereo (internal DAC)", (unsigned long)s_current_sample_rate);

    return ESP_OK;
}

esp_err_t audio_reconfigure(uint32_t sample_rate, uint8_t channels)
{
    s_current_sample_rate = sample_rate;
    s_current_channels = (channels <= 1) ? 1 : 2;

    if (!s_audio_initialized) {
        return ESP_OK;
    }

    i2s_channel_t channel_cfg = (s_current_channels == 1) ? I2S_CHANNEL_MONO : I2S_CHANNEL_STEREO;
    esp_err_t err = i2s_set_clk(I2S_NUM_CH, s_current_sample_rate, I2S_BITS_PER_SAMPLE_16BIT, channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_set_clk failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "audio clk: %lu Hz, %s",
             (unsigned long)s_current_sample_rate,
             (s_current_channels == 1) ? "mono" : "stereo");
    return ESP_OK;
}

void audio_data_callback(const uint8_t *data, uint32_t len)
{
    if (!s_audio_initialized || s_audio_queue == NULL || data == NULL || len == 0) {
        return;
    }

    uint32_t offset = 0;
    while (offset < len) {
        audio_buffer_t buffer;
        uint32_t chunk_len = len - offset;
        if (chunk_len > AUDIO_BUFFER_SIZE) {
            chunk_len = AUDIO_BUFFER_SIZE;
        }

        chunk_len &= ~1U;
        if (chunk_len == 0) {
            break;
        }

        memcpy((uint8_t *)buffer.samples, data + offset, chunk_len);
        buffer.len = chunk_len;

        if (xQueueSend(s_audio_queue, &buffer, pdMS_TO_TICKS(2)) != pdTRUE) {
            static uint32_t dropped_chunks = 0;
            dropped_chunks++;
            if ((dropped_chunks % 100) == 0) {
                ESP_LOGW(TAG, "audio queue drops: %lu", (unsigned long)dropped_chunks);
            }
            break;
        }

        offset += chunk_len;
    }
}

void audio_reset_buffers(void)
{
    if (!s_audio_initialized || s_audio_queue == NULL) {
        return;
    }

    xQueueReset(s_audio_queue);
    i2s_zero_dma_buffer(I2S_NUM_CH);
}
