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
#include "status_led.h"
#include "orchestrator.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

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
    // This callback only fires for orchestrator's own real state-driven
    // expression changes (LISTENING/THINKING/SPEAKING+emotion/ERROR) --
    // never for the idle-fidget cycle below, which calls
    // face_display_show() directly. That split is exactly what
    // status_led_set_expression() wants: solid mood colors for real state,
    // untouched by cosmetic idle fidgets. See status_led.h.
    status_led_set_expression((face_expression_t)expression);
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

// Idle-animation timing, in milliseconds. Tunable; not exposed via Kconfig
// since these are purely cosmetic defaults, not a hardware/protocol
// constant. See the idle-animation block in orchestrator_task() below.
#define IDLE_ANIM_FIRST_DELAY_MS   20000  // how long to sit at plain idle before the first fidget
#define IDLE_ANIM_CYCLE_GAP_MS     15000  // plain-idle time between fidgets thereafter
#define IDLE_ANIM_THOUGHTFUL_MS     3000  // how long a "thoughtful" fidget stays on screen
#define IDLE_ANIM_BORED_MS          4000  // how long a "bored" fidget stays on screen
#define IDLE_ANIM_BORED_AFTER_MS  90000   // total continuous idle time before "bored" can be picked at all

typedef enum { IDLE_ANIM_PLAIN, IDLE_ANIM_THOUGHTFUL, IDLE_ANIM_BORED, IDLE_ANIM_LOOKING } idle_anim_state_t;

// Eye liveliness tuning, see the liveliness block in orchestrator_task()
// below for how these are used. Originally this was camera-based gaze
// tracking (a face_detect model on an OV2640 feed driving the eyes toward a
// tracked face) -- removed after real-hardware testing showed detections
// firing too rarely and erratically to feel responsive (confirmed via
// logging: gaps from a few seconds up to 400+ seconds between detections
// even with a clearly visible, centered, well-lit face; see the
// camera_face_track component's git history). Replaced with two cheap,
// always-available signals instead: small random eye micro-saccades (idle
// "wandering", never fully still) and a nudge toward loud sounds using the
// stereo mic's existing per-channel amplitude data (wake_word.c) -- neither
// needs the camera or its CPU-heavy on-device inference.
#define EYE_OFFSET_SMOOTH_ALPHA 0.15f // low-pass filter weight per tick (0..1: higher = snappier, lower = smoother/laggier)
#define SACCADE_MIN_INTERVAL_MS 1500  // how often a new micro-saccade target is picked, min/max for a non-mechanical cadence
#define SACCADE_MAX_INTERVAL_MS 4000
#define SACCADE_MAX_PX 3             // subtle idle wander -- small relative to SOUND_MAX_PX so it doesn't look like a reaction to something
// Pixel offset at the edge of a loud sound's [-1,1] direction estimate.
// Bounded by the tighter of the two eye sockets' clearances (render_pose()'s
// eye_cy=HEIGHT*0.38, eye_h=HEIGHT*0.42 leaves ~11px above the eye before it
// would clip the top of the display) -- 9px stays inside that with margin.
#define SOUND_MAX_PX 9.0f
#define EYE_OFFSET_UPDATE_MS 150      // minimum time between face_display_set_gaze_offset() calls (I2C redraw cost)

// Blink: a short, non-animated-transition eyelid close/open, independent of
// (and layered on top of, via face_display's shared overlay state) the
// saccade/sound offset above. Randomized interval so it doesn't look
// metronomic.
#define BLINK_MIN_INTERVAL_MS 2000
#define BLINK_MAX_INTERVAL_MS 6000
typedef struct { float openness; uint32_t duration_ms; } blink_step_t;
#define BLINK_STEP_COUNT 4
static const blink_step_t BLINK_STEPS[BLINK_STEP_COUNT] = {
    { 0.4f,  30 },
    { 0.05f, 40 },
    { 0.4f,  30 },
    { 1.0f,  40 },
};

// Classic Anki Cozmo "looking around" fidget: a short scripted sequence of
// glances rather than a single held expression, played one step at a time
// by the IDLE_ANIM_LOOKING case below.
typedef struct { face_expression_t expr; uint32_t duration_ms; } idle_look_step_t;
#define IDLE_LOOK_STEP_COUNT 3
static const idle_look_step_t IDLE_LOOK_STEPS[IDLE_LOOK_STEP_COUNT] = {
    { EXPR_LOOKING_LEFT,  500 },
    { EXPR_LOOKING_RIGHT, 500 },
    { EXPR_LOOKING_LEFT,  350 },
};

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

    // Idle-animation state: purely cosmetic, and deliberately NOT routed
    // through orchestrator -- these expressions (EXPR_THINKING/EXPR_BORED
    // reused here as idle "fidgets", plus EXPR_IDLE itself) are called on
    // face_display directly, the same way main.c's one-shot post-init
    // face_display_show(EXPR_IDLE) call already does, so orchestrator's own
    // state machine and its face.show callback (used for LISTENING/
    // THINKING/SPEAKING/ERROR) never need to know this happens. Reset
    // whenever orchestrator leaves HARO_STATE_IDLE so a fidget never lingers
    // into (or preempts the very start of) a real interaction.
    bool was_idle = false;
    idle_anim_state_t idle_anim = IDLE_ANIM_PLAIN;
    TickType_t idle_since = 0;
    TickType_t idle_anim_deadline = 0;
    int idle_look_step = 0;

    // Eye liveliness: smoothed continuously (cheap, no I/O) every loop tick,
    // but only pushed to the display periodically (see EYE_OFFSET_UPDATE_MS)
    // -- face_display_set_gaze_offset() does a full I2C framebuffer redraw
    // (~1024 bytes at 400kHz is ~25ms), so calling it every ~20ms loop
    // iteration would saturate the I2C bus and blow this task's own timing
    // budget. Applies regardless of orchestrator state (idle, listening,
    // speaking, ...) -- it's a small positional nudge on top of whatever
    // expression is already showing, not a mood change.
    float saccade_target_dx_px = 0.0f, saccade_target_dy_px = 0.0f;
    float eye_dx_smooth = 0.0f, eye_dy_smooth = 0.0f;
    int eye_last_px_dx = 0, eye_last_px_dy = 0;
    TickType_t eye_next_update = 0;
    TickType_t saccade_next_pick = 0;

    // Blink: independent short animation, scheduled at random intervals.
    // blink_step < 0 means "not currently blinking, waiting for blink_next".
    int blink_step = -1;
    TickType_t blink_next = 0;

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

        TickType_t now = xTaskGetTickCount();

        // Eye liveliness: pick a new small random saccade target
        // periodically, blend in a stronger nudge toward a loud sound's
        // direction when one was heard recently (wake_word_get_sound_direction()
        // is just an atomic read -- cheap, safe every tick), smooth
        // continuously, redraw throttled -- see this block's declaration
        // comment above for why.
        {
            if (now >= saccade_next_pick) {
                int range = 2 * SACCADE_MAX_PX + 1;
                saccade_target_dx_px = (float)((int)(esp_random() % (unsigned)range) - SACCADE_MAX_PX);
                saccade_target_dy_px = (float)((int)(esp_random() % (unsigned)range) - SACCADE_MAX_PX);
                saccade_next_pick = now + pdMS_TO_TICKS(SACCADE_MIN_INTERVAL_MS +
                    (esp_random() % (SACCADE_MAX_INTERVAL_MS - SACCADE_MIN_INTERVAL_MS)));
            }

            float sound_dir = 0.0f;
            bool sound_present = wake_word_get_sound_direction(&sound_dir);
            float target_dx = saccade_target_dx_px + (sound_present ? sound_dir * SOUND_MAX_PX : 0.0f);
            float target_dy = saccade_target_dy_px;
            eye_dx_smooth += (target_dx - eye_dx_smooth) * EYE_OFFSET_SMOOTH_ALPHA;
            eye_dy_smooth += (target_dy - eye_dy_smooth) * EYE_OFFSET_SMOOTH_ALPHA;

            if (now >= eye_next_update) {
                int px_dx = (int)lroundf(eye_dx_smooth);
                int px_dy = (int)lroundf(eye_dy_smooth);
                if (px_dx != eye_last_px_dx || px_dy != eye_last_px_dy) {
                    face_display_set_gaze_offset(px_dx, px_dy);
                    eye_last_px_dx = px_dx;
                    eye_last_px_dy = px_dy;
                }
                eye_next_update = now + pdMS_TO_TICKS(EYE_OFFSET_UPDATE_MS);
            }
        }

        // Blink: independent of the saccade/sound offset above -- both land
        // in face_display's shared overlay state (see
        // face_display_set_blink()'s header comment), so no coordination is
        // needed here beyond each running on its own schedule.
        {
            if (now >= blink_next) {
                if (blink_step < 0) {
                    blink_step = 0;
                }
                face_display_set_blink(BLINK_STEPS[blink_step].openness);
                blink_next = now + pdMS_TO_TICKS(BLINK_STEPS[blink_step].duration_ms);
                blink_step++;
                if (blink_step >= BLINK_STEP_COUNT) {
                    blink_step = -1;
                    blink_next = now + pdMS_TO_TICKS(BLINK_MIN_INTERVAL_MS +
                        (esp_random() % (BLINK_MAX_INTERVAL_MS - BLINK_MIN_INTERVAL_MS)));
                }
            }
        }

        // Idle-animation cycle: only while orchestrator has been
        // continuously idle (no interaction in flight). Any other state
        // resets it immediately, so a fidget never shows mid-conversation.
        if (orchestrator_get_state() == HARO_STATE_IDLE) {
            if (!was_idle) {
                was_idle = true;
                idle_since = now;
                idle_anim = IDLE_ANIM_PLAIN;
                idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_ANIM_FIRST_DELAY_MS);
                // Ambient breathing tracks real orchestrator state (IDLE or
                // not), not the idle-fidget cycle -- see status_led.h.
                status_led_set_idle_ambient(true);
            } else if (now >= idle_anim_deadline) {
                switch (idle_anim) {
                case IDLE_ANIM_PLAIN: {
                    // "Looking around" is always in the mix (it's a quick,
                    // low-commitment fidget); "bored" only joins once past
                    // IDLE_ANIM_BORED_AFTER_MS of unbroken idle, so a
                    // long-idle Haro doesn't look stuck pondering forever
                    // but also doesn't turn "bored" the moment it's left
                    // alone.
                    bool bored_eligible = (now - idle_since) >= pdMS_TO_TICKS(IDLE_ANIM_BORED_AFTER_MS);
                    int variant_count = bored_eligible ? 3 : 2;
                    int pick = (int)(esp_random() % (unsigned)variant_count); // 0=thoughtful, 1=looking, 2=bored
                    if (pick == 2) {
                        idle_anim = IDLE_ANIM_BORED;
                        face_display_show(EXPR_BORED);
                        idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_ANIM_BORED_MS);
                    } else if (pick == 1) {
                        idle_anim = IDLE_ANIM_LOOKING;
                        idle_look_step = 0;
                        face_display_show(IDLE_LOOK_STEPS[0].expr);
                        idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_LOOK_STEPS[0].duration_ms);
                    } else {
                        idle_anim = IDLE_ANIM_THOUGHTFUL;
                        face_display_show(EXPR_THINKING);
                        idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_ANIM_THOUGHTFUL_MS);
                    }
                    break;
                }
                case IDLE_ANIM_LOOKING:
                    idle_look_step++;
                    if (idle_look_step < IDLE_LOOK_STEP_COUNT) {
                        face_display_show(IDLE_LOOK_STEPS[idle_look_step].expr);
                        idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_LOOK_STEPS[idle_look_step].duration_ms);
                    } else {
                        idle_anim = IDLE_ANIM_PLAIN;
                        face_display_show(EXPR_IDLE);
                        idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_ANIM_CYCLE_GAP_MS);
                    }
                    break;
                case IDLE_ANIM_THOUGHTFUL:
                case IDLE_ANIM_BORED:
                default:
                    idle_anim = IDLE_ANIM_PLAIN;
                    face_display_show(EXPR_IDLE);
                    idle_anim_deadline = now + pdMS_TO_TICKS(IDLE_ANIM_CYCLE_GAP_MS);
                    break;
                }
            }
        } else {
            was_idle = false;
            // Leaving idle: status_led_set_expression() (via face_show(),
            // orchestrator's face.show callback) will land the right solid
            // color for whatever state we entered within the same
            // orchestrator call that changed s_state, so this only needs to
            // stop the breathing task, not pick a replacement color itself.
            status_led_set_idle_ambient(false);
        }

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
                // A live mic-to-speaker echo diagnostic used to run here.
                // Removed: found on real hardware that playing captured
                // audio back to the speaker WHILE still listening on the
                // mic is a feedback loop by construction (the mic picks up
                // the speaker's own output and re-forwards it, which
                // re-plays louder, ...) -- it drove the amplifier into a
                // loud, sustained squeal until the board was unplugged. Do
                // not reintroduce playback here without also muting/
                // disarming the mic for its duration, e.g. the same way
                // wake_word_set_audio_forwarding() already gates forwarding.
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

    // face_display shares audio_pipeline's I2C bus rather than owning one:
    // found on real hardware that this board's external header does not
    // expose a second, independent I2C bus -- its silkscreen "SDA"/"SCL"
    // pins are physically GPIO11/GPIO10, the same pins audio_pipeline
    // already drives for the onboard codecs (see audio_pipeline.h's comment
    // on audio_pipeline_get_i2c_bus()).
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(audio_pipeline_get_i2c_bus(&i2c_bus));

    // face_display_init() probes the bus for the display first
    // (i2c_master_probe(), bounded timeout) before touching esp_lcd's
    // unbounded I2C transactions -- safe to call unconditionally whether or
    // not a physical SSD1306 is wired up. A missing display (ESP_ERR_NOT_FOUND)
    // is non-fatal: face_display_show() no-ops when s_panel was never set.
    esp_err_t face_err = face_display_init(i2c_bus);
    if (face_err != ESP_OK) {
        ESP_LOGW(TAG, "face_display_init() failed: %s -- continuing without display", esp_err_to_name(face_err));
    }

    // Independent GPIO (RMT peripheral, not I2C) -- no shared-bus concerns
    // with face_display/audio_pipeline. Non-fatal on failure, same
    // reasoning as face_display: a missing/misbehaving LED chain shouldn't
    // block the rest of the device from working.
    esp_err_t led_err = status_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "status_led_init() failed: %s -- continuing without status LEDs", esp_err_to_name(led_err));
    }

    char server_url[128];
    ESP_ERROR_CHECK(haro_config_get_server_url(server_url, sizeof(server_url)));

    s_wake_queue = xQueueCreate(4, sizeof(wake_word_event_type_t));
    s_server_queue = xQueueCreate(8, sizeof(server_client_event_t));
    // Depth 8: at low-cost AFE mode the feed chunk period is roughly
    // 16-32ms, and orchestrator_task's own poll cadence is bounded by the
    // 20ms server_queue timeout below -- 8 frames gives a few iterations'
    // worth of slack before a frame would be dropped as "queue full".
    s_audio_frame_queue = xQueueCreate(8, sizeof(wake_word_audio_frame_t));

    // server_client_init() only starts the WebSocket client's internal task
    // (esp_websocket_client_start() does not block until connected) --
    // sending the protocol `hello` message happens inside server_client.c
    // itself, from its WEBSOCKET_EVENT_CONNECTED handler, once a real
    // connection exists. See server_client.h for why there is no separate
    // "send hello" call here (Finding 2 of the final review: this used to
    // be a synchronous server_client_send_hello() call right here, which was
    // guaranteed to fail on every boot since the WebSocket cannot possibly
    // be connected yet at this point).
    ESP_ERROR_CHECK(server_client_init(server_url, "haro-session", s_server_queue));
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
