#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HARO_STATE_IDLE, HARO_STATE_LISTENING, HARO_STATE_THINKING, HARO_STATE_SPEAKING } haro_state_t;

typedef enum {
    ORCHESTRATOR_SERVER_EVENT_PROTOCOL,
    ORCHESTRATOR_SERVER_EVENT_AUDIO,
    ORCHESTRATOR_SERVER_EVENT_DISCONNECTED,
} orchestrator_server_event_type_t;

typedef struct {
    orchestrator_server_event_type_t type;
    protocol_event_t protocol_event;
    const uint8_t *audio_data;
    size_t audio_len;
} orchestrator_server_event_t;  // mirrors server_client_event_t (Task 6) field-for-field; main.c translates

typedef struct {
    esp_err_t (*send_audio_frame)(void *ctx, const uint8_t *data, size_t len);
    esp_err_t (*send_end_of_speech)(void *ctx);
    void *ctx;
} orchestrator_server_ops_t;

typedef struct {
    void (*play_chunk)(void *ctx, const uint8_t *data, size_t len);
    void (*stop)(void *ctx);
    void *ctx;
} orchestrator_audio_out_ops_t;

typedef struct {
    void (*show)(void *ctx, int expression);  // expression values match face_display.h's enum, Task 8
    void *ctx;
} orchestrator_face_ops_t;

typedef struct {
    orchestrator_server_ops_t server;
    orchestrator_audio_out_ops_t audio_out;
    orchestrator_face_ops_t face;
} orchestrator_ops_t;

void orchestrator_init(orchestrator_ops_t ops);
haro_state_t orchestrator_get_state(void);
void orchestrator_on_wake_word(void);
void orchestrator_on_audio_frame(const uint8_t *frame, size_t len, bool is_end_of_speech);
void orchestrator_on_server_event(orchestrator_server_event_t event);

#ifdef __cplusplus
}
#endif
