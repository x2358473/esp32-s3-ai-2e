#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <model_path.h>
#include "audio_codec.h"
#include "audio_processor.h"

#define OPUS_FRAME_DURATION_MS 60
#define AUDIO_TESTING_MAX_DURATION_MS 10000

#define AS_EVENT_AUDIO_TESTING_RUNNING   (1 << 0)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING (1 << 1)

typedef void (*audio_service_vad_cb_t)(bool speaking, void* user_ctx);

typedef struct {
    audio_service_vad_cb_t on_vad_change;
    void* user_ctx;
} AudioServiceCallbacks;

typedef struct {
    int16_t* pcm;
    size_t samples;
    uint32_t timestamp;
} AudioTask;

typedef struct {
    uint32_t input_count;
    uint32_t decode_count;
    uint32_t encode_count;
    uint32_t playback_count;
} DebugStatistics;

typedef struct {
    AudioCodec* codec;
    AudioServiceCallbacks callbacks;
    AudioProcessor* audio_processor;
    DebugStatistics debug_statistics;
    srmodel_list_t* models_list;
    EventGroupHandle_t event_group;
    QueueHandle_t audio_testing_queue;
    QueueHandle_t audio_playback_queue;
    TaskHandle_t audio_input_task_handle;
    TaskHandle_t audio_output_task_handle;
    bool audio_processor_initialized;
    bool voice_detected;
    bool service_stopped;
} AudioService;

void AudioService_Init(AudioService* service);
void AudioService_Deinit(AudioService* service);
void AudioService_Initialize(AudioService* service, AudioCodec* codec);
void AudioService_Start(AudioService* service);
void AudioService_Stop(AudioService* service);
bool AudioService_IsVoiceDetected(const AudioService* service);
bool AudioService_IsIdle(AudioService* service);
bool AudioService_IsAudioProcessorRunning(const AudioService* service);
void AudioService_EnableVoiceProcessing(AudioService* service, bool enable);
void AudioService_EnableAudioTesting(AudioService* service, bool enable);
void AudioService_EnableDeviceAec(AudioService* service, bool enable);
void AudioService_SetCallbacks(AudioService* service, const AudioServiceCallbacks* callbacks);
void AudioService_AbortPlayback(AudioService* service);
void AudioService_ResetDecoder(AudioService* service);
void AudioService_SetModelsList(AudioService* service, srmodel_list_t* models_list);
void AudioService_ClearVadState(AudioService* service);

#endif
