#include "afe_audio_processor.h"
#include "audio_codec.h"

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_config.h>
#include <model_path.h>

#define TAG "AfeAudioProcessor"
#define PROCESSOR_RUNNING (1 << 0)

typedef struct {
    EventGroupHandle_t event_group;
    const esp_afe_sr_iface_t* afe_iface;
    esp_afe_sr_data_t* afe_data;

    audio_processor_output_cb_t output_cb;
    void* output_ctx;
    audio_processor_vad_cb_t vad_cb;
    void* vad_ctx;

    AudioCodec* codec;
    int frame_samples;
    bool is_speaking;
    bool device_aec_runtime_enabled;

    int16_t* output_buffer;
    size_t output_len;
    size_t output_cap;
} AfeAudioProcessorImpl;

static void afe_task(void* arg);

static void afe_destroy(AudioProcessor* processor) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    if (impl != NULL) {
        if (impl->afe_data != NULL && impl->afe_iface != NULL) {
            impl->afe_iface->destroy(impl->afe_data);
            impl->afe_data = NULL;
        }
        if (impl->event_group != NULL) {
            vEventGroupDelete(impl->event_group);
        }
        free(impl->output_buffer);
        free(impl);
    }
    free(processor);
}

static void afe_initialize(AudioProcessor* processor, AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    int ref_num = 0;
    char input_format[8];
    int idx = 0;
    char* ns_model_name = NULL;
    char* vad_model_name = NULL;
    afe_config_t* afe_config = NULL;

    impl->codec = codec;
    impl->frame_samples = frame_duration_ms * 16000 / 1000;

    /* Build input_format like "MMMR" based on codec channels + reference. */
    ref_num = AudioCodec_GetInputReference(codec) ? 1 : 0;
    for (idx = 0; idx < AudioCodec_GetInputChannels(codec) - ref_num && idx < (int)sizeof(input_format) - 2; ++idx) {
        input_format[idx] = 'M';
    }
    if (ref_num > 0 && idx < (int)sizeof(input_format) - 1) {
        input_format[idx++] = 'R';
    }
    input_format[idx] = '\0';

    if (models_list != NULL) {
        ns_model_name = esp_srmodel_filter(models_list, ESP_NSNET_PREFIX, NULL);
        vad_model_name = esp_srmodel_filter(models_list, ESP_VADN_PREFIX, NULL);
    }

    afe_config = afe_config_init(input_format, models_list, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != NULL) {
        afe_config->vad_model_name = vad_model_name;
    }
    if (ns_model_name != NULL) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#if CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    /* Keep VAD initialized even when device AEC is enabled. */
    afe_config->vad_init = true;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    impl->afe_iface = esp_afe_handle_from_config(afe_config);
    impl->afe_data = impl->afe_iface->create_from_config(afe_config);
    impl->device_aec_runtime_enabled = false;

    /* Allocate output buffer for framing. */
    impl->output_cap = (size_t)impl->frame_samples * 4;
    impl->output_buffer = (int16_t*)malloc(impl->output_cap * sizeof(int16_t));
    impl->output_len = 0;

    ESP_LOGI(TAG, "AFE initialized (format=%s, frame_samples=%d)", input_format, impl->frame_samples);

    xTaskCreate(afe_task, "audio_communication", 4096, impl, 3, NULL);
}

static void afe_feed(AudioProcessor* processor, const int16_t* data, size_t samples) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    (void)samples;
    if (impl->afe_data == NULL || impl->afe_iface == NULL || data == NULL) {
        return;
    }
    impl->afe_iface->feed(impl->afe_data, data);
}

static void afe_start(AudioProcessor* processor) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    xEventGroupSetBits(impl->event_group, PROCESSOR_RUNNING);
}

static void afe_stop(AudioProcessor* processor) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    xEventGroupClearBits(impl->event_group, PROCESSOR_RUNNING);
    if (impl->afe_data != NULL && impl->afe_iface != NULL) {
        impl->afe_iface->reset_buffer(impl->afe_data);
    }
    if (impl->vad_cb != NULL && impl->is_speaking) {
        impl->is_speaking = false;
        impl->vad_cb(false, impl->vad_ctx);
    }
}

static bool afe_is_running(AudioProcessor* processor) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    return (xEventGroupGetBits(impl->event_group) & PROCESSOR_RUNNING) != 0;
}

static void afe_on_output(AudioProcessor* processor, audio_processor_output_cb_t callback, void* user_ctx) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    impl->output_cb = callback;
    impl->output_ctx = user_ctx;
}

static void afe_on_vad(AudioProcessor* processor, audio_processor_vad_cb_t callback, void* user_ctx) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    impl->vad_cb = callback;
    impl->vad_ctx = user_ctx;
}

static size_t afe_get_feed_size(AudioProcessor* processor) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
    if (impl->afe_data == NULL || impl->afe_iface == NULL) {
        return 0;
    }
    return (size_t)impl->afe_iface->get_feed_chunksize(impl->afe_data);
}

static void afe_enable_device_aec(AudioProcessor* processor, bool enable) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)processor->impl;
#if CONFIG_USE_DEVICE_AEC
    if (impl->afe_data == NULL || impl->afe_iface == NULL) {
        return;
    }
    if (enable) {
        if (impl->device_aec_runtime_enabled) {
            return;
        }
        impl->afe_iface->enable_aec(impl->afe_data);
        impl->device_aec_runtime_enabled = true;
    } else {
        if (!impl->device_aec_runtime_enabled) {
            return;
        }
        impl->afe_iface->disable_aec(impl->afe_data);
        impl->device_aec_runtime_enabled = false;
    }
#else
    static bool s_warned = false;
    (void)enable;
    if (!s_warned && enable) {
        s_warned = true;
        ESP_LOGW(TAG, "Device AEC requested but not enabled in Kconfig (CONFIG_USE_DEVICE_AEC=0)");
    }
#endif
}

static void afe_task(void* arg) {
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)arg;
    int fetch_size = 0;
    int feed_size = 0;

    if (impl->afe_data == NULL || impl->afe_iface == NULL) {
        vTaskDelete(NULL);
        return;
    }
    fetch_size = impl->afe_iface->get_fetch_chunksize(impl->afe_data);
    feed_size = impl->afe_iface->get_feed_chunksize(impl->afe_data);
    ESP_LOGI(TAG, "AFE task started, feed=%d fetch=%d", feed_size, fetch_size);

    while (true) {
        afe_fetch_result_t* res = NULL;
        xEventGroupWaitBits(impl->event_group, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);
        res = impl->afe_iface->fetch_with_delay(impl->afe_data, portMAX_DELAY);
        if ((xEventGroupGetBits(impl->event_group) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == NULL || res->ret_value == ESP_FAIL) {
            continue;
        }

        /* VAD state change */
        if (impl->vad_cb != NULL) {
            if (res->vad_state == VAD_SPEECH && !impl->is_speaking) {
                impl->is_speaking = true;
                impl->vad_cb(true, impl->vad_ctx);
            } else if (res->vad_state == VAD_SILENCE && impl->is_speaking) {
                impl->is_speaking = false;
                impl->vad_cb(false, impl->vad_ctx);
            }
        }

        /* Output framing to match original C++ behavior. */
        if (impl->output_cb != NULL && res->data != NULL && res->data_size > 0) {
            size_t samples = (size_t)res->data_size / sizeof(int16_t);
            if (impl->output_len + samples > impl->output_cap) {
                size_t new_cap = (impl->output_len + samples) * 2;
                int16_t* new_buf = (int16_t*)realloc(impl->output_buffer, new_cap * sizeof(int16_t));
                if (new_buf != NULL) {
                    impl->output_buffer = new_buf;
                    impl->output_cap = new_cap;
                } else {
                    impl->output_len = 0;
                }
            }
            if (impl->output_buffer != NULL && impl->output_len + samples <= impl->output_cap) {
                memcpy(impl->output_buffer + impl->output_len, res->data, samples * sizeof(int16_t));
                impl->output_len += samples;
                while (impl->output_len >= (size_t)impl->frame_samples) {
                    impl->output_cb(impl->output_buffer, (size_t)impl->frame_samples, impl->output_ctx);
                    impl->output_len -= (size_t)impl->frame_samples;
                    if (impl->output_len > 0) {
                        memmove(impl->output_buffer, impl->output_buffer + impl->frame_samples, impl->output_len * sizeof(int16_t));
                    }
                }
            }
        }
    }
}

AudioProcessor* AfeAudioProcessor_Create(void) {
    AudioProcessor* processor = (AudioProcessor*)calloc(1, sizeof(AudioProcessor));
    AfeAudioProcessorImpl* impl = (AfeAudioProcessorImpl*)calloc(1, sizeof(AfeAudioProcessorImpl));
    if (processor == NULL || impl == NULL) {
        free(processor);
        free(impl);
        return NULL;
    }

    impl->event_group = xEventGroupCreate();
    processor->impl = impl;
    processor->destroy = afe_destroy;
    processor->initialize = afe_initialize;
    processor->feed = afe_feed;
    processor->start = afe_start;
    processor->stop = afe_stop;
    processor->is_running = afe_is_running;
    processor->on_output = afe_on_output;
    processor->on_vad_state_change = afe_on_vad;
    processor->get_feed_size = afe_get_feed_size;
    processor->enable_device_aec = afe_enable_device_aec;
    return processor;
}
