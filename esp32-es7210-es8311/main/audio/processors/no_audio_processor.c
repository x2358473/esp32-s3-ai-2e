#include "no_audio_processor.h"
#include "audio_codec.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#define TAG "NoAudioProcessor"

typedef struct {
    AudioCodec* codec;
    int frame_samples;
    bool is_running;
    audio_processor_output_cb_t output_cb;
    void* output_ctx;
    audio_processor_vad_cb_t vad_cb;
    void* vad_ctx;
    int16_t* mono_buffer;
    size_t mono_buffer_samples;
} NoAudioProcessorImpl;

static void no_audio_destroy(AudioProcessor* processor) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    if (impl != NULL) {
        free(impl->mono_buffer);
        free(impl);
    }
    free(processor);
}

static void no_audio_initialize(AudioProcessor* processor, AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    (void)models_list;
    impl->codec = codec;
    impl->frame_samples = frame_duration_ms * 16000 / 1000;
}

static void no_audio_feed(AudioProcessor* processor, const int16_t* data, size_t samples) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    size_t i = 0;
    size_t j = 0;
    if (!impl->is_running || impl->output_cb == NULL || data == NULL || samples == 0) {
        return;
    }

    if (impl->codec != NULL && AudioCodec_GetInputChannels(impl->codec) == 2) {
        size_t mono_samples = samples / 2;
        if (impl->mono_buffer_samples < mono_samples) {
            int16_t* new_buffer = (int16_t*)realloc(impl->mono_buffer, mono_samples * sizeof(int16_t));
            if (new_buffer == NULL) {
                return;
            }
            impl->mono_buffer = new_buffer;
            impl->mono_buffer_samples = mono_samples;
        }
        for (i = 0, j = 0; i < mono_samples; ++i, j += 2) {
            impl->mono_buffer[i] = data[j];
        }
        impl->output_cb(impl->mono_buffer, mono_samples, impl->output_ctx);
        return;
    }

    impl->output_cb(data, samples, impl->output_ctx);
}

static void no_audio_start(AudioProcessor* processor) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    impl->is_running = true;
}

static void no_audio_stop(AudioProcessor* processor) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    impl->is_running = false;
}

static bool no_audio_is_running(AudioProcessor* processor) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    return impl->is_running;
}

static void no_audio_on_output(AudioProcessor* processor, audio_processor_output_cb_t callback, void* user_ctx) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    impl->output_cb = callback;
    impl->output_ctx = user_ctx;
}

static void no_audio_on_vad(AudioProcessor* processor, audio_processor_vad_cb_t callback, void* user_ctx) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    impl->vad_cb = callback;
    impl->vad_ctx = user_ctx;
}

static size_t no_audio_get_feed_size(AudioProcessor* processor) {
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)processor->impl;
    if (impl->codec == NULL) {
        return 0;
    }
    return (size_t)impl->frame_samples;
}

static void no_audio_enable_device_aec(AudioProcessor* processor, bool enable) {
    (void)processor;
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}

AudioProcessor* NoAudioProcessor_Create(void) {
    AudioProcessor* processor = (AudioProcessor*)calloc(1, sizeof(AudioProcessor));
    NoAudioProcessorImpl* impl = (NoAudioProcessorImpl*)calloc(1, sizeof(NoAudioProcessorImpl));
    if (processor == NULL || impl == NULL) {
        free(processor);
        free(impl);
        return NULL;
    }

    processor->impl = impl;
    processor->destroy = no_audio_destroy;
    processor->initialize = no_audio_initialize;
    processor->feed = no_audio_feed;
    processor->start = no_audio_start;
    processor->stop = no_audio_stop;
    processor->is_running = no_audio_is_running;
    processor->on_output = no_audio_on_output;
    processor->on_vad_state_change = no_audio_on_vad;
    processor->get_feed_size = no_audio_get_feed_size;
    processor->enable_device_aec = no_audio_enable_device_aec;
    return processor;
}
