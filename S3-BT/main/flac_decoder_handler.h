#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t flac_decoder_init(void);
esp_err_t flac_decoder_start_stream(void);
esp_err_t flac_decoder_feed(const uint8_t *data, uint32_t len, bool eos);
void flac_decoder_stop_stream(void);
