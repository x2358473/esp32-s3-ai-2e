#include "audio_debugger.h"
#include "sdkconfig.h"

#if CONFIG_USE_AUDIO_DEBUGGER
#include <esp_log.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#endif

#define TAG "AudioDebugger"

void AudioDebugger_Init(AudioDebugger* debugger) {
#if CONFIG_USE_AUDIO_DEBUGGER
    const char* server_addr = CONFIG_AUDIO_DEBUG_UDP_SERVER;
    const char* colon = NULL;
    if (debugger == NULL) return;
    memset(debugger, 0, sizeof(AudioDebugger));
    debugger->udp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (debugger->udp_sockfd < 0) {
        ESP_LOGW(TAG, "Failed to create UDP socket: %d", errno);
        return;
    }
    colon = strchr(server_addr, ':');
    if (colon == NULL) {
        ESP_LOGW(TAG, "Invalid server address: %s, should be IP:PORT", server_addr);
        close(debugger->udp_sockfd);
        debugger->udp_sockfd = -1;
        return;
    }
    memset(&debugger->udp_server_addr, 0, sizeof(debugger->udp_server_addr));
    debugger->udp_server_addr.sin_family = AF_INET;
    debugger->udp_server_addr.sin_port = htons((uint16_t)atoi(colon + 1));
    {
        size_t ip_len = (size_t)(colon - server_addr);
        char ip_buf[64];
        if (ip_len >= sizeof(ip_buf)) ip_len = sizeof(ip_buf) - 1;
        memcpy(ip_buf, server_addr, ip_len);
        ip_buf[ip_len] = '\0';
        inet_pton(AF_INET, ip_buf, &debugger->udp_server_addr.sin_addr);
    }
#else
    (void)debugger;
#endif
}

void AudioDebugger_Deinit(AudioDebugger* debugger) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (debugger != NULL && debugger->udp_sockfd >= 0) {
        close(debugger->udp_sockfd);
        debugger->udp_sockfd = -1;
    }
#else
    (void)debugger;
#endif
}

void AudioDebugger_Feed(AudioDebugger* debugger, const int16_t* data, size_t samples) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (debugger == NULL || debugger->udp_sockfd < 0 || data == NULL || samples == 0) {
        return;
    }
    sendto(debugger->udp_sockfd, data, samples * sizeof(int16_t), 0, (struct sockaddr*)&debugger->udp_server_addr, sizeof(debugger->udp_server_addr));
#else
    (void)debugger;
    (void)data;
    (void)samples;
#endif
}
