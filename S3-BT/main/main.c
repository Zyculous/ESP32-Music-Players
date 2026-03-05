#include "main.h"
#include <stdlib.h>
#include "esp_heap_caps.h"

static const char *TAG = "MAIN";

// Global state
player_state_t g_player_state = {0};
SemaphoreHandle_t g_player_state_mutex = NULL;
esp_lcd_panel_handle_t g_panel_handle = NULL;

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Bluetooth Music Player for ES3C28P (ESP32-S3)");

    g_player_state.volume = 0x60;
    g_player_state_mutex = xSemaphoreCreateMutex();
    if (!g_player_state_mutex) {
        ESP_LOGE(TAG, "Failed to create player state mutex");
        abort();
    }
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize display
    ESP_LOGI(TAG, "Initializing display...");
    ESP_ERROR_CHECK(display_init());
    
    // Draw initial UI as early as possible so display status is always visible
    display_clear_screen();
    display_draw_ui();
    display_draw_string(12, 12, "BOOTING...", 0xFFFF);

    // Initialize audio
    ESP_LOGI(TAG, "Initializing audio...");
    ret = audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed: %s", esp_err_to_name(ret));
        display_draw_string(12, 24, "AUDIO INIT FAIL", 0xF800);
    }

    // Initialize touch
    ESP_LOGI(TAG, "Initializing touch...");
    ret = touch_init();
    if (ret == ESP_OK) {
        if (xTaskCreatePinnedToCore(touch_task,
                                    "touch_task",
                                    4096,
                                    NULL,
                                    4,
                                    NULL,
                                    1) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create touch task");
            display_draw_string(12, 36, "TOUCH TASK FAIL", 0xF800);
        }
    } else {
        ESP_LOGE(TAG, "Touch init failed: %s", esp_err_to_name(ret));
        display_draw_string(12, 36, "TOUCH INIT FAIL", 0xF800);
    }
    
    // Initialize Wi-Fi and start SlimProto player session
    ESP_LOGI(TAG, "Connecting Wi-Fi...");
    ret = wifi_init_and_connect(MA_WIFI_SSID, MA_WIFI_PASSWORD, pdMS_TO_TICKS(20000));
    if (ret == ESP_OK) {
        g_player_state.connected = true;
        display_draw_string(12, 48, "WIFI CONNECTED", 0x07E0);

        ESP_LOGI(TAG, "Starting SlimProto player...");
        ret = slimproto_player_start(MA_SLIMPROTO_HOST, MA_SLIMPROTO_PORT);
        if (ret == ESP_OK) {
            display_draw_string(12, 60, "SLIMPROTO READY", 0x07E0);
        } else {
            ESP_LOGE(TAG, "SlimProto start failed: %s", esp_err_to_name(ret));
            display_draw_string(12, 60, "SLIMPROTO FAIL", 0xF800);
        }
    } else {
        ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(ret));
        display_draw_string(12, 48, "WIFI CONNECT FAIL", 0xF800);
    }
    
    ESP_LOGI(TAG, "Wi-Fi Music Player initialized successfully");
    ESP_LOGI(TAG, "Heap internal free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Heap PSRAM free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "SlimProto mode active");
    
    uint32_t status_log_ticks = 0;

    // Main loop
    while (1) {
        ui_update_display();
        ui_tick_250ms();

        status_log_ticks++;
        if (status_log_ticks >= 20) {
            status_log_ticks = 0;
            ESP_LOGI(TAG, "Wi-Fi stream active - Playing: %s", g_player_state.metadata.is_playing ? "Yes" : "No");
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
