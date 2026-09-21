#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTOCOL_EVENT_EMOTION,
    PROTOCOL_EVENT_RESPONSE_END,
    PROTOCOL_EVENT_ERROR,
    PROTOCOL_EVENT_ACTION,
    // Reply to a camera_frame message once haro-server's face_tracking.py
    // has run on it -- see that module's docstring for why face detection
    // runs server-side rather than on this device. Feeds the SAME gaze/
    // servo system the (removed) on-device detection used to drive; see
    // camera_face_track.cpp's camera_face_track_set_remote_position().
    PROTOCOL_EVENT_FACE_POSITION,
} protocol_event_type_t;

typedef struct {
    protocol_event_type_t type;
    char value[32];
    char message[128];
    // Populated only for PROTOCOL_EVENT_ACTION -- a deterministic
    // server-side command (dice roll, coin flip, "music_playing" when a
    // Navidrome track starts, ...) matched on the transcript before any
    // LLM call, see haro-server's actions.py/session.py. action_result is
    // always a string: the server's "result" field can be a JSON number
    // (dice) or string (coin, music track label), so protocol.c formats
    // either into this one field rather than carrying a variant type here.
    // 64 bytes: comfortably fits "<title> - <artist>" for most tracks
    // without frequent truncation, not just dice/coin's few-character
    // results.
    char action_name[32];
    char action_result[64];
    // Populated only for PROTOCOL_EVENT_FACE_POSITION. face_found mirrors
    // the server's "found" field; face_dx/face_dy (normalized [-1,1], 0 =
    // frame center) are only meaningful when face_found is true.
    bool face_found;
    float face_dx;
    float face_dy;
} protocol_event_t;

char *protocol_encode_hello(const char *session_id);
char *protocol_encode_end_of_speech(void);
// Sent when the wake word interrupts a long-running, server-driven audio
// stream (currently: music playback, see orchestrator.c's
// HARO_STATE_PLAYING_MUSIC handling) that a normal short TTS reply doesn't
// need this for -- see haro-server's protocol.py InterruptMessage.
char *protocol_encode_interrupt(void);
// Base64-encodes `jpeg` (jpeg_len bytes) into a {"type":"camera_frame",
// "data":"<base64>"} message. Consumed server-side two ways: admin.py's
// /admin/camera live-preview page, and face_tracking.py's detection (see
// PROTOCOL_EVENT_FACE_POSITION above for the reply) -- this device does
// not run face detection itself. Returns NULL on allocation failure (same
// convention as the other encode_ functions); caller owns the returned
// string and must free() it.
char *protocol_encode_camera_frame(const uint8_t *jpeg, size_t jpeg_len);
esp_err_t protocol_parse_server_message(const char *text, protocol_event_t *out);

#ifdef __cplusplus
}
#endif
