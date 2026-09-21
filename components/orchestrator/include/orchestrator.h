#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// HARO_STATE_PLAYING_MUSIC is distinct from HARO_STATE_SPEAKING: a TTS
// reply is a short, bounded stream the robot always finishes; music can run
// for minutes and must be interruptible by a fresh wake word (see
// orchestrator_on_wake_word()'s handling of this state) without waiting
// for it to end naturally the way SPEAKING does.
typedef enum {
    HARO_STATE_IDLE,
    HARO_STATE_LISTENING,
    HARO_STATE_THINKING,
    HARO_STATE_SPEAKING,
    HARO_STATE_PLAYING_MUSIC,
} haro_state_t;

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
    // Called when the wake word interrupts HARO_STATE_PLAYING_MUSIC (see
    // orchestrator_on_wake_word()) -- tells the server to stop streaming
    // more of the track. Optional: NULL is safe if music playback is never
    // wired up, matching show_action()'s existing optional-callback
    // convention below.
    esp_err_t (*send_interrupt)(void *ctx);
    void *ctx;
} orchestrator_server_ops_t;

typedef struct {
    void (*play_chunk)(void *ctx, const uint8_t *data, size_t len);
    void (*stop)(void *ctx);
    void *ctx;
} orchestrator_audio_out_ops_t;

typedef struct {
    void (*show)(void *ctx, int expression);  // expression values match face_display.h's enum, Task 8
    // Renders a deterministic action result (dice roll, coin flip, ...)
    // matching face_display_show_action()'s (name, result) contract --
    // action_name/action_result are protocol_event_t's fields verbatim.
    // Optional: NULL is safe to leave unset if a build has no display.
    void (*show_action)(void *ctx, const char *action_name, const char *action_result);
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
// Safety-net recovery for HARO_STATE_THINKING/HARO_STATE_SPEAKING, which
// (unlike LISTENING's MAX_LISTEN_MS in main.c, or PLAYING_MUSIC's wake-word
// escape in orchestrator_on_wake_word()) have no other way out once the
// server event that would normally end them (response_end, error, or
// disconnected) is dropped -- a full server_client_event_t queue silently
// drops exactly those events (see server_client.c's "event queue full"
// logs). main.c calls this after a generous timeout with no progress; a
// no-op if already idle (same idle-audio-pop guard as the DISCONNECTED
// case in orchestrator_on_server_event() below).
void orchestrator_force_idle(void);

#ifdef __cplusplus
}
#endif
