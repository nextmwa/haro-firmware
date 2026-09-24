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
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "server_client";
static esp_websocket_client_handle_t s_client;
static QueueHandle_t s_event_queue;

// Bounded, not portMAX_DELAY, on the three orchestrator_task-bound sends
// below (audio frame, end-of-speech, interrupt): confirmed on real
// hardware that a degraded WiFi link (weak signal, congested channel) can
// make esp_websocket_client block on a send far longer than any caller
// should ever wait -- and since orchestrator_task's whole loop (mic
// forwarding, server event draining, display/servo updates, ...) is one
// single-threaded while(true), a portMAX_DELAY block on any one of these
// doesn't just delay that one message, it freezes the entire device until
// the network recovers (observed: 30+ seconds with zero recovery, both
// server_client.c's own event queue and wake_word's audio frame queue
// overflowing the whole time because nothing was draining either -- the
// device needed a physical reset to come back).
//
// NOT "one dropped/failed message during a rough network patch", though,
// contrary to what an earlier version of this comment claimed: a timed-out
// send here aborts the WHOLE connection (see esp_websocket_client_abort_
// connection()'s mention in HELLO_SEND_TIMEOUT_MS's comment below for the
// exact mechanism), so too tight a bound trades the
// device-freeze risk for "drops mid-conversation on any network slower than
// this value". 200ms (the original value) was too tight for a real
// corporate network encountered in use -- raised to 1000ms as a middle
// ground: still tighter than HELLO_SEND_TIMEOUT_MS/CAMERA_FRAME_SEND_
// TIMEOUT_MS below despite server_client_send_hello() now ALSO running on
// orchestrator_task (see that constant's own comment for why it's still
// deliberately looser than this one) -- these three fire on every single
// conversational turn, so any added latency here is felt constantly, unlike
// hello's rare, once-per-(re)connect timing. 5x more slack against the same
// kind of latency that made hello/camera-frame sends fail below. main.c's
// audio-frame drain loop also now caps how many sends it can stack in one
// iteration (AUDIO_FRAME_DRAIN_BUDGET_MS), so a run of slow sends here can
// no longer compound into a multi-second single-iteration stall the way it
// could when this bound was raised without that change too.
// Raised again, 1000 -> 3000, after real-hardware testing on a different
// network (this project's own development WiFi) hit the exact same
// pattern the 200->1000 raise above already documents: audio-frame sends
// during LISTENING failing and tearing down the connection mid-turn
// (confirmed via added logging -- server_client.c's WEBSOCKET_EVENT_
// DISCONNECTED case and orchestrator.c's DISCONNECTED handler were both
// completely silent before that, so this failure mode was invisible on
// the serial console despite visibly happening: a red EXPR_ERROR flash
// on every wake word). Ruled out first: WiFi modem-sleep power save
// (esp_wifi_set_ps(WIFI_PS_NONE), no change), TCP keepalive
// (server_client_init()'s keep_alive_* fields, no change), and Docker
// Desktop's port-forwarding proxy (network_mode: host in the server's
// docker-compose.yml, no change) -- none of those affected this specific
// failure, pointing at the WiFi link itself needing more time per send
// rather than a connection-freshness or transport-layer cause. Same
// device-freeze risk tradeoff as the 200->1000 raise: a genuinely dead
// connection now takes longer to detect via this path, but a real dead
// connection is still caught by esp_websocket_client's own PING/PONG and
// keepalive mechanisms independent of this value.
#define SEND_TIMEOUT_MS 3000

// server_client_send_hello() and server_client_send_camera_frame() below
// are the two exceptions to SEND_TIMEOUT_MS -- but NOT the same bound as
// each other (see CAMERA_FRAME_SEND_TIMEOUT_MS below for why they diverged
// after starting out identical). Found on real hardware (a corporate WiFi
// network with more latency than the network SEND_TIMEOUT_MS was tuned
// against): a failed send here isn't a harmless skip of that one message
// -- esp_websocket_client_send_with_exact_opcode() calls
// esp_websocket_client_abort_connection() on ANY write that doesn't
// complete in time ("Calling abort_connection due to send error", in
// esp_websocket_client.c), tearing down the WHOLE connection over a single
// slow write.
//
// HELLO_SEND_TIMEOUT_MS went through unbounded (portMAX_DELAY) before this
// value -- found, root-caused on real hardware, to be a MUCH worse bug than
// the one it was trying to fix. Sending hello used to happen synchronously
// inside websocket_event_handler()'s WEBSOCKET_EVENT_CONNECTED case, which
// esp_websocket_client.c calls via esp_websocket_client_dispatch_event() ->
// esp_event_loop_run() -- run SYNCHRONOUSLY, on the WebSocket client's own
// internal task, while that same task is still holding client->lock (see
// esp_websocket_client.c's WEBSOCKET_STATE_INIT case, which doesn't release
// client->lock until after this whole dispatch returns). A portMAX_DELAY
// send stuck there (a silently black-holed connection with no RST, exactly
// the kind of thing a corporate WiFi network produces more often than a
// clean home one -- confirmed as the actual failure mode in the field) then
// blocks that task FOREVER, still holding client->lock -- which stops the
// WHOLE esp_websocket_client state machine dead: no more PING/PONG, no more
// disconnect detection, no more automatic reconnect, because all of that
// logic runs on the very same now-frozen task. The device is left
// "connected" forever from the library's own point of view while actually
// dead on the wire, recoverable only by a physical power cycle -- this is
// what "loses the connection and never reconnects" (as opposed to the
// flaky-but-recovering disconnect/reconnect cycling this file's other
// comments describe) turned out to actually be.
//
// Fixed by moving the hello send off the WebSocket client's own internal
// task entirely: websocket_event_handler()'s WEBSOCKET_EVENT_CONNECTED case
// below now only enqueues SERVER_CLIENT_EVENT_CONNECTED (as it already did)
// -- main.c's orchestrator_task calls the now-public
// server_client_send_hello() in response to consuming that event, the same
// task its three other calls into this file (audio frame, end-of-speech,
// interrupt, all SEND_TIMEOUT_MS-bound) already run on. With the send now
// off the client's own internal task, there's no lockup-the-whole-library
// risk left to justify staying unbounded -- bounded the same way
// CAMERA_FRAME_SEND_TIMEOUT_MS is, for the same reason: generous enough for
// a real slow network (hello is tiny -- one short JSON message), but
// bounded so a stuck send can no longer freeze anything indefinitely.
// Deliberately NOT as tight as SEND_TIMEOUT_MS despite now sharing
// orchestrator_task with those three sends: hello only fires once per
// (re)connect, not on every conversational turn, so an occasional multi-
// second stall right after reconnecting (worst case: this whole value,
// delaying orchestrator_task's wake-word/audio-draining/display work for
// that long) is a far smaller practical cost than the same stall would be
// on the three per-turn sends -- and after HELLO_SEND_TIMEOUT_MS elapses,
// esp_websocket_client_abort_connection() runs and reconnection proceeds
// normally regardless, the same recoverable failure mode SEND_TIMEOUT_MS's
// own comment already describes.
#define HELLO_SEND_TIMEOUT_MS 8000

// server_client_send_camera_frame() started out sharing hello's (then-)
// unbounded wait too -- reverted after that choice
// caused a DIFFERENT real disconnect, root-caused by reading uvicorn's
// actual Config defaults (ws_ping_interval=20.0, ws_ping_timeout=20.0,
// neither overridden anywhere in haro-server) against this library's own
// source: unlike hello, this send repeats every camera_face_track.c poll
// cycle (FACE_TRACK_POLL_MS), so an unbounded wait here can hold
// client->tx_lock (CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y, see
// sdkconfig.defaults' comment on that option) for long enough that the
// SAME lock's PING/PONG handling ("Could not lock ws-client within 2000
// timeout for PONG", confirmed on real hardware) can't run either -- the
// server never gets a PONG back, and closes the "unresponsive" connection
// at its own fixed 20+20=40s mark regardless of anything this file's own
// SEND_TIMEOUT_MS-family constants control. A camera-frame write that's
// still ongoing is not actually a problem (server_client_send_camera_
// frame() is expected to sometimes fail on this network, per its own
// comment on FACE_TRACK_POLL_MS's cadence below) -- what matters is that
// the write releases the lock again well inside the 20s ping window
// instead of monopolizing it, bounded so it can't stack up worse than a
// single frame at a time. 8000ms: long enough to succeed more often than
// the 5000ms that was tried and found insufficient before going
// unbounded, short enough to leave real room in a 20s ping cycle for
// PING/PONG traffic to get a turn even if one camera-frame write uses the
// whole budget.
#define CAMERA_FRAME_SEND_TIMEOUT_MS 8000

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

// -1 = never connected (or currently disconnected) -- see
// server_client_ms_since_connected()'s doc comment in the header for why
// this exists (camera_face_track.c's connection-warmup grace period).
static int64_t s_connected_at_us = -1;

int64_t server_client_ms_since_connected(void)
{
    if (s_connected_at_us < 0) {
        return -1;
    }
    return (esp_timer_get_time() - s_connected_at_us) / 1000;
}

// Sends the protocol `hello` message. Public (not static): called from
// main.c's orchestrator_task, in response to consuming a
// SERVER_CLIENT_EVENT_CONNECTED off the event queue -- deliberately NOT
// called synchronously from websocket_event_handler()'s own
// WEBSOCKET_EVENT_CONNECTED case anymore (see HELLO_SEND_TIMEOUT_MS's
// comment above for the real-hardware bug that caused: calling it there ran
// this send on the WebSocket client's own internal task while it held
// client->lock, and a stuck send could freeze the whole client, and hence
// the connection, forever). esp_websocket_client_send_text() (via
// esp_websocket_client_send_with_exact_opcode()) checks
// esp_websocket_client_is_connected() first and fails immediately if not
// connected yet, so this must never be called unconditionally at boot (that
// was Finding 2 of an earlier whole-codebase review: server_client_send_
// hello() used to be called synchronously right after server_client_init(),
// before esp_websocket_client_start()'s internally-spawned task could
// possibly have connected) -- calling it only in response to a real,
// already-observed SERVER_CLIENT_EVENT_CONNECTED (posted only from a real
// WEBSOCKET_EVENT_CONNECTED, see below) keeps that guarantee.
esp_err_t server_client_send_hello(void)
{
    char *json = protocol_encode_hello(s_session_id);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(HELLO_SEND_TIMEOUT_MS));
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
        // Deliberately NOT calling server_client_send_hello() here anymore
        // -- see HELLO_SEND_TIMEOUT_MS's comment for why running that send
        // on THIS task (the WebSocket client's own internal task, which
        // this handler runs on synchronously) could freeze the whole
        // connection forever. main.c's orchestrator_task sends hello
        // instead, once it consumes the SERVER_CLIENT_EVENT_CONNECTED
        // posted just below. On the rare case this event is itself dropped
        // (queue full, same "event queue full" log as every other event
        // this file posts), hello is simply skipped for that connection --
        // a real, minor tradeoff for no longer risking a permanent freeze;
        // the next successful (re)connect posts CONNECTED again and gets
        // another chance.
        ESP_LOGI(TAG, "WebSocket connected (session=%s)", s_session_id);
        s_connected_at_us = esp_timer_get_time();
        server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_CONNECTED };
        if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "event queue full, dropping connected event");
        }
        break;
    }
    case WEBSOCKET_EVENT_CLOSED:
        // A clean close handshake (server shut down/restarted) arrives as
        // CLOSED, not DISCONNECTED -- see enable_close_reconnect in
        // server_client_init(). Same consequences for the rest of Haro.
        ESP_LOGW(TAG, "WebSocket closed by server, reconnecting");
        // fallthrough
    case WEBSOCKET_EVENT_DISCONNECTED: {
        // Previously silent -- this case had no log line at all, so a
        // fast disconnect/reconnect cycle (the underlying esp_websocket_
        // client reconnecting before the next thing anyone looks at) was
        // completely invisible on the serial console even though its
        // side effect (orchestrator.c's ORCHESTRATOR_SERVER_EVENT_
        // DISCONNECTED case flashing EXPR_ERROR) was visibly happening on
        // real hardware -- found while chasing exactly that on real
        // hardware.
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_connected_at_us = -1;
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
        // Total retry cycle = network_timeout_ms (how long one connect
        // attempt waits before giving up) + reconnect_timeout_ms (pause
        // before the next attempt) -- leaving network_timeout_ms unset
        // defaults to 10000ms on its own, so reconnect_timeout_ms=1000
        // alone gives an ~11s cycle, not the requested "one attempt every
        // 10s". (Briefly suspected this setting of causing the connect-
        // then-immediately-drop bug below -- ruled out by reverting it on
        // real hardware and seeing the exact same failure; the real cause
        // was HELLO_SEND_TIMEOUT_MS's/CAMERA_FRAME_SEND_TIMEOUT_MS's,
        // unrelated to this config.)
        .network_timeout_ms = 9000,
        .reconnect_timeout_ms = 1000,
        // TCP-level keepalive, off by default (keep_alive_enable defaults
        // to false). Found on real hardware: a connection can go dead
        // (NAT/port-forward path drops it, no FIN/RST reaches either
        // side -- confirmed happening through Docker Desktop's own
        // virtualized networking during this project's development,
        // separately from the corporate-network case this file's other
        // comments already document) while Haro sits idle waiting for a
        // wake word, and this is only discovered on the NEXT real write
        // -- the wake-word-triggered hello/interrupt send -- producing a
        // visible red-LED "unreachable" blip exactly when the user
        // speaks to it, before the automatic reconnect (already working
        // correctly) catches up a few seconds later. Enabling TCP
        // keepalive makes the OS itself probe the idle connection and
        // detect a dead peer proactively, during the idle wait, not at
        // the moment of the user's own turn. 5s idle before the first
        // probe, a probe every 5s after that, dead after 3 unanswered
        // probes -- so a truly dead connection is caught and reconnect
        // begun within ~15-20s of going idle, comfortably before most
        // wake-word gaps, without probing so aggressively it wastes
        // power/airtime on a healthy connection.
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        // Found on real hardware (2026-09-24): after a CLEAN server
        // shutdown (uvicorn stopping/restarting sends a WebSocket CLOSE
        // frame) esp_websocket_client, by default, ends its task for good
        // -- `client->run = false` in esp_websocket_client.c's CLOSING
        // branch, only WEBSOCKET_EVENT_CLOSED/FINISH are dispatched, no
        // reconnect ever. Haro then sat silently "connected" to nothing
        // until power-cycled, every time the server was restarted
        // (abrupt drops were fine: they take the auto-reconnect path).
        // This flag makes a clean close reconnect like any other drop.
        .enable_close_reconnect = true,
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
    int sent = esp_websocket_client_send_bin(s_client, (const char *)data, (int)len, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_end_of_speech(void)
{
    char *json = protocol_encode_end_of_speech();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_interrupt(void)
{
    char *json = protocol_encode_interrupt();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t server_client_send_camera_frame(const uint8_t *jpeg, size_t jpeg_len)
{
    char *json = protocol_encode_camera_frame(jpeg, jpeg_len);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // esp_websocket_client_send_text() fragments large payloads into
    // multiple WebSocket frames internally -- a base64-encoded JPEG
    // (several KB, far bigger than any other message this file sends)
    // needs a longer bound than SEND_TIMEOUT_MS, similar in spirit to
    // server_client_send_hello()'s HELLO_SEND_TIMEOUT_MS -- but NOT the
    // same unbounded wait that constant briefly went through: see
    // CAMERA_FRAME_SEND_TIMEOUT_MS's comment for why going unbounded here
    // specifically caused a different real disconnect (this send repeats
    // every poll cycle, unlike hello's once-per-connection). Bounded, this
    // call is still expected to sometimes fail outright on a slow network
    // -- camera_face_track.c sends a fresh frame every FACE_TRACK_POLL_MS
    // anyway, so losing one is a non-issue, same as always. That file's
    // own face_track_task calls this, not orchestrator_task (unlike hello,
    // which now runs on orchestrator_task -- see server_client_send_
    // hello()'s own comment for why moving it there, off THIS function's
    // task, was the actual fix for a real connection-freeze bug).
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(CAMERA_FRAME_SEND_TIMEOUT_MS));
    free(json);
    return sent >= 0 ? ESP_OK : ESP_FAIL;
}
