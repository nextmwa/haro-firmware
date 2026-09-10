#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVER_CLIENT_EVENT_PROTOCOL,
    SERVER_CLIENT_EVENT_AUDIO,
    SERVER_CLIENT_EVENT_DISCONNECTED,
} server_client_event_type_t;

typedef struct {
    server_client_event_type_t type;
    protocol_event_t protocol_event;
    uint8_t *audio_data;
    size_t audio_len;
} server_client_event_t;

// Starts the WebSocket client (esp_websocket_client_start() under the hood,
// which only spawns the client's internal task and returns -- it does not
// block until connected). session_id is stored and used to send the
// protocol `hello` message automatically once the connection actually
// completes (WEBSOCKET_EVENT_CONNECTED, handled internally in
// server_client.c), matching the reference Python client's
// connect-then-hello sequencing (haro/src/haro/orchestrator.py). This also
// means hello is (re)sent on every automatic reconnect, not just the first
// connect. There is deliberately no public "send hello" entry point:
// sending it before a real connection exists is guaranteed to fail (see
// server_client.c's WEBSOCKET_EVENT_CONNECTED handler for the only call
// site), so callers should not need one.
esp_err_t server_client_init(const char *url, const char *session_id, QueueHandle_t event_queue);
esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len);
esp_err_t server_client_send_end_of_speech(void);

#ifdef __cplusplus
}
#endif
