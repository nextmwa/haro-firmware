#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVER_CLIENT_EVENT_PROTOCOL,
    SERVER_CLIENT_EVENT_AUDIO,
    SERVER_CLIENT_EVENT_DISCONNECTED,
} server_client_event_type_t;

typedef struct {
    server_client_event_type_t type;
    protocol_event_t protocol_event;
    uint8_t *audio_data;
    size_t audio_len;
} server_client_event_t;

esp_err_t server_client_init(const char *url, QueueHandle_t event_queue);
esp_err_t server_client_send_hello(const char *session_id);
esp_err_t server_client_send_audio_frame(const uint8_t *data, size_t len);
esp_err_t server_client_send_end_of_speech(void);

#ifdef __cplusplus
}
#endif
