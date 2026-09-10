#include "orchestrator.h"
#include <string.h>

static orchestrator_ops_t s_ops;
static haro_state_t s_state = HARO_STATE_IDLE;

// Must match face_display.h's Expression enum values (Task 8) once wired in main.c;
// duplicated here only as plain ints so this component doesn't need to depend on face_display.
enum { EXPR_IDLE, EXPR_LISTENING, EXPR_THINKING, EXPR_SPEAKING_HAPPY, EXPR_SPEAKING_SAD,
       EXPR_SPEAKING_CONFUSED, EXPR_SPEAKING_NEUTRAL, EXPR_ERROR, EXPR_SETUP };

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
    if (s_state != HARO_STATE_IDLE) return;
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
    if (s_state != HARO_STATE_THINKING && s_state != HARO_STATE_SPEAKING) {
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
        }
        break;
    case ORCHESTRATOR_SERVER_EVENT_AUDIO:
        s_state = HARO_STATE_SPEAKING;
        s_ops.audio_out.play_chunk(s_ops.audio_out.ctx, event.audio_data, event.audio_len);
        break;
    case ORCHESTRATOR_SERVER_EVENT_DISCONNECTED:
        s_ops.face.show(s_ops.face.ctx, EXPR_ERROR);
        return_to_idle();
        break;
    }
}
