#include "main.h"
#include "freertos/queue.h"
#include <limits.h>
#include <string.h>
#include <math.h>
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "AUDIO";

#define AUDIO_QUEUE_SIZE 96
#define AUDIO_BUFFER_SIZE 1024
#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY (configMAX_PRIORITIES - 1)

typedef struct {
	uint8_t data[AUDIO_BUFFER_SIZE];
	uint32_t len;
} audio_buffer_t;

static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static SemaphoreHandle_t s_audio_io_mutex = NULL;
static bool s_audio_initialized = false;
static uint32_t s_current_sample_rate = 44100;
static uint8_t s_current_channels = 2;
static uint32_t s_audio_packets_seen = 0;

static i2s_chan_handle_t s_i2s_tx_handle = NULL;
static i2s_chan_handle_t s_i2s_rx_handle = NULL;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static esp_codec_dev_handle_t s_codec_handle = NULL;

static void audio_ensure_output_active(void)
{
	if (!s_codec_handle) {
		return;
	}

	bool muted = false;
	if (esp_codec_dev_get_out_mute(s_codec_handle, &muted) == ESP_CODEC_DEV_OK && muted) {
		if (esp_codec_dev_set_out_mute(s_codec_handle, false) == ESP_CODEC_DEV_OK) {
			ESP_LOGW(TAG, "Codec output was muted during playback; forced unmute");
		}
	}

	int current_vol = 0;
	if (esp_codec_dev_get_out_vol(s_codec_handle, &current_vol) == ESP_CODEC_DEV_OK) {
		if (current_vol < 10) {
			int target_vol = 90;
			(void)esp_codec_dev_set_out_vol(s_codec_handle, target_vol);
			ESP_LOGW(TAG,
					 "Codec output volume low during playback (%d); forced to %d",
					 current_vol,
					 target_vol);
		}
	}
}

static esp_err_t audio_open_codec_stream(uint32_t sample_rate, uint8_t channels)
{
	if (!s_codec_handle) {
		return ESP_ERR_INVALID_STATE;
	}

	esp_codec_dev_sample_info_t sample_cfg = {
		.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
		.channel = (channels <= 1) ? 1 : 2,
		.channel_mask = (channels <= 1) ? 0x01 : 0x03,
		.sample_rate = sample_rate,
		.mclk_multiple = I2S_MCLK_MULTIPLE_256,
	};

	int ret = esp_codec_dev_open(s_codec_handle, &sample_cfg);
	if (ret != ESP_CODEC_DEV_OK) {
		ESP_LOGE(TAG, "esp_codec_dev_open failed ret=%d", ret);
		return ESP_FAIL;
	}

	esp_codec_dev_set_out_mute(s_codec_handle, false);
	ESP_LOGI(TAG, "Codec stream opened: %lu Hz, %d ch, unmuted",
			 (unsigned long)sample_rate, (channels <= 1) ? 1 : 2);
	return ESP_OK;
}

static void audio_probe_pa_polarity(void)
{
#if AUDIO_PA_AUTOPROBE
	int other_level = AUDIO_PA_EN_LEVEL ? 0 : 1;
	gpio_set_level(AUDIO_PA_EN_IO, AUDIO_PA_EN_LEVEL);
	vTaskDelay(pdMS_TO_TICKS(60));
	gpio_set_level(AUDIO_PA_EN_IO, other_level);
	vTaskDelay(pdMS_TO_TICKS(60));
	gpio_set_level(AUDIO_PA_EN_IO, AUDIO_PA_EN_LEVEL);
	ESP_LOGI(TAG,
			 "PA polarity probe toggled GPIO %d (active=%d, alt=%d)",
			 AUDIO_PA_EN_IO,
			 AUDIO_PA_EN_LEVEL,
			 other_level);
#endif
}

static void audio_force_enable_pa(void)
{
	gpio_config_t pa_cfg = {
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = 1ULL << AUDIO_PA_EN_IO,
	};
	if (gpio_config(&pa_cfg) == ESP_OK) {
		gpio_set_level(AUDIO_PA_EN_IO, AUDIO_PA_EN_LEVEL);
		ESP_LOGI(TAG, "PA enable forced on GPIO %d (active level=%d)", AUDIO_PA_EN_IO, AUDIO_PA_EN_LEVEL);
		audio_probe_pa_polarity();
	} else {
		ESP_LOGW(TAG, "PA GPIO setup failed on GPIO %d", AUDIO_PA_EN_IO);
	}
}

static esp_err_t audio_setup_i2c_bus(void)
{
	if (s_i2c_bus_handle) {
		return ESP_OK;
	}

	i2c_master_bus_config_t i2c_cfg = {
		.i2c_port = I2C_NUM_CH,
		.sda_io_num = I2C_SDA_IO,
		.scl_io_num = I2C_SCL_IO,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	return i2c_new_master_bus(&i2c_cfg, &s_i2c_bus_handle);
}

i2c_master_bus_handle_t audio_get_shared_i2c_bus(void)
{
	return s_i2c_bus_handle;
}

static esp_err_t audio_setup_i2s(uint32_t sample_rate, uint8_t channels)
{
	if (s_i2s_tx_handle || s_i2s_rx_handle) {
		if (s_i2s_tx_handle) {
			i2s_channel_disable(s_i2s_tx_handle);
		}
		if (s_i2s_rx_handle) {
			i2s_channel_disable(s_i2s_rx_handle);
		}
		if (s_i2s_tx_handle) {
			i2s_del_channel(s_i2s_tx_handle);
			s_i2s_tx_handle = NULL;
		}
		if (s_i2s_rx_handle) {
			i2s_del_channel(s_i2s_rx_handle);
			s_i2s_rx_handle = NULL;
		}
	}

	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_CH, I2S_ROLE_MASTER);
	chan_cfg.auto_clear = true;
	ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, NULL), TAG, "i2s_new_channel failed");

	i2s_slot_mode_t slot_mode = (channels <= 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
	i2s_std_config_t std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
		.gpio_cfg = {
			.mclk = I2S_MCLK_IO,
			.bclk = I2S_BCK_IO,
			.ws = I2S_WS_IO,
			.dout = I2S_DO_IO,
			.din = I2S_DI_IO,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv = false,
			},
		},
	};
	std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

	ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg), TAG, "i2s tx init failed");
	ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_handle), TAG, "i2s tx enable failed");

	s_current_sample_rate = sample_rate;
	s_current_channels = (channels <= 1) ? 1 : 2;
	return ESP_OK;
}

static esp_err_t audio_setup_codec(void)
{
	if (s_codec_handle) {
		return ESP_OK;
	}

	audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
		.port = I2C_NUM_CH,
		.addr = ES8311_CODEC_DEFAULT_ADDR,
		.bus_handle = s_i2c_bus_handle,
	};
	const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
	if (!ctrl_if) {
		return ESP_FAIL;
	}

	audio_codec_i2s_cfg_t i2s_data_cfg = {
		.port = I2S_NUM_CH,
		.rx_handle = NULL,
		.tx_handle = s_i2s_tx_handle,
	};
	const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
	if (!data_if) {
		return ESP_FAIL;
	}

	const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
	if (!gpio_if) {
		return ESP_FAIL;
	}

	es8311_codec_cfg_t es8311_cfg = {
		.ctrl_if = ctrl_if,
		.gpio_if = gpio_if,
		.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
		.master_mode = false,
		.use_mclk = true,
		.pa_pin = AUDIO_PA_EN_IO,
		.pa_reverted = (AUDIO_PA_EN_LEVEL == 0),
		.hw_gain = {
			.pa_voltage = 5.0,
			.codec_dac_voltage = 3.3,
		},
		.mclk_div = I2S_MCLK_MULTIPLE_256,
	};
	const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
	if (!codec_if) {
		return ESP_FAIL;
	}

	esp_codec_dev_cfg_t dev_cfg = {
		.dev_type = ESP_CODEC_DEV_TYPE_OUT,
		.codec_if = codec_if,
		.data_if = data_if,
	};
	s_codec_handle = esp_codec_dev_new(&dev_cfg);
	if (!s_codec_handle) {
		return ESP_FAIL;
	}

	if (audio_open_codec_stream(s_current_sample_rate, s_current_channels) != ESP_OK) {
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Codec output unmuted");
	return ESP_OK;
}

void audio_set_volume(uint8_t volume)
{
	if (volume > 0x7F) {
		volume = 0x7F;
	}

	if (xSemaphoreTake(g_player_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		g_player_state.volume = volume;
		xSemaphoreGive(g_player_state_mutex);
	}

	if (!s_codec_handle) {
		return;
	}

	int percent = (int)(((uint32_t)volume * 100U) / 0x7FU);
	if (percent < 0) {
		percent = 0;
	} else if (percent > 100) {
		percent = 100;
	}

	if (s_audio_io_mutex && xSemaphoreTake(s_audio_io_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
		ESP_LOGW(TAG, "set_out_vol skipped: audio mutex timeout");
		return;
	}

	int ret = esp_codec_dev_set_out_vol(s_codec_handle, percent);
	if (s_audio_io_mutex) {
		xSemaphoreGive(s_audio_io_mutex);
	}
	if (ret != ESP_CODEC_DEV_OK) {
		ESP_LOGW(TAG, "set_out_vol failed ret=%d (req=%d)", ret, percent);
	}
}

static void audio_task(void *params)
{
	(void)params;

	audio_buffer_t buffer;

	while (1) {
		if (xQueueReceive(s_audio_queue, &buffer, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		if (!s_audio_initialized || !s_i2s_tx_handle) {
			continue;
		}

		if (s_audio_io_mutex && xSemaphoreTake(s_audio_io_mutex, pdMS_TO_TICKS(80)) != pdTRUE) {
			continue;
		}

		int codec_ret = esp_codec_dev_write(s_codec_handle, buffer.data, (int)buffer.len);
		if (codec_ret != ESP_CODEC_DEV_OK) {
			static uint32_t write_errors = 0;
			write_errors++;
			if (write_errors <= 5 || (write_errors % 100) == 0) {
				ESP_LOGW(TAG,
						 "esp_codec_dev_write failed ret=%d total_errors=%lu",
						 codec_ret,
						 (unsigned long)write_errors);
			}

			if (s_i2s_tx_handle) {
				size_t bytes_written = 0;
				esp_err_t i2s_ret = i2s_channel_write(s_i2s_tx_handle,
													  buffer.data,
													  buffer.len,
													  &bytes_written,
													  pdMS_TO_TICKS(40));
				if (i2s_ret != ESP_OK && (write_errors <= 5 || (write_errors % 100) == 0)) {
					ESP_LOGW(TAG, "i2s fallback write failed: %s", esp_err_to_name(i2s_ret));
				}
			}
		} else {
			s_audio_packets_seen++;
			if (s_audio_packets_seen <= 8 || (s_audio_packets_seen % 200) == 0) {
				audio_ensure_output_active();
			}
			if (s_audio_packets_seen <= 5 || (s_audio_packets_seen % 500) == 0) {
				ESP_LOGI(TAG, "audio packet out len=%u packets=%lu",
						 (unsigned)buffer.len,
						 (unsigned long)s_audio_packets_seen);
			}
			/* Diagnostic: print first few PCM samples of early packets */
			if (s_audio_packets_seen == 1 && buffer.len >= 8) {
				const int16_t *samples = (const int16_t *)buffer.data;
				bool all_zero = true;
				for (uint32_t i = 0; i < buffer.len / 2 && i < 64; i++) {
					if (samples[i] != 0) { all_zero = false; break; }
				}
				ESP_LOGI(TAG, "PCM diag pkt1: %04x %04x %04x %04x %s",
						 (uint16_t)samples[0], (uint16_t)samples[1],
						 (uint16_t)samples[2], (uint16_t)samples[3],
						 all_zero ? "ALL-ZERO!" : "non-zero ok");
			}
		}

		if (s_audio_io_mutex) {
			xSemaphoreGive(s_audio_io_mutex);
		}
	}
}

esp_err_t audio_init(void)
{
	if (s_audio_initialized) {
		return ESP_OK;
	}

	ESP_RETURN_ON_ERROR(audio_setup_i2c_bus(), TAG, "i2c bus init failed");
	ESP_RETURN_ON_ERROR(audio_setup_i2s(44100, 2), TAG, "i2s init failed");
	ESP_RETURN_ON_ERROR(audio_setup_codec(), TAG, "codec init failed");

	s_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_buffer_t));
	if (!s_audio_queue) {
		return ESP_ERR_NO_MEM;
	}

	s_audio_io_mutex = xSemaphoreCreateMutex();
	if (!s_audio_io_mutex) {
		vQueueDelete(s_audio_queue);
		s_audio_queue = NULL;
		return ESP_ERR_NO_MEM;
	}

	BaseType_t task_ok = xTaskCreatePinnedToCore(audio_task,
												  "audio_task",
												  AUDIO_TASK_STACK_SIZE,
												  NULL,
												  AUDIO_TASK_PRIORITY,
												  &s_audio_task_handle,
												  1);
	if (task_ok != pdPASS) {
		vQueueDelete(s_audio_queue);
		s_audio_queue = NULL;
		vSemaphoreDelete(s_audio_io_mutex);
		s_audio_io_mutex = NULL;
		return ESP_ERR_NO_MEM;
	}

	s_audio_initialized = true;
	audio_set_volume(g_player_state.volume);
	ESP_LOGI(TAG, "audio init ok: %lu Hz %s (ES8311)",
			 (unsigned long)s_current_sample_rate,
			 (s_current_channels == 1) ? "mono" : "stereo");

	/* ---- BOOT-TIME AUDIO DIAGNOSTIC ---- */
	{
		/* Force PA GPIO high manually to be absolutely sure */
		gpio_config_t pa_cfg = {
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = 1ULL << AUDIO_PA_EN_IO,
		};
		gpio_config(&pa_cfg);
		gpio_set_level(AUDIO_PA_EN_IO, 1);
		vTaskDelay(pdMS_TO_TICKS(50));
		int pa_level = gpio_get_level(AUDIO_PA_EN_IO);
		ESP_LOGW(TAG, "DIAG: PA GPIO%d readback = %d (expect 1)", AUDIO_PA_EN_IO, pa_level);

		/* Set volume to max via ES8311 */
		(void)esp_codec_dev_set_out_vol(s_codec_handle, 100);
		(void)esp_codec_dev_set_out_mute(s_codec_handle, false);

		/* Play 1kHz test tone at 44100 Hz via esp_codec_dev_write */
		ESP_LOGW(TAG, "DIAG: Boot tone via codec_dev_write (44100 Hz, 1s)");
		const uint32_t sr = 44100;
		const int total_frames = (int)sr;  /* 1 second */
		const float freq = 1000.0f;
		const float amp = 24000.0f;
		int16_t tbuf[256];  /* 128 stereo frames */
		int frame_idx = 0;
		uint32_t total_written = 0;
		int write_errors = 0;

		while (frame_idx < total_frames) {
			int chunk = total_frames - frame_idx;
			if (chunk > 128) chunk = 128;
			for (int i = 0; i < chunk; i++) {
				float t = (float)(frame_idx + i) / (float)sr;
				int16_t s = (int16_t)(amp * sinf(2.0f * 3.14159265f * freq * t));
				tbuf[i * 2 + 0] = s;
				tbuf[i * 2 + 1] = s;
			}
			int ret = esp_codec_dev_write(s_codec_handle, tbuf, chunk * 4);
			if (ret != ESP_CODEC_DEV_OK) {
				write_errors++;
				if (write_errors <= 3) {
					ESP_LOGE(TAG, "DIAG: codec_dev_write err=%d at frame %d", ret, frame_idx);
				}
			} else {
				total_written += chunk * 4;
			}
			frame_idx += chunk;
		}
		ESP_LOGW(TAG, "DIAG: Boot tone done. Wrote %lu bytes, errors=%d",
				 (unsigned long)total_written, write_errors);

		/* Now try direct I2S write as comparison */
		ESP_LOGW(TAG, "DIAG: Boot tone via i2s_channel_write (44100 Hz, 0.5s)");
		frame_idx = 0;
		total_written = 0;
		const int i2s_frames = (int)(sr / 2);
		while (frame_idx < i2s_frames) {
			int chunk = i2s_frames - frame_idx;
			if (chunk > 128) chunk = 128;
			for (int i = 0; i < chunk; i++) {
				float t = (float)(frame_idx + i) / (float)sr;
				int16_t s = (int16_t)(amp * sinf(2.0f * 3.14159265f * freq * t));
				tbuf[i * 2 + 0] = s;
				tbuf[i * 2 + 1] = s;
			}
			size_t bw = 0;
			esp_err_t wr = i2s_channel_write(s_i2s_tx_handle, tbuf, chunk * 4, &bw, pdMS_TO_TICKS(200));
			if (wr != ESP_OK) {
				write_errors++;
				if (write_errors <= 3) {
					ESP_LOGE(TAG, "DIAG: i2s_write err=%s bw=%u", esp_err_to_name(wr), (unsigned)bw);
				}
			} else {
				total_written += bw;
			}
			frame_idx += chunk;
		}
		ESP_LOGW(TAG, "DIAG: I2S tone done. Wrote %lu bytes", (unsigned long)total_written);

		/* Read back volume register */
		int vol_reg = 0;
		esp_codec_dev_read_reg(s_codec_handle, 0x32, &vol_reg);
		int mute_reg = 0;
		esp_codec_dev_read_reg(s_codec_handle, 0x31, &mute_reg);
		ESP_LOGW(TAG, "DIAG: After tone: vol_reg=0x%02x mute_reg=0x%02x PA=%d",
				 vol_reg, mute_reg, gpio_get_level(AUDIO_PA_EN_IO));
	}

	return ESP_OK;
}

esp_err_t audio_reconfigure(uint32_t sample_rate, uint8_t channels)
{
	if (sample_rate == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	uint8_t norm_channels = (channels <= 1) ? 1 : 2;

	if (!s_audio_initialized) {
		s_current_sample_rate = sample_rate;
		s_current_channels = norm_channels;
		return ESP_OK;
	}

	if (sample_rate == s_current_sample_rate && norm_channels == s_current_channels) {
		return ESP_OK;
	}

	if (s_audio_io_mutex && xSemaphoreTake(s_audio_io_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
		ESP_LOGW(TAG, "audio_reconfigure timeout waiting for audio mutex");
		return ESP_ERR_TIMEOUT;
	}

	audio_reset_buffers();

	/* Close the codec stream.  This calls es8311_enable(false) which
	   suspends the ES8311 and disables PA, then disables the I2S channel
	   via the codec-dev data interface.  No manual I2S manipulation needed. */
	if (s_codec_handle) {
		esp_codec_dev_close(s_codec_handle);
	}

	s_current_sample_rate = sample_rate;
	s_current_channels = norm_channels;

	/* Re-open with new settings.  The codec-dev library internally:
	   1) reconfigures I2S clock/slot for the new sample rate
	   2) re-enables the I2S channel
	   3) configures ES8311 registers for the new sample rate
	   4) calls es8311_start() + es8311_pa_power(ENABLE)
	   5) restores volume and mute settings */
	esp_err_t err = ESP_OK;
	if (s_codec_handle) {
		err = audio_open_codec_stream(s_current_sample_rate, s_current_channels);
		if (err == ESP_OK) {
			uint8_t volume = g_player_state.volume;
			if (volume > 0x7F) {
				volume = 0x7F;
			}
			int percent = (int)(((uint32_t)volume * 100U) / 0x7FU);
			if (percent < 0) {
				percent = 0;
			} else if (percent > 100) {
				percent = 100;
			}
			(void)esp_codec_dev_set_out_vol(s_codec_handle, percent);
			ESP_LOGI(TAG, "audio reconfig done: %lu Hz %s vol=%d%%",
					 (unsigned long)sample_rate,
					 (norm_channels == 1) ? "mono" : "stereo",
					 percent);
		}
	}

	if (s_audio_io_mutex) {
		xSemaphoreGive(s_audio_io_mutex);
	}

	ESP_RETURN_ON_ERROR(err, TAG, "audio reconfigure failed");

	/* ---- DIAGNOSTIC: dump key ES8311 registers ---- */
	if (s_codec_handle) {
		int val = 0;
		const int regs[] = {0x00, 0x01, 0x02, 0x09, 0x0A, 0x0D, 0x0E, 0x12, 0x14, 0x31, 0x32, 0x37, 0x44, 0x45};
		for (int i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
			if (esp_codec_dev_read_reg(s_codec_handle, regs[i], &val) == ESP_CODEC_DEV_OK) {
				ESP_LOGI(TAG, "ES8311 reg[0x%02x] = 0x%02x", regs[i], val);
			}
		}
	}

	/* ---- DIAGNOSTIC: 1kHz test tone via direct I2S write ---- */
	if (s_i2s_tx_handle) {
		ESP_LOGW(TAG, ">>> Playing 1kHz test tone (0.5s) via I2S <<<");
		const uint32_t tone_sr = s_current_sample_rate;
		const int tone_frames = (int)(tone_sr / 2);  /* 0.5 seconds */
		const float freq = 1000.0f;
		const float amplitude = 16000.0f;  /* ~half of int16 max */
		int16_t tone_buf[256];  /* 128 stereo frames */
		int frame_idx = 0;

		while (frame_idx < tone_frames) {
			int chunk = tone_frames - frame_idx;
			if (chunk > 128) chunk = 128;

			for (int i = 0; i < chunk; i++) {
				float t = (float)(frame_idx + i) / (float)tone_sr;
				int16_t sample = (int16_t)(amplitude * sinf(2.0f * 3.14159265f * freq * t));
				tone_buf[i * 2 + 0] = sample;  /* L */
				tone_buf[i * 2 + 1] = sample;  /* R */
			}

			size_t bytes_written = 0;
			esp_err_t wr = i2s_channel_write(s_i2s_tx_handle,
				tone_buf, chunk * 4, &bytes_written, pdMS_TO_TICKS(200));
			if (wr != ESP_OK) {
				ESP_LOGE(TAG, "I2S tone write failed: %s", esp_err_to_name(wr));
				break;
			}
			frame_idx += chunk;
		}
		ESP_LOGW(TAG, ">>> Test tone done <<<");
	}

	return ESP_OK;
}

void audio_data_callback(const uint8_t *data, uint32_t len)
{
	if (!s_audio_initialized || !s_audio_queue || !data || len == 0) {
		return;
	}

	uint32_t offset = 0;
	while (offset < len) {
		audio_buffer_t buffer;
		uint32_t chunk_len = len - offset;
		if (chunk_len > AUDIO_BUFFER_SIZE) {
			chunk_len = AUDIO_BUFFER_SIZE;
		}

		chunk_len &= ~1U;
		if (chunk_len == 0) {
			break;
		}

		memcpy(buffer.data, data + offset, chunk_len);
		buffer.len = chunk_len;

		if (xQueueSend(s_audio_queue, &buffer, pdMS_TO_TICKS(20)) != pdTRUE) {
			static uint32_t dropped_chunks = 0;
			dropped_chunks++;
			if (dropped_chunks <= 5 || (dropped_chunks % 100) == 0) {
				ESP_LOGW(TAG, "audio queue drops: %lu", (unsigned long)dropped_chunks);
			}
			break;
		}

		offset += chunk_len;
	}
}

void audio_reset_buffers(void)
{
	if (!s_audio_initialized || !s_audio_queue) {
		return;
	}

	xQueueReset(s_audio_queue);
	s_audio_packets_seen = 0;
}
