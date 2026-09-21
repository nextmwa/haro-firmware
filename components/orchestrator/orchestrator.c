#include "orchestrator.h"
#include <string.h>

static orchestrator_ops_t s_ops;
static haro_state_t s_state = HARO_STATE_IDLE;

// Must match face_display.h's face_expression_t values exactly; duplicated
// here only as plain ints so this component doesn't need to depend on
// face_display. Each entry carries its own explicit `= N` (matching that
// header's ordinals) rather than relying on declaration order -- that
// header only ever appends new values, but pinning the numbers here too
// means a future reordering on either side fails loud (mismatched values)
// instead of silently desyncing.
enum {
    EXPR_IDLE = 0, EXPR_LISTENING = 1, EXPR_THINKING = 2, EXPR_SPEAKING_HAPPY = 3, EXPR_SPEAKING_SAD = 4,
    EXPR_SPEAKING_CONFUSED = 5, EXPR_SPEAKING_NEUTRAL = 6, EXPR_ERROR = 7, EXPR_SETUP = 8,
    // 9-11 (EXPR_BORED/EXPR_LOOKING_LEFT/EXPR_LOOKING_RIGHT) have no
    // orchestrator-side use -- main.c's idle animation calls
    // face_display_show() with them directly.
    EXPR_ANGRY = 12, EXPR_DISGUSTED = 13, EXPR_SURPRISED = 14, EXPR_FEARFUL = 15,
};

void orchestrator_init(orchestrator_ops_t ops)
{
    s_ops = ops;
    s_state = HARO_STATE_IDLE;
}

haro_state_t orchestrator_get_state(void)
{
    return s_state;
}

void orchestrator_on_wake_word(void)
{
    if (s_state == HARO_STATE_PLAYING_MUSIC) {
        // Interrupting music: stop local playback immediately and tell the
        // server to stop streaming more of the track, then fall through to
        // the normal "start listening" transition below -- unlike every
        // other non-IDLE state, this one doesn't just ignore a wake word.
        s_ops.audio_out.stop(s_ops.audio_out.ctx);
        if (s_ops.server.send_interrupt) {
            s_ops.server.send_interrupt(s_ops.server.ctx);
        }
    } else if (s_state != HARO_STATE_IDLE) {
        return;
    }
    s_state = HARO_STATE_LISTENING;
    s_ops.face.show(s_ops.face.ctx, EXPR_LISTENING);
}

void orchestrator_on_audio_frame(const uint8_t *frame, size_t len, bool is_end_of_speech)
{
    if (s_state != HARO_STATE_LISTENING) return;

    s_ops.server.send_audio_frame(s_ops.server.ctx, frame, len);

    if (is_end_of_speech) {
        s_state = HARO_STATE_THINKING;
        s_ops.face.show(s_ops.face.ctx, EXPR_THINKING);
        s_ops.server.send_end_of_speech(s_ops.server.ctx);
    }
}

static int expression_for_emotion(const char *value)
{
    if (strcmp(value, "happy") == 0) return EXPR_SPEAKING_HAPPY;
    if (strcmp(value, "sad") == 0) return EXPR_SPEAKING_SAD;
    if (strcmp(value, "confused") == 0) return EXPR_SPEAKING_CONFUSED;
    // Newer tags matching face_display's Anki Cozmo mood reference; the
    // Python server's emotion vocabulary doesn't send these yet as of this
    // change, but the firmware side is ready for them.
    if (strcmp(value, "angry") == 0) return EXPR_ANGRY;
    if (strcmp(value, "disgusted") == 0) return EXPR_DISGUSTED;
    if (strcmp(value, "surprised") == 0) return EXPR_SURPRISED;
    if (strcmp(value, "fearful") == 0) return EXPR_FEARFUL;
    return EXPR_SPEAKING_NEUTRAL;
}

static void return_to_idle(void)
{
    s_ops.audio_out.stop(s_ops.audio_out.ctx);
    s_state = HARO_STATE_IDLE;
    s_ops.face.show(s_ops.face.ctx, EXPR_IDLE);
}

void orchestrator_on_server_event(orchestrator_server_event_t event)
{
    if (s_state != HARO_STATE_THINKING && s_state != HARO_STATE_SPEAKING && s_state != HARO_STATE_PLAYING_MUSIC) {
        if (event.type != ORCHESTRATOR_SERVER_EVENT_DISCONNECTED) return;
    }

    switch (event.type) {
    case ORCHESTRATOR_SERVER_EVENT_PROTOCOL:
        if (event.protocol_event.type == PROTOCOL_EVENT_EMOTION) {
            s_state = HARO_STATE_SPEAKING;
            s_ops.face.show(s_ops.face.ctx, expression_for_emotion(event.protocol_event.value));
        } else if (event.protocol_event.type == PROTOCOL_EVENT_RESPONSE_END) {
            return_to_idle();
        } else if (event.protocol_event.type == PROTOCOL_EVENT_ERROR) {
            s_ops.face.show(s_ops.face.ctx, EXPR_ERROR);
            return_to_idle();
        } else if (event.protocol_event.type == PROTOCOL_EVENT_ACTION) {
            if (strcmp(event.protocol_event.action_name, "music_playing") == 0) {
                // Long-running, wake-word-interruptible playback -- driven
                // by main.c's own state-polling loop (a scrolling note
                // animation), not the one-shot blocking show_action()
                // reveal dice/coin use below. No face_display call here at
                // all: main.c already polls orchestrator_get_state() every
                // tick for the idle-fidget cycle, and does the same for
                // this state.
                s_state = HARO_STATE_PLAYING_MUSIC;
            } else {
                // No audio for these actions (session.py sends action +
                // response_end back to back, no TTS) -- SPEAKING here just
                // borrows the state that keeps the guard above accepting
                // the response_end that follows immediately after, not
                // because anything is playing.
                s_state = HARO_STATE_SPEAKING;
                if (s_ops.face.show_action) {
                    s_ops.face.show_action(s_ops.face.ctx, event.protocol_event.action_name,
                                            event.protocol_event.action_result);
                }
            }
        }
        break;
    case ORCHESTRATOR_SERVER_EVENT_AUDIO:
        // Deliberately does NOT downgrade PLAYING_MUSIC back to SPEAKING:
        // every subsequent chunk of the same track re-enters this case,
        // and forcing SPEAKING here would silently lose the
        // wake-word-interrupts-music behavior (orchestrator_on_wake_word()
        // only special-cases PLAYING_MUSIC specifically) after the very
        // first chunk.
        if (s_state != HARO_STATE_PLAYING_MUSIC) {
            s_state = HARO_STATE_SPEAKING;
        }
        s_ops.audio_out.play_chunk(s_ops.audio_out.ctx, event.audio_data, event.audio_len);
        break;
    case ORCHESTRATOR_SERVER_EVENT_DISCONNECTED:
        // Already idle: nothing to reset, so skip return_to_idle()'s
        // audio_out.stop() call. That call used to be a harmless no-op
        // (see audio_pipeline_stop_playback()'s comment for why it now
        // genuinely closes/reopens the speaker codec, audibly) -- and a
        // flaky WiFi link reconnecting repeatedly can fire this DISCONNECTED
        // event many times in a row while nothing has changed in between
        // (confirmed on real hardware: a "Reconnect after 1000 ms" cycle
        // repeating for 30+ seconds, each attempt re-triggering this case).
        // Without this guard, every one of those redundant events was an
        // audible pop through the speaker for no reason -- a rapid,
        // repeating "tatatata" instead of one clean stop.
        if (s_state == HARO_STATE_IDLE) {
            break;
        }
        s_ops.face.show(s_ops.face.ctx, EXPR_ERROR);
        return_to_idle();
        break;
    }
}

void orchestrator_force_idle(void)
{
    if (s_state == HARO_STATE_IDLE) {
        return;
    }
    s_ops.face.show(s_ops.face.ctx, EXPR_ERROR);
    return_to_idle();
}
