#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <stdint.h>
#include <stddef.h>

#include <sys/socket.h>
#include <netinet/in.h>

typedef struct {
    int udp_sockfd;
    struct sockaddr_in udp_server_addr;
} AudioDebugger;

void AudioDebugger_Init(AudioDebugger* debugger);
void AudioDebugger_Deinit(AudioDebugger* debugger);
void AudioDebugger_Feed(AudioDebugger* debugger, const int16_t* data, size_t samples);

#endif 