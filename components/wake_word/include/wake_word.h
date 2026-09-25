#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKE_WORD_DETECTED,
    // Posted once per wake-word trigger, when AFE's per-fetch vad_state
    // transitions from VAD_SPEECH back to VAD_SILENCE after having been seen
    // in VAD_SPEECH at least once since the last WAKE_WORD_DETECTED. This is
    // the real end-of-speech signal Task 10's orchestrator wiring needs for
    // `orchestrator_on_audio_frame`'s `is_end_of_speech` argument while in
    // HARO_STATE_LISTENING -- see wake_word.c top-of-file comment (item 5)
    // for why this lives here rather than as a separate silence-timer
    // component: AFE already computes vad_state on every fetch (vad_init is
    // enabled below), so duplicating that work elsewhere would just be a
    // second, less accurate VAD running over the same audio.
    WAKE_WORD_SPEECH_END,
    // Follow-up window (wake_word_arm_follow_up()): near-field speech was
    // detected, so the user is answering without repeating the wake word.
    // From here it behaves exactly like after WAKE_WORD_DETECTED --
    // WAKE_WORD_SPEECH_END follows when they stop talking.
    WAKE_WORD_FOLLOW_UP_SPEECH,
    // The follow-up window elapsed with no near-field speech.
    WAKE_WORD_FOLLOW_UP_TIMEOUT,
} wake_word_event_type_t;

// One raw mic frame, as read by wake_word's feed_task -- the SAME bytes fed
// to AFE (pre-truncation, i.e. still audio_pipeline's native 32-bit/sample
// format; see wake_word.c file header comment item 3), handed off to
// whoever wants a copy of the live mic stream without calling
// audio_pipeline_read() themselves.
//
// `data` is heap-allocated (heap_caps_malloc, MALLOC_CAP_SPIRAM); the
// receiver takes ownership and must free() it after use.
typedef struct {
    uint8_t *data;
    size_t len;
} wake_word_audio_frame_t;

// Starts the AFE feed+detect tasks. `event_queue` receives
// wake_word_event_type_t values: WAKE_WORD_DETECTED as WakeNet reports a
// detection, and WAKE_WORD_SPEECH_END once speech following that detection
// falls silent (see the enum comment above).
//
// `audio_frame_queue` (may be NULL if the caller doesn't need raw audio)
// receives wake_word_audio_frame_t values -- see that struct's comment.
// Frames are only produced while forwarding is armed via
// wake_word_set_audio_forwarding(true); this is the single physical reader
// of the mic (audio_pipeline_read() is called from nowhere else in the
// component graph -- see wake_word.c file header comment item 6), so any
// other consumer of the live mic stream (e.g. main.c's
// server-audio-forwarding path during HARO_STATE_LISTENING) must go through
// this queue rather than reading audio_pipeline directly.
esp_err_t wake_word_start(QueueHandle_t event_queue, QueueHandle_t audio_frame_queue);

// Opens a window of `window_ms` in which the user can keep talking without
// the wake word (right after Haro finished a reply). Only close, sustained
// speech counts -- VAD speech at a near-field volume for a minimum time --
// so background noise and people talking across the room don't open a new
// turn. Posts WAKE_WORD_FOLLOW_UP_SPEECH or WAKE_WORD_FOLLOW_UP_TIMEOUT.
// A real wake word during the window cancels it.
void wake_word_arm_follow_up(uint32_t window_ms);
void wake_word_cancel_follow_up(void);

// Arms/disarms production of wake_word_audio_frame_t values onto
// `audio_frame_queue` (see wake_word_start). Safe to call from any task;
// takes effect on feed_task's next read. Frames are dropped (not queued,
// and no allocation performed) while disarmed, so callers that don't
// currently need the raw stream (e.g. outside HARO_STATE_LISTENING) should
// disarm it rather than draining and discarding a queue.
void wake_word_set_audio_forwarding(bool enable);

// Coarse "which mic channel is louder" estimate, for a decorative
// look-toward-sound effect -- NOT real sound localization. Returns true and
// fills *direction with a normalized value in [-1, 1] (sign convention:
// negative = hardware channel 0 louder, positive = channel 1 louder; this
// board's physical mic-to-channel layout was never confirmed against a
// datasheet, so treat the sign as "a direction", not a verified left/right)
// if a sufficiently loud transient was seen within the last couple of
// seconds. Returns false (do not use *direction) otherwise, e.g. during
// quiet/ambient audio. Safe to call from any task; backed by feed_task's
// existing per-frame amplitude tracking, no extra CPU cost.
bool wake_word_get_sound_direction(float *direction);

#ifdef __cplusplus
}
#endif
