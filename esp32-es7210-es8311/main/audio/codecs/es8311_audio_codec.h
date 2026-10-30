#ifndef ES8311_AUDIO_CODEC_H
#define ES8311_AUDIO_CODEC_H

#include "audio_codec.h"
#include <driver/i2c_master.h>
#include <driver/gpio.h>

AudioCodec* Es8311AudioCodec_Create(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
                                    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                                    gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk, bool pa_inverted);

#endif