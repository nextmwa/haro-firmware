#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKE_WORD_DETECTED,
} wake_word_event_type_t;

// Starts the AFE feed+detect tasks. `event_queue` receives
// wake_word_event_type_t values (WAKE_WORD_DETECTED) as WakeNet reports a
// detection. VAD/end-of-speech is not produced here -- see wake_word.c
// top-of-file comment for why.
esp_err_t wake_word_start(QueueHandle_t event_queue);

#ifdef __cplusplus
}
#endif
