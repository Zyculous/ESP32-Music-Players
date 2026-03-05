#include "main.h"

#include <string.h>
#include "esp_check.h"
#include "esp_http_client.h"
#include "flac_decoder_handler.h"

static const char *TAG = "MA_STREAM";

#define STREAM_READ_BUFFER_SIZE 1024
#define STREAM_HEADER_PROBE_SIZE 4096
#define STREAM_TASK_STACK_SIZE 12288
#define STREAM_TASK_PRIORITY 5

typedef struct {
    char url[MA_STREAM_URL_MAX];
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t format;
} ma_stream_cfg_t;

static TaskHandle_t s_stream_task = NULL;
static ma_stream_cfg_t s_stream_cfg = {0};
static SemaphoreHandle_t s_stream_mutex = NULL;
static uint32_t s_stream_generation = 0;
static bool s_stream_running = false;
static bool s_stream_paused = false;

static int find_wav_data_offset(const uint8_t *buf, size_t len)
{
    if (!buf || len < 12) {
        return -1;
    }
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return -1;
    }

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *chunk = buf + pos;
        uint32_t chunk_len = (uint32_t)chunk[4] |
                             ((uint32_t)chunk[5] << 8) |
                             ((uint32_t)chunk[6] << 16) |
                             ((uint32_t)chunk[7] << 24);
        size_t next_pos = pos + 8 + (size_t)chunk_len + ((chunk_len & 1U) ? 1U : 0U);
        if (memcmp(chunk, "data", 4) == 0) {
            return (int)(pos + 8);
        }
        if (next_pos <= pos || next_pos > len) {
            break;
        }
        pos = next_pos;
    }
    return -1;
}

static void ma_stream_task(void *arg)
{
    (void)arg;

    uint8_t buffer[STREAM_READ_BUFFER_SIZE];
    uint8_t *header_probe = malloc(STREAM_HEADER_PROBE_SIZE);
    if (!header_probe) {
        ESP_LOGE(TAG, "Failed to allocate stream header probe buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (s_stream_mutex && xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (!s_stream_running) {
                xSemaphoreGive(s_stream_mutex);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            xSemaphoreGive(s_stream_mutex);
        }

        esp_http_client_config_t http_cfg = {
            .url = s_stream_cfg.url,
            .timeout_ms = 10000,
            .buffer_size = STREAM_READ_BUFFER_SIZE,
            .buffer_size_tx = 512,
            .keep_alive_enable = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        esp_http_client_set_header(client, "Accept", "audio/L16");
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int64_t content_len = esp_http_client_fetch_headers(client);
        if (content_len < 0) {
            ESP_LOGW(TAG, "Failed to fetch HTTP headers (%lld)", (long long)content_len);
        }

        int http_status = esp_http_client_get_status_code(client);
        int64_t http_content_length = esp_http_client_get_content_length(client);

        ESP_LOGI(TAG, "Connected to stream: %s", s_stream_cfg.url);
        ESP_LOGI(TAG,
             "HTTP stream status=%d content_length=%lld",
               http_status,
               (long long)http_content_length);
        uint32_t start_gen = s_stream_generation;
        bool header_checked = false;
        bool stream_supported = true;
        bool use_flac_decoder = (s_stream_cfg.format == 'f');
        size_t header_probe_len = 0;
        size_t forwarded_bytes = 0;
        bool ended_by_server = false;

        if (use_flac_decoder) {
            esp_err_t flac_err = flac_decoder_start_stream();
            if (flac_err != ESP_OK) {
                ESP_LOGE(TAG, "FLAC decoder start failed: %s", esp_err_to_name(flac_err));
                stream_supported = false;
            }
        }
        while (1) {
            if (s_stream_mutex && xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(0)) == pdTRUE) {
                bool running = s_stream_running;
                bool paused = s_stream_paused;
                bool changed = (start_gen != s_stream_generation);
                xSemaphoreGive(s_stream_mutex);

                if (!running || changed) {
                    break;
                }

                if (paused) {
                    vTaskDelay(pdMS_TO_TICKS(40));
                    continue;
                }
            }

            int read = esp_http_client_read(client, (char *)buffer, sizeof(buffer));
            if (read > 0) {
                if (use_flac_decoder) {
                    esp_err_t flac_feed_err = flac_decoder_feed(buffer, (uint32_t)read, false);
                    if (flac_feed_err != ESP_OK) {
                        ESP_LOGW(TAG, "FLAC feed failed: %s", esp_err_to_name(flac_feed_err));
                        stream_supported = false;
                        if (flac_feed_err == ESP_ERR_NOT_SUPPORTED && s_stream_mutex &&
                            xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                            s_stream_running = false;
                            xSemaphoreGive(s_stream_mutex);
                            ESP_LOGW(TAG, "Stopping stream: FLAC profile not supported on-device");
                        }
                        break;
                    }
                    forwarded_bytes += (size_t)read;
                    continue;
                }

                if (!header_checked) {
                    size_t copy_len = (size_t)read;
                    if (header_probe_len + copy_len > STREAM_HEADER_PROBE_SIZE) {
                        copy_len = STREAM_HEADER_PROBE_SIZE - header_probe_len;
                    }
                    if (copy_len > 0) {
                        memcpy(header_probe + header_probe_len, buffer, copy_len);
                        header_probe_len += copy_len;
                    }

                    if (s_stream_cfg.format == 'w') {
                        int data_off = find_wav_data_offset(header_probe, header_probe_len);
                        if (data_off >= 0) {
                            size_t off = (size_t)data_off;
                            if (off < header_probe_len) {
                                uint32_t chunk = (uint32_t)(header_probe_len - off);
                                audio_data_callback(header_probe + off, chunk);
                                forwarded_bytes += chunk;
                            }
                            header_checked = true;
                            continue;
                        }
                        if (header_probe_len < STREAM_HEADER_PROBE_SIZE) {
                            continue;
                        }

                        ESP_LOGW(TAG, "WAV header parse failed after %u bytes; using stream as raw PCM", (unsigned)header_probe_len);
                        header_checked = true;
                        if (header_probe_len > 0) {
                            audio_data_callback(header_probe, (uint32_t)header_probe_len);
                            forwarded_bytes += header_probe_len;
                        }
                        continue;
                    } else {
                        if (header_probe_len >= 4 && memcmp(header_probe, "fLaC", 4) == 0) {
                            ESP_LOGW(TAG, "FLAC stream routed without decoder; check format handling");
                            stream_supported = false;
                        } else if (header_probe_len >= 3 && memcmp(header_probe, "ID3", 3) == 0) {
                            ESP_LOGW(TAG, "MP3 stream not supported on-device; select WAV output codec in Music Assistant");
                            stream_supported = false;
                        }

                        header_checked = true;
                        if (stream_supported && header_probe_len > 0) {
                            audio_data_callback(header_probe, (uint32_t)header_probe_len);
                            forwarded_bytes += header_probe_len;
                        }
                        if (!stream_supported) {
                            break;
                        }
                        continue;
                    }

                    if (!header_checked && header_probe_len > 0 && s_stream_cfg.format != 'w') {
                        header_checked = true;
                        audio_data_callback(header_probe, (uint32_t)header_probe_len);
                    }
                    continue;
                }

                if (!stream_supported) {
                    break;
                }
                audio_data_callback(buffer, (uint32_t)read);
                forwarded_bytes += (size_t)read;
            } else if (read == 0) {
                ESP_LOGW(TAG, "Stream ended by server");
                ended_by_server = true;
                break;
            } else {
                ESP_LOGW(TAG, "Stream read error (%d)", read);
                break;
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (use_flac_decoder) {
            flac_decoder_stop_stream();
        }

        if (ended_by_server) {
            ESP_LOGI(TAG, "Track stream ended, forwarded=%u bytes", (unsigned)forwarded_bytes);
            if (forwarded_bytes == 0) {
                ESP_LOGW(TAG,
                         "No audio payload received (status=%d, content_length=%lld)",
                         http_status,
                         (long long)http_content_length);
            }
            if (xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (start_gen == s_stream_generation) {
                    s_stream_running = false;
                }
                xSemaphoreGive(s_stream_mutex);
            }
        }

        if (s_stream_running) {
            ESP_LOGW(TAG, "Reconnecting stream in 500ms...");
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t music_assistant_stream_start(const char *stream_url, uint32_t sample_rate, uint8_t channels, uint8_t format)
{
    if (!stream_url || strlen(stream_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_stream_mutex) {
        s_stream_mutex = xSemaphoreCreateMutex();
        if (!s_stream_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    size_t url_len = strlen(stream_url);
    if (url_len >= sizeof(s_stream_cfg.url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(s_stream_cfg.url, stream_url);
        s_stream_cfg.sample_rate = sample_rate;
        s_stream_cfg.channels = (channels <= 1) ? 1 : 2;
        s_stream_cfg.format = format;
        s_stream_running = true;
        s_stream_paused = false;
        s_stream_generation++;
        xSemaphoreGive(s_stream_mutex);
    }

    ESP_RETURN_ON_ERROR(audio_reconfigure(s_stream_cfg.sample_rate, s_stream_cfg.channels),
                        TAG,
                        "audio_reconfigure failed");

    if (s_stream_task) {
        ESP_LOGI(TAG,
                 "Music Assistant stream updated (%s, %lu Hz, %s)",
                 s_stream_cfg.url,
                 (unsigned long)s_stream_cfg.sample_rate,
                 (s_stream_cfg.channels == 1) ? "mono" : "stereo");
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(ma_stream_task,
                                            "ma_stream_task",
                                            STREAM_TASK_STACK_SIZE,
                                            NULL,
                                            STREAM_TASK_PRIORITY,
                                            &s_stream_task,
                                            1);
    if (ok != pdPASS) {
        s_stream_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Music Assistant stream started (%s, %lu Hz, %s)",
             s_stream_cfg.url,
             (unsigned long)s_stream_cfg.sample_rate,
             (s_stream_cfg.channels == 1) ? "mono" : "stereo");
    return ESP_OK;
}

void music_assistant_stream_stop(void)
{
    if (!s_stream_mutex) {
        return;
    }
    if (xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_stream_running = false;
        s_stream_paused = false;
        s_stream_generation++;
        xSemaphoreGive(s_stream_mutex);
    }
    audio_reset_buffers();
}

void music_assistant_stream_pause(bool pause)
{
    if (!s_stream_mutex) {
        return;
    }
    if (xSemaphoreTake(s_stream_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_stream_paused = pause;
        xSemaphoreGive(s_stream_mutex);
    }
    if (pause) {
        audio_reset_buffers();
    }
}
