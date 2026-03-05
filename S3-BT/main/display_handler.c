#include "main.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include <stdio.h>

static const char *TAG = "DISPLAY";
static SemaphoreHandle_t s_lcd_spi_mutex = NULL;
static uint16_t *s_draw_line_buffer = NULL;
static uint16_t *s_cover_decode_buffer = NULL;
static uint16_t s_cover_scale_line_buffer[COVER_ART_SIZE];
static int s_panel_width = LCD_H_RES;
static int s_panel_height = LCD_V_RES;

// Colors (RGB565 format)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_GRAY    0x8410
#define COLOR_LIGHT_GRAY 0xC618
#define COLOR_DARK_GRAY  0x4208
#define COLOR_ORANGE  0xFD20
#define COLOR_CYAN    0x07FF
#define COLOR_DEEP_BLUE 0x10A2

#define COVER_DECODE_TARGET 160

static esp_err_t create_panel_with_driver(esp_lcd_panel_io_handle_t io_handle,
                                          const esp_lcd_panel_dev_config_t *panel_config,
                                          bool use_st7789)
{
    if (use_st7789) {
        ESP_LOGI(TAG, "Trying ST7789 panel driver");
        return esp_lcd_new_panel_st7789(io_handle, panel_config, &g_panel_handle);
    }

    ESP_LOGI(TAG, "Trying ILI9341 panel driver");
    return esp_lcd_new_panel_ili9341(io_handle, panel_config, &g_panel_handle);
}

static esp_err_t apply_panel_orientation(bool swap_xy, bool mirror_x, bool mirror_y, int width, int height)
{
    esp_err_t ret = esp_lcd_panel_swap_xy(g_panel_handle, swap_xy);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "swap_xy failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_lcd_panel_mirror(g_panel_handle, mirror_x, mirror_y);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mirror failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_panel_width = width;
    s_panel_height = height;
    return ESP_OK;
}

static void *alloc_prefer_spiram(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) {
        return ptr;
    }
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void draw_cover_placeholder(const char *line1, const char *line2)
{
    display_draw_rectangle(COVER_ART_X, COVER_ART_Y, COVER_ART_SIZE, COVER_ART_SIZE, COLOR_GRAY);
    if (line1) {
        display_draw_string(COVER_ART_X + 18, COVER_ART_Y + 48, line1, COLOR_BLACK);
    }
    if (line2) {
        display_draw_string(COVER_ART_X + 12, COVER_ART_Y + 64, line2, COLOR_BLACK);
    }
}

static void draw_scaled_rgb565(const uint16_t *src, uint16_t src_w, uint16_t src_h)
{
    if (!src || src_w == 0 || src_h == 0) {
        draw_cover_placeholder("No Cover", NULL);
        return;
    }

    uint16_t dst_w = COVER_ART_SIZE;
    uint16_t dst_h = COVER_ART_SIZE;
    if ((uint32_t)src_w * COVER_ART_SIZE > (uint32_t)src_h * COVER_ART_SIZE) {
        dst_h = (uint16_t)(((uint32_t)COVER_ART_SIZE * src_h) / src_w);
    } else {
        dst_w = (uint16_t)(((uint32_t)COVER_ART_SIZE * src_w) / src_h);
    }
    if (dst_w == 0) dst_w = 1;
    if (dst_h == 0) dst_h = 1;

    uint16_t x_off = COVER_ART_X + (COVER_ART_SIZE - dst_w) / 2;
    uint16_t y_off = COVER_ART_Y + (COVER_ART_SIZE - dst_h) / 2;
    display_draw_rectangle(COVER_ART_X, COVER_ART_Y, COVER_ART_SIZE, COVER_ART_SIZE, COLOR_BLACK);

    for (uint16_t y = 0; y < dst_h; y++) {
        uint16_t src_y = (uint16_t)(((uint32_t)y * src_h) / dst_h);
        const uint16_t *src_row = src + (src_y * src_w);
        for (uint16_t x = 0; x < dst_w; x++) {
            uint16_t src_x = (uint16_t)(((uint32_t)x * src_w) / dst_w);
            s_cover_scale_line_buffer[x] = src_row[src_x];
        }

        if (!display_acquire_bus()) {
            continue;
        }
        esp_lcd_panel_draw_bitmap(g_panel_handle,
                                  x_off,
                                  y_off + y,
                                  x_off + dst_w,
                                  y_off + y + 1,
                                  s_cover_scale_line_buffer);
        display_release_bus();
    }
}

static void render_cover_art_now(const cover_art_t *cover_art)
{
    if (!cover_art || !cover_art->valid || !cover_art->data || cover_art->size < 8) {
        draw_cover_placeholder("No Cover", NULL);
        return;
    }

    if (!s_cover_decode_buffer) {
        draw_cover_placeholder("No Memory", NULL);
        return;
    }

    const uint16_t decode_target = COVER_DECODE_TARGET;
    const size_t decoded_buffer_size = (size_t)decode_target * (size_t)decode_target * sizeof(uint16_t);
    esp_jpeg_image_output_t outimg = {0};
    esp_err_t ret = ESP_FAIL;

    const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0,
        JPEG_IMAGE_SCALE_1_2,
        JPEG_IMAGE_SCALE_1_4,
    };

    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        esp_jpeg_image_cfg_t jpeg_cfg = {
            .indata = (uint8_t *)cover_art->data,
            .indata_size = cover_art->size,
            .outbuf = (uint8_t *)s_cover_decode_buffer,
            .outbuf_size = decoded_buffer_size,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .out_scale = scales[i],
            .flags = {
                .swap_color_bytes = 1,
            }
        };

        ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
        if (ret == ESP_OK && outimg.width > 0 && outimg.height > 0 &&
            outimg.width <= decode_target && outimg.height <= decode_target) {
            draw_scaled_rgb565(s_cover_decode_buffer, outimg.width, outimg.height);
            return;
        }
    }

    draw_cover_placeholder("Decode Fail", NULL);
}

bool display_acquire_bus(void)
{
    if (!s_lcd_spi_mutex) {
        return true;
    }

    if (xSemaphoreTakeRecursive(s_lcd_spi_mutex, pdMS_TO_TICKS(120)) == pdTRUE) {
        return true;
    } else {
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        const char *self_name = pcTaskGetName(self);
        ESP_LOGW(TAG, "Failed to acquire SPI mutex (self=%s)", self_name ? self_name : "unknown");
        return false;
    }
}

void display_release_bus(void)
{
    if (!s_lcd_spi_mutex) {
        return;
    }

    if (xSemaphoreGiveRecursive(s_lcd_spi_mutex) != pdTRUE) {
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        const char *self_name = pcTaskGetName(self);
        ESP_LOGW(TAG, "SPI mutex release failed (self=%s)", self_name ? self_name : "unknown");
    }
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing LCD display");

    if (!s_lcd_spi_mutex) {
        s_lcd_spi_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_lcd_spi_mutex) {
            ESP_LOGE(TAG, "Failed to create LCD SPI mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Initialize SPI bus
    spi_bus_config_t spi_bus_config = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    esp_err_t spi_ret = spi_bus_initialize(LCD_HOST, &spi_bus_config, SPI_DMA_CH_AUTO);
    if (spi_ret != ESP_OK && spi_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(spi_ret));
        return spi_ret;
    }

    // Initialize LCD panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    // Initialize LCD panel controller
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_err_t panel_ret;
#if LCD_DRIVER_AUTORETRY
#if LCD_PRIMARY_ST7789
    panel_ret = create_panel_with_driver(io_handle, &panel_config, true);
    if (panel_ret != ESP_OK) {
        ESP_LOGW(TAG, "ST7789 create failed (%s), retrying with ILI9341", esp_err_to_name(panel_ret));
        panel_ret = create_panel_with_driver(io_handle, &panel_config, false);
    }
#else
    panel_ret = create_panel_with_driver(io_handle, &panel_config, false);
    if (panel_ret != ESP_OK) {
        ESP_LOGW(TAG, "ILI9341 create failed (%s), retrying with ST7789", esp_err_to_name(panel_ret));
        panel_ret = create_panel_with_driver(io_handle, &panel_config, true);
    }
#endif
#else
#if LCD_PRIMARY_ST7789
    panel_ret = create_panel_with_driver(io_handle, &panel_config, true);
#else
    panel_ret = create_panel_with_driver(io_handle, &panel_config, false);
#endif
#endif
    ESP_ERROR_CHECK(panel_ret);

    // Reset and initialize the panel
    gpio_num_t rst_pin = PIN_NUM_RST;
    if (rst_pin != GPIO_NUM_NC) {
        uint64_t rst_mask = 1ULL << (uint32_t)rst_pin;
        gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = rst_mask,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_gpio_config));
        gpio_set_level(rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level(rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel_handle));
    vTaskDelay(pdMS_TO_TICKS(40));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel_handle));
    vTaskDelay(pdMS_TO_TICKS(60));

    esp_err_t sleep_ret = esp_lcd_panel_disp_sleep(g_panel_handle, false);
    if (sleep_ret != ESP_OK) {
        ESP_LOGW(TAG, "disp_sleep(false) returned %s", esp_err_to_name(sleep_ret));
    } else {
        ESP_LOGI(TAG, "Panel sleep exit command sent");
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    
    // Preferred orientation from board config, fallback to portrait limits if panel rejects writes
    ESP_ERROR_CHECK(apply_panel_orientation(LCD_SWAP_XY, LCD_MIRROR_X, LCD_MIRROR_Y, 320, 240));
    ESP_LOGI(TAG,
             "Panel orientation swap_xy=%d mirror_x=%d mirror_y=%d",
             LCD_SWAP_XY,
             LCD_MIRROR_X,
             LCD_MIRROR_Y);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel_handle, false));

    // Initialize backlight
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    // Probe both polarities briefly for board variants with inverted BL control
    gpio_set_level(PIN_NUM_BK_LIGHT, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(PIN_NUM_BK_LIGHT, 0);
    vTaskDelay(pdMS_TO_TICKS(80));

    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Turn on the screen
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel_handle, true));
    ESP_LOGI(TAG, "Panel display ON command sent");
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL);
    ESP_LOGI(TAG, "Backlight configured on GPIO %d (active level=%d)", PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL);

    if (!s_draw_line_buffer) {
        s_draw_line_buffer = malloc(320 * sizeof(uint16_t));
        if (!s_draw_line_buffer) {
            ESP_LOGE(TAG, "Failed to allocate shared draw line buffer");
            goto init_nomem;
        }
    }

    if (!s_cover_decode_buffer) {
        s_cover_decode_buffer = alloc_prefer_spiram(COVER_DECODE_TARGET * COVER_DECODE_TARGET * sizeof(uint16_t));
        if (!s_cover_decode_buffer) {
            ESP_LOGE(TAG, "Failed to allocate cover decode buffer");
            goto init_nomem;
        }
    }

    for (int i = 0; i < 320; i++) {
        s_draw_line_buffer[i] = COLOR_WHITE;
    }
    esp_err_t probe_ret = esp_lcd_panel_draw_bitmap(g_panel_handle, 0, 0, s_panel_width, 1, s_draw_line_buffer);
    if (probe_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Landscape probe failed (%s), falling back to portrait bounds",
                 esp_err_to_name(probe_ret));
        ESP_ERROR_CHECK(apply_panel_orientation(false, false, false, 240, 320));
    }


    // Visible bring-up check colors (high contrast)
    display_draw_rectangle(0, 0, s_panel_width, s_panel_height, COLOR_WHITE);
    vTaskDelay(pdMS_TO_TICKS(40));
    display_draw_rectangle(0, 0, s_panel_width, s_panel_height, COLOR_DEEP_BLUE);

    ESP_LOGI(TAG, "LCD display initialized");
    return ESP_OK;

init_nomem:
    if (s_cover_decode_buffer) {
        free(s_cover_decode_buffer);
        s_cover_decode_buffer = NULL;
    }
    if (s_draw_line_buffer) {
        free(s_draw_line_buffer);
        s_draw_line_buffer = NULL;
    }
    return ESP_ERR_NO_MEM;
}

void display_clear_screen(void)
{
    if (!display_acquire_bus()) {
        return;
    }

    if (s_draw_line_buffer) {
        for (int i = 0; i < s_panel_width; i++) {
            s_draw_line_buffer[i] = COLOR_BLACK;
        }

        for (int y = 0; y < s_panel_height; y++) {
            esp_lcd_panel_draw_bitmap(g_panel_handle, 0, y, s_panel_width, y + 1, s_draw_line_buffer);
        }
    }

    display_release_bus();
}

void display_draw_rectangle(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0 || !s_draw_line_buffer) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + width;
    int y1 = y + height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_panel_width) x1 = s_panel_width;
    if (y1 > s_panel_height) y1 = s_panel_height;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    int draw_w = x1 - x0;
    int draw_h = y1 - y0;

    if (!display_acquire_bus()) {
        return;
    }

    for (int i = 0; i < draw_w; i++) {
        s_draw_line_buffer[i] = color;
    }

    for (int row = 0; row < draw_h; row++) {
        esp_lcd_panel_draw_bitmap(g_panel_handle, x0, y0 + row, x1, y0 + row + 1, s_draw_line_buffer);
    }

    display_release_bus();
}

void display_draw_circle(int cx, int cy, int radius, uint16_t color, bool filled)
{
    if (!display_acquire_bus()) {
        return;
    }

    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        if (filled) {
            // Draw horizontal lines for filled circle
            for (int i = cx - x; i <= cx + x; i++) {
                esp_lcd_panel_draw_bitmap(g_panel_handle, i, cy + y, i + 1, cy + y + 1, &color);
                esp_lcd_panel_draw_bitmap(g_panel_handle, i, cy - y, i + 1, cy - y + 1, &color);
            }
            for (int i = cx - y; i <= cx + y; i++) {
                esp_lcd_panel_draw_bitmap(g_panel_handle, i, cy + x, i + 1, cy + x + 1, &color);
                esp_lcd_panel_draw_bitmap(g_panel_handle, i, cy - x, i + 1, cy - x + 1, &color);
            }
        } else {
            // Draw circle outline
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx + x, cy + y, cx + x + 1, cy + y + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx + y, cy + x, cx + y + 1, cy + x + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx - y, cy + x, cx - y + 1, cy + x + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx - x, cy + y, cx - x + 1, cy + y + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx - x, cy - y, cx - x + 1, cy - y + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx - y, cy - x, cx - y + 1, cy - x + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx + y, cy - x, cx + y + 1, cy - x + 1, &color);
            esp_lcd_panel_draw_bitmap(g_panel_handle, cx + x, cy - y, cx + x + 1, cy - y + 1, &color);
        }

        if (err <= 0) {
            y += 1;
            err += 2*y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2*x + 1;
        }
    }

    display_release_bus();
}

void display_draw_char(int x, int y, char c, uint16_t color)
{
    if (!display_acquire_bus()) {
        return;
    }

    // 8x8 font patterns for ASCII characters
    uint8_t char_pattern[8] = {0};
    
    if (c >= 'A' && c <= 'Z') {
        // Uppercase letters A-Z
        switch(c) {
            case 'A': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x7E; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'B': char_pattern[0] = 0x7C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x7C; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x7C; char_pattern[7] = 0x00; break;
            case 'C': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x60; char_pattern[4] = 0x60; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'D': char_pattern[0] = 0x78; char_pattern[1] = 0x6C; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x6C;
                      char_pattern[6] = 0x78; char_pattern[7] = 0x00; break;
            case 'E': char_pattern[0] = 0x7E; char_pattern[1] = 0x60; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x78; char_pattern[4] = 0x60; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
            case 'F': char_pattern[0] = 0x7E; char_pattern[1] = 0x60; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x78; char_pattern[4] = 0x60; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x60; char_pattern[7] = 0x00; break;
            case 'G': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x6E; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'H': char_pattern[0] = 0x66; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x7E; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'I': char_pattern[0] = 0x3C; char_pattern[1] = 0x18; char_pattern[2] = 0x18;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'J': char_pattern[0] = 0x0F; char_pattern[1] = 0x06; char_pattern[2] = 0x06;
                      char_pattern[3] = 0x06; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'K': char_pattern[0] = 0x66; char_pattern[1] = 0x6C; char_pattern[2] = 0x78;
                      char_pattern[3] = 0x70; char_pattern[4] = 0x78; char_pattern[5] = 0x6C;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'L': char_pattern[0] = 0x60; char_pattern[1] = 0x60; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x60; char_pattern[4] = 0x60; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
            case 'M': char_pattern[0] = 0x63; char_pattern[1] = 0x77; char_pattern[2] = 0x7F;
                      char_pattern[3] = 0x6B; char_pattern[4] = 0x63; char_pattern[5] = 0x63;
                      char_pattern[6] = 0x63; char_pattern[7] = 0x00; break;
            case 'N': char_pattern[0] = 0x66; char_pattern[1] = 0x76; char_pattern[2] = 0x7E;
                      char_pattern[3] = 0x7E; char_pattern[4] = 0x6E; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'O': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'P': char_pattern[0] = 0x7C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x7C; char_pattern[4] = 0x60; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x60; char_pattern[7] = 0x00; break;
            case 'Q': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x6A; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3E; char_pattern[7] = 0x00; break;
            case 'R': char_pattern[0] = 0x7C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x7C; char_pattern[4] = 0x78; char_pattern[5] = 0x6C;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'S': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x3C; char_pattern[4] = 0x06; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'T': char_pattern[0] = 0x7E; char_pattern[1] = 0x18; char_pattern[2] = 0x18;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case 'U': char_pattern[0] = 0x66; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'V': char_pattern[0] = 0x66; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x3C;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case 'W': char_pattern[0] = 0x63; char_pattern[1] = 0x63; char_pattern[2] = 0x63;
                      char_pattern[3] = 0x6B; char_pattern[4] = 0x7F; char_pattern[5] = 0x77;
                      char_pattern[6] = 0x63; char_pattern[7] = 0x00; break;
            case 'X': char_pattern[0] = 0x66; char_pattern[1] = 0x66; char_pattern[2] = 0x3C;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x3C; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'Y': char_pattern[0] = 0x66; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x3C; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case 'Z': char_pattern[0] = 0x7E; char_pattern[1] = 0x06; char_pattern[2] = 0x0C;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x30; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
            default: // Default for other uppercase
                      char_pattern[0] = 0x3E; char_pattern[1] = 0x63; char_pattern[2] = 0x63;
                      char_pattern[3] = 0x63; char_pattern[4] = 0x63; char_pattern[5] = 0x63;
                      char_pattern[6] = 0x3E; char_pattern[7] = 0x00; break;
        }
    } else if (c >= 'a' && c <= 'z') {
        // Lowercase letters a-z
        switch(c) {
            case 'a': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3C;
                      char_pattern[3] = 0x06; char_pattern[4] = 0x3E; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3E; char_pattern[7] = 0x00; break;
            case 'b': char_pattern[0] = 0x60; char_pattern[1] = 0x60; char_pattern[2] = 0x7C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x7C; char_pattern[7] = 0x00; break;
            case 'c': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3C;
                      char_pattern[3] = 0x60; char_pattern[4] = 0x60; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'd': char_pattern[0] = 0x06; char_pattern[1] = 0x06; char_pattern[2] = 0x3E;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3E; char_pattern[7] = 0x00; break;
            case 'e': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x7E; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'f': char_pattern[0] = 0x0E; char_pattern[1] = 0x18; char_pattern[2] = 0x7E;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case 'g': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3E;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x3E;
                      char_pattern[6] = 0x06; char_pattern[7] = 0x3C; break;
            case 'h': char_pattern[0] = 0x60; char_pattern[1] = 0x60; char_pattern[2] = 0x7C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'i': char_pattern[0] = 0x18; char_pattern[1] = 0x00; char_pattern[2] = 0x38;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'j': char_pattern[0] = 0x06; char_pattern[1] = 0x00; char_pattern[2] = 0x0E;
                      char_pattern[3] = 0x06; char_pattern[4] = 0x06; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'k': char_pattern[0] = 0x60; char_pattern[1] = 0x60; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x6C; char_pattern[4] = 0x78; char_pattern[5] = 0x6C;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'l': char_pattern[0] = 0x38; char_pattern[1] = 0x18; char_pattern[2] = 0x18;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'm': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x77;
                      char_pattern[3] = 0x7F; char_pattern[4] = 0x6B; char_pattern[5] = 0x63;
                      char_pattern[6] = 0x63; char_pattern[7] = 0x00; break;
            case 'n': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x7C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'o': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case 'p': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x7C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x7C;
                      char_pattern[6] = 0x60; char_pattern[7] = 0x60; break;
            case 'q': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3E;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x3E;
                      char_pattern[6] = 0x06; char_pattern[7] = 0x06; break;
            case 'r': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x7C;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x60; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x60; char_pattern[7] = 0x00; break;
            case 's': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3E;
                      char_pattern[3] = 0x60; char_pattern[4] = 0x3C; char_pattern[5] = 0x06;
                      char_pattern[6] = 0x7C; char_pattern[7] = 0x00; break;
            case 't': char_pattern[0] = 0x18; char_pattern[1] = 0x18; char_pattern[2] = 0x7E;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x0E; char_pattern[7] = 0x00; break;
            case 'u': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3E; char_pattern[7] = 0x00; break;
            case 'v': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x3C;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case 'w': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x63;
                      char_pattern[3] = 0x63; char_pattern[4] = 0x6B; char_pattern[5] = 0x7F;
                      char_pattern[6] = 0x36; char_pattern[7] = 0x00; break;
            case 'x': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x3C; char_pattern[4] = 0x18; char_pattern[5] = 0x3C;
                      char_pattern[6] = 0x66; char_pattern[7] = 0x00; break;
            case 'y': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x66; char_pattern[5] = 0x3E;
                      char_pattern[6] = 0x06; char_pattern[7] = 0x7C; break;
            case 'z': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x7E;
                      char_pattern[3] = 0x0C; char_pattern[4] = 0x18; char_pattern[5] = 0x30;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
            default: // Default for other lowercase
                      char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x3E;
                      char_pattern[3] = 0x63; char_pattern[4] = 0x63; char_pattern[5] = 0x63;
                      char_pattern[6] = 0x3E; char_pattern[7] = 0x00; break;
        }
    } else if (c >= '0' && c <= '9') {
        // Numbers 0-9
        switch(c) {
            case '0': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x6E;
                      char_pattern[3] = 0x76; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case '1': char_pattern[0] = 0x18; char_pattern[1] = 0x38; char_pattern[2] = 0x18;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
            case '2': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x06;
                      char_pattern[3] = 0x0C; char_pattern[4] = 0x30; char_pattern[5] = 0x60;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
            case '3': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x06;
                      char_pattern[3] = 0x1C; char_pattern[4] = 0x06; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case '4': char_pattern[0] = 0x06; char_pattern[1] = 0x0E; char_pattern[2] = 0x1E;
                      char_pattern[3] = 0x66; char_pattern[4] = 0x7F; char_pattern[5] = 0x06;
                      char_pattern[6] = 0x06; char_pattern[7] = 0x00; break;
            case '5': char_pattern[0] = 0x7E; char_pattern[1] = 0x60; char_pattern[2] = 0x7C;
                      char_pattern[3] = 0x06; char_pattern[4] = 0x06; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case '6': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x60;
                      char_pattern[3] = 0x7C; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case '7': char_pattern[0] = 0x7E; char_pattern[1] = 0x66; char_pattern[2] = 0x0C;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case '8': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x3C; char_pattern[4] = 0x66; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
            case '9': char_pattern[0] = 0x3C; char_pattern[1] = 0x66; char_pattern[2] = 0x66;
                      char_pattern[3] = 0x3E; char_pattern[4] = 0x06; char_pattern[5] = 0x66;
                      char_pattern[6] = 0x3C; char_pattern[7] = 0x00; break;
        }
    } else {
        // Special characters and punctuation
        switch(c) {
            case ' ': memset(char_pattern, 0, 8); break; // Space - blank
            case '.': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x00;
                      char_pattern[3] = 0x00; char_pattern[4] = 0x00; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case ',': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x00;
                      char_pattern[3] = 0x00; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x30; char_pattern[7] = 0x00; break;
            case ':': char_pattern[0] = 0x00; char_pattern[1] = 0x18; char_pattern[2] = 0x18;
                      char_pattern[3] = 0x00; char_pattern[4] = 0x00; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case '-': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x00;
                      char_pattern[3] = 0x7E; char_pattern[4] = 0x00; char_pattern[5] = 0x00;
                      char_pattern[6] = 0x00; char_pattern[7] = 0x00; break;
            case '(': char_pattern[0] = 0x0C; char_pattern[1] = 0x18; char_pattern[2] = 0x30;
                      char_pattern[3] = 0x30; char_pattern[4] = 0x30; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x0C; char_pattern[7] = 0x00; break;
            case ')': char_pattern[0] = 0x30; char_pattern[1] = 0x18; char_pattern[2] = 0x0C;
                      char_pattern[3] = 0x0C; char_pattern[4] = 0x0C; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x30; char_pattern[7] = 0x00; break;
            case '>': char_pattern[0] = 0x10; char_pattern[1] = 0x18; char_pattern[2] = 0x1C;
                      char_pattern[3] = 0x1E; char_pattern[4] = 0x1C; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x10; char_pattern[7] = 0x00; break;
            case '|': char_pattern[0] = 0x18; char_pattern[1] = 0x18; char_pattern[2] = 0x18;
                      char_pattern[3] = 0x18; char_pattern[4] = 0x18; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x18; char_pattern[7] = 0x00; break;
            case '<': char_pattern[0] = 0x08; char_pattern[1] = 0x18; char_pattern[2] = 0x38;
                      char_pattern[3] = 0x78; char_pattern[4] = 0x38; char_pattern[5] = 0x18;
                      char_pattern[6] = 0x08; char_pattern[7] = 0x00; break;
            case '_': char_pattern[0] = 0x00; char_pattern[1] = 0x00; char_pattern[2] = 0x00;
                      char_pattern[3] = 0x00; char_pattern[4] = 0x00; char_pattern[5] = 0x00;
                      char_pattern[6] = 0x00; char_pattern[7] = 0xFF; break;
            case '&': char_pattern[0] = 0x38; char_pattern[1] = 0x6C; char_pattern[2] = 0x38;
                      char_pattern[3] = 0x76; char_pattern[4] = 0xDC; char_pattern[5] = 0xCC;
                      char_pattern[6] = 0x76; char_pattern[7] = 0x00; break;
            case '\'': char_pattern[0] = 0x18; char_pattern[1] = 0x18; char_pattern[2] = 0x30;
                      char_pattern[3] = 0x00; char_pattern[4] = 0x00; char_pattern[5] = 0x00;
                      char_pattern[6] = 0x00; char_pattern[7] = 0x00; break;
            default: // Solid block for unknown characters
                      char_pattern[0] = 0x7E; char_pattern[1] = 0x42; char_pattern[2] = 0x42;
                      char_pattern[3] = 0x42; char_pattern[4] = 0x42; char_pattern[5] = 0x42;
                      char_pattern[6] = 0x7E; char_pattern[7] = 0x00; break;
        }
    }
    
    // Draw the character pixel by pixel
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (char_pattern[row] & (0x80 >> col)) {
                esp_lcd_panel_draw_bitmap(g_panel_handle, x + col, y + row, 
                                        x + col + 1, y + row + 1, &color);
            }
        }
    }

    display_release_bus();
}

void display_draw_string(int x, int y, const char *str, uint16_t color)
{
    int char_x = x;
    while (*str) {
        display_draw_char(char_x, y, *str, color);
        char_x += 8; // Character width
        str++;
    }
}

void display_draw_string_padded(int x, int y, const char *str, uint16_t color, int max_chars)
{
    if (max_chars <= 0) {
        return;
    }

    display_draw_rectangle(x, y, max_chars * 8, 8, COLOR_BLACK);

    int char_x = x;
    int i = 0;
    while (str && str[i] && i < max_chars) {
        display_draw_char(char_x, y, str[i], color);
        char_x += 8;
        i++;
    }
}

void display_draw_ui(void)
{
    ESP_LOGI(TAG, "Drawing initial UI");

    // Main background
    display_draw_rectangle(0, 0, LCD_H_RES, LCD_V_RES, COLOR_BLACK);

    // Cover art frame
    display_draw_rectangle(COVER_ART_X - 3, COVER_ART_Y - 3, COVER_ART_SIZE + 6, COVER_ART_SIZE + 6, COLOR_DARK_GRAY);
    display_draw_rectangle(COVER_ART_X - 2, COVER_ART_Y - 2, COVER_ART_SIZE + 4, COVER_ART_SIZE + 4, COLOR_CYAN);
    display_draw_rectangle(COVER_ART_X, COVER_ART_Y, COVER_ART_SIZE, COVER_ART_SIZE, COLOR_BLACK);

    // Draw metadata panel backdrop
    display_draw_rectangle(136, 34, 174, 64, COLOR_DARK_GRAY);
    display_draw_rectangle(136, 34, 174, 1, COLOR_CYAN);

    // Draw "No Cover" placeholder
    display_draw_string(COVER_ART_X + 21, COVER_ART_Y + 55, "NO COVER", COLOR_GRAY);

    // Draw control buttons
    ui_draw_button(PREV_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, "<<", false);
    ui_draw_button(PLAY_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, 
                   g_player_state.metadata.is_playing ? "||" : ">", false);
    ui_draw_button(NEXT_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, ">>", false);

    // Draw track info
    display_update_track_info(&g_player_state.metadata);
}

void display_update_track_info(const track_metadata_t *metadata)
{
    const int max_chars = 21; // 170 px / 8 px per char
    
    if (strlen(metadata->title) > 0) {
        display_draw_string_padded(SONG_TITLE_X, SONG_TITLE_Y, metadata->title, COLOR_WHITE, max_chars);
    } else {
        display_draw_string_padded(SONG_TITLE_X, SONG_TITLE_Y, "No title", COLOR_GRAY, max_chars);
    }
    
    if (strlen(metadata->artist) > 0) {
        display_draw_string_padded(ARTIST_NAME_X, ARTIST_NAME_Y, metadata->artist, COLOR_CYAN, max_chars);
    } else {
        display_draw_string_padded(ARTIST_NAME_X, ARTIST_NAME_Y, "Unknown artist", COLOR_GRAY, max_chars);
    }
    
    if (strlen(metadata->album) > 0) {
        display_draw_string_padded(ALBUM_NAME_X, ALBUM_NAME_Y, metadata->album, COLOR_ORANGE, max_chars);
    } else {
        display_draw_string_padded(ALBUM_NAME_X, ALBUM_NAME_Y, "Unknown album", COLOR_GRAY, max_chars);
    }
}

void display_update_timeline(const track_metadata_t *metadata)
{
    (void)metadata;
}

void display_update_cover_art(const cover_art_t *cover_art)
{
    render_cover_art_now(cover_art);
}

void display_reset_cover_art_queue(void)
{
    return;
}

void display_update_play_button(bool is_playing)
{
    ui_draw_button(PLAY_BUTTON_X, PLAY_BUTTON_Y, BUTTON_SIZE, 
                   is_playing ? "||" : ">", false);
}