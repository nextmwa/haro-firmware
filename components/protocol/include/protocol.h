#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROTOCOL_EVENT_EMOTION,
    PROTOCOL_EVENT_RESPONSE_END,
    PROTOCOL_EVENT_ERROR,
} protocol_event_type_t;

typedef struct {
    protocol_event_type_t type;
    char value[32];
    char message[128];
} protocol_event_t;

char *protocol_encode_hello(const char *session_id);
char *protocol_encode_end_of_speech(void);
esp_err_t protocol_parse_server_message(const char *text, protocol_event_t *out);

#ifdef __cplusplus
}
#endif
