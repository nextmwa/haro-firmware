// wake_word: ESP-SR AFE (Audio Front-End) + WakeNet "Hi,ESP" wake word
// detection, adapted from Waveshare's `mic_speech.c` reference, dropping the
// MultiNet/on-device command-recognition half (Haro streams recognized audio
// to a server instead of parsing commands on-device).
//
// Deviations from the Task 5 brief's sketch, found by reading the resolved
// esp-sr 2.5.3 headers under managed_components/espressif__esp-sr/include/esp32s3/
// (see task-5-report.md for the full comparison):
//
//  1. `esp_afe_handle_from_config()` (managed_components/espressif__esp-sr/include/esp32s3/esp_afe_sr_models.h)
//     returns `const esp_afe_sr_iface_t *`, not `esp_afe_sr_iface_t *`. The
//     brief's sketch declared a non-const handle; fixed below.
//
//  2. The brief's `afe_config_init("MR", ...)` assumed a mic+echo-reference
//     input layout ('M' = mic channel, 'R' = playback reference channel, per
//     esp_afe_config.h's doc comment on afe_config_init). This board's
//     components/audio_pipeline never routes the ES8311 speaker output back
//     into the ES7210 ADC path (audio_pipeline.c's init_speaker_codec() only
//     opens a TX-only codec dev; init_mic_codec() only opens an RX-only one)
//     -- there is no echo-reference signal for AFE to consume. The 2 channels
//     audio_pipeline_read() actually delivers are both raw microphone
//     channels. Using "MR" here would make AFE treat live mic audio as an
//     AEC reference signal, which is wrong. Using "MM" (2 mic channels, no
//     reference) instead.
//
//  3. audio_pipeline's mic codec is opened with `bits_per_sample = 32`
//     (components/audio_pipeline/audio_pipeline.c init_mic_codec(), matching
//     the 32-bit I2S Philips slot width used in init_i2s()) -- confirmed by
//     managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c,
//     which sizes I2S DMA words directly off `fs.bits_per_sample`.
//     `audio_pipeline_read()` therefore delivers int32_t-per-sample PCM, not
//     int16_t. esp_afe_sr_iface.h's `feed` op doc is explicit: "only support
//     signed 16-bit @ 16 KHZ". The brief's sketch fed audio_pipeline's raw
//     bytes straight into `feed()`, which would have silently misinterpreted
//     the byte layout (wrong sample count/values, not just a build error).
//     feed_task() below reads native 32-bit frames and truncates each sample
//     to its high 16 bits before feeding AFE.
//
//  4. WakeNet models are opt-in via Kconfig (see
//     managed_components/espressif__esp-sr/Kconfig.projbuild) -- every
//     `SR_WN_*` symbol defaults to `n`, and esp-sr's own build step
//     (movemodel.py, invoked from esp-sr's CMakeLists.txt when
//     CONFIG_PARTITION_TABLE_CUSTOM is set) only packs models whose Kconfig
//     symbol is enabled into the "model" SPIFFS partition. Without enabling
//     one, `esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp")` would resolve
//     to NULL at runtime even though the build succeeds. sdkconfig.defaults
//     sets `CONFIG_SR_WN_WN9_HIESP=y` (the ESP32-S3 WakeNet9 "Hi,ESP" model --
//     confirmed present at managed_components/espressif__esp-sr/model/wakenet_model/wn9_hiesp
//     and listed in wakeword_list.md) to make this concrete.
//
//  5. (Task 10) End-of-speech: `afe_fetch_result_t.vad_state` is already
//     computed on every fetch() call because `afe_config->vad_init = true`
//     below, regardless of wakenet_init -- confirmed in esp_afe_sr_iface.h's
//     struct doc comment ("vad_state: the value is afe_vad_state_t") and in
//     esp_vad.h (`vad_state_t` = { VAD_SILENCE = 0, VAD_SPEECH = 1 }; the
//     deprecated `afe_vad_state_t` alias in esp_afe_sr_iface.h carries the
//     identical two values). detect_task() below tracks that field across
//     fetches after a wake word fires and posts WAKE_WORD_SPEECH_END on the
//     first VAD_SPEECH -> VAD_SILENCE transition it sees, requiring at least
//     one VAD_SPEECH observation first so a wake word that fires mid-silence
//     (vad_state already VAD_SILENCE right at detection, e.g. between the
//     spoken wake phrase and the user's actual question) doesn't immediately
//     end listening before any question audio arrives.
//
//  6. (Task 10 review fix) Single-reader invariant: `i2s_channel_read()`
//     (which `audio_pipeline_read()` wraps, via esp_codec_dev) is a single-
//     consumer, semaphore-serialized read over one DMA ring buffer -- it
//     does not fan out to multiple readers. Task 10's first pass had BOTH
//     feed_task() below and a second task in main.c independently calling
//     audio_pipeline_read() during HARO_STATE_LISTENING, which silently
//     *split* the one physical mic stream between the two callers (each got
//     a fraction, with gaps) instead of each seeing the complete stream --
//     corrupting both AFE's feed (and therefore the vad_state/
//     WAKE_WORD_SPEECH_END signal from item 5) and the raw audio actually
//     forwarded to the server. Fixed by making feed_task() the ONLY caller
//     of audio_pipeline_read() in the whole component graph: it now also
//     hands a copy of each raw frame it reads to `audio_frame_queue`
//     (wake_word_start's new second argument) whenever forwarding is armed
//     via wake_word_set_audio_forwarding(), so main.c's
//     server-audio-forwarding path gets the same complete, gap-free stream
//     AFE does, without a second reader.
#include "wake_word.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include "audio_pipeline.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "wake_word";

static const esp_afe_sr_iface_t *s_afe_handle;
static QueueHandle_t s_event_queue;
// May be NULL (caller doesn't want raw audio). See file header comment
// item 6 and wake_word.h's wake_word_audio_frame_t.
static QueueHandle_t s_audio_frame_queue;
// Written only from wake_word_set_audio_forwarding() (any task), read only
// from feed_task(). A plain bool is sufficient here: ESP32-S3 word-aligned
// reads/writes are atomic in practice, there's exactly one writer, and the
// consequence of a torn read is at worst one stale/skipped frame decision,
// not a corrupted stream -- `volatile` is enough to stop the compiler from
// caching feed_task()'s read of it across loop iterations.
static volatile bool s_audio_forwarding_enabled;
// Kept alive for the lifetime of the AFE instance: wakenet_model_name (and
// the model coefficient data AFE reads at runtime) point into memory owned
// by this list, which esp_srmodel_init() may mmap directly from the "model"
// partition rather than copy.
static srmodel_list_t *s_models;

void wake_word_set_audio_forwarding(bool enable)
{
    s_audio_forwarding_enabled = enable;
}

static void feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int chunk_size = s_afe_handle->get_feed_chunksize(afe_data);   // samples per channel
    int channels = s_afe_handle->get_feed_channel_num(afe_data);   // total feed channels ("MM" -> 2)
    size_t frame_samples = (size_t)chunk_size * (size_t)channels;
    size_t raw_bytes = frame_samples * sizeof(int32_t);

    // audio_pipeline's native format is 32-bit/sample (see file header
    // comment #3); AFE's feed() requires 16-bit/sample. Two buffers: one for
    // the raw read, one truncated for AFE. `raw` is also the source data
    // copied out to audio_frame_queue when forwarding is armed (item 6).
    int32_t *raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_SPIRAM);
    int16_t *pcm16 = heap_caps_malloc(frame_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (raw == NULL || pcm16 == NULL) {
        ESP_LOGE(TAG, "feed_task: buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        size_t bytes_read = 0;
        esp_err_t err = audio_pipeline_read(raw, raw_bytes, &bytes_read);
        if (err != ESP_OK || bytes_read != raw_bytes) {
            ESP_LOGW(TAG, "feed_task: short/failed read (%s, %u bytes)", esp_err_to_name(err), (unsigned)bytes_read);
            continue;
        }

        if (s_audio_frame_queue != NULL && s_audio_forwarding_enabled) {
            uint8_t *copy = heap_caps_malloc(raw_bytes, MALLOC_CAP_SPIRAM);
            if (copy == NULL) {
                ESP_LOGW(TAG, "feed_task: audio frame forward alloc failed, dropping frame");
            } else {
                memcpy(copy, raw, raw_bytes);
                wake_word_audio_frame_t frame = { .data = copy, .len = raw_bytes };
                if (xQueueSend(s_audio_frame_queue, &frame, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "feed_task: audio_frame_queue full, dropping frame");
                    heap_caps_free(copy);
                }
            }
        }

        for (size_t i = 0; i < frame_samples; i++) {
            pcm16[i] = (int16_t)(raw[i] >> 16);
        }
        s_afe_handle->feed(afe_data, pcm16);
    }
}

static void detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    // Set once a wake word fires, cleared once WAKE_WORD_SPEECH_END is
    // posted for it -- gates end-of-speech tracking to "listening" periods.
    bool awaiting_speech_end = false;
    // Set the first time vad_state == VAD_SPEECH is observed while
    // awaiting_speech_end is true; only after this do we treat a VAD_SILENCE
    // reading as the end of speech (see file header comment item 5).
    bool seen_speech = false;

    while (true) {
        afe_fetch_result_t *res = s_afe_handle->fetch(afe_data);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch error");
            continue;
        }
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected (word index %d)", res->wake_word_index);
            wake_word_event_type_t evt = WAKE_WORD_DETECTED;
            if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "event_queue full, dropped WAKE_WORD_DETECTED");
            }
            awaiting_speech_end = true;
            seen_speech = false;
        }

        if (awaiting_speech_end) {
            if (res->vad_state == VAD_SPEECH) {
                seen_speech = true;
            } else if (res->vad_state == VAD_SILENCE && seen_speech) {
                ESP_LOGI(TAG, "speech end detected");
                wake_word_event_type_t evt = WAKE_WORD_SPEECH_END;
                if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "event_queue full, dropped WAKE_WORD_SPEECH_END");
                }
                awaiting_speech_end = false;
                seen_speech = false;
            }
        }
    }
}

esp_err_t wake_word_start(QueueHandle_t event_queue, QueueHandle_t audio_frame_queue)
{
    if (event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_event_queue = event_queue;
    s_audio_frame_queue = audio_frame_queue;  // may be NULL; see file header comment item 6

    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed (no models in \"model\" partition)");
        return ESP_FAIL;
    }

    char *wakenet_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "hiesp");
    if (wakenet_model_name == NULL) {
        ESP_LOGE(TAG, "\"hiesp\" WakeNet model not found -- check CONFIG_SR_WN_WN9_HIESP in sdkconfig");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "using WakeNet model: %s", wakenet_model_name);

    afe_config_t *afe_config = afe_config_init("MM", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = wakenet_model_name;
    afe_config->vad_init = true;
    afe_config = afe_config_check(afe_config);

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data == NULL) {
        ESP_LOGE(TAG, "AFE init failed");
        return ESP_FAIL;
    }

    xTaskCreatePinnedToCore(detect_task, "wake_word_detect", 8192, afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(feed_task, "wake_word_feed", 8192, afe_data, 5, NULL, 0);
    return ESP_OK;
}
