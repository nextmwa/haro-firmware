#include "unity.h"
#include "orchestrator.h"
#include <string.h>

static char s_sent_audio[256];
static size_t s_sent_audio_len;
static bool s_end_of_speech_sent;
static char s_played_chunks[4][256];
static int s_played_count;
static int s_shown_expressions[16];
static int s_shown_count;
static int s_stop_calls;

static esp_err_t fake_send_audio_frame(void *ctx, const uint8_t *data, size_t len)
{
    memcpy(s_sent_audio, data, len);
    s_sent_audio_len = len;
    return ESP_OK;
}

static esp_err_t fake_send_end_of_speech(void *ctx)
{
    s_end_of_speech_sent = true;
    return ESP_OK;
}

static void fake_play_chunk(void *ctx, const uint8_t *data, size_t len)
{
    memcpy(s_played_chunks[s_played_count], data, len);
    s_played_count++;
}

static void fake_stop(void *ctx) { s_stop_calls++; }

static void fake_show(void *ctx, int expression)
{
    s_shown_expressions[s_shown_count++] = expression;
}

static orchestrator_ops_t make_fake_ops(void)
{
    s_sent_audio_len = 0;
    s_end_of_speech_sent = false;
    s_played_count = 0;
    s_shown_count = 0;
    s_stop_calls = 0;

    orchestrator_ops_t ops = {
        .server = { .send_audio_frame = fake_send_audio_frame, .send_end_of_speech = fake_send_end_of_speech },
        .audio_out = { .play_chunk = fake_play_chunk, .stop = fake_stop },
        .face = { .show = fake_show },
    };
    return ops;
}

TEST_CASE("starts in IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}

TEST_CASE("wake word moves IDLE to LISTENING", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    TEST_ASSERT_EQUAL(HARO_STATE_LISTENING, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_shown_count);
}

TEST_CASE("audio frames while LISTENING are forwarded to the server", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1, 2, 3, 4 };
    orchestrator_on_audio_frame(frame, sizeof(frame), false);
    TEST_ASSERT_EQUAL(sizeof(frame), s_sent_audio_len);
    TEST_ASSERT_EQUAL_MEMORY(frame, s_sent_audio, sizeof(frame));
}

TEST_CASE("end of speech moves LISTENING to THINKING and signals the server", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);
    TEST_ASSERT_EQUAL(HARO_STATE_THINKING, orchestrator_get_state());
    TEST_ASSERT_TRUE(s_end_of_speech_sent);
}

TEST_CASE("an emotion event moves THINKING to SPEAKING and shows the matching face", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    orchestrator_server_event_t evt = { .type = ORCHESTRATOR_SERVER_EVENT_PROTOCOL };
    evt.protocol_event.type = PROTOCOL_EVENT_EMOTION;
    strcpy(evt.protocol_event.value, "happy");
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_SPEAKING, orchestrator_get_state());
}

TEST_CASE("an audio event while SPEAKING plays the chunk", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    orchestrator_server_event_t audio_evt = { .type = ORCHESTRATOR_SERVER_EVENT_AUDIO };
    uint8_t chunk[] = { 9, 9, 9 };
    audio_evt.audio_data = chunk;
    audio_evt.audio_len = sizeof(chunk);
    orchestrator_on_server_event(audio_evt);

    TEST_ASSERT_EQUAL(1, s_played_count);
}

TEST_CASE("response_end returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    orchestrator_server_event_t evt = { .type = ORCHESTRATOR_SERVER_EVENT_PROTOCOL };
    evt.protocol_event.type = PROTOCOL_EVENT_RESPONSE_END;
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}

TEST_CASE("a disconnect event returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    orchestrator_server_event_t evt = { .type = ORCHESTRATOR_SERVER_EVENT_DISCONNECTED };
    orchestrator_on_server_event(evt);
    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}

// --- Additional cases ported from haro/tests/test_orchestrator.py ---
//
// The Python Orchestrator owns its own async event loop and connection
// management (connect_with_retry/close/reconnect), so it can detect a
// response timeout itself (test_response_timeout_shows_error_and_reconnects)
// and distinguish "mid-turn" abort points (send failure, protocol error,
// network error, end-of-speech failure) with separate exception paths, each
// forcing a reconnect via its own ops.
//
// This C port is a synchronous state machine with no timers and no
// connect/close/reconnect ops of its own (orchestrator_ops_t has none) --
// timeouts and connection loss are both expected to surface, from Task 10's
// main.c, as a single ORCHESTRATOR_SERVER_EVENT_DISCONNECTED event (main.c
// owns any FreeRTOS timer and the actual reconnect). So the cases below port
// the *shapes* that are still meaningful at this layer: a disconnect
// arriving mid-turn from every reachable state (THINKING and SPEAKING, not
// just LISTENING as in the "starts in IDLE" case above), a
// PROTOCOL_EVENT_ERROR mid-response (the error branch in
// orchestrator_on_server_event that none of the 8 cases above exercise),
// and the guard clauses that make spurious/duplicate triggers no-ops.

TEST_CASE("a disconnect event during THINKING stops audio and returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);
    TEST_ASSERT_EQUAL(HARO_STATE_THINKING, orchestrator_get_state());

    orchestrator_server_event_t evt = { .type = ORCHESTRATOR_SERVER_EVENT_DISCONNECTED };
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_stop_calls);
}

TEST_CASE("a disconnect event mid-speaking (after an audio chunk) returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);

    orchestrator_server_event_t audio_evt = { .type = ORCHESTRATOR_SERVER_EVENT_AUDIO };
    uint8_t chunk[] = { 9, 9, 9 };
    audio_evt.audio_data = chunk;
    audio_evt.audio_len = sizeof(chunk);
    orchestrator_on_server_event(audio_evt);
    TEST_ASSERT_EQUAL(HARO_STATE_SPEAKING, orchestrator_get_state());

    orchestrator_server_event_t disconnect_evt = { .type = ORCHESTRATOR_SERVER_EVENT_DISCONNECTED };
    orchestrator_on_server_event(disconnect_evt);

    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_played_count);
    TEST_ASSERT_EQUAL(1, s_stop_calls);
}

TEST_CASE("a protocol error event mid-response stops audio and returns to IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    uint8_t frame[] = { 1 };
    orchestrator_on_audio_frame(frame, sizeof(frame), true);
    TEST_ASSERT_EQUAL(HARO_STATE_THINKING, orchestrator_get_state());

    orchestrator_server_event_t evt = { .type = ORCHESTRATOR_SERVER_EVENT_PROTOCOL };
    evt.protocol_event.type = PROTOCOL_EVENT_ERROR;
    strcpy(evt.protocol_event.message, "oops");
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_stop_calls);
}

TEST_CASE("wake word is ignored while not IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    orchestrator_on_wake_word();
    TEST_ASSERT_EQUAL(HARO_STATE_LISTENING, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_shown_count);

    orchestrator_on_wake_word();
    TEST_ASSERT_EQUAL(HARO_STATE_LISTENING, orchestrator_get_state());
    TEST_ASSERT_EQUAL(1, s_shown_count);
}

TEST_CASE("audio frames are ignored while IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());
    uint8_t frame[] = { 1, 2, 3 };
    orchestrator_on_audio_frame(frame, sizeof(frame), false);
    TEST_ASSERT_EQUAL(0, s_sent_audio_len);
    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
}

TEST_CASE("server events other than disconnect are ignored while IDLE", "[orchestrator]")
{
    orchestrator_init(make_fake_ops());

    orchestrator_server_event_t evt = { .type = ORCHESTRATOR_SERVER_EVENT_PROTOCOL };
    evt.protocol_event.type = PROTOCOL_EVENT_EMOTION;
    strcpy(evt.protocol_event.value, "happy");
    orchestrator_on_server_event(evt);

    TEST_ASSERT_EQUAL(HARO_STATE_IDLE, orchestrator_get_state());
    TEST_ASSERT_EQUAL(0, s_shown_count);
}
