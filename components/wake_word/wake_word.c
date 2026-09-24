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
//     one, `esp_srmodel_filter(models, ESP_WN_PREFIX, "heykira")` would
//     resolve to NULL at runtime even though the build succeeds.
//     sdkconfig.defaults sets `CONFIG_SR_WN_WN9_HEYKIRA_TTS3=y` and
//     `CONFIG_SR_WN_WN9_HIWALLE_TTS2=y` (confirmed present at
//     managed_components/espressif__esp-sr/model/wakenet_model/wn9_heykira_tts3
//     and .../wn9_hiwalle_tts2) to make this concrete.
//
//     Two wake words, not one: esp_afe_config_t has wakenet_model_name
//     (wakenet 1) AND wakenet_model_name_2 (wakenet 2) -- see
//     wake_word_start() below -- each ~290KB against the 5900KB "model"
//     partition, so running both is not a size concern. They DO need to be
//     mutually compatible, though: confirmed on real hardware that a first
//     attempt pairing Jarvis with "Hey,GiGi" crash-looped on every single
//     boot with `assert failed: model_detect_mfcc wakenet9_quantized.c:1008
//     (cq->n == model->layers[0]->n)` -- a real dimension mismatch inside
//     the WakeNet library itself, not a config mistake. Each model's
//     `_MODEL_INFO_` file encodes its internal hidden-layer size in its
//     name as "...h<N>..." -- Hey,Kira and Hi,Wall,E are both "h12"
//     (Jarvis/Alexa, this pairing's predecessor, were both "h8"; Hey,GiGi
//     is "h18" -- none of these three sizes are interchangeable with each
//     other). Two WakeNet models sharing the same AFE feature pipeline
//     need matching hidden-layer sizes; pick any new second wake word by
//     checking its `_MODEL_INFO_` "h<N>" against the first one's before
//     swapping either.
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
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

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

// Backing state for wake_word_get_sound_direction() -- see wake_word.h for
// the contract. Written every chunk from feed_task, read from whatever task
// wants a "look toward sound" cue (main.c's orchestrator_task); int64_t
// isn't word-aligned-safe as a plain volatile on a 32-bit MCU, hence
// explicit atomics here rather than this file's usual volatile-bool pattern.
#define SOUND_DIRECTION_LOUD_THRESHOLD 1500 // peak sample magnitude (of 32767) to count as "a loud sound" -- normal speech at ~20-30cm peaked in the 1000-8000 range on real hardware
#define SOUND_DIRECTION_STALE_MS 1500
static _Atomic float s_sound_direction;
static _Atomic int64_t s_sound_last_loud_us;

bool wake_word_get_sound_direction(float *direction)
{
    int64_t last_loud = atomic_load(&s_sound_last_loud_us);
    if (last_loud == 0 || esp_timer_get_time() - last_loud > (int64_t)SOUND_DIRECTION_STALE_MS * 1000) {
        return false;
    }
    *direction = atomic_load(&s_sound_direction);
    return true;
}

void wake_word_set_audio_forwarding(bool enable)
{
    s_audio_forwarding_enabled = enable;
}

// audio_pipeline's codec is opened with 2 hardware channels regardless of
// how many channels AFE is fed (see audio_pipeline.c's init_mic_codec(),
// fs.channel = 2) -- the I2S/TDM capture geometry doesn't change just
// because AFE's afe_config_init() string below is "M" (1 mic channel, BSS
// bypassed) rather than "MM" (2-mic BSS/beamforming). "M" is the confirmed
// setting on real hardware: "MM"'s BSS stage assumes a specific mic array
// geometry, and on this board it degraded detection rather than helping it
// (wake word did not reliably trigger until switched to "M" + the ES7210
// gain fix in audio_pipeline.c's init_mic_codec()). feed_task reads full
// 2-channel hardware frames and extracts just channel 0 before handing
// samples to AFE, so the hardware read size must stay independent of AFE's
// (smaller) feed channel
// count.
#define HW_CHANNELS 2

static void feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int chunk_size = s_afe_handle->get_feed_chunksize(afe_data);   // samples per channel
    int afe_channels = s_afe_handle->get_feed_channel_num(afe_data);   // total feed channels ("M" -> 1)
    size_t hw_frame_samples = (size_t)chunk_size * HW_CHANNELS;
    size_t raw_bytes = hw_frame_samples * sizeof(int32_t);
    size_t afe_frame_samples = (size_t)chunk_size * (size_t)afe_channels;

    // audio_pipeline's native format is 32-bit/sample (see file header
    // comment #3); AFE's feed() requires 16-bit/sample. Two buffers: one for
    // the raw read (always HW_CHANNELS-wide), one truncated/channel-selected
    // for AFE. `raw` is also the source data copied out to audio_frame_queue
    // when forwarding is armed (item 6).
    int32_t *raw = heap_caps_malloc(raw_bytes, MALLOC_CAP_SPIRAM);
    int16_t *pcm16 = heap_caps_malloc(afe_frame_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
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
            // vTaskDelay, not a bare continue: audio_pipeline_read()
            // returns immediately (not blocking) on a codec/I2S error, so
            // a PERSISTENT fault here spun this loop at 100% CPU with no
            // yield -- on core 0 at priority 5, found by whole-codebase
            // review to be enough to starve IDLE0 and trip the task
            // watchdog. One tick is enough to let the scheduler breathe
            // without adding meaningful latency to the (rare) real error
            // case this guards.
            vTaskDelay(1);
            continue;
        }

        int16_t chunk_peak[HW_CHANNELS] = { 0, 0 };
        for (size_t i = 0; i < hw_frame_samples; i++) {
            int16_t sample16 = (int16_t)(raw[i] >> 16);
            int ch = (int)(i % HW_CHANNELS);
            int16_t mag = (sample16 < 0) ? (int16_t)(-sample16) : sample16;
            if (mag > chunk_peak[ch]) chunk_peak[ch] = mag;

            if (ch == 0) {
                pcm16[i / HW_CHANNELS] = sample16;   // AFE gets hardware channel 0 only
            }
        }

        // Sound-direction cue for the "look toward sound" idle behavior --
        // see wake_word_get_sound_direction()'s comment above. Updated every
        // chunk (~tens of ms).
        if (chunk_peak[0] >= SOUND_DIRECTION_LOUD_THRESHOLD || chunk_peak[1] >= SOUND_DIRECTION_LOUD_THRESHOLD) {
            float total = (float)chunk_peak[0] + (float)chunk_peak[1];
            float direction = (total > 0.0f) ? ((float)chunk_peak[1] - (float)chunk_peak[0]) / total : 0.0f;
            atomic_store(&s_sound_direction, direction);
            atomic_store(&s_sound_last_loud_us, esp_timer_get_time());
        }

        // Found on real hardware: the server's STT decodes incoming audio
        // as 16-bit mono PCM (haro_server/stt.py's _bytes_to_float32 does
        // np.frombuffer(..., dtype=np.int16)) -- forwarding `raw` here (this
        // board's native 32-bit/2-channel capture format) sent 4x too many
        // bytes per sample-frame with a completely different bit layout,
        // which the server silently misdecoded as noise. Every real turn
        // came back "empty transcript, skipping LLM/TTS turn" as a result.
        // `pcm16` above is already exactly the wire format the server
        // expects (16-bit, single channel, same 16kHz capture rate) --
        // forward a copy of THAT instead of `raw`.
        if (s_audio_frame_queue != NULL && s_audio_forwarding_enabled) {
            size_t pcm16_bytes = afe_frame_samples * sizeof(int16_t);
            uint8_t *copy = heap_caps_malloc(pcm16_bytes, MALLOC_CAP_SPIRAM);
            if (copy == NULL) {
                ESP_LOGW(TAG, "feed_task: audio frame forward alloc failed, dropping frame");
            } else {
                memcpy(copy, pcm16, pcm16_bytes);
                wake_word_audio_frame_t frame = { .data = copy, .len = pcm16_bytes };
                if (xQueueSend(s_audio_frame_queue, &frame, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "feed_task: audio_frame_queue full, dropping frame");
                    heap_caps_free(copy);
                }
            }
        }

        s_afe_handle->feed(afe_data, pcm16);
    }
}

// Found on real hardware: speech-end fired on the very first VAD_SILENCE
// reading after any speech at all, which is far too trigger-happy for
// natural speech -- a brief pause right after the wake word (before the
// user's actual question even starts) was enough to end listening and
// forward almost nothing, reliably producing an empty STT transcript. A
// real end-of-speech detector needs SUSTAINED silence, not a single silent
// frame, so normal pauses (after the wake word, mid-sentence) don't get
// mistaken for "done talking". 700ms is a common voice-assistant
// trailing-silence threshold: long enough to survive a natural breath,
// short enough not to make the device feel unresponsive once the user
// really has finished.
#define SPEECH_END_SILENCE_MS 700

// Found on real hardware (still cutting users off after "Jarvis", even
// with the 700ms threshold above): the wake phrase itself IS speech, so
// AFE's vad_state is already VAD_SPEECH at the moment WAKENET_DETECTED
// fires -- seen_speech (file header item 5) goes true from the tail of
// the wake word's own audio, not from the user's actual command. The
// natural pause everyone makes after saying a wake word (a habit trained
// by assistants that beep first) is then enough on its own to cross
// SPEECH_END_SILENCE_MS before the command has even started, ending
// listening with little or nothing captured. Ignoring VAD entirely for a
// short grace window right after detection -- not counting it toward
// seen_speech OR toward a silence run -- gives that pause somewhere to
// go: only speech/silence transitions from the actual command, observed
// after the grace window, can now trigger end-of-speech. 500ms covers
// the reported case without making the device feel sluggish; the
// mid-silence-wakeup protection from item 5 (a wake word firing after
// vad_state has already decayed to VAD_SILENCE) still applies once the
// grace window elapses.
#define WAKE_WORD_GRACE_MS 500

// Reported on real hardware (2026-09-24): with other people talking in the
// room, VAD correctly keeps reporting VAD_SPEECH after the user has
// finished -- it detects *a* voice, not *the user's* -- so listening only
// ended at main.c's MAX_LISTEN_MS failsafe, with the other voices forwarded
// to STT too. The user speaks right next to the device and is much louder
// than anyone across the room, so a frame whose volume (AFE's own
// afe_fetch_result_t.data_volume, dB, computed before AGC -- free, no extra
// RAM or CPU) sits more than SPEECH_END_VOLUME_DROP_DB below the loudest
// level reached so far in this command also counts as "silence" toward
// SPEECH_END_SILENCE_MS, even while VAD still says VAD_SPEECH.
// Calibrated on real hardware (2026-09-24): the user's own speech measured
// -23..-31 dB (smoothed) close to the device, everything after they
// stopped -37..-58 dB while VAD still reported VAD_SPEECH -- 12dB keeps the
// user's natural level swings (~8dB) well clear of the threshold.
#define SPEECH_END_VOLUME_DROP_DB 12.0f
// Smoothing for data_volume (one value per ~32ms fetch) so a single quiet
// frame between syllables doesn't read as a drop.
#define SPEECH_END_VOLUME_SMOOTHING 0.3f

static void detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    // Set once a wake word fires, cleared once WAKE_WORD_SPEECH_END is
    // posted for it -- gates end-of-speech tracking to "listening" periods.
    bool awaiting_speech_end = false;
    // Timestamp of the most recent WAKENET_DETECTED -- see
    // WAKE_WORD_GRACE_MS's comment. Only meaningful while
    // awaiting_speech_end is true.
    int64_t wake_detected_us = 0;
    // Set the first time vad_state == VAD_SPEECH is observed while
    // awaiting_speech_end is true; only after this do we treat VAD_SILENCE
    // as (possibly) the end of speech (see file header comment item 5).
    bool seen_speech = false;
    // Tracks an in-progress run of continuous VAD_SILENCE, so a single
    // silent frame can't fire speech-end on its own -- only
    // SPEECH_END_SILENCE_MS of *unbroken* silence can. Any VAD_SPEECH
    // frame resets this (see the vad_state == VAD_SPEECH branch below).
    bool in_silence = false;
    int64_t silence_start_us = 0;
    // See SPEECH_END_VOLUME_DROP_DB: smoothed per-fetch volume, and the
    // loudest smoothed volume seen during this command's VAD_SPEECH frames
    // (taken as the user's own speaking level).
    float smoothed_volume_db = -100.0f;
    float speech_level_db = -100.0f;

    while (true) {
        afe_fetch_result_t *res = s_afe_handle->fetch(afe_data);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch error");
            // Same reasoning as feed_task's read-error path above: fetch()
            // normally blocks, so this is the lesser risk of the two, but
            // a persistent AFE fault returning immediately every call
            // would still spin core 1 at 100% with no yield.
            vTaskDelay(1);
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
            in_silence = false;
            wake_detected_us = esp_timer_get_time();
            speech_level_db = -100.0f;
        }

        smoothed_volume_db += SPEECH_END_VOLUME_SMOOTHING * (res->data_volume - smoothed_volume_db);

        if (awaiting_speech_end && (esp_timer_get_time() - wake_detected_us) < (int64_t)WAKE_WORD_GRACE_MS * 1000) {
            // Still inside the post-wake-word grace window -- ignore
            // vad_state entirely (see WAKE_WORD_GRACE_MS's comment): the
            // wake phrase's own trailing audio must not arm seen_speech,
            // and a pause here must not start a silence run either.
        } else if (awaiting_speech_end) {
            // Far quieter than the user's own level: background voices
            // (see SPEECH_END_VOLUME_DROP_DB), treated like VAD_SILENCE.
            bool far_below_user = seen_speech && smoothed_volume_db < speech_level_db - SPEECH_END_VOLUME_DROP_DB;
            if (res->vad_state == VAD_SPEECH && !far_below_user) {
                seen_speech = true;
                in_silence = false;
                if (smoothed_volume_db > speech_level_db) {
                    speech_level_db = smoothed_volume_db;
                }
            } else if (seen_speech) {
                int64_t now_us = esp_timer_get_time();
                if (!in_silence) {
                    in_silence = true;
                    silence_start_us = now_us;
                } else if (now_us - silence_start_us >= (int64_t)SPEECH_END_SILENCE_MS * 1000) {
                    ESP_LOGI(TAG, "speech end detected");
                    wake_word_event_type_t evt = WAKE_WORD_SPEECH_END;
                    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "event_queue full, dropped WAKE_WORD_SPEECH_END");
                    }
                    awaiting_speech_end = false;
                    seen_speech = false;
                    in_silence = false;
                }
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

    char *wakenet_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "heykira");
    if (wakenet_model_name == NULL) {
        ESP_LOGE(TAG, "\"heykira\" WakeNet model not found -- check CONFIG_SR_WN_WN9_HEYKIRA_TTS3 in sdkconfig");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    char *wakenet_model_name_2 = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "hiwalle");
    if (wakenet_model_name_2 == NULL) {
        ESP_LOGE(TAG, "\"hiwalle\" WakeNet model not found -- check CONFIG_SR_WN_WN9_HIWALLE_TTS2 in sdkconfig");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "using WakeNet models: %s, %s", wakenet_model_name, wakenet_model_name_2);

    // "M" (mono, single-channel, no BSS) instead of "MM" (2-mic
    // BSS/beamforming) -- see feed_task's file header comment for why.
    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = wakenet_model_name;
    afe_config->wakenet_model_name_2 = wakenet_model_name_2;
    afe_config->vad_init = true;
    // Found on real hardware: background noise or other people talking in
    // the room kept vad_state at VAD_SPEECH indefinitely, so
    // WAKE_WORD_SPEECH_END (below, requiring sustained VAD_SILENCE) never
    // fired -- the robot listened until the caller gave up. Verified
    // against Espressif's own esp-skainet voice_activity_detection example
    // (espressif/esp-skainet, examples/voice_activity_detection/main/main.c)
    // before changing this: its comment on vad_mode is explicit -- "The
    // larger the mode, the higher the speech trigger probability" -- so
    // VAD_MODE_0 (not a larger value) is the conservative, fewer-false-
    // positives end of the scale. vad_min_noise_ms/vad_min_speech_ms are
    // set to that same example's values (documented AFE defaults, pinned
    // explicitly here so a future esp-sr default change can't silently
    // shift this behavior).
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 1000;
    afe_config->vad_min_speech_ms = 128;
    afe_config = afe_config_check(afe_config);

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (afe_data == NULL) {
        ESP_LOGE(TAG, "AFE init failed");
        return ESP_FAIL;
    }

    // detect_task's stack moves to PSRAM (WithCaps, same fix as
    // orchestrator_task's -- see main.c) -- found by whole-codebase review
    // that NEITHER task creation here checked its result, on a board
    // already found to run internal SRAM down to ~2KB free at times.
    // detect_task holds only AFE-handle-derived state, no DMA/ISR-only
    // buffers, so it has no reason to need internal-only memory. Its
    // return value is now checked and propagated: a silent allocation
    // failure here previously meant a device that boots fine and never
    // responds to the wake word at all, with no log line anywhere.
    // feed_task was initially left on internal RAM out of caution (it's
    // the hot path reading from the I2S/esp_codec_dev driver every cycle,
    // and that path wasn't verified safe against PSRAM-backed call
    // stacks) -- reverted after checking this call's result exposed a
    // REAL failure on real hardware: even after moving detect_task, this
    // file's caller, and status_led_breathe off internal RAM (freeing
    // ~14.5KB combined), feed_task's own 8KB internal-RAM allocation still
    // failed outright (ESP_ERR_NO_MEM), crash-looping the device via
    // main.c's ESP_ERROR_CHECK(wake_word_start(...)). Internal RAM is
    // tighter than that freed-up amount could fix. Moved to PSRAM too;
    // verify on real hardware afterward that wake-word detection and
    // audio capture are still solid (this is the one migration the
    // review flagged as unverified, not confirmed safe).
    BaseType_t detect_created = xTaskCreatePinnedToCoreWithCaps(detect_task, "wake_word_detect", 8192, afe_data, 5,
                                                                 NULL, 1, MALLOC_CAP_SPIRAM);
    if (detect_created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(wake_word_detect) failed: %d", (int)detect_created);
        return ESP_ERR_NO_MEM;
    }
    BaseType_t feed_created = xTaskCreatePinnedToCoreWithCaps(feed_task, "wake_word_feed", 8192, afe_data, 5, NULL, 0,
                                                               MALLOC_CAP_SPIRAM);
    if (feed_created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(wake_word_feed) failed: %d", (int)feed_created);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
