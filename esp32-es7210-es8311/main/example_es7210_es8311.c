#include "audio/codecs/box_audio_codec.h"
#include "audio/audio_codec.h"
#include "audio_application.h"
#include "button.h"
#include "config.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

#define TAG "example_es7210_es8311"

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    Button boot_button;
    bool in_test_mode;
    bool pcm_autoplay_enabled;
    TaskHandle_t pcm_autoplay_task_handle;
} ExampleEs7210Es8311;

static ExampleEs7210Es8311 s_platform;
extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start") __attribute__((weak));
extern const uint8_t canon_pcm_end[] asm("_binary_canon_pcm_end") __attribute__((weak));

static void example_pcm_autoplay_task(void* arg) {
    ExampleEs7210Es8311* self = (ExampleEs7210Es8311*)arg;
    AudioCodec* codec = NULL;
    Application* app = Application_GetInstance();
    AudioService* audio_service = Application_GetAudioService(app);
    const uint8_t* pcm_begin = (const uint8_t*)canon_pcm_start;
    const uint8_t* pcm_end = (const uint8_t*)canon_pcm_end;
    const int16_t* pcm_samples = (const int16_t*)pcm_begin;
    size_t total_samples = (size_t)(pcm_end - pcm_begin) / sizeof(int16_t);
    size_t chunk_samples = 1024;
    bool vad_was_running = false;
    uint32_t loop_index = 0;
    /* More conservative defaults to avoid false barge-in from playback leakage. */
    const int64_t kLocalBargeInVadThresholdUs = 500 * 1000;
    const int64_t kLocalBargeInStartupGuardUs = 1000 * 1000;
    const int64_t kLocalBargeInArmSilenceUs = 300 * 1000;

    vTaskDelay(pdMS_TO_TICKS(1000));
    codec = AudioCodec_GetInstance();
    if (!self->in_test_mode || !self->pcm_autoplay_enabled) {
        vTaskDelete(NULL);
        return;
    }
    if (pcm_begin == NULL || pcm_end == NULL || pcm_end <= pcm_begin || codec == NULL) {
        ESP_LOGW(TAG, "canon.pcm missing or codec unavailable, skip auto playback");
        vTaskDelete(NULL);
        return;
    }

    vad_was_running = AudioService_IsAudioProcessorRunning(audio_service);
    if (!vad_was_running) {
        AudioService_EnableVoiceProcessing(audio_service, true);
    }
    /* Keep device AEC enabled during playback if available to reduce VAD false positives. */
#if CONFIG_USE_DEVICE_AEC
    AudioService_EnableDeviceAec(audio_service, true);
#else
    AudioService_EnableDeviceAec(audio_service, false);
#endif
    AudioService_ClearVadState(audio_service);
    AudioCodec_EnableOutput(codec, true);
    ESP_LOGI(TAG, "Auto PCM playback started, samples=%u (loop mode)", (unsigned)total_samples);

    while (self->in_test_mode && self->pcm_autoplay_enabled) {
        size_t offset = 0;
        bool barge_in_armed = false;
        int64_t silence_since_us = 0;
        int64_t vad_speaking_since_us = 0;
        int64_t loop_start_us = esp_timer_get_time();
        ++loop_index;
        ESP_LOGI(TAG, "Auto PCM playback loop #%u started", (unsigned)loop_index);

        while (self->in_test_mode && self->pcm_autoplay_enabled && offset < total_samples) {
            int64_t now_us = esp_timer_get_time();
            bool voice_detected = Application_IsVoiceDetected(app);
            size_t n = 0;

            if ((now_us - loop_start_us) < kLocalBargeInStartupGuardUs) {
                silence_since_us = 0;
                vad_speaking_since_us = 0;
            } else if (!barge_in_armed) {
                if (!voice_detected) {
                    if (silence_since_us <= 0) {
                        silence_since_us = now_us;
                    } else if ((now_us - silence_since_us) >= kLocalBargeInArmSilenceUs) {
                        barge_in_armed = true;
                        ESP_LOGI(TAG, "local barge-in armed after silence");
                    }
                } else {
                    silence_since_us = 0;
                }
                vad_speaking_since_us = 0;
            } else if (voice_detected) {
                if (vad_speaking_since_us <= 0) {
                    vad_speaking_since_us = now_us;
                } else if ((now_us - vad_speaking_since_us) >= kLocalBargeInVadThresholdUs) {
                    ESP_LOGI(TAG, "local barge-in triggered, vad_speech_ms=%u",
                             (unsigned)((now_us - vad_speaking_since_us) / 1000));
                    ESP_LOGI(TAG, "current PCM playback interrupted by user speech");
                    self->pcm_autoplay_enabled = false;
                    break;
                }
            } else {
                vad_speaking_since_us = 0;
            }

            if (codec == NULL) {
                codec = AudioCodec_GetInstance();
                if (codec == NULL) {
                    break;
                }
            }
            n = (total_samples - offset > chunk_samples) ? chunk_samples : (total_samples - offset);
            AudioCodec_OutputData(codec, pcm_samples + offset, n);
            offset += n;
        }
        ESP_LOGI(TAG, "Auto PCM playback loop #%u finished", (unsigned)loop_index);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    if (!vad_was_running) {
        AudioService_EnableVoiceProcessing(audio_service, false);
    }
#if CONFIG_USE_DEVICE_AEC
    AudioService_EnableDeviceAec(audio_service, true);
#else
    AudioService_EnableDeviceAec(audio_service, false);
#endif
    ESP_LOGI(TAG, "Auto PCM playback finished");
    self->pcm_autoplay_task_handle = NULL;
    vTaskDelete(NULL);
}

static void boot_button_click(void* ctx) {
    ExampleEs7210Es8311* self = (ExampleEs7210Es8311*)ctx;
    Application* app = Application_GetInstance();
    ESP_LOGI(TAG, "BOOT single click: in_test_mode=%d audio_testing=%d", (int)self->in_test_mode, (int)Application_IsAudioTesting(app));
    if (self->in_test_mode) {
        if (Application_IsAudioTesting(app)) {
            Application_ToggleAudioLocalTesting(app);
        } else {
            self->pcm_autoplay_enabled = false;
            Application_ToggleAudioLocalTesting(app);
        }
        return;
    }
    Application_ToggleAudioLocalTesting(app);
}

static void boot_button_double_click(void* ctx) {
    ExampleEs7210Es8311* self = (ExampleEs7210Es8311*)ctx;
    Application* app = Application_GetInstance();
    /* Double click: exit recording test and resume default PCM loop playback. */
    if (self->in_test_mode) {
        Application_StopAudioLocalTesting(app);
        self->pcm_autoplay_enabled = true;
        if (self->pcm_autoplay_task_handle == NULL) {
            xTaskCreate(example_pcm_autoplay_task, "example_pcm_autoplay", 4096, self, 4, &self->pcm_autoplay_task_handle);
        }
    }
}

static void initialize_i2c(ExampleEs7210Es8311* self) {
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = (i2c_port_t)1,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {0},
    };
    i2c_bus_cfg.flags.enable_internal_pullup = 1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &self->i2c_bus));
}

static void initialize_buttons(ExampleEs7210Es8311* self) {
    if (!Button_InitGpio(&self->boot_button, BOOT_BUTTON_GPIO, false, 0, 0, false)) {
        ESP_LOGW(TAG, "Failed to init boot button");
        return;
    }
    Button_SetContext(&self->boot_button, self);
    Button_OnClick(&self->boot_button, boot_button_click);
    Button_OnDoubleClick(&self->boot_button, boot_button_double_click);
}

static void example_platform_init(ExampleEs7210Es8311* self) {
    self->in_test_mode = true;
    self->pcm_autoplay_enabled = true;
    initialize_i2c(self);
    initialize_buttons(self);
    self->pcm_autoplay_task_handle = NULL;
    xTaskCreate(example_pcm_autoplay_task, "example_pcm_autoplay", 4096, self, 4, &self->pcm_autoplay_task_handle);
}

static void setup_audio_codec(ExampleEs7210Es8311* self) {
    AudioCodec* codec = BoxAudioCodec_Create(self->i2c_bus,
                                             AUDIO_INPUT_SAMPLE_RATE,
                                             AUDIO_OUTPUT_SAMPLE_RATE,
                                             AUDIO_I2S_GPIO_MCLK,
                                             AUDIO_I2S_GPIO_BCLK,
                                             AUDIO_I2S_GPIO_WS,
                                             AUDIO_I2S_GPIO_DOUT,
                                             AUDIO_I2S_GPIO_DIN,
                                             AUDIO_CODEC_PA_PIN,
                                             AUDIO_CODEC_ES8311_ADDR,
                                             AUDIO_CODEC_ES7210_ADDR,
                                             AUDIO_INPUT_REFERENCE);
    AudioCodec_SetInstance(codec);
}

void app_main(void) {
    example_platform_init(&s_platform);
    setup_audio_codec(&s_platform);
    Application_Run(Application_GetInstance());
}
