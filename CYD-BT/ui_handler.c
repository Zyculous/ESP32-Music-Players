#include "main.h"
#include <stdio.h>

static const char *TAG = "UI";

#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_BLUE        0x001F
#define COLOR_LIGHT_GRAY  0xC618
#define COLOR_DARK_GRAY   0x4208
#define COLOR_CYAN        0x07FF

#define UI_STATE_LOCK_RETRIES 3
#define UI_STATE_LOCK_TIMEOUT_MS 20
#define VOLUME_SLIDER_PAD 2

static void ui_draw_volume_slider(uint8_t volume)
{
    int fill_width = ((VOLUME_SLIDER_W - (VOLUME_SLIDER_PAD * 2)) * volume) / 0x7F;

    display_draw_rectangle(VOLUME_SLIDER_X, VOLUME_SLIDER_Y, VOLUME_SLIDER_W, VOLUME_SLIDER_H, COLOR_DARK_GRAY);
    display_draw_rectangle(VOLUME_SLIDER_X, VOLUME_SLIDER_Y, VOLUME_SLIDER_W, 1, COLOR_LIGHT_GRAY);
    display_draw_rectangle(VOLUME_SLIDER_X, VOLUME_SLIDER_Y + VOLUME_SLIDER_H - 1, VOLUME_SLIDER_W, 1, COLOR_LIGHT_GRAY);
    display_draw_rectangle(VOLUME_SLIDER_X, VOLUME_SLIDER_Y, 1, VOLUME_SLIDER_H, COLOR_LIGHT_GRAY);
    display_draw_rectangle(VOLUME_SLIDER_X + VOLUME_SLIDER_W - 1, VOLUME_SLIDER_Y, 1, VOLUME_SLIDER_H, COLOR_LIGHT_GRAY);

    display_draw_rectangle(VOLUME_SLIDER_X + VOLUME_SLIDER_PAD,
                           VOLUME_SLIDER_Y + VOLUME_SLIDER_PAD,
                           VOLUME_SLIDER_W - (VOLUME_SLIDER_PAD * 2),
                           VOLUME_SLIDER_H - (VOLUME_SLIDER_PAD * 2),
                           COLOR_BLACK);

    if (fill_width > 0) {
        display_draw_rectangle(VOLUME_SLIDER_X + VOLUME_SLIDER_PAD,
                               VOLUME_SLIDER_Y + VOLUME_SLIDER_PAD,
                               fill_width,
                               VOLUME_SLIDER_H - (VOLUME_SLIDER_PAD * 2),
                               COLOR_CYAN);
    }

    char vol_label[10];
    snprintf(vol_label, sizeof(vol_label), "VOL %3u", (unsigned)((volume * 100U) / 0x7FU));
    display_draw_rectangle(12, 24, 88, 8, COLOR_BLACK);
    display_draw_string(14, 24, vol_label, COLOR_WHITE);
}

void ui_draw_button(int x, int y, int size, const char *text, bool pressed)
{
    uint16_t bg_color = pressed ? COLOR_BLUE : COLOR_DARK_GRAY;
    uint16_t border_color = pressed ? COLOR_CYAN : COLOR_LIGHT_GRAY;
    uint16_t text_color = COLOR_WHITE;

    display_draw_rectangle(x, y, size, size, bg_color);

    display_draw_rectangle(x, y, size, 2, border_color);
    display_draw_rectangle(x, y, 2, size, border_color);
    display_draw_rectangle(x + size - 2, y, 2, size, border_color);
    display_draw_rectangle(x, y + size - 2, size, 2, border_color);

    int text_len = strlen(text);
    int text_x = x + (size - text_len * 8) / 2;
    int text_y = y + (size - 8) / 2;

    display_draw_string(text_x, text_y, text, text_color);
}

static bool ui_try_take_state_lock(void)
{
    for (int attempt = 0; attempt < UI_STATE_LOCK_RETRIES; attempt++) {
        if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(UI_STATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    ESP_LOGW(TAG, "Player state lock timeout, skipping UI cycle");
    return false;
}

void ui_update_display(void)
{
    static track_metadata_t last_metadata = {0};
    static bool last_playing = false;
    static uint8_t last_volume = 0xFF;

    track_metadata_t metadata_snapshot = {0};
    bool playing_snapshot = false;
    uint8_t volume_snapshot = 0;

    if (!ui_try_take_state_lock()) {
        return;
    }
    metadata_snapshot = g_player_state.metadata;
    playing_snapshot = g_player_state.metadata.is_playing;
    volume_snapshot = g_player_state.volume;
    xSemaphoreGive(g_player_state_mutex);

    if (volume_snapshot != last_volume) {
        ui_draw_volume_slider(volume_snapshot);
        last_volume = volume_snapshot;
    }

    if (strcmp(last_metadata.title, metadata_snapshot.title) != 0 ||
        strcmp(last_metadata.artist, metadata_snapshot.artist) != 0 ||
        strcmp(last_metadata.album, metadata_snapshot.album) != 0) {
        ESP_LOGI(TAG, "Track metadata changed");
        display_update_track_info(&metadata_snapshot);
        last_metadata = metadata_snapshot;
    }

    if (playing_snapshot != last_playing) {
        ESP_LOGI(TAG, "Play state changed: %s", playing_snapshot ? "Playing" : "Paused");
        display_update_play_button(playing_snapshot);
        last_playing = playing_snapshot;
    }
}

void ui_request_refresh(uint32_t flags)
{
    (void)flags;
}

void ui_tick_250ms(void)
{
}
