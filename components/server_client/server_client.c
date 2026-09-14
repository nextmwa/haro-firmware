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

// Found on real hardware: TTS playback came out garbled/stuttering.
// esp_websocket_client's receive buffer defaults to 1024 bytes
// (WEBSOCKET_BUFFER_SIZE_BYTE in esp_websocket_client.c, not overridden by
// server_client_init()'s config below), but a single TTS audio chunk from
// the server (one Kokoro synthesis segment, tts.py's
// KokoroTtsEngine._synthesize_sentence) easily runs to tens of KB -- far
// past that. esp_websocket_client's own documented behavior for this case
// (see esp_websocket_client.h's payload_len/payload_offset doc comments:
// "payloads exceeding buffer will be posted through multiple events") is
// to fire WEBSOCKET_EVENT_DATA once per ~1024-byte fragment of the SAME
// logical message. The code below used to treat every one of those
// fragments as its own complete, independent audio chunk -- shredding
// every TTS reply into dozens of tiny out-of-context pieces (and,
// separately, overflowing s_event_queue's depth of 8 in the process, since
// what should have been a handful of real chunks became a hundred-plus
// fragments). Reassemble fragments into one buffer per logical message
// using payload_offset/payload_len instead, and only enqueue once a
// message is complete.
static uint8_t *s_audio_reassembly_buf;
static size_t s_audio_reassembly_len;   // bytes written so far
static size_t s_audio_reassembly_total; // expected total, from payload_len

// Set once by server_client_init() and read only from
// websocket_event_handler() on WEBSOCKET_EVENT_CONNECTED -- the event
// handler has no natural way to receive it as a parameter (it's an
// esp_event callback with a fixed signature), so it's stashed here instead.
#define MAX_SESSION_ID_LEN 64
static char s_session_id[MAX_SESSION_ID_LEN];

// Sends the protocol `hello` message. Only ever called from
// websocket_event_handler() below, in response to a real
// WEBSOCKET_EVENT_CONNECTED -- esp_websocket_client_send_text() (via
// esp_websocket_client_send_with_exact_opcode()) checks
// esp_websocket_client_is_connected() first and fails immediately if not
// connected yet, so this must never be called unconditionally at boot
// (that was Finding 2 of the final review: server_client_send_hello() used
// to be called synchronously right after server_client_init(), before
// esp_websocket_client_start()'s internally-spawned task could possibly
// have connected).
static esp_err_t send_hello(void)
{
    char *json = protocol_encode_hello(s_session_id);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), portMAX_DELAY);
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

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
        if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) { // binary: TTS audio chunk (possibly fragmented -- see comment above)
            if (data->payload_offset == 0) {
                // Start of a new logical message. Free any previous
                // reassembly buffer first -- normally NULL already (the
                // completed branch below clears it), but a message that
                // never reached its declared payload_len (e.g. a dropped
                // fragment) would otherwise leak it here.
                free(s_audio_reassembly_buf);
                s_audio_reassembly_buf = NULL;
                s_audio_reassembly_total = (size_t)data->payload_len;
                s_audio_reassembly_len = 0;
                if (s_audio_reassembly_total > 0) {
                    s_audio_reassembly_buf = malloc(s_audio_reassembly_total);
                    if (s_audio_reassembly_buf == NULL) {
                        ESP_LOGE(TAG, "OOM allocating %u-byte audio reassembly buffer",
                                 (unsigned)s_audio_reassembly_total);
                    }
                }
            }
            if (s_audio_reassembly_buf != NULL &&
                (size_t)data->payload_offset == s_audio_reassembly_len &&
                s_audio_reassembly_len + (size_t)data->data_len <= s_audio_reassembly_total) {
                memcpy(s_audio_reassembly_buf + s_audio_reassembly_len, data->data_ptr, (size_t)data->data_len);
                s_audio_reassembly_len += (size_t)data->data_len;
            }
            if (s_audio_reassembly_buf != NULL && s_audio_reassembly_len >= s_audio_reassembly_total) {
                server_client_event_t evt = {
                    .type = SERVER_CLIENT_EVENT_AUDIO,
                    .audio_data = s_audio_reassembly_buf,
                    .audio_len = s_audio_reassembly_len,
                };
                if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "event queue full, dropping audio chunk");
                    free(s_audio_reassembly_buf);
                }
                s_audio_reassembly_buf = NULL; // ownership transferred to the queue (or freed just above)
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
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WebSocket connected, sending hello (session=%s)", s_session_id);
        esp_err_t err = send_hello();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to send hello: %s", esp_err_to_name(err));
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

esp_err_t server_client_init(const char *url, const char *session_id, QueueHandle_t event_queue)
{
    s_event_queue = event_queue;

    if (session_id != NULL) {
        strncpy(s_session_id, session_id, sizeof(s_session_id) - 1);
        s_session_id[sizeof(s_session_id) - 1] = '\0';
    } else {
        s_session_id[0] = '\0';
    }

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
