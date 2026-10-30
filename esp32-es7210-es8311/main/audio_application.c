#include "audio_application.h"
#include "audio/audio_codec.h"

static Application s_app;
static bool s_initialized = false;

static void application_sync_voice_processing(Application* app) {
    if (app->audio_testing_active) {
        return;
    }
    AudioService_EnableVoiceProcessing(&app->audio_service, false);
}

Application* Application_GetInstance(void) {
    if (!s_initialized) {
        AudioService_Init(&s_app.audio_service);
        s_initialized = true;
    }
    return &s_app;
}

void Application_Run(Application* app) {
    AudioServiceCallbacks callbacks = {0};
    AudioService_Initialize(&app->audio_service, AudioCodec_GetInstance());
    AudioService_Start(&app->audio_service);
    AudioService_SetCallbacks(&app->audio_service, &callbacks);
    app->audio_testing_active = false;
    application_sync_voice_processing(app);
}

bool Application_IsAudioTesting(const Application* app) {
    return app->audio_testing_active;
}

void Application_ToggleAudioLocalTesting(Application* app) {
    if (!app->audio_testing_active) {
        AudioService_EnableAudioTesting(&app->audio_service, true);
        app->audio_testing_active = true;
        return;
    }
    Application_StopAudioLocalTesting(app);
}

void Application_StopAudioLocalTesting(Application* app) {
    if (!app->audio_testing_active) {
        application_sync_voice_processing(app);
        return;
    }
    AudioService_EnableAudioTesting(&app->audio_service, false);
    app->audio_testing_active = false;
    application_sync_voice_processing(app);
}

bool Application_IsVoiceDetected(const Application* app) {
    return AudioService_IsVoiceDetected(&app->audio_service);
}

AudioService* Application_GetAudioService(Application* app) {
    return &app->audio_service;
}
