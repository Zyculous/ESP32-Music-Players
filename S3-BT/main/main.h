#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

// Wi-Fi + Music Assistant stream configuration (edit for your network/server)
#define MA_WIFI_SSID       "The Force"
#define MA_WIFI_PASSWORD   "jedimaster69"
#define MA_SLIMPROTO_HOST  "192.168.50.146"
#define MA_SLIMPROTO_PORT  3483
#define MA_STREAM_URL_MAX  640

// Display configuration for LCDWiki ES3C28P 2.8" (ILI9341V)
#define LCD_HOST           SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (8 * 1000 * 1000)  // Extra conservative for bring-up stability
#define LCD_PRIMARY_ST7789 0
#define LCD_DRIVER_AUTORETRY 1
#define LCD_SWAP_XY         1
#define LCD_MIRROR_X        0
#define LCD_MIRROR_Y        0
#define LCD_BK_LIGHT_ON_LEVEL  1
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL
#define PIN_NUM_MISO       GPIO_NUM_13
#define PIN_NUM_MOSI       GPIO_NUM_11
#define PIN_NUM_CLK        GPIO_NUM_12
#define PIN_NUM_CS         GPIO_NUM_10
#define PIN_NUM_DC         GPIO_NUM_46
#define PIN_NUM_RST        GPIO_NUM_NC
#define PIN_NUM_BK_LIGHT   GPIO_NUM_45

// Shared I2C bus (touch + ES8311 + expansion)
#define I2C_NUM_CH         I2C_NUM_0
#define I2C_SDA_IO         GPIO_NUM_16
#define I2C_SCL_IO         GPIO_NUM_15

// FT6336G capacitive touch
#define TOUCH_I2C_ADDR     0x38
#define TOUCH_INT_IO       GPIO_NUM_17
#define TOUCH_RST_IO       GPIO_NUM_18
#define TOUCH_MAP_SWAP_XY  1
#define TOUCH_INVERT_X     0
#define TOUCH_INVERT_Y     0

// ES8311 codec + I2S
#define I2S_NUM_CH         I2S_NUM_0
#define I2S_MCLK_IO        GPIO_NUM_4
#define I2S_BCK_IO         GPIO_NUM_5
#define I2S_DO_IO          GPIO_NUM_6
#define I2S_WS_IO          GPIO_NUM_7
#define I2S_DI_IO          GPIO_NUM_8
#define AUDIO_PA_EN_IO     GPIO_NUM_1
#define AUDIO_PA_EN_LEVEL  1
#define AUDIO_PA_AUTOPROBE 0

// LCD dimensions
#define LCD_H_RES          320
#define LCD_V_RES          240

#define WAIT_NAME_Y        116
#define TITLE_LINE1_Y      8
#define TITLE_LINE2_Y      20
#define CONNECTED_COVER_X  92
#define CONNECTED_COVER_Y  34
#define CONNECTED_COVER_SIZE 136
#define SEEK_TOUCH_WIDTH   64
#define SEEK_ICON_Y        112
#define TIMELINE_X         24
#define TIMELINE_Y         220
#define TIMELINE_W         272
#define TIMELINE_H         10

// UI Elements positions and sizes
#define PLAY_BUTTON_X      140
#define PLAY_BUTTON_Y      180  
#define BUTTON_SIZE        50
#define PREV_BUTTON_X      70
#define NEXT_BUTTON_X      220

#define COVER_ART_X        10
#define COVER_ART_Y        35
#define COVER_ART_SIZE     120

#define SONG_TITLE_X       140
#define SONG_TITLE_Y       40
#define ARTIST_NAME_X      140
#define ARTIST_NAME_Y       60
#define ALBUM_NAME_X       140
#define ALBUM_NAME_Y       80

#define VOLUME_SLIDER_X    12
#define VOLUME_SLIDER_Y    8
#define VOLUME_SLIDER_W    296
#define VOLUME_SLIDER_H    14

// Bluetooth device name
#define BT_DEVICE_NAME     "S3 Music Player"

// Track metadata structure
typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    uint32_t duration;
    uint32_t position;
    bool is_playing;
} track_metadata_t;

// Cover art structure  
typedef struct {
    uint8_t *data;
    size_t size;
    uint16_t width;
    uint16_t height;
    bool valid;
    uint32_t version;
} cover_art_t;

// Player state structure
typedef struct {
    track_metadata_t metadata;
    cover_art_t cover_art;
    bool connected;
    uint8_t volume;
    int connection_state;
    int audio_state;
} player_state_t;

typedef enum {
    MEDIA_CMD_PLAY = 1,
    MEDIA_CMD_PAUSE = 2,
    MEDIA_CMD_NEXT = 3,
    MEDIA_CMD_PREV = 4,
} media_cmd_t;

typedef enum {
    UI_DIRTY_NONE = 0,
    UI_DIRTY_LAYOUT = 1 << 0,
    UI_DIRTY_METADATA = 1 << 1,
    UI_DIRTY_PLAY_STATE = 1 << 2,
    UI_DIRTY_TIMELINE = 1 << 3,
    UI_DIRTY_COVER_ART = 1 << 4,
    UI_DIRTY_ALL = 0xFFFFFFFFu,
} ui_dirty_flag_t;

// Function prototypes
extern player_state_t g_player_state;
extern SemaphoreHandle_t g_player_state_mutex;
extern esp_lcd_panel_handle_t g_panel_handle;

// Bluetooth functions
esp_err_t bluetooth_init(void);
void bluetooth_send_passthrough_cmd(media_cmd_t cmd);
esp_err_t bluetooth_set_ble_audio_stream(uint32_t sample_rate, uint8_t channels);
esp_err_t bluetooth_receive_ble_audio_packet(const uint8_t *packet, uint32_t len);

// Wi-Fi + stream functions
esp_err_t wifi_init_and_connect(const char *ssid, const char *password, TickType_t timeout_ticks);
esp_err_t music_assistant_stream_start(const char *stream_url, uint32_t sample_rate, uint8_t channels, uint8_t format);
void music_assistant_stream_stop(void);
void music_assistant_stream_pause(bool pause);
esp_err_t slimproto_player_start(const char *host, uint16_t port);

// Display functions
esp_err_t display_init(void);
void display_update_track_info(const track_metadata_t *metadata);
void display_update_play_button(bool is_playing);
void display_update_cover_art(const cover_art_t *cover_art);
void display_update_timeline(const track_metadata_t *metadata);
void display_reset_cover_art_queue(void);
void display_clear_screen(void);
void display_draw_ui(void);
void display_draw_char(int x, int y, char c, uint16_t color);
bool display_acquire_bus(void);
void display_release_bus(void);

// Audio functions
esp_err_t audio_init(void);
esp_err_t audio_reconfigure(uint32_t sample_rate, uint8_t channels);
void audio_data_callback(const uint8_t *data, uint32_t len);
void audio_reset_buffers(void);
void audio_set_volume(uint8_t volume);
i2c_master_bus_handle_t audio_get_shared_i2c_bus(void);

// UI functions  
void ui_draw_button(int x, int y, int size, const char *text, bool pressed);
void ui_update_display(void);
void ui_request_refresh(uint32_t flags);
void ui_tick_250ms(void);

// Touch functions
esp_err_t touch_init(void);
void touch_task(void *params);

// Helper display functions
void display_draw_rectangle(int x, int y, int width, int height, uint16_t color);
void display_draw_string(int x, int y, const char *str, uint16_t color);
void display_draw_string_padded(int x, int y, const char *str, uint16_t color, int max_chars);

// Utility functions
void send_avrc_command(media_cmd_t cmd);
void bluetooth_send_seek_cmd(bool forward, uint32_t hold_ms);
void request_metadata(void);
void request_cover_art(void);

#endif // MAIN_H