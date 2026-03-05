#include "main.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_mac.h"

static const char *TAG = "SLIMPROTO";

#define SLIMPROTO_TASK_STACK 8192
#define SLIMPROTO_TASK_PRIO  5
#define SLIMPROTO_MAX_PACKET 1536
#define SLIMPROTO_CONNECT_RETRY_MS 3000
#define SLIMPROTO_HEARTBEAT_MS 2000

typedef struct {
    char host[64];
    uint16_t port;
} slimproto_cfg_t;

static TaskHandle_t s_slimproto_task = NULL;
static slimproto_cfg_t s_cfg = {0};

static void set_player_waiting_metadata(void)
{
    if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(g_player_state.metadata.title, sizeof(g_player_state.metadata.title), "ESP32S3 SlimProto");
        snprintf(g_player_state.metadata.artist, sizeof(g_player_state.metadata.artist), "Waiting for stream");
        snprintf(g_player_state.metadata.album, sizeof(g_player_state.metadata.album), "%s:%u", s_cfg.host, (unsigned)s_cfg.port);
        g_player_state.metadata.is_playing = false;
        xSemaphoreGive(g_player_state_mutex);
    }
}

static void set_player_stream_metadata(const char *path, uint32_t sample_rate, uint8_t channels)
{
    if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(g_player_state.metadata.title, sizeof(g_player_state.metadata.title), "%s", path ? path : "SlimProto Stream");
        snprintf(g_player_state.metadata.artist, sizeof(g_player_state.metadata.artist), "Music Assistant");
        snprintf(g_player_state.metadata.album,
                 sizeof(g_player_state.metadata.album),
                 "%lu Hz %s",
                 (unsigned long)sample_rate,
                 (channels == 1) ? "mono" : "stereo");
        g_player_state.metadata.is_playing = true;
        xSemaphoreGive(g_player_state_mutex);
    }
}

static uint32_t decode_sample_rate(uint8_t code)
{
    switch (code) {
        case '0': return 11025;
        case '1': return 22050;
        case '2': return 32000;
        case '3': return 44100;
        case '4': return 48000;
        case '5': return 8000;
        case '6': return 12000;
        case '7': return 16000;
        case '8': return 24000;
        case '9': return 96000;
        default: return 44100;
    }
}

static uint8_t decode_channels(uint8_t code)
{
    return (code == '1') ? 1 : 2;
}

static bool recv_exact(int sock, void *buf, size_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        int r = recv(sock, dst + got, len - got, 0);
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

static bool send_client_packet(int sock, const char cmd[4], const uint8_t *data, uint32_t data_len)
{
    uint8_t header[8];
    memcpy(header, cmd, 4);
    uint32_t len_be = htonl(data_len);
    memcpy(header + 4, &len_be, sizeof(len_be));

    if (send(sock, header, sizeof(header), 0) != (ssize_t)sizeof(header)) {
        return false;
    }
    if (data_len > 0 && data) {
        if (send(sock, data, data_len, 0) != (ssize_t)data_len) {
            return false;
        }
    }
    return true;
}

static void send_stat_event(int sock, const char event_code[4])
{
    uint8_t payload[64] = {0};
    memcpy(payload, event_code, 4);
    send_client_packet(sock, "STAT", payload, sizeof(payload));
}

static void send_helo(int sock)
{
    uint8_t payload[128] = {0};
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    payload[0] = 12; // squeezeplay-like device id
    payload[1] = 1;  // revision
    memcpy(payload + 2, mac, sizeof(mac));
    for (int i = 0; i < 16; i++) {
        payload[8 + i] = mac[i % 6];
    }
    payload[24] = 0x07;
    payload[25] = 0xFF;

    const char *caps = "Model=squeezelite,ModelName=ESP32S3,MaxSampleRate=48000,pcm,flc";
    size_t caps_len = strlen(caps);
    size_t base_len = 36;
    if (base_len + caps_len >= sizeof(payload)) {
        caps_len = sizeof(payload) - base_len - 1;
    }
    memcpy(payload + base_len, caps, caps_len);

    send_client_packet(sock, "HELO", payload, (uint32_t)(base_len + caps_len));
    ESP_LOGI(TAG, "HELO sent");
}

static void send_stat_t(int sock)
{
    send_stat_event(sock, "STMt");
}

static bool parse_http_get_path(const uint8_t *hdr, size_t len, char *out_path, size_t out_len)
{
    const char *p = (const char *)hdr;
    const char *get = strstr(p, "GET ");
    if (!get) {
        return false;
    }
    get += 4;
    const char *sp = strchr(get, ' ');
    if (!sp || sp <= get) {
        return false;
    }
    size_t path_len = (size_t)(sp - get);
    if (path_len + 1 > out_len) {
        return false;
    }
    memcpy(out_path, get, path_len);
    out_path[path_len] = '\0';
    return true;
}

static bool parse_http_url(const uint8_t *data, size_t len, char *out_url, size_t out_len)
{
    const char *start = NULL;
    const char *as_text = (const char *)data;

    start = strstr(as_text, "http://");
    if (!start) {
        start = strstr(as_text, "https://");
    }
    if (!start) {
        return false;
    }

    const char *end = start;
    while ((size_t)(end - as_text) < len && *end >= 0x20 && *end != '\r' && *end != '\n') {
        end++;
    }

    size_t n = (size_t)(end - start);
    if (n == 0 || n + 1 > out_len) {
        return false;
    }

    memcpy(out_url, start, n);
    out_url[n] = '\0';
    return true;
}

static void handle_audg(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) {
        return;
    }

    uint8_t raw_vol = data[0];
    if (len >= 4 && data[3] <= 100) {
        raw_vol = (uint8_t)((data[3] * 0x7FU) / 100U);
    } else if (raw_vol > 0x7F) {
        raw_vol = (uint8_t)((raw_vol * 0x7FU) / 255U);
    }

    audio_set_volume(raw_vol);
    ESP_LOGI(TAG, "audg volume update -> %u/127 (len=%u)", (unsigned)raw_vol, (unsigned)len);
}

static void handle_strm(int sock, const uint8_t *data, uint16_t len)
{
    if (!data || len < 24) {
        return;
    }

    uint8_t command = data[0];
    uint8_t format = data[2];

    if (command == 'q' || command == 'f') {
        ESP_LOGI(TAG, "strm cmd %c", command);
        music_assistant_stream_stop();
        send_stat_event(sock, "STMf");
        if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_player_state.metadata.is_playing = false;
            xSemaphoreGive(g_player_state_mutex);
        }
        return;
    }

    if (command == 'p') {
        ESP_LOGI(TAG, "strm cmd p (pause)");
        music_assistant_stream_pause(true);
        send_stat_event(sock, "STMp");
        if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_player_state.metadata.is_playing = false;
            xSemaphoreGive(g_player_state_mutex);
        }
        return;
    }

    if (command == 'u') {
        ESP_LOGI(TAG, "strm cmd u (unpause)");
        music_assistant_stream_pause(false);
        send_stat_event(sock, "STMr");
        if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_player_state.metadata.is_playing = true;
            xSemaphoreGive(g_player_state_mutex);
        }
        return;
    }

    if (command == 't') {
        send_stat_t(sock);
        return;
    }

    if (command != 's' && command != 'a') {
        return;
    }

    if (format != 'p' && format != 'w' && format != 'f') {
        ESP_LOGW(TAG, "Unsupported format '%c' (supported: p/w/f).", format);
        send_stat_event(sock, "STMn");
        return;
    }

    uint32_t sample_rate = decode_sample_rate(data[4]);
    uint8_t channels = decode_channels(data[5]);
    uint16_t server_port = ((uint16_t)data[18] << 8) | data[19];

    uint32_t ip_raw = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                      ((uint32_t)data[22] << 8) | data[23];
    struct in_addr addr = { .s_addr = htonl(ip_raw) };

    char url[640] = {0};
    char path[512] = {0};

    if (parse_http_url(data + 24, len - 24, url, sizeof(url))) {
        // full URL already provided
    } else if (parse_http_get_path(data + 24, len - 24, path, sizeof(path))) {
        const char *host = (ip_raw != 0) ? inet_ntoa(addr) : s_cfg.host;
        uint16_t port = (server_port != 0) ? server_port : 9000;
        snprintf(url, sizeof(url), "http://%s:%u%s", host, (unsigned)port, path);
    } else {
        ESP_LOGW(TAG, "strm had no parsable URL/path; cmd=%c len=%u", command, (unsigned)len);
        send_stat_event(sock, "STMn");
        return;
    }

    ESP_LOGI(TAG, "Starting PCM stream %s (%lu Hz, %s)",
             url,
             (unsigned long)sample_rate,
             (channels == 1) ? "mono" : "stereo");
    set_player_stream_metadata(url, sample_rate, channels);
    esp_err_t stream_err = music_assistant_stream_start(url, sample_rate, channels, format);
    if (stream_err == ESP_OK) {
        send_stat_event(sock, "STMc");
        send_stat_event(sock, "STMe");
        send_stat_event(sock, "STMs");
    } else {
        ESP_LOGW(TAG, "stream start failed: %s", esp_err_to_name(stream_err));
        send_stat_event(sock, "STMn");
    }
}

static void slimproto_task(void *arg)
{
    (void)arg;

    while (1) {
        int sock = -1;
        struct sockaddr_in dest = {
            .sin_family = AF_INET,
            .sin_port = htons(s_cfg.port),
        };

        if (inet_pton(AF_INET, s_cfg.host, &dest.sin_addr) != 1) {
            ESP_LOGE(TAG, "Invalid MA_SLIMPROTO_HOST: %s", s_cfg.host);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket create failed");
            vTaskDelay(pdMS_TO_TICKS(SLIMPROTO_CONNECT_RETRY_MS));
            continue;
        }

        ESP_LOGI(TAG, "Connecting to SlimProto server %s:%u", s_cfg.host, (unsigned)s_cfg.port);
        if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
            ESP_LOGE(TAG, "connect failed");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(SLIMPROTO_CONNECT_RETRY_MS));
            continue;
        }

        struct timeval tv = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        send_helo(sock);
        send_stat_event(sock, "STMc");
        send_stat_t(sock);
        set_player_waiting_metadata();

        uint64_t last_hb = (uint64_t)esp_log_timestamp();

        while (1) {
            uint8_t len_be[2];
            if (!recv_exact(sock, len_be, sizeof(len_be))) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    uint64_t now = (uint64_t)esp_log_timestamp();
                    if ((now - last_hb) >= SLIMPROTO_HEARTBEAT_MS) {
                        send_stat_t(sock);
                        last_hb = now;
                    }
                    continue;
                }
                ESP_LOGW(TAG, "SlimProto disconnected");
                break;
            }

            uint16_t packet_len = ((uint16_t)len_be[0] << 8) | len_be[1];
            if (packet_len < 4 || packet_len > SLIMPROTO_MAX_PACKET) {
                ESP_LOGW(TAG, "Invalid packet length: %u", (unsigned)packet_len);
                break;
            }

            uint8_t packet[SLIMPROTO_MAX_PACKET];
            if (!recv_exact(sock, packet, packet_len)) {
                ESP_LOGW(TAG, "Packet read failed");
                break;
            }

            char op[5] = {0};
            memcpy(op, packet, 4);
            uint8_t *data = packet + 4;
            uint16_t data_len = packet_len - 4;

            if (memcmp(op, "strm", 4) == 0) {
                handle_strm(sock, data, data_len);
            } else if (memcmp(op, "audg", 4) == 0) {
                handle_audg(data, data_len);
            } else if (memcmp(op, "stat", 4) == 0) {
                send_stat_t(sock);
            } else if (memcmp(op, "vers", 4) == 0) {
                ESP_LOGI(TAG, "Server version: %.*s", data_len, (char *)data);
            }
        }

        set_player_waiting_metadata();
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(SLIMPROTO_CONNECT_RETRY_MS));
    }
}

esp_err_t slimproto_player_start(const char *host, uint16_t port)
{
    if (!host || host[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_slimproto_task) {
        return ESP_OK;
    }

    size_t host_len = strlen(host);
    if (host_len >= sizeof(s_cfg.host)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(s_cfg.host, host);
    s_cfg.port = port;

    BaseType_t ok = xTaskCreatePinnedToCore(slimproto_task,
                                            "slimproto_task",
                                            SLIMPROTO_TASK_STACK,
                                            NULL,
                                            SLIMPROTO_TASK_PRIO,
                                            &s_slimproto_task,
                                            1);
    if (ok != pdPASS) {
        s_slimproto_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SlimProto player task started");
    return ESP_OK;
}
