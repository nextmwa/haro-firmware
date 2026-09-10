// server_client: esp_websocket_client wrapper mirroring
// haro/src/haro/server_client.py -- connects to the `server/` backend over
// a WebSocket, encodes/decodes protocol.h JSON control messages, and
// forwards TTS audio chunks and protocol events to `event_queue` for
// `orchestrator` (Task 8) to consume.
//
// Deviation check against the Task 6 brief's sketch, done by reading the
// resolved esp_websocket_client 1.8.0 headers under
// managed_components/espressif__esp_websocket_client/include/esp_websocket_client.h
// (see task-6-report.md for the full comparison):
//
//  1. WEBSOCKET_EVENT_ANY *is* real and behaves as the brief assumed:
//     esp_websocket_event_id_t defines `WEBSOCKET_EVENT_ANY = -1`
//     (esp_websocket_client.h:40), and esp_websocket_register_events()
//     (esp_websocket_client.c:1693) forwards the `event` argument verbatim
//     into esp_event_handler_register_with(client->event_handle,
//     WEBSOCKET_EVENTS, event, ...). esp_event_base.h:30 defines
//     `ESP_EVENT_ANY_ID -1`, the same numeric value, so passing
//     WEBSOCKET_EVENT_ANY there really does register one handler for every
//     websocket event id via the standard esp_event "any id" wildcard --
//     not a re-implementation, the genuine mechanism. No fallback to
//     per-event registration was needed.
//
//  2. esp_websocket_client_config_t's `.uri` and `.reconnect_timeout_ms`
//     fields, esp_websocket_event_data_t's `.op_code`/`.data_ptr`/`.data_len`
//     fields, and the send_text/send_bin signatures
//     (`int esp_websocket_client_send_{text,bin}(handle, const char *data,
//     int len, TickType_t timeout)`) all match the brief's sketch exactly.
//     Nothing else needed adjusting there.
//
//  3. The brief's sketch used the magic numbers 0x01/0x02 for
//     text/binary opcodes and referenced an undefined
//     `orchestrator_server_event_t` in the WEBSOCKET_EVENT_DISCONNECTED
//     branch (orchestrator doesn't exist until Task 8, and this component
//     doesn't depend on it). Below uses the symbolic
//     WS_TRANSPORT_OPCODES_TEXT/BINARY constants from
//     esp_transport_ws.h (pulled in transitively by esp_websocket_client.h)
//     and the correct server_client_event_t type for both queued event
//     kinds.
#include "server_client.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "server_client";
static esp_websocket_client_handle_t s_client;
static QueueHandle_t s_event_queue;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_DATA: {
        if (data->data_len <= 0) {
            break;
        }
        if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) { // binary: TTS audio chunk
            uint8_t *audio_data = malloc(data->data_len);
            if (audio_data == NULL) {
                ESP_LOGE(TAG, "OOM allocating %d-byte audio chunk", data->data_len);
                break;
            }
            memcpy(audio_data, data->data_ptr, data->data_len);
            server_client_event_t evt = {
                .type = SERVER_CLIENT_EVENT_AUDIO,
                .audio_data = audio_data,
                .audio_len = (size_t)data->data_len,
            };
            if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "event queue full, dropping audio chunk");
                free(audio_data);
            }
        } else if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) { // text: JSON control message
            char *text = malloc((size_t)data->data_len + 1);
            if (text == NULL) {
                ESP_LOGE(TAG, "OOM allocating %d-byte text message", data->data_len);
                break;
            }
            memcpy(text, data->data_ptr, data->data_len);
            text[data->data_len] = '\0';

            server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_PROTOCOL };
            if (protocol_parse_server_message(text, &evt.protocol_event) == ESP_OK) {
                if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "event queue full, dropping protocol event");
                }
            } else {
                ESP_LOGW(TAG, "dropping unparseable server message: %s", text);
            }
            free(text);
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED: {
        server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_DISCONNECTED };
        if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "event queue full, dropping disconnect event");
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t server_client_init(const char *url, QueueHandle_t event_queue)
{
    s_event_queue = event_queue;

    esp_websocket_client_config_t config = {
        .uri = url,
        .reconnect_timeout_ms = 1000,
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    return esp_websocket_client_start(s_client);
}

esp_err_t server_client_send_hello(const char *session_id)
{
    char *json = protocol_encode_hello(session_id);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), portMAX_DELAY);
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len)
{
    int sent = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len, portMAX_DELAY);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_end_of_speech(void)
{
    char *json = protocol_encode_end_of_speech();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), portMAX_DELAY);
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}
