#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVER_CLIENT_EVENT_PROTOCOL,
    SERVER_CLIENT_EVENT_AUDIO,
    SERVER_CLIENT_EVENT_DISCONNECTED,
    // Mirrors DISCONNECTED: posted from WEBSOCKET_EVENT_CONNECTED, once per
    // successful (re)connect, including automatic reconnects -- not just
    // the first one. main.c uses the DISCONNECTED/CONNECTED pair to track
    // "is the server currently reachable" independently of orchestrator's
    // own conversation state machine (which only cares about a single
    // transient disconnect flash, not the ongoing reachability).
    SERVER_CLIENT_EVENT_CONNECTED,
} server_client_event_type_t;

typedef struct {
    server_client_event_type_t type;
    protocol_event_t protocol_event;
    uint8_t *audio_data;
    size_t audio_len;
} server_client_event_t;

// Starts the WebSocket client (esp_websocket_client_start() under the hood,
// which only spawns the client's internal task and returns -- it does not
// block until connected). session_id is stored and used by
// server_client_send_hello() below.
esp_err_t server_client_init(const char *url, const char *session_id, QueueHandle_t event_queue);
// Sends the protocol `hello` message, matching the reference Python
// client's connect-then-hello sequencing (haro/src/haro/orchestrator.py).
// Call this in response to consuming a SERVER_CLIENT_EVENT_CONNECTED off
// the event queue (posted once per successful (re)connect, including
// automatic reconnects -- so this needs calling again on every one, not
// just the first) -- never unconditionally at boot or from any other
// context, since sending before a real connection exists is guaranteed to
// fail. Deliberately NOT sent automatically inside server_client.c's own
// WEBSOCKET_EVENT_CONNECTED handling anymore: that handler runs
// synchronously on the WebSocket client's own internal task, and a send
// stuck there (a silently black-holed connection, no RST) could freeze the
// whole client -- and with it, the connection, forever, with no way back
// short of a power cycle (found on real hardware; see server_client.c's
// HELLO_SEND_TIMEOUT_MS comment for the full mechanism). Calling this from
// main.c's orchestrator_task instead keeps it off that task entirely.
esp_err_t server_client_send_hello(void);
esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len);
esp_err_t server_client_send_end_of_speech(void);
// See protocol_encode_interrupt()'s comment for when to call this.
esp_err_t server_client_send_interrupt(void);
// See protocol_encode_camera_frame()'s comment. Sent as a text (not
// binary) WebSocket message -- protocol_encode_camera_frame() base64-
// encodes the JPEG into the JSON payload itself.
esp_err_t server_client_send_camera_frame(const uint8_t *jpeg, size_t jpeg_len);

// Milliseconds since the current connection's WEBSOCKET_EVENT_CONNECTED
// fired, or -1 if never connected yet (or already disconnected again --
// see server_client.c's WEBSOCKET_EVENT_DISCONNECTED case). Lets a
// caller wait out a grace period on a freshly (re)connected client
// before sending something -- see camera_face_track.c's
// CAMERA_SEND_CONNECTION_WARMUP_MS for why that matters for camera
// frames specifically.
int64_t server_client_ms_since_connected(void);

// Where incoming binary WebSocket data (the server's audio stream) goes.
// When set, audio bypasses the event queue entirely: each received
// fragment is handed to `sink` in order, straight from the WebSocket task
// -- no reassembly malloc and no SERVER_CLIENT_EVENT_AUDIO per chunk (see
// audio_player.h for why the queue path broke down). `sink` must not
// block. Set before server_client_init().
void server_client_set_audio_sink(void (*sink)(const uint8_t *data, size_t len));

#ifdef __cplusplus
}
#endif
