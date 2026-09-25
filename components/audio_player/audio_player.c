#include "audio_player.h"
#include "audio_pipeline.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>

static const char *TAG = "audio_player";

// 3s of 16kHz mono PCM16. haro-server keeps at most ~1.5s ahead of
// playback (server.py's audio pacing), so this leaves margin for network
// jitter. PSRAM: internal RAM is reserved for WiFi (see sdkconfig.defaults).
#define BUFFER_BYTES (3 * 16000 * 2)
// Per I2S write: 32ms of audio, small enough that a stop request is
// honored quickly. Kept even (whole 16-bit samples).
#define WRITE_CHUNK_BYTES 1024
// After the last I2S write returns, the DMA ring (audio_pipeline.c: 6 x 240
// frames) still holds up to ~90ms of audio to clock out.
#define DMA_TAIL_US 150000

static StreamBufferHandle_t s_buffer;
static _Atomic bool s_accepting;
static _Atomic bool s_stop_requested;
static _Atomic bool s_busy;           // holding bytes taken from s_buffer, not yet written
static _Atomic int64_t s_last_write_end_us;
static _Atomic unsigned s_dropped_bytes;

static void discard_buffered(void)
{
    uint8_t scratch[256];
    while (xStreamBufferReceive(s_buffer, scratch, sizeof(scratch), 0) > 0) {
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[WRITE_CHUNK_BYTES];
    size_t have = 0; // bytes in chunk[] not yet written (an odd tail byte waits here)
    while (true) {
        if (s_stop_requested) {
            discard_buffered();
            have = 0;
            s_busy = false;
            audio_pipeline_stop_playback();
            s_stop_requested = false;
        }
        size_t got = xStreamBufferReceive(s_buffer, chunk + have, sizeof(chunk) - have, pdMS_TO_TICKS(20));
        have += got;
        s_busy = have > 0;
        size_t whole = have & ~(size_t)1;
        if (whole > 0 && !s_stop_requested) {
            audio_pipeline_write(chunk, whole);
            s_last_write_end_us = esp_timer_get_time();
            if (have > whole) {
                chunk[0] = chunk[whole];
            }
            have -= whole;
            s_busy = have > 0;
        }
        unsigned dropped = atomic_exchange(&s_dropped_bytes, 0);
        if (dropped) {
            ESP_LOGW(TAG, "buffer full, dropped %u bytes of audio", dropped);
        }
    }
}

esp_err_t audio_player_start(void)
{
    s_buffer = xStreamBufferCreateWithCaps(BUFFER_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (s_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Internal-RAM stack: this task never touches flash, but it runs
    // constantly and its stack is small.
    if (xTaskCreatePinnedToCore(playback_task, "audio_player", 3072, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_player_write(const uint8_t *data, size_t len)
{
    if (!s_accepting || len == 0) {
        return;
    }
    size_t sent = xStreamBufferSend(s_buffer, data, len, 0);
    if (sent < len) {
        s_dropped_bytes += (unsigned)(len - sent);
    }
}

void audio_player_stop(void)
{
    s_stop_requested = true;
    for (int i = 0; i < 25 && s_stop_requested; i++) { // up to ~250ms
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void audio_player_set_accepting(bool accepting)
{
    bool was = atomic_exchange(&s_accepting, accepting);
    if (was && !accepting) {
        audio_player_stop();
    }
}

bool audio_player_is_idle(void)
{
    return xStreamBufferIsEmpty(s_buffer) == pdTRUE && !s_busy &&
           esp_timer_get_time() - s_last_write_end_us >= DMA_TAIL_US;
}
