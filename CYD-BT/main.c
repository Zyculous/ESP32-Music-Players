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
    ESP_LOGI(TAG, "Starting Bluetooth Music Player for CYD");

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
    
    // Initialize audio
    ESP_LOGI(TAG, "Initializing audio...");
    ESP_ERROR_CHECK(audio_init());
    
    // Initialize bluetooth
    ESP_LOGI(TAG, "Initializing bluetooth...");
    ESP_ERROR_CHECK(bluetooth_init());

#if CYD_FEATURE_TOUCH
    // Initialize touch
    ESP_LOGI(TAG, "Initializing touch...");
    ESP_ERROR_CHECK(touch_init());
    BaseType_t touch_task_ok = xTaskCreatePinnedToCore(touch_task,
                                                        "touch_task",
                                                        4096,
                                                        NULL,
                                                        5,
                                                        NULL,
                                                        1);
    if (touch_task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        abort();
    }
#else
    ESP_LOGI(TAG, "Touch module disabled at build time");
#endif
    
    // Draw initial UI
    display_clear_screen();
    display_draw_ui();
    
    ESP_LOGI(TAG, "Bluetooth Music Player initialized successfully");
    ESP_LOGI(TAG, "Device name: %s", bluetooth_get_device_name());
    ESP_LOGI(TAG, "Heap internal free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Heap PSRAM free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Waiting for Bluetooth connection...");
    
    uint32_t status_log_ticks = 0;

    // Main loop
    while (1) {
           ESP_LOGI(TAG, "[HEARTBEAT] Main loop tick");
        
        ui_update_display();
        ui_tick_250ms();

        status_log_ticks++;
        if (status_log_ticks >= 20) {
            status_log_ticks = 0;
            if (g_player_state.connected) {
                ESP_LOGI(TAG, "Connected - Playing: %s", g_player_state.metadata.is_playing ? "Yes" : "No");
            } else {
                ESP_LOGI(TAG, "Waiting for Bluetooth connection...");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
