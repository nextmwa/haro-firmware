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
#include "servo.h"
#include "camera_face_track.h"
#include "orchestrator.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "haro";
static QueueHandle_t s_wake_queue;
static QueueHandle_t s_server_queue;
static QueueHandle_t s_audio_frame_queue;

// Connectivity, tracked independently of orchestrator's own conversation
// state machine (HARO_STATE_IDLE/LISTENING/...): orchestrator only cares
// about a single transient disconnect flash-then-return-to-idle (see its
// ORCHESTRATOR_SERVER_EVENT_DISCONNECTED handling), which stays correct
// and unmodified below. This flag instead drives a PERSISTENT "can't reach
// the server" screen (red LED + scrolling IP:port text) for as long as
// the condition lasts, set false on SERVER_CLIENT_EVENT_DISCONNECTED and
// back to true on the new SERVER_CLIENT_EVENT_CONNECTED (posted on every
// successful (re)connect, including automatic reconnects). Starts true:
// optimistic until the first real event says otherwise, rather than
// flashing the error screen for the brief normal connect window at boot.
static volatile bool s_server_reachable = true;

// Built once in app_main() from the same server_url passed to
// server_client_init() (stripped of its "ws://" scheme, since the user
// asked for "ip del server ... con la porta specificata", not the full
// URL) -- read from orchestrator_task()'s loop below whenever
// s_server_reachable is false.
static char s_server_error_text[160];

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

static esp_err_t server_send_interrupt(void *ctx)
{
    esp_err_t err = server_client_send_interrupt();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send_interrupt failed: %s", esp_err_to_name(err));
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
    esp_err_t err = audio_pipeline_stop_playback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_pipeline_stop_playback failed: %s -- residual audio may keep playing briefly",
                 esp_err_to_name(err));
    }
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

static void face_show_action(void *ctx, const char *action_name, const char *action_result)
{
    face_display_show_action(action_name, action_result);
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
    case SERVER_CLIENT_EVENT_CONNECTED:
        // Never actually reaches here: the dequeue loop below intercepts
        // SERVER_CLIENT_EVENT_CONNECTED before calling this function, the
        // same way it already intercepts PROTOCOL_EVENT_FACE_POSITION --
        // orchestrator has no concept of "connected", only "disconnected"/
        // back-to-idle. Case kept (not folded into a `default:`) so this
        // switch stays exhaustive over server_client_event_type_t.
        dst.type = ORCHESTRATOR_SERVER_EVENT_DISCONNECTED;
        break;
    }
    dst.audio_data = src->audio_data;
    dst.audio_len = src->audio_len;
    return dst;
}

// Safety cap on HARO_STATE_LISTENING: normally speech-end is detected via
// wake_word's VAD-based silence debounce (WAKE_WORD_SPEECH_END), but
// background noise or other people talking in the room can keep AFE's
// vad_state at VAD_SPEECH indefinitely, so that signal alone never fires --
// found on real hardware as "stays listening until the user gives up".
// This bounds the worst case regardless of what's confusing the VAD: after
// this long in LISTENING, force the turn to end with whatever audio has
// been captured so far, same as a real detected silence would.
#define MAX_LISTEN_MS 12000

// Safety cap on HARO_STATE_THINKING/HARO_STATE_SPEAKING: unlike LISTENING
// above (which MAX_LISTEN_MS covers) and PLAYING_MUSIC (escapable by a
// fresh wake word, see orchestrator_on_wake_word()), these two states have
// no way out except a server-sent response_end/error or a real
// disconnected event -- and a full s_server_queue silently drops exactly
// those (see server_client.c's "event queue full, dropping ..." logs).
// Without this, a single dropped event leaves Haro stuck showing
// THINKING/SPEAKING and ignoring the wake word forever, recoverable only
// by a manual power cycle -- a real finding from a whole-codebase review,
// not yet observed as a live symptom. Refreshed on every server event
// actually processed while in either state (see the two call sites
// below), not just on entry, so a long multi-chunk TTS/LLM reply in
// genuine progress is never cut short -- this is a recovery backstop for
// no progress at all, not a latency bound.
#define MAX_THINKING_SPEAKING_MS 60000

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
// below for how these are used. Three signals, in priority order: a
// tracked face (camera_face_track.h) wins whenever one is present; failing
// that, a nudge toward a loud sound (wake_word_get_sound_direction());
// failing that, small random micro-saccades so the eyes are never fully
// still. Face tracking was tried once before at a 200ms poll interval and
// removed -- real-hardware testing showed detections firing too rarely and
// erratically to feel responsive even after fixing a core-pinning
// contention bug, and it wasn't worth the CPU cost at that rate. This
// version polls at 500ms instead (see camera_face_track.cpp's
// FACE_TRACK_POLL_MS) specifically to keep that cost down, on the
// understanding that main.c's own exponential smoothing below makes even
// a 500ms-stepped signal look reasonably smooth on screen.
#define EYE_OFFSET_SMOOTH_ALPHA 0.15f // low-pass filter weight per tick (0..1: higher = snappier, lower = smoother/laggier)
#define SACCADE_MIN_INTERVAL_MS 1500  // how often a new micro-saccade target is picked, min/max for a non-mechanical cadence
#define SACCADE_MAX_INTERVAL_MS 4000
#define SACCADE_MAX_PX 3             // subtle idle wander -- small relative to SOUND_MAX_PX so it doesn't look like a reaction to something
// Pixel offset at the edge of a loud sound's (or a tracked face's) [-1,1]
// offset estimate. Bounded by the tighter of the two eye sockets'
// clearances (render_pose()'s eye_cy=HEIGHT*0.38, eye_h=HEIGHT*0.42 leaves
// ~11px above the eye before it would clip the top of the display) -- 9px
// stays inside that with margin. Shared by both signals since either one
// fully replaces the other (never summed), so one scale is enough.
#define SOUND_MAX_PX 9.0f
#define FACE_TRACK_MAX_PX 9.0f
#define EYE_OFFSET_UPDATE_MS 150      // minimum time between face_display_set_gaze_offset() calls (I2C redraw cost)

// Physical pan/tilt servos (servo.h): driven by the SAME offset as the
// on-screen eyes (px_dx/px_dy below), scaled from pixels to degrees, so
// the head turns along with the eyes for every idle behavior (micro-
// saccades, sound-reactive gaze) rather than needing its own separate
// logic. At the max combined offset (SACCADE_MAX_PX + SOUND_MAX_PX = 12px)
// this swings each servo about +-36deg from center -- noticeably more than
// the on-screen eye movement itself (deliberately amplified per user
// feedback: the 1:1-ish original scale looked too subtle to notice), still
// safely inside each servo's 0-180deg range (54deg..126deg). Tilt shares
// this same scale for now, sight unseen (the tilt servo isn't physically
// wired up yet) -- may need its own separate constant once mounted, e.g.
// if its range of motion or mounting orientation calls for a different
// scale or an inverted sign from pan's.
#define SERVO_DEG_PER_PX 3.0f

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

    // MAX_LISTEN_MS failsafe state -- see that constant's comment above.
    bool was_listening_for_timeout = false;
    TickType_t listening_since = 0;

    // MAX_THINKING_SPEAKING_MS failsafe state -- see that constant's
    // comment above. Seeded/refreshed at the two call sites below that are
    // the ONLY ways into HARO_STATE_THINKING/HARO_STATE_SPEAKING
    // (orchestrator_on_audio_frame()'s end-of-speech branch and every
    // orchestrator_on_server_event() call), so by the time the check
    // further down ever sees either state, this deadline is always fresh
    // -- never left at its zero initial value while that check is live.
    TickType_t thinking_or_speaking_deadline = 0;

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

    // Music-notes scroll animation (HARO_STATE_PLAYING_MUSIC), same
    // poll-state-every-tick-but-throttle-the-redraw pattern as the idle
    // fidgets above and the eye-liveliness block below.
    int music_scroll_offset = 0;
    TickType_t music_next_update = 0;

    // Server-unreachable error-text scroll animation, same pattern as the
    // music-notes state just above.
    int error_scroll_offset = 0;
    TickType_t error_next_update = 0;

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

    // Wake word interrupting PLAYING_MUSIC (orchestrator_on_wake_word())
    // flips straight to LISTENING and calls audio_stop() (this file,
    // below), which now genuinely aborts playback (closes/reopens the
    // codec device -- see audio_pipeline_stop_playback()'s comment), not
    // just a no-op like an earlier version of this code. Real-hardware
    // testing before that fix found even the data-plane fixes above
    // (bigger queues, bounded-timeout sends) weren't enough on their own:
    // audio kept audibly playing for seconds after the interrupt, which
    // the mic picked back up as continued speech, running LISTENING into
    // the 12s MAX_LISTEN_MS failsafe every time. audio_pipeline_stop_playback()
    // should silence the speaker close to immediately, but this small hold
    // stays as cheap defense-in-depth against whatever's already in the
    // analog output path at the instant of closing (e.g. amplifier/DMA
    // settling) -- not the multi-second margin the pre-fix code needed.
    #define MUSIC_INTERRUPT_FORWARDING_HOLD_MS 300
    TickType_t forwarding_hold_until = 0;

    while (true) {
        wake_word_event_type_t wake_evt;
        while (xQueueReceive(s_wake_queue, &wake_evt, 0) == pdTRUE) {
            if (wake_evt == WAKE_WORD_DETECTED && !s_server_reachable) {
                // Ignored while unreachable: responding to a wake word is
                // futile with no server to talk to, and orchestrator_on_
                // wake_word() would call face.show()/status_led_set_
                // expression() for HARO_STATE_LISTENING -- painting over
                // the error-text/red-LED screen for a turn that can never
                // complete (found by inspection after the same hazard was
                // fixed for the eye-liveliness/blink/idle-fidget blocks;
                // wake_word_start()'s own detection keeps running
                // regardless, so this is purely about not visually
                // thrashing between the error screen and a dead-end
                // LISTENING state).
            } else if (wake_evt == WAKE_WORD_DETECTED) {
                bool was_playing_music = (orchestrator_get_state() == HARO_STATE_PLAYING_MUSIC);
                if (was_playing_music) {
                    // Flush whatever was still in flight for the
                    // interrupted track -- stale audio chunks/protocol
                    // events queued moments before the interrupt would
                    // otherwise still get played/processed after we've
                    // already moved on to LISTENING, compounding the
                    // residual-audio problem above. Same "discard rather
                    // than process late" philosophy as the mic-frame drain
                    // loop below. Done BEFORE orchestrator_on_wake_word()
                    // (not after, as this used to be ordered): that call's
                    // own send_interrupt() can enqueue a synthetic
                    // SERVER_CLIENT_EVENT_DISCONNECTED (server_send_
                    // interrupt() above, on a failed send) onto
                    // s_server_queue -- draining afterward silently threw
                    // that recovery event away along with the real stale
                    // data, found by inspection.
                    server_client_event_t stale_server_evt;
                    while (xQueueReceive(s_server_queue, &stale_server_evt, 0) == pdTRUE) {
                        if (stale_server_evt.type == SERVER_CLIENT_EVENT_AUDIO) {
                            free(stale_server_evt.audio_data);
                        }
                    }
                    wake_word_audio_frame_t stale_frame;
                    while (xQueueReceive(s_audio_frame_queue, &stale_frame, 0) == pdTRUE) {
                        free(stale_frame.data);
                    }
                }
                orchestrator_on_wake_word();
                pending_speech_end = false;
                if (was_playing_music) {
                    forwarding_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(MUSIC_INTERRUPT_FORWARDING_HOLD_MS);
                }
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
        bool listening = (orchestrator_get_state() == HARO_STATE_LISTENING) &&
                          xTaskGetTickCount() >= forwarding_hold_until;
        wake_word_set_audio_forwarding(listening);

        // MAX_LISTEN_MS failsafe: track when LISTENING started, and force
        // speech-end once we've been in it too long -- see that constant's
        // comment above. Checked here (not just relying on wake_word's VAD
        // signal) because that signal is exactly what can fail to fire.
        TickType_t tick_now_for_timeout = xTaskGetTickCount();
        if (listening && !was_listening_for_timeout) {
            was_listening_for_timeout = true;
            listening_since = tick_now_for_timeout;
        } else if (!listening) {
            was_listening_for_timeout = false;
        } else if (tick_now_for_timeout - listening_since >= pdMS_TO_TICKS(MAX_LISTEN_MS)) {
            ESP_LOGW(TAG, "HARO_STATE_LISTENING exceeded %dms with no VAD silence -- forcing speech-end", MAX_LISTEN_MS);
            pending_speech_end = true;
            was_listening_for_timeout = false;
        }

        // MAX_THINKING_SPEAKING_MS failsafe -- see that constant's comment
        // above. thinking_or_speaking_deadline is kept fresh by the two
        // call sites below, so seeing it expire here means no progress
        // arrived at all during the whole window, not just a slow turn.
        {
            haro_state_t state_for_timeout = orchestrator_get_state();
            bool thinking_or_speaking = (state_for_timeout == HARO_STATE_THINKING ||
                                          state_for_timeout == HARO_STATE_SPEAKING);
            if (thinking_or_speaking && tick_now_for_timeout >= thinking_or_speaking_deadline) {
                ESP_LOGW(TAG, "stuck in THINKING/SPEAKING for %dms with no progress -- forcing back to idle",
                         MAX_THINKING_SPEAKING_MS);
                orchestrator_force_idle();
            }
        }

        TickType_t now = xTaskGetTickCount();

        // Eye liveliness: a tracked face wins outright when one's present
        // (camera_face_track_get_offset() is just an atomic read -- cheap,
        // safe every tick); otherwise pick a new small random saccade
        // target periodically, blended with a nudge toward a loud sound's
        // direction when one was heard recently
        // (wake_word_get_sound_direction(), same cheap-atomic-read
        // property). Smoothed continuously, redraw throttled -- see this
        // block's declaration comment above for why. Gated on
        // eyes_and_led_allowed: face_display_set_gaze_offset() redraws
        // eyes directly from internal pose state, which would otherwise
        // paint over the error-text screen (real hardware: text and eyes
        // visibly alternating) or the music-notes screen (same hazard,
        // found by inspection after the error-text case -- neither
        // face_display_set_music_notes() below nor this block knew about
        // the other, so both would push a redraw independently and fight
        // over s_framebuffer). Face tracking also has nothing useful to
        // show while unreachable, since face detection is itself
        // server-side (no face_position events can arrive).
        bool eyes_and_led_allowed = s_server_reachable && orchestrator_get_state() != HARO_STATE_PLAYING_MUSIC;
        if (eyes_and_led_allowed) {
            if (now >= saccade_next_pick) {
                int range = 2 * SACCADE_MAX_PX + 1;
                saccade_target_dx_px = (float)((int)(esp_random() % (unsigned)range) - SACCADE_MAX_PX);
                saccade_target_dy_px = (float)((int)(esp_random() % (unsigned)range) - SACCADE_MAX_PX);
                saccade_next_pick = now + pdMS_TO_TICKS(SACCADE_MIN_INTERVAL_MS +
                    (esp_random() % (SACCADE_MAX_INTERVAL_MS - SACCADE_MIN_INTERVAL_MS)));
            }

            float target_dx, target_dy;
            float face_dx = 0.0f, face_dy = 0.0f;
            if (camera_face_track_get_offset(&face_dx, &face_dy)) {
                target_dx = face_dx * FACE_TRACK_MAX_PX;
                target_dy = face_dy * FACE_TRACK_MAX_PX;
            } else {
                float sound_dir = 0.0f;
                bool sound_present = wake_word_get_sound_direction(&sound_dir);
                target_dx = saccade_target_dx_px + (sound_present ? sound_dir * SOUND_MAX_PX : 0.0f);
                target_dy = saccade_target_dy_px;
            }
            eye_dx_smooth += (target_dx - eye_dx_smooth) * EYE_OFFSET_SMOOTH_ALPHA;
            eye_dy_smooth += (target_dy - eye_dy_smooth) * EYE_OFFSET_SMOOTH_ALPHA;

            if (now >= eye_next_update) {
                int px_dx = (int)lroundf(eye_dx_smooth);
                int px_dy = (int)lroundf(eye_dy_smooth);
                if (px_dx != eye_last_px_dx || px_dy != eye_last_px_dy) {
                    face_display_set_gaze_offset(px_dx, px_dy);
                    servo_set_pan_angle(90 + (int)lroundf(px_dx * SERVO_DEG_PER_PX));
                    servo_set_tilt_angle(90 + (int)lroundf(px_dy * SERVO_DEG_PER_PX));
                    eye_last_px_dx = px_dx;
                    eye_last_px_dy = px_dy;
                }
                eye_next_update = now + pdMS_TO_TICKS(EYE_OFFSET_UPDATE_MS);
            }
        }

        // Blink: independent of the saccade/sound offset above -- both land
        // in face_display's shared overlay state (see
        // face_display_set_blink()'s header comment), so no coordination is
        // needed here beyond each running on its own schedule. Gated on
        // eyes_and_led_allowed for the same reason as the eye-liveliness
        // block above -- face_display_set_blink() redraws eyes directly and
        // would otherwise paint over the error-text or music-notes screen.
        if (eyes_and_led_allowed) {
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
        // continuously idle (no interaction in flight) AND the server is
        // reachable -- the error-text block below owns the screen while
        // !s_server_reachable, and both blocks pushing redraws on their own
        // independent schedules would otherwise fight over it (alternating
        // idle eyes/error text instead of a clean cut between the two).
        // Losing s_server_reachable resets this the same way any other
        // non-idle state already does (the `else` below), so idle fidgets
        // resume cleanly once reachable again.
        if (orchestrator_get_state() == HARO_STATE_IDLE && s_server_reachable) {
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

        // Music-notes scroll animation: only while actually playing music,
        // driven by polling state the same way the idle fidgets above are
        // -- see face_display_set_music_notes()'s header comment for why
        // this can't just be a single blocking call like the dice/coin
        // reveal.
        if (orchestrator_get_state() == HARO_STATE_PLAYING_MUSIC) {
            if (now >= music_next_update) {
                music_scroll_offset += 3;
                face_display_set_music_notes(music_scroll_offset);
                music_next_update = now + pdMS_TO_TICKS(100);
            }
        } else {
            music_scroll_offset = 0;
        }

        // Server-unreachable screen: red LED + scrolling "server non
        // raggiungibile <ip:port>" text in place of the eyes, for as long
        // as s_server_reachable is false (not just a brief flash -- see
        // that flag's own comment). Same poll-state-every-tick-but-
        // throttle-the-redraw pattern as the blocks above. Re-asserts the
        // red LED on every redraw (not just once on entry) as cheap
        // defense-in-depth against anything else touching status_led while
        // this is showing.
        if (!s_server_reachable) {
            if (now >= error_next_update) {
                error_scroll_offset += 2;
                face_display_set_scrolling_text(s_server_error_text, error_scroll_offset);
                status_led_set_expression(EXPR_ERROR);
                error_next_update = now + pdMS_TO_TICKS(80);
            }
        } else {
            error_scroll_offset = 0;
        }

        server_client_event_t server_evt;
        if (xQueueReceive(s_server_queue, &server_evt, pdMS_TO_TICKS(20)) == pdTRUE) {
            // face_position is camera_face_track's own state, not a
            // conversation event -- orchestrator has no case for it (and
            // shouldn't need one), so it's consumed here instead of being
            // routed through to_orchestrator_event()/
            // orchestrator_on_server_event() like every other protocol
            // event below. SERVER_CLIENT_EVENT_CONNECTED gets the same
            // treatment, for the same reason (see to_orchestrator_event()'s
            // comment on its own CONNECTED case) -- it only updates
            // s_server_reachable here, main.c's own connectivity tracking.
            //
            // s_server_reachable = true also runs for face_position and
            // every non-DISCONNECTED branch below, not just the explicit
            // CONNECTED case: found by inspection that SERVER_CLIENT_EVENT_
            // CONNECTED, like every event on this queue, can be silently
            // dropped by xQueueSend(..., 0) on a full queue ("event queue
            // full, dropping connected event" in server_client.c) -- and
            // unlike a dropped audio chunk, a dropped CONNECTED had no other
            // path back to true, leaving the red error screen showing
            // forever even once the connection was genuinely healthy again.
            // Receiving ANY real data from the server is proof enough that
            // it's reachable, whether or not the CONNECTED event that
            // announced it survived the queue.
            if (server_evt.type == SERVER_CLIENT_EVENT_PROTOCOL &&
                server_evt.protocol_event.type == PROTOCOL_EVENT_FACE_POSITION) {
                s_server_reachable = true;
                camera_face_track_set_remote_position(server_evt.protocol_event.face_found,
                                                       server_evt.protocol_event.face_dx,
                                                       server_evt.protocol_event.face_dy);
            } else if (server_evt.type == SERVER_CLIENT_EVENT_CONNECTED) {
                s_server_reachable = true;
            } else {
                s_server_reachable = (server_evt.type != SERVER_CLIENT_EVENT_DISCONNECTED);
                orchestrator_on_server_event(to_orchestrator_event(&server_evt));
                // Refresh the MAX_THINKING_SPEAKING_MS deadline on every
                // event actually processed while the result is THINKING or
                // SPEAKING -- see that constant's comment above for why
                // this is progress-based, not a fixed deadline from entry.
                haro_state_t state_after_event = orchestrator_get_state();
                if (state_after_event == HARO_STATE_THINKING || state_after_event == HARO_STATE_SPEAKING) {
                    thinking_or_speaking_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MAX_THINKING_SPEAKING_MS);
                }
            }
            if (server_evt.type == SERVER_CLIENT_EVENT_AUDIO) {
                free(server_evt.audio_data);
            }
        }

        // Drain frames currently queued, budgeted -- NOT necessarily every
        // one in a single pass. server_client_send_audio_frame() (inside
        // orchestrator_on_audio_frame() below) can block up to
        // SEND_TIMEOUT_MS per call; a whole-queue drain with no bound on a
        // slow network could stack up to queue-depth * SEND_TIMEOUT_MS of
        // blocking in one iteration -- exactly the kind of loop-starving
        // main.c's other failsafes (MAX_LISTEN_MS, MAX_THINKING_SPEAKING_MS
        // above) exist to recover FROM, not something to reintroduce here.
        // Any frames left over are simply picked up on the next iteration
        // a few milliseconds later. Frames that arrived while we were
        // still listening but that we're processing after state has since
        // moved on (e.g. speech-end just flipped us to THINKING) are freed
        // without forwarding -- `listening` above was sampled once at the
        // top of this iteration and applies to the whole drain, so a frame
        // queued moments before forwarding was disarmed is discarded here
        // rather than sent late.
        #define AUDIO_FRAME_DRAIN_BUDGET_MS 100
        TickType_t drain_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_FRAME_DRAIN_BUDGET_MS);
        wake_word_audio_frame_t frame;
        while (xTaskGetTickCount() < drain_deadline && xQueueReceive(s_audio_frame_queue, &frame, 0) == pdTRUE) {
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
                // Same MAX_THINKING_SPEAKING_MS seeding as the server-event
                // call site above -- this is the OTHER of the only two
                // ways into THINKING/SPEAKING (the end-of-speech branch
                // here transitions LISTENING -> THINKING).
                haro_state_t state_after_frame = orchestrator_get_state();
                if (state_after_frame == HARO_STATE_THINKING || state_after_frame == HARO_STATE_SPEAKING) {
                    thinking_or_speaking_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MAX_THINKING_SPEAKING_MS);
                }
            }
            free(frame.data);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(haro_config_init());
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
    //
    // Deliberately BEFORE wifi_provisioning_ensure_connected() now (used to
    // come after): wifi_provisioning.c shows live WiFi status on this same
    // display (searching/connected/AP-mode icons) while it runs, so the
    // display needs to already be up by then. audio_pipeline_init() itself
    // has no network dependency, so moving it earlier is safe.
    esp_err_t face_err = face_display_init(i2c_bus);
    if (face_err != ESP_OK) {
        ESP_LOGW(TAG, "face_display_init() failed: %s -- continuing without display", esp_err_to_name(face_err));
    }

    ESP_ERROR_CHECK(wifi_provisioning_ensure_connected());

    // Independent GPIO (RMT peripheral, not I2C) -- no shared-bus concerns
    // with face_display/audio_pipeline. Non-fatal on failure, same
    // reasoning as face_display: a missing/misbehaving LED chain shouldn't
    // block the rest of the device from working.
    esp_err_t led_err = status_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "status_led_init() failed: %s -- continuing without status LEDs", esp_err_to_name(led_err));
    }

    // Independent GPIO (LEDC peripheral) -- no shared-bus concerns. Non-
    // fatal on failure, same reasoning as status_led: no physical head-pan
    // servo shouldn't block the rest of the device from working.
    esp_err_t servo_err = servo_init();
    if (servo_err != ESP_OK) {
        ESP_LOGW(TAG, "servo_init() failed: %s -- continuing without head pan", esp_err_to_name(servo_err));
    }

    // Must come after audio_pipeline_init(): shares its I2C bus (for the
    // camera's SCCB control lines and the TCA9555 PWDN/RESET write) the
    // same way face_display does. Non-fatal on failure, same reasoning as
    // face_display/status_led -- no camera means no face tracking, not a
    // broken device.
    esp_err_t cam_err = camera_face_track_init();
    if (cam_err != ESP_OK) {
        ESP_LOGW(TAG, "camera_face_track_init() failed: %s -- continuing without face tracking", esp_err_to_name(cam_err));
    }

    char server_url[128];
    ESP_ERROR_CHECK(haro_config_get_server_url(server_url, sizeof(server_url)));

    // "ip del server ... con la porta specificata", not the full ws://
    // URL -- server_url is always "ws://<host>:<port>" (haro_config.c's
    // CONFIG_HARO_DEFAULT_SERVER_URL default and every wifi_profiles
    // server_url are both shaped this way), so stripping the fixed "ws://"
    // prefix is enough; no general URL parser needed.
    const char *host_port = server_url;
    if (strncmp(host_port, "ws://", 5) == 0) {
        host_port += 5;
    }
    snprintf(s_server_error_text, sizeof(s_server_error_text), "server non raggiungibile   %s   ", host_port);

    s_wake_queue = xQueueCreate(4, sizeof(wake_word_event_type_t));
    // Depth 32 (was 8): confirmed on real hardware that 8 was too shallow
    // during music playback -- server events (paced audio chunks +
    // protocol messages, e.g. face_position replies every ~500ms) arrive
    // in bursts main.c's single orchestrator_task loop iteration can't
    // always drain fast enough, logging "event queue full, dropping audio
    // chunk"/"dropping protocol event" repeatedly and audibly corrupting
    // playback. A deeper queue absorbs those bursts instead of discarding
    // real data.
    s_server_queue = xQueueCreate(32, sizeof(server_client_event_t));
    // Depth 32 (was 8, same real-hardware finding as s_server_queue above):
    // confirmed "feed_task: audio_frame_queue full, dropping frame"
    // repeatedly during the same music-interrupt test that overflowed
    // s_server_queue -- the one loop draining both queues falls behind on
    // one while busy with the other, so both need the same deeper margin.
    s_audio_frame_queue = xQueueCreate(32, sizeof(wake_word_audio_frame_t));

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
        .server = { .send_audio_frame = server_send_audio_frame, .send_end_of_speech = server_send_end_of_speech,
                    .send_interrupt = server_send_interrupt },
        .audio_out = { .play_chunk = audio_play_chunk, .stop = audio_stop },
        .face = { .show = face_show, .show_action = face_show_action },
    };
    orchestrator_init(ops);
    face_display_show(EXPR_IDLE);

    // Boot-time wake-word reminder: scroll which two wake words are active
    // for WAKE_WORD_REMINDER_MS before starting normal operation, so
    // checking which pair is currently flashed (see wake_word.c's file
    // header comment on why the pairing has already changed more than
    // once) never requires a serial monitor -- just watching the screen
    // for a few seconds after power-on. Blocking is deliberate and safe
    // here specifically: orchestrator_task hasn't started yet, so nothing
    // else is competing for the display (unlike face_display_set_
    // scrolling_text()'s other use, the error screen in orchestrator_
    // task's own loop, which has to coexist with everything else that
    // touches the display -- see that call site's gating).
    //
    // The text is a plain literal, not derived from wake_word.c's loaded
    // model names (those are internal identifiers like "wn9_heykira_
    // tts3", not display-friendly) -- keep it in sync by hand alongside
    // sdkconfig.defaults' CONFIG_SR_WN_* selection and wake_word.c's
    // esp_srmodel_filter() strings whenever the pairing changes again.
    #define WAKE_WORD_REMINDER_MS 10000
    {
        const char *reminder_text = "wake words are hey kira and hi wall e   ";
        TickType_t reminder_until = xTaskGetTickCount() + pdMS_TO_TICKS(WAKE_WORD_REMINDER_MS);
        int reminder_scroll_offset = 0;
        while (xTaskGetTickCount() < reminder_until) {
            face_display_set_scrolling_text(reminder_text, reminder_scroll_offset);
            reminder_scroll_offset += 2;
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        face_display_show(EXPR_IDLE);
    }

    ESP_LOGI(TAG, "Haro ready, waiting for wake word");
    // xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM), not plain xTaskCreate():
    // found on real hardware that internal SRAM is down to ~2KB free by
    // this point (WiFi + dual WakeNet models + camera + websocket buffers
    // between them leave almost nothing) -- not enough for a contiguous
    // 4096-byte stack, so xTaskCreate() was failing with -1
    // (errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) and orchestrator_task never
    // ran at all (confirmed: it never even logged entering its loop).
    // PSRAM has megabytes free and this task's own state (mostly floats/
    // ints/enums, no DMA-only-capable buffers) has no reason to need
    // internal-only memory, so moving its stack there is a clean fix. The
    // TCB itself still comes from internal RAM automatically (per
    // idf_additions.h's doc comment) -- only the stack moves.
    BaseType_t task_created = xTaskCreateWithCaps(orchestrator_task, "orchestrator", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps(orchestrator_task) failed: %d", (int)task_created);
    }
}
#endif
