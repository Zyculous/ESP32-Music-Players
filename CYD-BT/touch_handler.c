#include "main.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

static const char *TAG = "TOUCH";

#define TOUCH_POLL_MS 30
#define TOUCH_DEBOUNCE_MS 180
#define TOUCH_BUTTON_HITBOX_PAD 20
#define TOUCH_MOVE_LOG_INTERVAL_MS 200
#define TOUCH_SAMPLES 5
#define TOUCH_PRESSURE_THRESHOLD 100

/* XPT2046 control byte bit fields:
 *   S  A2 A1 A0  MODE SER/DFR PD1 PD0
 *   1  x  x  x   0     0      0   0
 * X+: A2=1, A1=0, A0=1 → 0xD0
 * Y+: A2=0, A1=0, A0=1 → 0x90
 * Z1: A2=0, A1=1, A0=1 → 0xB0
 * Z2: A2=1, A1=0, A0=0 → 0xC0
 */
#define XPT2046_CMD_X  0xD0
#define XPT2046_CMD_Y  0x90
#define XPT2046_CMD_Z1 0xB0
#define XPT2046_CMD_Z2 0xC0

static bool s_touch_initialized = false;
static uint32_t s_last_touch_tick = 0;

/* ── Software SPI bit-bang for XPT2046 ─────────────────────────────── */

static void xpt2046_gpio_init(void)
{
    gpio_config_t out_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << TOUCH_SPI_CLK) |
                        (1ULL << TOUCH_SPI_CS)  |
                        (1ULL << TOUCH_SPI_MOSI),
    };
    gpio_config(&out_cfg);

    gpio_config_t in_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << TOUCH_SPI_MISO) |
                        (1ULL << TOUCH_IRQ_IO),
    };
    gpio_config(&in_cfg);

    gpio_set_level(TOUCH_SPI_CS, 1);
    gpio_set_level(TOUCH_SPI_CLK, 0);
}

static uint16_t xpt2046_transfer(uint8_t cmd)
{
    gpio_set_level(TOUCH_SPI_CS, 0);

    /* Send 8-bit command, MSB first */
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(TOUCH_SPI_MOSI, (cmd >> bit) & 1);
        gpio_set_level(TOUCH_SPI_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(TOUCH_SPI_CLK, 0);
        esp_rom_delay_us(1);
    }

    /* One BUSY clock cycle */
    gpio_set_level(TOUCH_SPI_CLK, 1);
    esp_rom_delay_us(1);
    gpio_set_level(TOUCH_SPI_CLK, 0);
    esp_rom_delay_us(1);

    /* Read 12-bit result, MSB first */
    uint16_t val = 0;
    for (int i = 0; i < 12; i++) {
        gpio_set_level(TOUCH_SPI_CLK, 1);
        esp_rom_delay_us(1);
        val <<= 1;
        if (gpio_get_level(TOUCH_SPI_MISO)) {
            val |= 1;
        }
        gpio_set_level(TOUCH_SPI_CLK, 0);
        esp_rom_delay_us(1);
    }

    gpio_set_level(TOUCH_SPI_CS, 1);
    return val;
}

/* Read a channel N times, discard min/max, return average */
static uint16_t xpt2046_read_avg(uint8_t cmd)
{
    uint16_t samples[TOUCH_SAMPLES];
    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        samples[i] = xpt2046_transfer(cmd);
    }

    uint16_t smin = samples[0], smax = samples[0];
    uint32_t sum = samples[0];
    for (int i = 1; i < TOUCH_SAMPLES; i++) {
        if (samples[i] < smin) smin = samples[i];
        if (samples[i] > smax) smax = samples[i];
        sum += samples[i];
    }

    if (TOUCH_SAMPLES > 2) {
        sum -= smin;
        sum -= smax;
        return (uint16_t)(sum / (TOUCH_SAMPLES - 2));
    }
    return (uint16_t)(sum / TOUCH_SAMPLES);
}

/* ── Coordinate mapping ────────────────────────────────────────────── */

static bool touch_read_point(uint16_t *out_x, uint16_t *out_y)
{
    if (!out_x || !out_y) {
        return false;
    }

    /* Check T_IRQ — active low when screen is being touched */
    if (gpio_get_level(TOUCH_IRQ_IO) != 0) {
        return false;
    }

    uint16_t raw_x = xpt2046_read_avg(XPT2046_CMD_X);
    uint16_t raw_y = xpt2046_read_avg(XPT2046_CMD_Y);

    /* Basic pressure check via Z readings */
    uint16_t z1 = xpt2046_transfer(XPT2046_CMD_Z1);
    uint16_t z2 = xpt2046_transfer(XPT2046_CMD_Z2);
    int pressure = z1 + (4095 - z2);
    if (pressure < TOUCH_PRESSURE_THRESHOLD) {
        return false;
    }

    /* Map raw ADC range → pixel range */
    int32_t px = (int32_t)(raw_x - TOUCH_RAW_X_MIN) * (int32_t)LCD_H_RES
                 / (int32_t)(TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
    int32_t py = (int32_t)(raw_y - TOUCH_RAW_Y_MIN) * (int32_t)LCD_V_RES
                 / (int32_t)(TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);

    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= LCD_H_RES) px = LCD_H_RES - 1;
    if (py >= LCD_V_RES) py = LCD_V_RES - 1;

    uint16_t mapped_x = (uint16_t)px;
    uint16_t mapped_y = (uint16_t)py;

#if TOUCH_MAP_SWAP_XY
    { uint16_t tmp = mapped_x; mapped_x = mapped_y; mapped_y = tmp; }
    if (mapped_x >= LCD_H_RES) mapped_x = LCD_H_RES - 1;
    if (mapped_y >= LCD_V_RES) mapped_y = LCD_V_RES - 1;
#endif

#if TOUCH_INVERT_X
    mapped_x = LCD_H_RES - 1 - mapped_x;
#endif

#if TOUCH_INVERT_Y
    mapped_y = LCD_V_RES - 1 - mapped_y;
#endif

    *out_x = mapped_x;
    *out_y = mapped_y;
    return true;
}

/* ── UI hit-testing (unchanged from original) ──────────────────────── */

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

static bool touch_in_rect_padded(uint16_t x,
                                 uint16_t y,
                                 int rect_x,
                                 int rect_y,
                                 int rect_w,
                                 int rect_h,
                                 int pad)
{
    int px = rect_x - pad;
    int py = rect_y - pad;
    int pw = rect_w + (pad * 2);
    int ph = rect_h + (pad * 2);
    return touch_in_rect(x, y, px, py, pw, ph);
}

static bool touch_in_transport_buttons(uint16_t x, uint16_t y)
{
    return touch_in_rect_padded(x,
                                y,
                                PREV_BUTTON_X,
                                PLAY_BUTTON_Y,
                                PREV_BUTTON_W,
                                BUTTON_H,
                                TOUCH_BUTTON_HITBOX_PAD) ||
           touch_in_rect_padded(x,
                                y,
                                PLAY_BUTTON_X,
                                PLAY_BUTTON_Y,
                                PLAY_BUTTON_W,
                                BUTTON_H,
                                TOUCH_BUTTON_HITBOX_PAD) ||
           touch_in_rect_padded(x,
                                y,
                                NEXT_BUTTON_X,
                                PLAY_BUTTON_Y,
                                NEXT_BUTTON_W,
                                BUTTON_H,
                                TOUCH_BUTTON_HITBOX_PAD);
}

static void touch_handle_press(uint16_t x, uint16_t y)
{
    if (touch_in_rect(x, y, VOLUME_SLIDER_X, VOLUME_SLIDER_Y, VOLUME_SLIDER_W, VOLUME_SLIDER_H + 6)) {
        uint8_t volume = touch_volume_from_x(x);
        audio_set_volume(volume);
        return;
    }

    if (touch_in_rect_padded(x,
                             y,
                             PREV_BUTTON_X,
                             PLAY_BUTTON_Y,
                             PREV_BUTTON_W,
                             BUTTON_H,
                             TOUCH_BUTTON_HITBOX_PAD)) {
        send_avrc_command(ESP_AVRC_PT_CMD_BACKWARD);
        return;
    }

    if (touch_in_rect_padded(x,
                             y,
                             PLAY_BUTTON_X,
                             PLAY_BUTTON_Y,
                             PLAY_BUTTON_W,
                             BUTTON_H,
                             TOUCH_BUTTON_HITBOX_PAD)) {
        bool is_playing = false;
        if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            is_playing = g_player_state.metadata.is_playing;
            xSemaphoreGive(g_player_state_mutex);
        }
        send_avrc_command(is_playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
        return;
    }

    if (touch_in_rect_padded(x,
                             y,
                             NEXT_BUTTON_X,
                             PLAY_BUTTON_Y,
                             NEXT_BUTTON_W,
                             BUTTON_H,
                             TOUCH_BUTTON_HITBOX_PAD)) {
        send_avrc_command(ESP_AVRC_PT_CMD_FORWARD);
    }
}

/* ── Init & task ───────────────────────────────────────────────────── */

esp_err_t touch_init(void)
{
    if (s_touch_initialized) {
        return ESP_OK;
    }

    xpt2046_gpio_init();

    /* Probe: check if T_IRQ responds (briefly touch or just verify GPIO reads) */
    /* Do a dummy read to wake up the XPT2046 */
    (void)xpt2046_transfer(XPT2046_CMD_X);
    (void)xpt2046_transfer(XPT2046_CMD_Y);

    s_touch_initialized = true;
    ESP_LOGI(TAG, "XPT2046 touch initialized (CLK=%d CS=%d MOSI=%d MISO=%d IRQ=%d)",
             TOUCH_SPI_CLK, TOUCH_SPI_CS, TOUCH_SPI_MOSI, TOUCH_SPI_MISO, TOUCH_IRQ_IO);
    return ESP_OK;
}

void touch_task(void *params)
{
    (void)params;

    bool transport_press_handled = false;
    bool was_pressed = false;
    uint32_t last_move_log_tick = 0;

    while (1) {
        if (!s_touch_initialized) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }

        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = touch_read_point(&x, &y);

        if (!pressed) {
            if (was_pressed) {
                ESP_LOGI(TAG, "touch up");
            }
            was_pressed = false;
            transport_press_handled = false;
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }

        uint32_t now = xTaskGetTickCount();
        if (!was_pressed) {
            ESP_LOGI(TAG, "touch down x=%u y=%u", (unsigned)x, (unsigned)y);
            last_move_log_tick = now;
        } else if ((now - last_move_log_tick) >= pdMS_TO_TICKS(TOUCH_MOVE_LOG_INTERVAL_MS)) {
            ESP_LOGI(TAG, "touch move x=%u y=%u", (unsigned)x, (unsigned)y);
            last_move_log_tick = now;
        }

        if (touch_in_rect(x, y, VOLUME_SLIDER_X, VOLUME_SLIDER_Y, VOLUME_SLIDER_W, VOLUME_SLIDER_H + 6)) {
            touch_handle_press(x, y);
        } else if (!transport_press_handled && touch_in_transport_buttons(x, y)) {
            if ((now - s_last_touch_tick) > pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
                s_last_touch_tick = now;
                ESP_LOGI(TAG, "touch x=%u y=%u", (unsigned)x, (unsigned)y);
                touch_handle_press(x, y);
                transport_press_handled = true;
            }
        }

        was_pressed = true;

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}
