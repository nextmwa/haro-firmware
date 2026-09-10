#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_LINUX
#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
#else
#include "haro_config.h"
#include "haro_wifi_provisioning.h"
#include "audio_pipeline.h"
#include "wake_word.h"
#include "server_client.h"
#include "face_display.h"
#include "orchestrator.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "haro";
static QueueHandle_t s_wake_queue;
static QueueHandle_t s_server_queue;
static QueueHandle_t s_audio_frame_queue;

// Mirrors the Python orchestrator's "a send failure during LISTENING
// triggers a reconnect" behavior (flagged in Task 7's review): on a failed
// send, enqueue a synthetic SERVER_CLIENT_EVENT_DISCONNECTED so it's
// processed the same way a real websocket drop is -- through
// orchestrator_task's normal server_evt dequeue, on its own next loop
// iteration.
//
// Deliberately NOT calling orchestrator_on_server_event() synchronously
// here: send_audio_frame/send_end_of_speech are invoked by orchestrator
// itself, from inside orchestrator_on_audio_frame(), which is still
// executing higher up the call stack at this point (mid-LISTENING). A
// synchronous re-entrant call here would run orchestrator's
// DISCONNECTED-handling path (return_to_idle(), which sets state back to
// HARO_STATE_IDLE) *while* orchestrator_on_audio_frame() is still on the
// stack -- and if is_end_of_speech was true, the line right after this
// callback returns (`s_state = HARO_STATE_THINKING;`) would silently
// clobber that IDLE state, undoing the recovery this is meant to trigger.
// Queuing avoids the re-entrancy entirely.
static void enqueue_synthetic_disconnect(void)
{
    server_client_event_t evt = { .type = SERVER_CLIENT_EVENT_DISCONNECTED };
    if (xQueueSend(s_server_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "server_queue full, dropped synthetic DISCONNECTED event");
    }
}

static esp_err_t server_send_audio_frame(void *ctx, const uint8_t *data, size_t len)
{
    esp_err_t err = server_client_send_audio_frame(data, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send_audio_frame failed: %s", esp_err_to_name(err));
        enqueue_synthetic_disconnect();
    }
    return err;
}

static esp_err_t server_send_end_of_speech(void *ctx)
{
    esp_err_t err = server_client_send_end_of_speech();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send_end_of_speech failed: %s", esp_err_to_name(err));
        enqueue_synthetic_disconnect();
    }
    return err;
}

static void audio_play_chunk(void *ctx, const uint8_t *data, size_t len)
{
    audio_pipeline_write(data, len);
}

static void audio_stop(void *ctx)
{
    // audio_pipeline has no explicit stop/flush primitive (Task 4's header
    // is write-only, matching esp_codec_dev's blocking-write model with no
    // separate "abort in-flight playback" call) -- nothing to do here.
}

static void face_show(void *ctx, int expression)
{
    face_display_show((face_expression_t)expression);
}

// orchestrator is deliberately decoupled from server_client (Task 7's
// ruling: keeps it buildable on the Linux host target without pulling in
// esp_websocket_client) -- translate at this one call site. Field-for-field
// mirror confirmed against both current headers (server_client.h,
// orchestrator.h): same three-case event-type enum ordering, same
// protocol_event_t payload, same audio_data/audio_len pair (server_client's
// audio_data is non-const `uint8_t *`, orchestrator's is `const uint8_t *` --
// a widening/qualifying conversion, not a mismatch).
static orchestrator_server_event_t to_orchestrator_event(const server_client_event_t *src)
{
    orchestrator_server_event_t dst = { .protocol_event = src->protocol_event };
    switch (src->type) {
    case SERVER_CLIENT_EVENT_PROTOCOL:     dst.type = ORCHESTRATOR_SERVER_EVENT_PROTOCOL; break;
    case SERVER_CLIENT_EVENT_AUDIO:        dst.type = ORCHESTRATOR_SERVER_EVENT_AUDIO; break;
    case SERVER_CLIENT_EVENT_DISCONNECTED: dst.type = ORCHESTRATOR_SERVER_EVENT_DISCONNECTED; break;
    }
    dst.audio_data = src->audio_data;
    dst.audio_len = src->audio_len;
    return dst;
}

static void orchestrator_task(void *arg)
{
    // Set when wake_word posts WAKE_WORD_SPEECH_END (see wake_word.c's
    // detect_task), consumed and cleared the next time an audio frame is
    // forwarded to orchestrator_on_audio_frame() below. wake_word's
    // detect_task and feed_task track the live mic stream independently in
    // real time, so exact same-chunk alignment between "AFE observed
    // silence" and "the next frame drained from s_audio_frame_queue" isn't
    // guaranteed, but both track the live stream, so the flag reflects
    // "speech just ended" within roughly one frame's latency either way.
    bool pending_speech_end = false;

    while (true) {
        wake_word_event_type_t wake_evt;
        while (xQueueReceive(s_wake_queue, &wake_evt, 0) == pdTRUE) {
            if (wake_evt == WAKE_WORD_DETECTED) {
                orchestrator_on_wake_word();
                pending_speech_end = false;
            } else if (wake_evt == WAKE_WORD_SPEECH_END) {
                pending_speech_end = true;
            }
        }

        // wake_word.c's feed_task is the sole caller of audio_pipeline_read()
        // in the whole component graph (see wake_word.c file header comment
        // item 6 -- i2s_channel_read() is single-consumer; two independent
        // readers would silently split the one physical mic stream instead
        // of each seeing the complete thing). Arm/disarm its forwarding of
        // raw frames onto s_audio_frame_queue to match whether we currently
        // want to forward mic audio to the server, rather than reading the
        // mic here ourselves.
        bool listening = (orchestrator_get_state() == HARO_STATE_LISTENING);
        wake_word_set_audio_forwarding(listening);

        server_client_event_t server_evt;
        if (xQueueReceive(s_server_queue, &server_evt, pdMS_TO_TICKS(20)) == pdTRUE) {
            orchestrator_on_server_event(to_orchestrator_event(&server_evt));
            if (server_evt.type == SERVER_CLIENT_EVENT_AUDIO) {
                free(server_evt.audio_data);
            }
        }

        // Drain every frame currently queued. Frames that arrived while we
        // were still listening but that we're processing after state has
        // since moved on (e.g. speech-end just flipped us to THINKING) are
        // freed without forwarding -- `listening` above was sampled once at
        // the top of this iteration and applies to the whole drain, so a
        // frame queued moments before forwarding was disarmed is discarded
        // here rather than sent late.
        wake_word_audio_frame_t frame;
        while (xQueueReceive(s_audio_frame_queue, &frame, 0) == pdTRUE) {
            if (listening) {
                bool is_end_of_speech = pending_speech_end;
                pending_speech_end = false;
                orchestrator_on_audio_frame(frame.data, frame.len, is_end_of_speech);
            }
            free(frame.data);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(haro_config_init());
    ESP_ERROR_CHECK(wifi_provisioning_ensure_connected());
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_ERROR_CHECK(face_display_init());

    char server_url[128];
    ESP_ERROR_CHECK(haro_config_get_server_url(server_url, sizeof(server_url)));

    s_wake_queue = xQueueCreate(4, sizeof(wake_word_event_type_t));
    s_server_queue = xQueueCreate(8, sizeof(server_client_event_t));
    // Depth 8: at low-cost AFE mode the feed chunk period is roughly
    // 16-32ms, and orchestrator_task's own poll cadence is bounded by the
    // 20ms server_queue timeout below -- 8 frames gives a few iterations'
    // worth of slack before a frame would be dropped as "queue full".
    s_audio_frame_queue = xQueueCreate(8, sizeof(wake_word_audio_frame_t));

    ESP_ERROR_CHECK(server_client_init(server_url, s_server_queue));
    ESP_ERROR_CHECK(server_client_send_hello("haro-session"));
    ESP_ERROR_CHECK(wake_word_start(s_wake_queue, s_audio_frame_queue));

    orchestrator_ops_t ops = {
        .server = { .send_audio_frame = server_send_audio_frame, .send_end_of_speech = server_send_end_of_speech },
        .audio_out = { .play_chunk = audio_play_chunk, .stop = audio_stop },
        .face = { .show = face_show },
    };
    orchestrator_init(ops);
    face_display_show(EXPR_IDLE);

    ESP_LOGI(TAG, "Haro ready, waiting for wake word");
    xTaskCreate(orchestrator_task, "orchestrator", 4096, NULL, 5, NULL);
}
#endif
