#include "main.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/queue.h"

static const char *TAG = "BLUETOOTH";

#define BT_NAME_NVS_NAMESPACE "cfg"
#define BT_NAME_NVS_KEY "bt_name"
#define BT_WEBFLASH_SEED_PREFIX "__WF_BT_NAME_SEED__:"
#define BT_WEBFLASH_SEED_LEN 31

__attribute__((used)) static const char s_webflash_bt_name_seed[] =
    BT_WEBFLASH_SEED_PREFIX "CYD Music Player               ";
static char s_runtime_bt_device_name[BT_WEBFLASH_SEED_LEN + 1] = BT_DEVICE_NAME;

#define COVER_ART_MAX_SIZE (64 * 1024)
#define COVER_ART_ENABLED CYD_FEATURE_IMAGE_LOADING
#define COVER_ART_MIN_FREE_HEAP (45 * 1024)
#define COVER_ART_MIN_LARGEST_BLOCK (20 * 1024)

// Static variables for bluetooth state
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;
static bool s_a2dp_sink_enabled = false;
static bool s_avrc_ct_enabled = false;
static bool s_avrc_tg_enabled = false;
static bool s_cover_art_connected = false;
static bool s_cover_art_handle_valid = false;
static uint8_t s_cover_art_handle[ESP_AVRC_CA_IMAGE_HANDLE_LEN] = {0};
static uint8_t *s_cover_art_rx_buffer = NULL;
static size_t s_cover_art_rx_size = 0;
static bool s_cover_art_request_in_progress = false;
static uint8_t s_cover_art_jpeg_profile = 0; // 0=160x160, 1=96x96
static bool s_cover_art_disabled_oom = false;
static bool s_cover_art_runtime_enabled = true;

typedef struct {
    esp_avrc_pt_cmd_t cmd;
} avrc_cmd_job_t;

static QueueHandle_t s_avrc_cmd_queue = NULL;
static TaskHandle_t s_avrc_cmd_task_handle = NULL;
#define AVRC_CMD_QUEUE_LEN 8
#define AVRC_CMD_TASK_STACK 3072
#define AVRC_CMD_TASK_PRIO 3

static bool s_volume_change_registered = false;

const char *bluetooth_get_device_name(void)
{
    return s_runtime_bt_device_name;
}

static void trim_trailing_spaces(char *text)
{
    if (!text) {
        return;
    }

    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == ' ') {
        text[len - 1] = '\0';
        len--;
    }
}

static void get_seeded_bt_name(char *out_name, size_t out_size)
{
    if (!out_name || out_size == 0) {
        return;
    }

    out_name[0] = '\0';
    const size_t prefix_len = strlen(BT_WEBFLASH_SEED_PREFIX);
    const size_t seed_len = strlen(s_webflash_bt_name_seed);
    if (seed_len < prefix_len + BT_WEBFLASH_SEED_LEN) {
        snprintf(out_name, out_size, "%s", BT_DEVICE_NAME);
        return;
    }

    size_t copy_len = BT_WEBFLASH_SEED_LEN;
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }

    memcpy(out_name, s_webflash_bt_name_seed + prefix_len, copy_len);
    out_name[copy_len] = '\0';
    trim_trailing_spaces(out_name);
    if (out_name[0] == '\0') {
        snprintf(out_name, out_size, "%s", BT_DEVICE_NAME);
    }
}

static void load_or_seed_bt_name(char *out_name, size_t out_size)
{
    if (!out_name || out_size == 0) {
        return;
    }

    out_name[0] = '\0';
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(BT_NAME_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace for BT name: %s", esp_err_to_name(err));
        get_seeded_bt_name(out_name, out_size);
        return;
    }

    size_t len = out_size;
    err = nvs_get_str(nvs_handle, BT_NAME_NVS_KEY, out_name, &len);
    if (err == ESP_OK && out_name[0] != '\0') {
        nvs_close(nvs_handle);
        return;
    }

    get_seeded_bt_name(out_name, out_size);
    err = nvs_set_str(nvs_handle, BT_NAME_NVS_KEY, out_name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist BT name to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
}

static void bt_avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
static void clear_cover_art_rx_buffer(void);
static void clear_cover_art_data(void);

static void register_avrc_notifications(void)
{
    if (!s_avrc_ct_enabled) {
        return;
    }

    esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_CHANGE, 0);
    esp_avrc_ct_send_register_notification_cmd(3, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
}

static void *alloc_prefer_spiram(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
        return ptr;
    }
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void avrc_cmd_task(void *params)
{
    (void)params;
    avrc_cmd_job_t job = {0};
    
    ESP_LOGI(TAG, "[DIAG] AVRCP command task started");

    while (1) {
        ESP_LOGI(TAG, "[DIAG] AVRCP task waiting for command...");
        if (xQueueReceive(s_avrc_cmd_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "[DIAG] AVRCP task received cmd=%d", job.cmd);

        if (!s_avrc_ct_enabled) {
            ESP_LOGW(TAG, "[DIAG] AVRCP controller not enabled, skipping");
            continue;
        }

        ESP_LOGI(TAG, "[DIAG] Sending PRESSED state");
        esp_avrc_ct_send_passthrough_cmd(1, job.cmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
        ESP_LOGI(TAG, "[DIAG] Delaying 30ms");
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_LOGI(TAG, "[DIAG] Sending RELEASED state");
        esp_avrc_ct_send_passthrough_cmd(1, job.cmd, ESP_AVRC_PT_CMD_STATE_RELEASED);
        ESP_LOGI(TAG, "[DIAG] Command completed");
    }
}

static void refresh_cover_art_memory_on_skip(void)
{
    s_cover_art_request_in_progress = false;
    s_cover_art_disabled_oom = false;
    s_cover_art_runtime_enabled = true;
    clear_cover_art_rx_buffer();
    s_cover_art_jpeg_profile = 0;
}

static bool cover_art_memory_ok(void)
{
    size_t free_heap = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    return (free_heap >= COVER_ART_MIN_FREE_HEAP) && (largest_block >= COVER_ART_MIN_LARGEST_BLOCK);
}

static void disable_cover_art_for_session(const char *reason)
{
    if (s_cover_art_disabled_oom) {
        return;
    }

    s_cover_art_disabled_oom = true;
    s_cover_art_request_in_progress = false;
    clear_cover_art_rx_buffer();
    clear_cover_art_data();
    ESP_LOGW(TAG, "Cover art disabled for this session: %s", reason ? reason : "memory pressure");
}

static bool ensure_cover_art_capacity(size_t needed)
{
    if (!s_cover_art_rx_buffer) {
        return false;
    }
    return needed <= COVER_ART_MAX_SIZE;
}

static void update_local_volume(uint8_t volume, bool notify_remote)
{
    if (volume > 0x7F) {
        volume = 0x7F;
    }

    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
    if (g_player_state.volume == volume) {
        xSemaphoreGive(g_player_state_mutex);
        return;
    }

    g_player_state.volume = volume;
    xSemaphoreGive(g_player_state_mutex);
    ESP_LOGI(TAG, "Volume updated: %d%%", (int)volume * 100 / 0x7F);

    if (notify_remote && s_volume_change_registered) {
        esp_avrc_rn_param_t rn_param = {0};
        rn_param.volume = volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        s_volume_change_registered = false;
    }
}

static void copy_metadata_text(char *dst, size_t dst_size, const uint8_t *src, int src_len)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src || src_len <= 0) {
        dst[0] = '\0';
        return;
    }

    size_t copy_len = (size_t)src_len;
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static uint32_t parse_metadata_u32(const uint8_t *src, int src_len)
{
    if (!src || src_len <= 0) {
        return 0;
    }

    char number_buf[16] = {0};
    size_t copy_len = (size_t)src_len;
    if (copy_len >= sizeof(number_buf)) {
        copy_len = sizeof(number_buf) - 1;
    }
    memcpy(number_buf, src, copy_len);
    return (uint32_t)strtoul(number_buf, NULL, 10);
}

static void clear_cover_art_data(void)
{
    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
    if (g_player_state.cover_art.data) {
        free(g_player_state.cover_art.data);
        g_player_state.cover_art.data = NULL;
    }
    g_player_state.cover_art.size = 0;
    g_player_state.cover_art.width = 0;
    g_player_state.cover_art.height = 0;
    g_player_state.cover_art.valid = false;
    g_player_state.cover_art.version++;
    xSemaphoreGive(g_player_state_mutex);

    cover_art_t empty_cover = {0};
    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
    empty_cover.version = g_player_state.cover_art.version;
    xSemaphoreGive(g_player_state_mutex);
    display_update_cover_art(&empty_cover);
    ui_request_refresh(UI_DIRTY_COVER_ART);
}

static void clear_cover_art_rx_buffer(void)
{
    s_cover_art_rx_size = 0;
}

static bool cover_art_looks_like_jpeg(const uint8_t *data, size_t len)
{
    if (!data || len < 8) {
        return false;
    }

    if (!(data[0] == 0xFF && data[1] == 0xD8)) {
        return false;
    }

    if (!(data[len - 2] == 0xFF && data[len - 1] == 0xD9)) {
        return false;
    }

    return true;
}

esp_err_t bluetooth_init(void)
{
    esp_err_t ret;

    load_or_seed_bt_name(s_runtime_bt_device_name, sizeof(s_runtime_bt_device_name));

    if (COVER_ART_ENABLED && !s_cover_art_rx_buffer) {
        s_cover_art_rx_buffer = alloc_prefer_spiram(COVER_ART_MAX_SIZE);
        if (!s_cover_art_rx_buffer) {
            ESP_LOGW(TAG, "Cover art disabled: unable to allocate RX buffer");
            s_cover_art_disabled_oom = true;
        }
    }
    // Release memory reserved for classic BT (not used)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    
    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "initialize controller failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Enable BT controller
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        ESP_LOGE(TAG, "enable controller failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize and enable bluedroid
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "initialize bluedroid failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "enable bluedroid failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set device name
    ret = esp_bt_dev_set_device_name(s_runtime_bt_device_name);
    if (ret) {
        ESP_LOGE(TAG, "set device name failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize AVRC controller and target (before A2DP)
    ret = esp_avrc_ct_register_callback(bt_avrc_callback);
    if (ret) {
        ESP_LOGE(TAG, "avrc controller register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_avrc_ct_init();
    if (ret) {
        ESP_LOGE(TAG, "avrc controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_avrc_tg_register_callback(bt_avrc_tg_callback);
    if (ret) {
        ESP_LOGE(TAG, "avrc target register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_avrc_tg_init();
    if (ret) {
        ESP_LOGE(TAG, "avrc target init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_avrc_tg_enabled = true;

    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    ret = esp_avrc_tg_set_rn_evt_cap(&evt_set);
    if (ret) {
        ESP_LOGW(TAG, "Failed to set AVRCP TG event caps: %s", esp_err_to_name(ret));
    }
    
    // Now initialize A2DP sink
    ret = esp_a2d_register_callback(&bt_a2dp_callback);
    if (ret) {
        ESP_LOGE(TAG, "a2dp register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_a2d_sink_register_data_callback(audio_data_callback);
    if (ret) {
        ESP_LOGE(TAG, "a2dp register data callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_a2d_sink_init();
    if (ret) {
        ESP_LOGE(TAG, "a2dp sink init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register GAP callback
    ret = esp_bt_gap_register_callback(bt_gap_callback);
    if (ret) {
        ESP_LOGE(TAG, "gap register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set connectable and discoverable mode
    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret) {
        ESP_LOGE(TAG, "set scan mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_avrc_cmd_queue) {
        s_avrc_cmd_queue = xQueueCreate(AVRC_CMD_QUEUE_LEN, sizeof(avrc_cmd_job_t));
        if (!s_avrc_cmd_queue) {
            ESP_LOGE(TAG, "Failed to create AVRCP command queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_avrc_cmd_task_handle) {
        if (xTaskCreatePinnedToCore(avrc_cmd_task,
                                    "avrc_cmd_task",
                                    AVRC_CMD_TASK_STACK,
                                    NULL,
                                    AVRC_CMD_TASK_PRIO,
                                    &s_avrc_cmd_task_handle,
                                    0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create AVRCP command task");
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "Bluetooth initialized successfully");
    return ESP_OK;
}

static void bt_avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRC TG connection state: %d", param->conn_stat.connected);
            if (!param->conn_stat.connected) {
                s_volume_change_registered = false;
            }
            break;

        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            update_local_volume(param->set_abs_vol.volume, true);
            break;

        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                s_volume_change_registered = true;
                esp_avrc_rn_param_t rn_param = {0};
                rn_param.volume = g_player_state.volume;
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
            }
            break;

        default:
            break;
    }
}

void bt_a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "A2DP connection state: %d", param->conn_stat.state);
            xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
            g_player_state.connection_state = param->conn_stat.state;
            xSemaphoreGive(g_player_state_mutex);
            
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected");
                xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                g_player_state.connected = true;
                xSemaphoreGive(g_player_state_mutex);
                s_cover_art_request_in_progress = false;
                s_cover_art_jpeg_profile = 0;
                s_cover_art_disabled_oom = (s_cover_art_rx_buffer == NULL);
                s_cover_art_runtime_enabled = true;
                
                // Request metadata only
                request_metadata();
                ui_request_refresh(UI_DIRTY_LAYOUT | UI_DIRTY_METADATA | UI_DIRTY_PLAY_STATE | UI_DIRTY_TIMELINE | UI_DIRTY_COVER_ART);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "A2DP disconnected");
                xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                g_player_state.connected = false;
                memset(&g_player_state.metadata, 0, sizeof(track_metadata_t));
                xSemaphoreGive(g_player_state_mutex);
                s_cover_art_handle_valid = false;
                memset(s_cover_art_handle, 0, sizeof(s_cover_art_handle));
                s_cover_art_request_in_progress = false;
                s_cover_art_jpeg_profile = 0;
                s_cover_art_disabled_oom = (s_cover_art_rx_buffer == NULL);
                s_cover_art_runtime_enabled = true;
                clear_cover_art_rx_buffer();
                clear_cover_art_data();
                ui_request_refresh(UI_DIRTY_ALL);
            }
            break;
            
        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "A2DP audio state: %d", param->audio_stat.state);
            xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
            g_player_state.audio_state = param->audio_stat.state;
            
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                g_player_state.metadata.is_playing = true;
                xSemaphoreGive(g_player_state_mutex);
                request_metadata();
#if COVER_ART_ENABLED
                request_cover_art();
#endif
                ui_request_refresh(UI_DIRTY_PLAY_STATE);
            } else {
                g_player_state.metadata.is_playing = false;
                xSemaphoreGive(g_player_state_mutex);
                ui_request_refresh(UI_DIRTY_PLAY_STATE);
            }
            break;
            
        case ESP_A2D_AUDIO_CFG_EVT:
            ESP_LOGI(TAG, "A2DP audio config received");
            
            // Decode codec config using sbc_info bit masks
            if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
                uint32_t sample_rate = 16000;
                uint8_t channels = 2;

                if (param->audio_cfg.mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
                    sample_rate = 32000;
                } else if (param->audio_cfg.mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
                    sample_rate = 44100;
                } else if (param->audio_cfg.mcc.cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
                    sample_rate = 48000;
                }

                if (param->audio_cfg.mcc.cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
                    channels = 1;
                }

                ESP_LOGI(TAG, "A2DP detected format: %lu Hz, %d channels", sample_rate, channels);
                
                ESP_LOGI(TAG, "Applying detected audio format to output");
                audio_reconfigure(sample_rate, channels);
            } else {
                ESP_LOGW(TAG, "Non-SBC codec type: %d", param->audio_cfg.mcc.type);
            }
            break;
            
        default:
            ESP_LOGW(TAG, "Unhandled A2DP event: %d", event);
            break;
    }
}

void bt_avrc_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRC connection state: %d", param->conn_stat.connected);
            if (param->conn_stat.connected) {
                ESP_LOGI(TAG, "AVRC connected");
                s_avrc_ct_enabled = true;
                s_cover_art_connected = false;
                s_cover_art_runtime_enabled = true;

#if COVER_ART_ENABLED
                esp_err_t ca_ret = esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX);
                if (ca_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Cover art connect failed: %s", esp_err_to_name(ca_ret));
                }
#endif
                
                request_metadata();
                register_avrc_notifications();
                ui_request_refresh(UI_DIRTY_LAYOUT | UI_DIRTY_METADATA | UI_DIRTY_PLAY_STATE | UI_DIRTY_TIMELINE | UI_DIRTY_COVER_ART);
            } else {
                ESP_LOGI(TAG, "AVRC disconnected");
                s_avrc_ct_enabled = false;
                s_cover_art_connected = false;
                s_cover_art_handle_valid = false;
                memset(s_cover_art_handle, 0, sizeof(s_cover_art_handle));
                s_cover_art_request_in_progress = false;
                s_cover_art_jpeg_profile = 0;
                s_cover_art_disabled_oom = (s_cover_art_rx_buffer == NULL);
                s_cover_art_runtime_enabled = true;
                xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                memset(&g_player_state.metadata, 0, sizeof(track_metadata_t));
                xSemaphoreGive(g_player_state_mutex);
                clear_cover_art_rx_buffer();
                clear_cover_art_data();
                ui_request_refresh(UI_DIRTY_ALL);
            }
            break;

        case ESP_AVRC_CT_COVER_ART_STATE_EVT:
            s_cover_art_connected = (param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED);
            ESP_LOGI(TAG, "Cover art state: %d, reason: %d", param->cover_art_state.state, param->cover_art_state.reason);
#if COVER_ART_ENABLED
            if (s_cover_art_connected && s_cover_art_handle_valid) {
                request_cover_art();
            }
#endif
            break;
            
        case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
            ESP_LOGI(TAG, "AVRC passthrough response: key_code=%d, key_state=%d",
                     param->psth_rsp.key_code, param->psth_rsp.key_state);
            break;
            
        case ESP_AVRC_CT_METADATA_RSP_EVT:
            ESP_LOGI(TAG, "AVRC metadata response: attr_id=%d", param->meta_rsp.attr_id);
            
            // Handle individual attribute (ESP-IDF v5.5 API change)
            switch (param->meta_rsp.attr_id) {
                case ESP_AVRC_MD_ATTR_TITLE:
                    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                    copy_metadata_text(g_player_state.metadata.title,
                                       sizeof(g_player_state.metadata.title),
                                       param->meta_rsp.attr_text,
                                       param->meta_rsp.attr_length);
                    xSemaphoreGive(g_player_state_mutex);
                    ESP_LOGI(TAG, "Title: %s", g_player_state.metadata.title);
                    ui_request_refresh(UI_DIRTY_METADATA);
                    break;
                    
                case ESP_AVRC_MD_ATTR_ARTIST:
                    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                    copy_metadata_text(g_player_state.metadata.artist,
                                       sizeof(g_player_state.metadata.artist),
                                       param->meta_rsp.attr_text,
                                       param->meta_rsp.attr_length);
                    xSemaphoreGive(g_player_state_mutex);
                    ESP_LOGI(TAG, "Artist: %s", g_player_state.metadata.artist);
                    ui_request_refresh(UI_DIRTY_METADATA);
                    break;
                    
                case ESP_AVRC_MD_ATTR_ALBUM:
                    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                    copy_metadata_text(g_player_state.metadata.album,
                                       sizeof(g_player_state.metadata.album),
                                       param->meta_rsp.attr_text,
                                       param->meta_rsp.attr_length);
                    xSemaphoreGive(g_player_state_mutex);
                    ESP_LOGI(TAG, "Album: %s", g_player_state.metadata.album);
                    ui_request_refresh(UI_DIRTY_METADATA);
                    break;

                case ESP_AVRC_MD_ATTR_COVER_ART:
#if COVER_ART_ENABLED
                    if (!s_cover_art_runtime_enabled) {
                        break;
                    }
                    if (param->meta_rsp.attr_length == ESP_AVRC_CA_IMAGE_HANDLE_LEN) {
                        if (memcmp(s_cover_art_handle, param->meta_rsp.attr_text, ESP_AVRC_CA_IMAGE_HANDLE_LEN) != 0) {
                            memcpy(s_cover_art_handle, param->meta_rsp.attr_text, ESP_AVRC_CA_IMAGE_HANDLE_LEN);
                            s_cover_art_handle_valid = true;
                            s_cover_art_request_in_progress = false;
                            s_cover_art_jpeg_profile = 0;
                            clear_cover_art_rx_buffer();
                            clear_cover_art_data();
                            ESP_LOGI(TAG, "Cover art handle updated");
                            request_cover_art();
                            ui_request_refresh(UI_DIRTY_COVER_ART);
                        }
                    } else {
                        ESP_LOGW(TAG, "Invalid cover art handle length: %d", param->meta_rsp.attr_length);
                    }
#endif
                    break;
                    
                case ESP_AVRC_MD_ATTR_TRACK_NUM:
                    ESP_LOGI(TAG, "Track number: %.*s",
                             param->meta_rsp.attr_length,
                             (const char *)param->meta_rsp.attr_text);
                    break;
                    
                case ESP_AVRC_MD_ATTR_PLAYING_TIME:
                    xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                    g_player_state.metadata.duration = parse_metadata_u32(
                        param->meta_rsp.attr_text,
                        param->meta_rsp.attr_length);
                        g_player_state.metadata.position = 0;
                    xSemaphoreGive(g_player_state_mutex);
                    ESP_LOGI(TAG, "Duration: %d ms", g_player_state.metadata.duration);
                        ui_request_refresh(UI_DIRTY_METADATA | UI_DIRTY_TIMELINE);
                    break;
            }
            break;
            
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            ESP_LOGI(TAG, "AVRC notify event: %d", param->change_ntf.event_id);
            if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE ||
                param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                request_metadata();
            }

            if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
                esp_avrc_ct_send_register_notification_cmd(2, ESP_AVRC_RN_TRACK_CHANGE, 0);
            } else if (param->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                esp_avrc_ct_send_register_notification_cmd(3, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
            }
            break;
            
        case ESP_AVRC_CT_COVER_ART_DATA_EVT:
#if COVER_ART_ENABLED
            if (!s_cover_art_runtime_enabled) {
                break;
            }
            ESP_LOGI(TAG, "Cover art data: status=%d, len=%d, final=%s", 
                     param->cover_art_data.status, param->cover_art_data.data_len, 
                     param->cover_art_data.final ? "true" : "false");

            if (param->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "Cover art transfer status error: %d", param->cover_art_data.status);
                clear_cover_art_rx_buffer();
                if (param->cover_art_data.final) {
                    s_cover_art_request_in_progress = false;
                }
                break;
            }
            
            if (param->cover_art_data.data_len > 0) {

                if (s_cover_art_rx_size + param->cover_art_data.data_len > COVER_ART_MAX_SIZE) {
                    ESP_LOGW(TAG, "Cover art too large, dropping (%u + %u bytes)",
                             (unsigned)s_cover_art_rx_size, param->cover_art_data.data_len);
                    clear_cover_art_rx_buffer();
                    s_cover_art_request_in_progress = false;
                    break;
                }

                size_t new_size = s_cover_art_rx_size + param->cover_art_data.data_len;
                if (!ensure_cover_art_capacity(new_size)) {
                    ESP_LOGE(TAG, "Cover art realloc failed");
                    clear_cover_art_rx_buffer();
                    s_cover_art_request_in_progress = false;
                    disable_cover_art_for_session("rx buffer allocation failed");
                    break;
                }
                memcpy(s_cover_art_rx_buffer + s_cover_art_rx_size,
                       param->cover_art_data.p_data,
                       param->cover_art_data.data_len);
                s_cover_art_rx_size += param->cover_art_data.data_len;
            }

            if (param->cover_art_data.final) {
                s_cover_art_request_in_progress = false;

                if (!cover_art_looks_like_jpeg(s_cover_art_rx_buffer, s_cover_art_rx_size)) {
                    ESP_LOGW(TAG, "Cover art payload is not JPEG-like, retrying with smaller JPEG profile=%u",
                             (unsigned)s_cover_art_jpeg_profile);
                    clear_cover_art_rx_buffer();
                    if (s_cover_art_jpeg_profile == 0) {
                        s_cover_art_jpeg_profile = 1;
                        request_cover_art();
                    }
                    break;
                }
                xSemaphoreTake(g_player_state_mutex, portMAX_DELAY);
                g_player_state.cover_art.version++;
                uint32_t new_version = g_player_state.cover_art.version;
                size_t assembled_size = s_cover_art_rx_size;
                xSemaphoreGive(g_player_state_mutex);

                cover_art_t render_cover = {0};
                render_cover.data = s_cover_art_rx_buffer;
                render_cover.size = s_cover_art_rx_size;
                render_cover.valid = (s_cover_art_rx_size > 0);
                render_cover.version = new_version;
                display_update_cover_art(&render_cover);
                ui_request_refresh(UI_DIRTY_COVER_ART);

                ESP_LOGI(TAG, "Cover art assembled: %u bytes", (unsigned)assembled_size);

                clear_cover_art_rx_buffer();
            }
            break;
#else
            break;
#endif
            
        default:
            ESP_LOGW(TAG, "Unhandled AVRC event: %d", event);
            break;
    }
}

void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
            }
            break;
            
        case ESP_BT_GAP_PIN_REQ_EVT:
            ESP_LOGI(TAG, "PIN request");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 0, pin_code);
            break;
            
        default:
            ESP_LOGW(TAG, "Unhandled GAP event: %d", event);
            break;
    }
}

void send_avrc_command(esp_avrc_pt_cmd_t cmd)
{
    ESP_LOGI(TAG, "[DIAG] send_avrc_command START cmd=%d", cmd);
    if (s_avrc_ct_enabled) {
        if (cmd == ESP_AVRC_PT_CMD_FORWARD || cmd == ESP_AVRC_PT_CMD_BACKWARD) {
            ESP_LOGI(TAG, "[DIAG] Skip detected, refreshing cover art memory");
            refresh_cover_art_memory_on_skip();
            request_metadata();
        }

        if (s_avrc_cmd_queue) {
            avrc_cmd_job_t job = {.cmd = cmd};
            ESP_LOGI(TAG, "[DIAG] Queueing command to async task");
            if (xQueueSend(s_avrc_cmd_queue, &job, 0) != pdTRUE) {
                ESP_LOGW(TAG, "AVRCP cmd queue full, dropping cmd=%d", cmd);
            }
            ESP_LOGI(TAG, "[DIAG] Command queued successfully");
        } else {
            ESP_LOGW(TAG, "[DIAG] AVRCP cmd queue not initialized!");
        }
    } else {
        ESP_LOGW(TAG, "[DIAG] AVRCP controller not enabled!");
    }
    ESP_LOGI(TAG, "[DIAG] send_avrc_command END");
}

void bluetooth_send_seek_cmd(bool forward, uint32_t hold_ms)
{
    if (!s_avrc_ct_enabled) {
        return;
    }

    esp_avrc_pt_cmd_t cmd = forward ? ESP_AVRC_PT_CMD_FORWARD : ESP_AVRC_PT_CMD_BACKWARD;
    uint32_t clamped_hold = hold_ms;
    if (clamped_hold < 250) {
        clamped_hold = 250;
    }
    if (clamped_hold > 1500) {
        clamped_hold = 1500;
    }

    esp_avrc_ct_send_passthrough_cmd(1, cmd, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(clamped_hold));
    esp_avrc_ct_send_passthrough_cmd(1, cmd, ESP_AVRC_PT_CMD_STATE_RELEASED);
    ui_request_refresh(UI_DIRTY_TIMELINE);
    ESP_LOGI(TAG, "Sent AVRCP seek command: %s (%ums)", forward ? "FF" : "REW", (unsigned)clamped_hold);
}

void request_metadata(void)
{
    if (s_avrc_ct_enabled) {
        uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                           ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_TRACK_NUM |
                           ESP_AVRC_MD_ATTR_PLAYING_TIME;
        if (COVER_ART_ENABLED && s_cover_art_runtime_enabled) {
            attr_mask |= ESP_AVRC_MD_ATTR_COVER_ART;
        }
        esp_avrc_ct_send_metadata_cmd(1, attr_mask);
        ESP_LOGI(TAG, "Requested metadata");
    }
}

void request_cover_art(void)
{
#if !COVER_ART_ENABLED
    return;
#else
    if (!s_cover_art_runtime_enabled) {
        return;
    }

    if (s_avrc_ct_enabled && s_cover_art_connected && s_cover_art_handle_valid) {
        if (s_cover_art_disabled_oom) {
            return;
        }

        if (!cover_art_memory_ok()) {
            disable_cover_art_for_session("insufficient heap before request");
            return;
        }

        static const uint8_t s_medium_image_descriptor[] =
            "<image-descriptor version=\"1.0\">"
            "<image encoding=\"JPEG\" pixel=\"160*160\"/>"
            "</image-descriptor>";
        static const uint8_t s_small_image_descriptor[] =
            "<image-descriptor version=\"1.0\">"
            "<image encoding=\"JPEG\" pixel=\"96*96\"/>"
            "</image-descriptor>";

        if (s_cover_art_request_in_progress) {
            ESP_LOGI(TAG, "Cover art request already in progress, skipping");
            return;
        }

        ESP_LOGI(TAG, "Requesting cover art image...");

        clear_cover_art_rx_buffer();

        const uint8_t *descriptor = (s_cover_art_jpeg_profile == 0) ? s_medium_image_descriptor : s_small_image_descriptor;
        uint16_t descriptor_len = (s_cover_art_jpeg_profile == 0)
                      ? (sizeof(s_medium_image_descriptor) - 1)
                      : (sizeof(s_small_image_descriptor) - 1);

        esp_err_t ret = esp_avrc_ct_cover_art_get_image(s_cover_art_handle,
                                                        (uint8_t *)descriptor,
                                                        descriptor_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "JPEG cover art request failed (profile=%u): %s",
                     (unsigned)s_cover_art_jpeg_profile,
                     esp_err_to_name(ret));
            if (s_cover_art_jpeg_profile == 0) {
                s_cover_art_jpeg_profile = 1;
                request_cover_art();
            }
        } else {
            s_cover_art_request_in_progress = true;
            ESP_LOGI(TAG, "Cover art JPEG request sent (profile=%u)", (unsigned)s_cover_art_jpeg_profile);
        }
    } else {
        ESP_LOGW(TAG, "Cover art not ready (avrc=%d connected=%d handle=%d)",
                 s_avrc_ct_enabled, s_cover_art_connected, s_cover_art_handle_valid);
    }
#endif
}

// Wrapper function for UI compatibility
void bluetooth_send_passthrough_cmd(esp_avrc_pt_cmd_t cmd)
{
    send_avrc_command(cmd);
}