#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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
} wake_word_event_type_t;

// Starts the AFE feed+detect tasks. `event_queue` receives
// wake_word_event_type_t values: WAKE_WORD_DETECTED as WakeNet reports a
// detection, and WAKE_WORD_SPEECH_END once speech following that detection
// falls silent (see the enum comment above).
esp_err_t wake_word_start(QueueHandle_t event_queue);

#ifdef __cplusplus
}
#endif
