#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Streaming playback of the server's 16kHz mono PCM16 audio (TTS replies,
// announcements, music) through its own PSRAM buffer and playback task,
// instead of one queued event per chunk in main.c's shared server-event
// queue. Found on real hardware (2026-09-24): with audio in that queue,
// main.c's loop played one chunk per iteration (blocking ~80ms each) and
// could not keep up with audio plus 5/s face-position messages, so the
// 32-slot queue overflowed -- audio chunks were dropped (choppy, "random
// blocks") and so was response_end, leaving Haro stuck in SPEAKING until
// the 60s failsafe turned the LED red.
//
// The playback task is the only caller of audio_pipeline_write() and
// audio_pipeline_stop_playback() once started, so the codec is never
// closed/reopened while a write is in flight.

esp_err_t audio_player_start(void);

// Appends audio. Never blocks (it's called from the WebSocket task): bytes
// that don't fit are dropped and logged -- with haro-server pacing its
// sends to real time this shouldn't happen. Ignored entirely while not
// accepting (see audio_player_set_accepting()).
void audio_player_write(const uint8_t *data, size_t len);

// Whether incoming audio is played at all. main.c turns this on only while
// the conversation state expects audio (THINKING/SPEAKING/PLAYING_MUSIC),
// so audio still in flight after an interrupt can't leak into LISTENING.
// Turning it off also stops playback, like audio_player_stop().
void audio_player_set_accepting(bool accepting);

// Discards everything buffered and stops the codec (interrupt, end of a
// turn). Waits briefly for the playback task to carry it out.
void audio_player_stop(void);

// True once everything written so far has actually been played -- main.c
// holds a response_end back until then, so a reply's tail isn't cut off.
bool audio_player_is_idle(void);

#ifdef __cplusplus
}
#endif
