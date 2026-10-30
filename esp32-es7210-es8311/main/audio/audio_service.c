#include "audio_service.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#define TAG "AudioService"
#define MAX_TEST_FRAMES (AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS)

static void audio_input_task(void* arg);
static void audio_output_task(void* arg);
static bool audio_service_read_resampled(AudioService* service, int16_t* out, size_t out_samples, int out_sample_rate);
static void audio_processor_out_cb(const int16_t* data, size_t samples, void* user_ctx);
static void audio_processor_vad_cb(bool speaking, void* user_ctx);

static void resample_linear(const int16_t* input, size_t in_samples, int16_t* output, size_t out_samples) {
    double scale = 0.0;
    size_t i = 0;
    if (out_samples == 0) {
        return;
    }
    if (in_samples == 0) {
        memset(output, 0, out_samples * sizeof(int16_t));
        return;
    }
    if (in_samples == out_samples) {
        memcpy(output, input, out_samples * sizeof(int16_t));
        return;
    }
    if (out_samples == 1) {
        output[0] = input[0];
        return;
    }
    if (in_samples == 1) {
        for (i = 0; i < out_samples; ++i) {
            output[i] = input[0];
        }
        return;
    }
    scale = (double)(in_samples - 1) / (double)(out_samples - 1);
    for (i = 0; i < out_samples; ++i) {
        double pos = (double)i * scale;
        size_t left = (size_t)pos;
        size_t right = (left + 1 < in_samples) ? (left + 1) : (in_samples - 1);
        double frac = pos - (double)left;
        double v = (double)input[left] * (1.0 - frac) + (double)input[right] * frac;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        output[i] = (int16_t)v;
    }
}

void AudioService_Init(AudioService* service) {
    memset(service, 0, sizeof(AudioService));
    service->event_group = xEventGroupCreate();
    service->audio_testing_queue = xQueueCreate(MAX_TEST_FRAMES, sizeof(AudioTask));
    service->audio_playback_queue = xQueueCreate(MAX_TEST_FRAMES, sizeof(AudioTask));
    service->service_stopped = true;
}

void AudioService_Deinit(AudioService* service) {
    AudioService_Stop(service);
    if (service->audio_processor != NULL && service->audio_processor->destroy != NULL) {
        service->audio_processor->destroy(service->audio_processor);
        service->audio_processor = NULL;
    }
    if (service->audio_testing_queue != NULL) {
        vQueueDelete(service->audio_testing_queue);
    }
    if (service->audio_playback_queue != NULL) {
        vQueueDelete(service->audio_playback_queue);
    }
    if (service->event_group != NULL) {
        vEventGroupDelete(service->event_group);
    }
}

void AudioService_Initialize(AudioService* service, AudioCodec* codec) {
    service->codec = codec;
    AudioCodec_Start(codec);
#if CONFIG_USE_AUDIO_PROCESSOR
    service->audio_processor = AfeAudioProcessor_Create();
#else
    service->audio_processor = NoAudioProcessor_Create();
#endif
    if (service->audio_processor != NULL && service->audio_processor->on_output != NULL) {
        service->audio_processor->on_output(service->audio_processor, audio_processor_out_cb, service);
    }
    if (service->audio_processor != NULL && service->audio_processor->on_vad_state_change != NULL) {
        service->audio_processor->on_vad_state_change(service->audio_processor, audio_processor_vad_cb, service);
    }
}

void AudioService_Start(AudioService* service) {
    service->service_stopped = false;
    /* audio_input now includes resampling + AFE feed path; give it more stack. */
    xTaskCreate(audio_input_task, "audio_input", 8192, service, 8, &service->audio_input_task_handle);
    xTaskCreate(audio_output_task, "audio_output", 4096, service, 4, &service->audio_output_task_handle);
}

void AudioService_Stop(AudioService* service) {
    service->service_stopped = true;
    xEventGroupClearBits(service->event_group, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);
}

bool AudioService_IsVoiceDetected(const AudioService* service) {
    return service->voice_detected;
}

bool AudioService_IsIdle(AudioService* service) {
    return uxQueueMessagesWaiting(service->audio_testing_queue) == 0 &&
           uxQueueMessagesWaiting(service->audio_playback_queue) == 0;
}

bool AudioService_IsAudioProcessorRunning(const AudioService* service) {
    return (xEventGroupGetBits(service->event_group) & AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0;
}

void AudioService_EnableVoiceProcessing(AudioService* service, bool enable) {
    if (service->audio_processor == NULL) {
        return;
    }
    if (enable) {
        if (!service->audio_processor_initialized && service->audio_processor->initialize != NULL) {
            service->audio_processor->initialize(service->audio_processor, service->codec, OPUS_FRAME_DURATION_MS, service->models_list);
            service->audio_processor_initialized = true;
        }
        if (service->audio_processor->start != NULL) {
            service->audio_processor->start(service->audio_processor);
        }
        xEventGroupSetBits(service->event_group, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        if (service->audio_processor->stop != NULL) {
            service->audio_processor->stop(service->audio_processor);
        }
        xEventGroupClearBits(service->event_group, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService_EnableAudioTesting(AudioService* service, bool enable) {
    if (enable) {
        xEventGroupSetBits(service->event_group, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        AudioTask task = {0};
        xEventGroupClearBits(service->event_group, AS_EVENT_AUDIO_TESTING_RUNNING);
        while (xQueueReceive(service->audio_testing_queue, &task, 0) == pdTRUE) {
            xQueueSend(service->audio_playback_queue, &task, 0);
        }
    }
}

void AudioService_EnableDeviceAec(AudioService* service, bool enable) {
    if (service->audio_processor != NULL && service->audio_processor->enable_device_aec != NULL) {
        service->audio_processor->enable_device_aec(service->audio_processor, enable);
    }
}

void AudioService_SetCallbacks(AudioService* service, const AudioServiceCallbacks* callbacks) {
    if (callbacks != NULL) {
        service->callbacks = *callbacks;
    }
}

void AudioService_AbortPlayback(AudioService* service) {
    AudioTask task = {0};
    while (xQueueReceive(service->audio_playback_queue, &task, 0) == pdTRUE) {
        free(task.pcm);
    }
}

void AudioService_ResetDecoder(AudioService* service) {
    AudioService_AbortPlayback(service);
}

void AudioService_SetModelsList(AudioService* service, srmodel_list_t* models_list) {
    service->models_list = models_list;
}

void AudioService_ClearVadState(AudioService* service) {
    if (service != NULL) {
        service->voice_detected = false;
    }
}

static bool audio_service_read_frame(AudioService* service, int16_t* data, size_t samples) {
    if (!AudioCodec_GetInputEnabled(service->codec)) {
        AudioCodec_EnableInput(service->codec, true);
    }
    if (!AudioCodec_InputData(service->codec, data, samples)) {
        return false;
    }
    service->debug_statistics.input_count++;
    return true;
}

static bool audio_service_read_resampled(AudioService* service, int16_t* out, size_t out_samples, int out_sample_rate) {
    int native_sr = 0;
    int channels = 0;
    size_t native_samples = 0;
    int16_t* native_buf = NULL;
    bool ok = false;

    if (service->codec == NULL) {
        return false;
    }
    native_sr = AudioCodec_GetInputSampleRate(service->codec);
    channels = AudioCodec_GetInputChannels(service->codec);
    if (channels <= 0) {
        channels = 1;
    }

    if (native_sr == out_sample_rate) {
        return audio_service_read_frame(service, out, out_samples * (size_t)channels);
    }

    native_samples = (size_t)((uint64_t)out_samples * (uint64_t)native_sr / (uint64_t)out_sample_rate);
    if (native_samples < 1) {
        native_samples = 1;
    }
    native_buf = (int16_t*)malloc(native_samples * (size_t)channels * sizeof(int16_t));
    if (native_buf == NULL) {
        return false;
    }

    ok = audio_service_read_frame(service, native_buf, native_samples * (size_t)channels);
    if (!ok) {
        free(native_buf);
        return false;
    }

    if (channels == 2) {
        size_t i = 0;
        size_t j = 0;
        int16_t* mic = (int16_t*)malloc(native_samples * sizeof(int16_t));
        int16_t* ref = (int16_t*)malloc(native_samples * sizeof(int16_t));
        int16_t* mic_r = (int16_t*)malloc(out_samples * sizeof(int16_t));
        int16_t* ref_r = (int16_t*)malloc(out_samples * sizeof(int16_t));
        if (mic == NULL || ref == NULL || mic_r == NULL || ref_r == NULL) {
            free(mic);
            free(ref);
            free(mic_r);
            free(ref_r);
            free(native_buf);
            return false;
        }
        for (i = 0, j = 0; i < native_samples; ++i, j += 2) {
            mic[i] = native_buf[j];
            ref[i] = native_buf[j + 1];
        }
        resample_linear(mic, native_samples, mic_r, out_samples);
        resample_linear(ref, native_samples, ref_r, out_samples);
        for (i = 0, j = 0; i < out_samples; ++i, j += 2) {
            out[j] = mic_r[i];
            out[j + 1] = ref_r[i];
        }
        free(mic);
        free(ref);
        free(mic_r);
        free(ref_r);
    } else {
        resample_linear(native_buf, native_samples, out, out_samples);
    }

    free(native_buf);
    return true;
}

static void audio_input_task(void* arg) {
    AudioService* service = (AudioService*)arg;
    size_t channels = (size_t)AudioCodec_GetInputChannels(service->codec);
    int native_sr = AudioCodec_GetInputSampleRate(service->codec);
    size_t samples_16k = (size_t)(OPUS_FRAME_DURATION_MS * 16000 / 1000);
    size_t samples_native = (size_t)(OPUS_FRAME_DURATION_MS * native_sr / 1000);
    size_t max_samples = (samples_native > samples_16k ? samples_native : samples_16k) * (channels == 0 ? 1 : channels);
    int16_t* buffer = (int16_t*)malloc(max_samples * sizeof(int16_t));
    if (buffer == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (!service->service_stopped) {
        EventBits_t bits = xEventGroupGetBits(service->event_group);
        if ((bits & (AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            AudioTask task = {0};
            /* Record test audio at codec native sample rate to preserve pitch/speed on playback. */
            size_t frame_samples = samples_native;
            size_t frame_total = frame_samples * channels;
            size_t i = 0;
            if (frame_total > max_samples) {
                frame_total = max_samples;
            }
            if (!audio_service_read_resampled(service, buffer, frame_samples, native_sr)) {
                continue;
            }
            /* Original behavior: if input has reference channel, record mic channel only. */
            if (channels == 2) {
                task.samples = frame_samples;
                task.pcm = (int16_t*)malloc(frame_samples * sizeof(int16_t));
                if (task.pcm != NULL) {
                    for (i = 0; i < frame_samples; ++i) {
                        task.pcm[i] = buffer[i * 2];
                    }
                }
            } else {
                task.samples = frame_total;
                task.pcm = (int16_t*)malloc(frame_total * sizeof(int16_t));
                if (task.pcm != NULL) {
                    memcpy(task.pcm, buffer, frame_total * sizeof(int16_t));
                }
            }
            if (task.pcm != NULL) {
                xQueueSend(service->audio_testing_queue, &task, pdMS_TO_TICKS(10));
            }
        }

        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            size_t feed = 0;
            if (service->audio_processor != NULL && service->audio_processor->get_feed_size != NULL) {
                feed = service->audio_processor->get_feed_size(service->audio_processor);
            }
            if (feed > 0) {
                size_t total = feed * channels;
                if (total > max_samples) {
                    int16_t* new_buf = (int16_t*)realloc(buffer, total * sizeof(int16_t));
                    if (new_buf != NULL) {
                        buffer = new_buf;
                        max_samples = total;
                    } else {
                        continue;
                    }
                }
                if (audio_service_read_resampled(service, buffer, feed, 16000)) {
                    if (service->audio_processor != NULL && service->audio_processor->feed != NULL) {
                        service->audio_processor->feed(service->audio_processor, buffer, total);
                    }
                }
            }
        }
    }
    free(buffer);
    vTaskDelete(NULL);
}

static void audio_output_task(void* arg) {
    AudioService* service = (AudioService*)arg;
    AudioTask task = {0};
    while (!service->service_stopped) {
        if (xQueueReceive(service->audio_playback_queue, &task, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!AudioCodec_GetOutputEnabled(service->codec)) {
            AudioCodec_EnableOutput(service->codec, true);
        }
        AudioCodec_OutputData(service->codec, task.pcm, task.samples);
        free(task.pcm);
        service->debug_statistics.playback_count++;
    }
    vTaskDelete(NULL);
}

static void audio_processor_out_cb(const int16_t* data, size_t samples, void* user_ctx) {
    (void)data;
    (void)samples;
    (void)user_ctx;
}

static void audio_processor_vad_cb(bool speaking, void* user_ctx) {
    AudioService* service = (AudioService*)user_ctx;
    service->voice_detected = speaking;
    if (service->callbacks.on_vad_change != NULL) {
        service->callbacks.on_vad_change(speaking, service->callbacks.user_ctx);
    }
}
