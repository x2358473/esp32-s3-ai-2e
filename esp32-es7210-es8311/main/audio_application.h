#ifndef AUDIO_APPLICATION_H_
#define AUDIO_APPLICATION_H_

#include <stdbool.h>
#include "audio/audio_service.h"

typedef struct {
    bool audio_testing_active;
    AudioService audio_service;
} Application;

Application* Application_GetInstance(void);
void Application_Run(Application* app);
bool Application_IsAudioTesting(const Application* app);
void Application_ToggleAudioLocalTesting(Application* app);
void Application_StopAudioLocalTesting(Application* app);
bool Application_IsVoiceDetected(const Application* app);
AudioService* Application_GetAudioService(Application* app);

#endif
