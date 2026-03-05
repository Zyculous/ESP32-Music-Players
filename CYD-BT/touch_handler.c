#include "main.h"
#include "esp_check.h"

static const char *TAG = "TOUCH";

#define TOUCH_POLL_MS 30
#define TOUCH_DEBOUNCE_MS 180
#define FT6336_REG_TD_STATUS 0x02
#define FT6336_REG_P1_XH 0x03
#define FT6336_REG_P1_XL 0x04
#define FT6336_REG_P1_YH 0x05
#define FT6336_REG_P1_YL 0x06

static bool s_touch_initialized = false;
static uint32_t s_last_touch_tick = 0;
static i2c_master_dev_handle_t s_touch_dev_handle = NULL;

static uint8_t touch_volume_from_x(uint16_t x)
{
    if (x <= VOLUME_SLIDER_X) {
        return 0;
    }
    if (x >= (VOLUME_SLIDER_X + VOLUME_SLIDER_W)) {
        return 0x7F;
    }

    uint32_t rel = (uint32_t)(x - VOLUME_SLIDER_X);
    uint32_t vol = (rel * 0x7F) / VOLUME_SLIDER_W;
    if (vol > 0x7F) {
        vol = 0x7F;
    }
    return (uint8_t)vol;
}

static bool touch_in_rect(uint16_t x, uint16_t y, int rect_x, int rect_y, int rect_w, int rect_h)
{
    return (x >= rect_x) && (x < (rect_x + rect_w)) && (y >= rect_y) && (y < (rect_y + rect_h));
}

static bool touch_read_point(uint16_t *out_x, uint16_t *out_y)
{
    if (!out_x || !out_y) {
        return false;
    }

    if (!s_touch_dev_handle) {
        return false;
    }

    uint8_t status_reg = FT6336_REG_TD_STATUS;
    uint8_t status = 0;
    esp_err_t ret = i2c_master_transmit_receive(s_touch_dev_handle,
                                                 &status_reg,
                                                 1,
                                                 &status,
                                                 1,
                                                 10);
    if (ret != ESP_OK) {
        return false;
    }

    uint8_t touch_points = status & 0x0F;
    if (touch_points == 0) {
        return false;
    }

    uint8_t xy_reg = FT6336_REG_P1_XH;
    uint8_t xy[4] = {0};
    ret = i2c_master_transmit_receive(s_touch_dev_handle,
                                      &xy_reg,
                                      1,
                                      xy,
                                      sizeof(xy),
                                      10);
    if (ret != ESP_OK) {
        return false;
    }

    uint16_t raw_x = ((xy[0] & 0x0F) << 8) | xy[1];
    uint16_t raw_y = ((xy[2] & 0x0F) << 8) | xy[3];

    uint16_t mapped_x = raw_x;
    uint16_t mapped_y = raw_y;

#if TOUCH_MAP_SWAP_XY
    mapped_x = raw_y;
    mapped_y = raw_x;
#endif

#if TOUCH_INVERT_X
    mapped_x = (mapped_x >= LCD_H_RES) ? 0 : (LCD_H_RES - 1 - mapped_x);
#endif

#if TOUCH_INVERT_Y
    mapped_y = (mapped_y >= LCD_V_RES) ? 0 : (LCD_V_RES - 1 - mapped_y);
#endif

    if (mapped_x >= LCD_H_RES) {
        mapped_x = LCD_H_RES - 1;
    }
    if (mapped_y >= LCD_V_RES) {
        mapped_y = LCD_V_RES - 1;
    }

    *out_x = mapped_x;
    *out_y = mapped_y;
    return true;
}

static void touch_handle_press(uint16_t x, uint16_t y)
{
    if (touch_in_rect(x, y, VOLUME_SLIDER_X, VOLUME_SLIDER_Y, VOLUME_SLIDER_W, VOLUME_SLIDER_H + 6)) {
        uint8_t volume = touch_volume_from_x(x);
        audio_set_volume(volume);
        return;
    }

    if (touch_in_rect(x, y, PREV_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, BUTTON_SIZE)) {
        send_avrc_command(ESP_AVRC_PT_CMD_BACKWARD);
        return;
    }

    if (touch_in_rect(x, y, PLAY_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, BUTTON_SIZE)) {
        bool is_playing = false;
        if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            is_playing = g_player_state.metadata.is_playing;
            xSemaphoreGive(g_player_state_mutex);
        }
        send_avrc_command(is_playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
        return;
    }

    if (touch_in_rect(x, y, NEXT_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, BUTTON_SIZE)) {
        send_avrc_command(ESP_AVRC_PT_CMD_FORWARD);
    }
}

esp_err_t touch_init(void)
{
    if (s_touch_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = audio_get_shared_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "Shared I2C bus not initialized");
        return ESP_FAIL;
    }

    if (!s_touch_dev_handle) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TOUCH_I2C_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_touch_dev_handle),
                            TAG,
                            "touch i2c add device failed");
    }

#if TOUCH_RST_IO != GPIO_NUM_NC
    {
        gpio_config_t rst_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << TOUCH_RST_IO,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "touch rst gpio config failed");

        gpio_set_level(TOUCH_RST_IO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TOUCH_RST_IO, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
#endif

#if TOUCH_INT_IO != GPIO_NUM_NC
    {
        gpio_config_t int_cfg = {
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pin_bit_mask = 1ULL << TOUCH_INT_IO,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "touch int gpio config failed");
    }
#endif

    uint16_t x = 0;
    uint16_t y = 0;
    (void)touch_read_point(&x, &y);

    s_touch_initialized = true;
    ESP_LOGI(TAG, "FT6336 touch initialized");
    return ESP_OK;
}

void touch_task(void *params)
{
    (void)params;

    bool was_pressed = false;

    while (1) {
        if (!s_touch_initialized) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }

        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = touch_read_point(&x, &y);

        if (pressed && touch_in_rect(x, y, VOLUME_SLIDER_X, VOLUME_SLIDER_Y, VOLUME_SLIDER_W, VOLUME_SLIDER_H + 6)) {
            touch_handle_press(x, y);
        } else if (pressed && !was_pressed) {
            uint32_t now = xTaskGetTickCount();
            if ((now - s_last_touch_tick) > pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
                s_last_touch_tick = now;
                ESP_LOGI(TAG, "touch x=%u y=%u", (unsigned)x, (unsigned)y);
                touch_handle_press(x, y);
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}
