#include "es8311_audio_codec.h"
#include <esp_log.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <stdlib.h>

#define TAG "Es8311AudioCodec"

typedef struct {
    AudioCodec base;
    const audio_codec_data_if_t* data_if;
    const audio_codec_ctrl_if_t* ctrl_if;
    const audio_codec_if_t* codec_if;
    const audio_codec_gpio_if_t* gpio_if;
    esp_codec_dev_handle_t dev;
    gpio_num_t pa_pin;
    bool pa_inverted;
} Es8311AudioCodec;

static Es8311AudioCodec* cast_es8311(AudioCodec* codec) { return (Es8311AudioCodec*)codec; }

static void es8311_create_duplex_channels(AudioCodec* codec, gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0, .role = I2S_ROLE_MASTER, .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM, .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true, .auto_clear_before_cb = false, .intr_priority = 0,
    };
    i2s_std_config_t std_cfg = {
        .clk_cfg = {.sample_rate_hz = (uint32_t)codec->output_sample_rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
                    .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                     .left_align = true, .big_endian = false, .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {.mclk = mclk, .bclk = bclk, .ws = ws, .dout = dout, .din = din, .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &codec->tx_handle, &codec->rx_handle));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->rx_handle, &std_cfg));
}

static void es8311_update_device_state(AudioCodec* codec) {
    Es8311AudioCodec* self = cast_es8311(codec);
    if ((codec->input_enabled || codec->output_enabled) && self->dev == NULL) {
        esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = self->codec_if, .data_if = self->data_if};
        esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = (uint32_t)codec->input_sample_rate, .mclk_multiple = 0};
        self->dev = esp_codec_dev_new(&dev_cfg);
        ESP_ERROR_CHECK(esp_codec_dev_open(self->dev, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(self->dev, codec->input_gain));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(self->dev, codec->output_volume));
    } else if (!codec->input_enabled && !codec->output_enabled && self->dev != NULL) {
        esp_codec_dev_close(self->dev);
        self->dev = NULL;
    }
    if (self->pa_pin != GPIO_NUM_NC) {
        int level = codec->output_enabled ? 1 : 0;
        gpio_set_level(self->pa_pin, self->pa_inverted ? !level : level);
    }
}

static int es8311_read(AudioCodec* codec, int16_t* dest, int samples) {
    Es8311AudioCodec* self = cast_es8311(codec);
    if (codec->input_enabled && self->dev != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(self->dev, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

static int es8311_write(AudioCodec* codec, const int16_t* data, int samples) {
    Es8311AudioCodec* self = cast_es8311(codec);
    if (codec->output_enabled && self->dev != NULL) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(self->dev, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}

static void es8311_set_vol(AudioCodec* codec, int volume) {
    Es8311AudioCodec* self = cast_es8311(codec);
    if (self->dev != NULL) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(self->dev, volume));
    }
}

static void es8311_enable_input(AudioCodec* codec, bool enable) { if (codec->input_enabled != enable) { codec->input_enabled = enable; es8311_update_device_state(codec); } }
static void es8311_enable_output(AudioCodec* codec, bool enable) { if (codec->output_enabled != enable) { codec->output_enabled = enable; es8311_update_device_state(codec); } }

static void es8311_destroy(AudioCodec* codec) {
    Es8311AudioCodec* self = cast_es8311(codec);
    if (self->dev != NULL) {
        esp_codec_dev_close(self->dev);
        esp_codec_dev_delete(self->dev);
    }
    audio_codec_delete_codec_if(self->codec_if);
    audio_codec_delete_ctrl_if(self->ctrl_if);
    audio_codec_delete_gpio_if(self->gpio_if);
    audio_codec_delete_data_if(self->data_if);
    free(self);
}

AudioCodec* Es8311AudioCodec_Create(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
                                    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                                    gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk, bool pa_inverted) {
    Es8311AudioCodec* self = (Es8311AudioCodec*)calloc(1, sizeof(Es8311AudioCodec));
    audio_codec_i2s_cfg_t i2s_cfg = {0};
    audio_codec_i2c_cfg_t i2c_cfg = {0};
    es8311_codec_cfg_t es8311_cfg = {0};
    if (self == NULL) {
        return NULL;
    }
    self->base.duplex = true;
    self->base.input_reference = false;
    self->base.input_channels = 1;
    self->base.output_channels = 1;
    self->base.input_sample_rate = input_sample_rate;
    self->base.output_sample_rate = output_sample_rate;
    self->base.input_gain = 30.0f;
    self->base.output_volume = 90;
    self->base.read = es8311_read;
    self->base.write = es8311_write;
    self->base.enable_input_impl = es8311_enable_input;
    self->base.enable_output_impl = es8311_enable_output;
    self->base.set_output_volume_impl = es8311_set_vol;
    self->base.destroy_impl = es8311_destroy;
    self->pa_pin = pa_pin;
    self->pa_inverted = pa_inverted;
    es8311_create_duplex_channels(&self->base, mclk, bclk, ws, dout, din);

    i2s_cfg.port = I2S_NUM_0; i2s_cfg.rx_handle = self->base.rx_handle; i2s_cfg.tx_handle = self->base.tx_handle;
    self->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    i2c_cfg.port = i2c_port; i2c_cfg.addr = es8311_addr; i2c_cfg.bus_handle = i2c_master_handle;
    self->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    self->gpio_if = audio_codec_new_gpio();
    es8311_cfg.ctrl_if = self->ctrl_if;
    es8311_cfg.gpio_if = self->gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0f;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3f;
    es8311_cfg.pa_reverted = pa_inverted;
    self->codec_if = es8311_codec_new(&es8311_cfg);
    ESP_LOGI(TAG, "Es8311AudioCodec initialized");
    return &self->base;
}
