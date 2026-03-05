#include "main.h"

#include "esp_check.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_gap.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

static const char *TAG = "BLE_MEDIA";

#define HID_RPT_ID_CC_IN 3
#define HID_CC_IN_RPT_LEN 2

#define HID_CONSUMER_PLAY        176
#define HID_CONSUMER_PAUSE       177
#define HID_CONSUMER_NEXT_TRACK  181
#define HID_CONSUMER_PREV_TRACK  182
#define HID_CONSUMER_PLAY_PAUSE  205
#define BLE_APPEARANCE_MEDIA_PLAYER 0x0840

#define BLE_AUDIO_PKT_CONFIG 0x01
#define BLE_AUDIO_PKT_PCM    0x02

#define BLE_AUDIO_APP_ID 0x55
#define BLE_AUDIO_MAX_DATA_LEN 512

#define BLE_AUDIO_SERVICE_UUID      0x00FF
#define BLE_AUDIO_CHAR_CFG_UUID     0xFF01
#define BLE_AUDIO_CHAR_DATA_UUID    0xFF02

enum {
    BLE_AUDIO_IDX_SVC = 0,
    BLE_AUDIO_IDX_CFG_CHAR,
    BLE_AUDIO_IDX_CFG_VAL,
    BLE_AUDIO_IDX_DATA_CHAR,
    BLE_AUDIO_IDX_DATA_VAL,
    BLE_AUDIO_IDX_NB,
};

static esp_hidd_dev_t *s_hid_dev = NULL;
static uint32_t s_ble_audio_sample_rate = 44100;
static uint8_t s_ble_audio_channels = 2;
static esp_gatt_if_t s_ble_audio_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_ble_audio_handles[BLE_AUDIO_IDX_NB] = {0};

static const uint16_t s_primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint8_t s_char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint16_t s_audio_service_uuid = BLE_AUDIO_SERVICE_UUID;
static const uint16_t s_audio_cfg_uuid = BLE_AUDIO_CHAR_CFG_UUID;
static const uint16_t s_audio_data_uuid = BLE_AUDIO_CHAR_DATA_UUID;
static uint8_t s_audio_cfg_value[4] = {2, 0x44, 0xAC, 0x00};
static uint8_t s_audio_data_value[BLE_AUDIO_MAX_DATA_LEN] = {0};

static const esp_gatts_attr_db_t s_ble_audio_gatt_db[BLE_AUDIO_IDX_NB] = {
        [BLE_AUDIO_IDX_SVC] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&s_primary_service_uuid, ESP_GATT_PERM_READ,
            sizeof(uint16_t), sizeof(uint16_t), (uint8_t *)&s_audio_service_uuid}},

        [BLE_AUDIO_IDX_CFG_CHAR] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&s_character_declaration_uuid, ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_char_prop_write}},

        [BLE_AUDIO_IDX_CFG_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&s_audio_cfg_uuid, ESP_GATT_PERM_WRITE,
            sizeof(s_audio_cfg_value), sizeof(s_audio_cfg_value), s_audio_cfg_value}},

        [BLE_AUDIO_IDX_DATA_CHAR] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&s_character_declaration_uuid, ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_char_prop_write}},

        [BLE_AUDIO_IDX_DATA_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&s_audio_data_uuid, ESP_GATT_PERM_WRITE,
            BLE_AUDIO_MAX_DATA_LEN, 0, s_audio_data_value}},
};

void ble_hid_task_start_up(void)
{
}

void ble_hid_task_shut_down(void)
{
}

static const unsigned char s_media_report_map[] = {
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, HID_RPT_ID_CC_IN,
    0x15, 0x00,
    0x26, 0xFF, 0x03,
    0x19, 0x00,
    0x2A, 0xFF, 0x03,
    0x75, 0x10,
    0x95, 0x01,
    0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t s_ble_report_maps[] = {
    {
        .data = s_media_report_map,
        .len = sizeof(s_media_report_map),
    },
};

static esp_hid_device_config_t s_ble_hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x4001,
    .version = 0x0100,
    .device_name = "ES3C28P Audio Remote",
    .manufacturer_name = "LCDWiki",
    .serial_number = "ES3C28P",
    .report_maps = s_ble_report_maps,
    .report_maps_len = 1,
};

static uint16_t media_cmd_to_usage(media_cmd_t cmd)
{
    switch (cmd) {
        case MEDIA_CMD_PLAY:
            return HID_CONSUMER_PLAY;
        case MEDIA_CMD_PAUSE:
            return HID_CONSUMER_PAUSE;
        case MEDIA_CMD_NEXT:
            return HID_CONSUMER_NEXT_TRACK;
        case MEDIA_CMD_PREV:
            return HID_CONSUMER_PREV_TRACK;
        default:
            return 0;
    }
}

static void ble_send_consumer_usage(uint16_t usage, bool pressed)
{
    if (!s_hid_dev || !esp_hidd_dev_connected(s_hid_dev)) {
        return;
    }

    uint16_t value = pressed ? usage : 0;
    uint8_t report[HID_CC_IN_RPT_LEN] = {
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
    };

    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, HID_RPT_ID_CC_IN, report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hidd_dev_input_set failed: %s", esp_err_to_name(err));
    }
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
        case ESP_HIDD_START_EVENT:
            ESP_LOGI(TAG, "BLE HID started, advertising");
            esp_hid_ble_gap_adv_start();
            break;

        case ESP_HIDD_CONNECT_EVENT:
            ESP_LOGI(TAG, "BLE HID connected");
            if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_player_state.connected = true;
                xSemaphoreGive(g_player_state_mutex);
            }
            break;

        case ESP_HIDD_DISCONNECT_EVENT:
            ESP_LOGI(TAG, "BLE HID disconnected, restart advertising");
            if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_player_state.connected = false;
                xSemaphoreGive(g_player_state_mutex);
            }
            esp_hid_ble_gap_adv_start();
            break;

        case ESP_HIDD_PROTOCOL_MODE_EVENT:
            ESP_LOGI(TAG, "BLE HID protocol mode event");
            break;

        case ESP_HIDD_CONTROL_EVENT:
            ESP_LOGI(TAG, "BLE HID control event: %u", (unsigned)param->control.control);
            break;

        default:
            break;
    }
}

static void ble_audio_gatts_handle_write(const esp_ble_gatts_cb_param_t *param)
{
    if (!param || !param->write.value || param->write.len == 0) {
        return;
    }

    uint16_t handle = param->write.handle;
    const uint8_t *value = param->write.value;
    uint16_t len = param->write.len;

    if (handle == s_ble_audio_handles[BLE_AUDIO_IDX_CFG_VAL]) {
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (len >= 4 && value[0] == BLE_AUDIO_PKT_CONFIG) {
            err = bluetooth_receive_ble_audio_packet(value, len);
        } else if (len >= 3) {
            uint8_t channels = value[0];
            uint16_t sample_rate = (uint16_t)value[1] | ((uint16_t)value[2] << 8);
            err = bluetooth_set_ble_audio_stream((uint32_t)sample_rate, channels);
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE audio cfg write rejected (len=%u): %s", (unsigned)len, esp_err_to_name(err));
        }
        return;
    }

    if (handle == s_ble_audio_handles[BLE_AUDIO_IDX_DATA_VAL]) {
        if (len >= 2 && (value[0] == BLE_AUDIO_PKT_PCM || value[0] == BLE_AUDIO_PKT_CONFIG)) {
            esp_err_t err = bluetooth_receive_ble_audio_packet(value, len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "BLE audio packet rejected: %s", esp_err_to_name(err));
            }
            return;
        }

        uint16_t pcm_len = len & ~1U;
        if (pcm_len > 0) {
            audio_data_callback(value, pcm_len);
        }
    }
}

static void ble_gatts_event_dispatcher(esp_gatts_cb_event_t event,
                                       esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t *param)
{
    esp_hidd_gatts_event_handler(event, gatts_if, param);

    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param && param->reg.status == ESP_GATT_OK && param->reg.app_id == BLE_AUDIO_APP_ID) {
                s_ble_audio_gatts_if = gatts_if;
                esp_err_t err = esp_ble_gatts_create_attr_tab(s_ble_audio_gatt_db,
                                                              gatts_if,
                                                              BLE_AUDIO_IDX_NB,
                                                              0);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "audio create attr table failed: %s", esp_err_to_name(err));
                }
            }
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (!param || gatts_if != s_ble_audio_gatts_if) {
                break;
            }
            if (param->add_attr_tab.status != ESP_GATT_OK || param->add_attr_tab.num_handle != BLE_AUDIO_IDX_NB) {
                ESP_LOGE(TAG, "audio attr table invalid (status=%d handles=%u)",
                         param->add_attr_tab.status,
                         (unsigned)param->add_attr_tab.num_handle);
                break;
            }
            memcpy(s_ble_audio_handles, param->add_attr_tab.handles, sizeof(s_ble_audio_handles));
            esp_err_t start_err = esp_ble_gatts_start_service(s_ble_audio_handles[BLE_AUDIO_IDX_SVC]);
            if (start_err != ESP_OK) {
                ESP_LOGE(TAG, "audio service start failed: %s", esp_err_to_name(start_err));
            } else {
                ESP_LOGI(TAG, "BLE audio GATT service started (UUID 0x%04X)", BLE_AUDIO_SERVICE_UUID);
            }
            break;

        case ESP_GATTS_WRITE_EVT:
            if (!param || gatts_if != s_ble_audio_gatts_if) {
                break;
            }
            if (!param->write.is_prep) {
                ble_audio_gatts_handle_write(param);
            }
            break;

        default:
            break;
    }
}

esp_err_t bluetooth_init(void)
{
    ESP_RETURN_ON_ERROR(esp_hid_gap_init(HIDD_BLE_MODE), TAG, "esp_hid_gap_init failed");
    ESP_RETURN_ON_ERROR(esp_hid_ble_gap_adv_init(BLE_APPEARANCE_MEDIA_PLAYER, s_ble_hid_config.device_name),
                        TAG,
                        "esp_hid_ble_gap_adv_init failed");

    ESP_RETURN_ON_ERROR(esp_ble_gatt_set_local_mtu(517),
                        TAG,
                        "set local mtu failed");

    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(ble_gatts_event_dispatcher),
                        TAG,
                        "register hidd gatts callback failed");

    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_ble_hid_config,
                                          ESP_HID_TRANSPORT_BLE,
                                          ble_hidd_event_callback,
                                          &s_hid_dev),
                        TAG,
                        "esp_hidd_dev_init failed");

    ESP_RETURN_ON_ERROR(esp_ble_gatts_app_register(BLE_AUDIO_APP_ID),
                        TAG,
                        "audio app register failed");

    ESP_LOGI(TAG, "BLE media remote + audio ingest initialized");
    return ESP_OK;
}

void bluetooth_send_passthrough_cmd(media_cmd_t cmd)
{
    uint16_t usage = media_cmd_to_usage(cmd);
    if (usage == 0) {
        return;
    }

    ble_send_consumer_usage(usage, true);
    vTaskDelay(pdMS_TO_TICKS(40));
    ble_send_consumer_usage(usage, false);
}

void send_avrc_command(media_cmd_t cmd)
{
    bluetooth_send_passthrough_cmd(cmd);
}

void bluetooth_send_seek_cmd(bool forward, uint32_t hold_ms)
{
    (void)hold_ms;
    bluetooth_send_passthrough_cmd(forward ? MEDIA_CMD_NEXT : MEDIA_CMD_PREV);
}

void request_metadata(void)
{
}

void request_cover_art(void)
{
}

esp_err_t bluetooth_set_ble_audio_stream(uint32_t sample_rate, uint8_t channels)
{
    if (sample_rate < 8000 || sample_rate > 48000) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t normalized_channels = (channels <= 1) ? 1 : 2;
    esp_err_t err = audio_reconfigure(sample_rate, normalized_channels);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_reconfigure failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ble_audio_sample_rate = sample_rate;
    s_ble_audio_channels = normalized_channels;
    ESP_LOGI(TAG, "BLE audio stream cfg: %lu Hz, %s",
             (unsigned long)s_ble_audio_sample_rate,
             (s_ble_audio_channels == 1) ? "mono" : "stereo");
    return ESP_OK;
}

esp_err_t bluetooth_receive_ble_audio_packet(const uint8_t *packet, uint32_t len)
{
    if (!packet || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t pkt_type = packet[0];
    switch (pkt_type) {
        case BLE_AUDIO_PKT_CONFIG: {
            if (len < 4) {
                return ESP_ERR_INVALID_SIZE;
            }

            uint8_t channels = packet[1];
            uint16_t sample_rate = (uint16_t)packet[2] | ((uint16_t)packet[3] << 8);
            if (sample_rate == 0) {
                return ESP_ERR_INVALID_ARG;
            }

            return bluetooth_set_ble_audio_stream((uint32_t)sample_rate, channels);
        }

        case BLE_AUDIO_PKT_PCM: {
            const uint8_t *pcm_data = packet + 1;
            uint32_t pcm_len = len - 1;
            if (pcm_len == 0) {
                return ESP_ERR_INVALID_SIZE;
            }

            if ((pcm_len & 0x01U) != 0) {
                pcm_len--;
            }
            if (pcm_len == 0) {
                return ESP_ERR_INVALID_SIZE;
            }

            audio_data_callback(pcm_data, pcm_len);
            return ESP_OK;
        }

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}
