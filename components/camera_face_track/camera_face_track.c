// camera_face_track: OV2640 capture + JPEG upload to the server. Face
// DETECTION itself runs server-side (haro-server's face_tracking.py) --
// see camera_face_track.h's file header comment for why this device
// doesn't run it locally anymore. Plain C now (was C++ only for esp-dl's
// C++ API, now removed) -- atomics via C11 <stdatomic.h>, the same pattern
// wake_word.c already uses for its own cross-task state.
#include "camera_face_track.h"
#include "audio_pipeline.h"
#include "server_client.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>

static const char *TAG = "camera_face_track";

// Camera data/clock pin mapping, verified against the board's official
// pinout diagram (docs.waveshare.com/ESP32-S3-AUDIO-Board "Camera" box) --
// the same source already used for face_display's I2C pins and
// status_led's GPIO38 in this project.
#define CAM_PIN_D0    2
#define CAM_PIN_D1    17
#define CAM_PIN_D2    18
#define CAM_PIN_D3    39
#define CAM_PIN_D4    45
#define CAM_PIN_D5    46
#define CAM_PIN_D6    47
#define CAM_PIN_D7    48
#define CAM_PIN_PCLK  44
#define CAM_PIN_VSYNC 21
#define CAM_PIN_HREF  1
#define CAM_PIN_XCLK  43
// SIO_CLK/SIO_DAT (the camera's SCCB/I2C control pins) are, per the same
// diagram, GPIO10/GPIO11 -- the SAME pins audio_pipeline.c already owns as
// its I2C bus for the onboard codecs (and face_display's SSD1306, and the
// TCA9555 below). camera_face_track_init() passes pin_sccb_sda=-1 to
// esp32-camera so it reuses that existing bus (esp32-camera's
// driver/sccb-ng.c, built by default on ESP-IDF >=5.4: pin_sccb_sda==-1
// makes it call i2c_master_get_bus_handle(sccb_i2c_port, ...) instead of
// creating a second, conflicting bus on the same pins) rather than
// initializing its own.

// PWDN/RESET are NOT direct MCU GPIOs on this board -- both sit behind the
// TCA9555 I2C expander already used for the speaker amplifier's PA_EN (see
// audio_pipeline.c's tca9555_enable_pa() for the address/register-map
// background reused here): PWDN = EXIO5 (port 0, bit 5), the pinout
// diagram's "SET" = EXIO6 (port 0, bit 6), read as camera RESET. Polarity
// confirmed correct on real hardware (PWDN active-high powers the sensor
// DOWN, so drive it LOW to power on; RESET released by driving it HIGH).
#define TCA9555_I2C_ADDR 0x20
#define TCA9555_REG_CONFIG_PORT0 0x06
#define TCA9555_REG_OUTPUT_PORT0 0x02
#define TCA9555_CAM_PWDN_BIT (1 << 5)  // EXIO5
#define TCA9555_CAM_RESET_BIT (1 << 6) // EXIO6

// How often to pull a frame and send it to the server. No local inference
// to pace anymore (see file header comment), so this can run at a
// straightforward, consistent cadence -- was 500ms, doubled to 1000ms
// after a real corporate network found where server_client_send_camera_
// frame() can legitimately take up to CAMERA_FRAME_SEND_TIMEOUT_MS
// (server_client.c) to complete or fail: every attempt holds the
// WebSocket client's shared tx_lock for that whole span, competing with
// the PING/PONG traffic that keeps the connection alive at all (see that
// constant's comment for the exact mechanism -- a real disconnect this
// caused, confirmed on real hardware). Attempting less often directly
// reduces how much of any given window this send can occupy that lock,
// on top of the bounded-not-unbounded fix already in server_client.c;
// 1000ms is still well under FACE_TRACK_STALE_MS below, so a lost frame
// doesn't read as "no face" on its own.
#define FACE_TRACK_POLL_MS 1000
// A face position older than this is treated as "no face" by
// camera_face_track_get_offset(), rather than main.c latching onto a stale
// position forever once the person steps out of frame or the connection
// to the server drops.
#define FACE_TRACK_STALE_MS 2000

// Auto-disable on connection instability. Found on real hardware
// (a corporate network): no single CAMERA_FRAME_SEND_TIMEOUT_MS value in
// server_client.c can fully solve this on every network Haro might be
// on -- too short and a slow-but-working send gets treated as fatal
// (abort_connection()); too long/unbounded and it starves the PING/PONG
// traffic keeping the connection alive at all, so the SERVER eventually
// drops it instead (see that constant's comment for both, confirmed on
// real hardware measuring the exact failure point move with the timeout
// value). Rather than continuing to hunt for one magic number, this
// tracks consecutive server_client_send_camera_frame() failures and stops
// attempting them once they're clearly not landing -- retrying
// periodically so a network that's currently bad (or a different one
// Haro reconnects to later) gets re-tried and re-enabled automatically if
// it turns out fine, with no server_url-style manual step needed. Not
// distinguishing "timed out" from "not currently connected" failures is
// deliberate: a real timeout on this send is exactly what forces the
// reconnect that then makes several immediately-following attempts fail
// with the latter, so a handful of consecutive failures reliably means
// "something is wrong with sending frames on this connection" either way.
#define CAMERA_SEND_FAILURE_THRESHOLD 3
#define CAMERA_SEND_RETRY_INTERVAL_MS 60000

static bool s_camera_sending_enabled = true;
static int s_consecutive_send_failures = 0;
static int64_t s_camera_sending_disabled_at_us = 0;

static _Atomic float s_face_dx;
static _Atomic float s_face_dy;
static _Atomic int64_t s_last_seen_us;

static esp_err_t tca9555_camera_power_on(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = audio_pipeline_get_i2c_bus(&bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TCA9555_I2C_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    esp_err_t probe_err = i2c_master_probe(bus, TCA9555_I2C_ADDR, 100);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9555 not found at 0x%02X (%s) -- camera PWDN/RESET left unmanaged",
                 TCA9555_I2C_ADDR, esp_err_to_name(probe_err));
        return probe_err;
    }

    i2c_master_dev_handle_t dev;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }

    // Read-modify-write port 0's config (direction) and output registers so
    // only PWDN/RESET's two bits change -- other port-0 bits may already be
    // configured for something else (e.g. camera-unrelated expander pins).
    uint8_t config_reg = TCA9555_REG_CONFIG_PORT0;
    uint8_t config_val = 0;
    err = i2c_master_transmit_receive(dev, &config_reg, 1, &config_val, 1, 100);
    if (err == ESP_OK) {
        config_val &= (uint8_t)~(TCA9555_CAM_PWDN_BIT | TCA9555_CAM_RESET_BIT); // both as outputs
        uint8_t config_write[2] = { TCA9555_REG_CONFIG_PORT0, config_val };
        err = i2c_master_transmit(dev, config_write, sizeof(config_write), 100);
    }

    if (err == ESP_OK) {
        uint8_t output_reg = TCA9555_REG_OUTPUT_PORT0;
        uint8_t output_val = 0;
        err = i2c_master_transmit_receive(dev, &output_reg, 1, &output_val, 1, 100);
        if (err == ESP_OK) {
            output_val &= (uint8_t)~TCA9555_CAM_PWDN_BIT;  // PWDN low: powered on
            output_val |= TCA9555_CAM_RESET_BIT;           // RESET high: released
            uint8_t output_write[2] = { TCA9555_REG_OUTPUT_PORT0, output_val };
            err = i2c_master_transmit(dev, output_write, sizeof(output_write), 100);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9555 camera PWDN/RESET write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Camera powered on (TCA9555 EXIO5/EXIO6)");
        vTaskDelay(pdMS_TO_TICKS(10)); // let the sensor's own reset/startup settle
    }

    i2c_master_bus_rm_device(dev);
    return err;
}

static void face_track_task(void *arg)
{
    while (true) {
        // Auto-disable/retry -- see CAMERA_SEND_FAILURE_THRESHOLD's
        // comment above. Skips straight past the whole capture+encode
        // step, not just the send, while disabled and not yet due for a
        // retry: no reason to spend the camera/JPEG-encode work on a
        // frame this loop already knows it won't attempt to send.
        bool attempt_send = s_camera_sending_enabled;
        if (!attempt_send) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_camera_sending_disabled_at_us >= (int64_t)CAMERA_SEND_RETRY_INTERVAL_MS * 1000) {
                attempt_send = true; // one retry attempt this cycle
            }
        }

        if (attempt_send) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb != NULL) {
                uint8_t *jpg_buf = NULL;
                size_t jpg_len = 0;
                if (fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 30, &jpg_buf, &jpg_len)) {
                    esp_err_t send_err = server_client_send_camera_frame(jpg_buf, jpg_len);
                    if (send_err == ESP_OK) {
                        s_consecutive_send_failures = 0;
                        if (!s_camera_sending_enabled) {
                            ESP_LOGI(TAG, "camera-frame sending recovered, re-enabling");
                            s_camera_sending_enabled = true;
                        }
                    } else {
                        s_consecutive_send_failures++;
                        if (s_camera_sending_enabled && s_consecutive_send_failures >= CAMERA_SEND_FAILURE_THRESHOLD) {
                            ESP_LOGW(TAG,
                                     "camera-frame sending disabled after %d consecutive failures -- "
                                     "retrying in %dms",
                                     s_consecutive_send_failures, CAMERA_SEND_RETRY_INTERVAL_MS);
                            s_camera_sending_enabled = false;
                            s_camera_sending_disabled_at_us = esp_timer_get_time();
                        } else if (!s_camera_sending_enabled) {
                            // A periodic retry attempt failed again --
                            // push the next retry a full interval out
                            // rather than trying again next cycle.
                            s_camera_sending_disabled_at_us = esp_timer_get_time();
                        }
                    }
                    free(jpg_buf); // fmt2jpg() allocates with standard malloc, per esp32-camera's img_converters.h
                } else {
                    ESP_LOGW(TAG, "fmt2jpg conversion failed, skipping this frame");
                }
                esp_camera_fb_return(fb);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(FACE_TRACK_POLL_MS));
    }
}

esp_err_t camera_face_track_init(void)
{
    esp_err_t err = tca9555_camera_power_on();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera power-on sequence incomplete -- esp_camera_init() below will likely fail too");
    }

    camera_config_t config = {};
    config.pin_pwdn = -1;  // handled via TCA9555 above, not a direct GPIO
    config.pin_reset = -1; // ditto
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_sccb_sda = -1; // reuse audio_pipeline's existing I2C_NUM_0 bus (see file header comment)
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = 0;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;
    // Found on real hardware: 20MHz (OV2640's typical documented XCLK)
    // produced persistent, periodic "cam_hal: EV-VSYNC-OVF" / "Failed to
    // get frame: timeout" from the moment the camera started. 10MHz is the
    // standard fallback for exactly this symptom on OV2640 modules, and
    // was confirmed stable on real hardware.
    config.xclk_freq_hz = 10000000;
    // TIMER_0/CHANNEL_0/CHANNEL_1 are servo.c's pan/tilt PWM -- avoid both.
    config.ledc_timer = LEDC_TIMER_2;
    config.ledc_channel = LEDC_CHANNEL_2;
    // RGB565 (not RGB888/JPEG at capture time): this chip's LCD-CAM capture
    // peripheral only supports GRAYSCALE/YUV422/RGB565/JPEG input sampling
    // (target/esp32s3/ll_cam.c's ll_cam_set_sample_mode() rejects anything
    // else) -- confirmed on real hardware (esp_camera_init() fails outright
    // with PIXFORMAT_RGB888). fmt2jpg() below converts RGB565 to JPEG for
    // the upload; no RGB888 conversion is needed anymore now that no local
    // detection model consumes it.
    config.pixel_format = PIXFORMAT_RGB565;
    // QVGA (320x240): reasonable preview/detection resolution without
    // spending extra cycles capturing/encoding a much larger frame than
    // face_tracking.py's Haar cascade needs.
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12; // unused at this pixel_format, kept at the driver's documented default
    // Found on real hardware: fb_count=1 + CAMERA_GRAB_WHEN_EMPTY produced
    // repeated "cam_hal: EV-VSYNC-OVF" / "Failed to get frame: timeout" --
    // the sensor free-runs at its own frame rate (tens of ms/frame) but
    // face_track_task only polls every FACE_TRACK_POLL_MS, so with a
    // single buffer the capture ISR has nowhere to write while we're still
    // holding the one buffer between esp_camera_fb_get()/
    // esp_camera_fb_return(). Two buffers plus CAMERA_GRAB_LATEST (its own
    // doc comment: "queue will always contain the last fb_count frames")
    // is the standard esp32-camera fix for exactly this "slow consumer,
    // fast producer" pattern -- always returns the freshest frame instead
    // of blocking/overflowing waiting for us.
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM; // RGB565 QVGA is 320*240*2 = 150KB/frame -- PSRAM only, not internal DRAM
    config.grab_mode = CAMERA_GRAB_LATEST;

    err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Core 0: wake_word_detect_task (wake_word.c) runs on core 1 at
    // priority 5 and is continuously busy running AFE/WakeNet inference,
    // which starved a face-tracking task pinned to the same core for
    // seconds at a time when this component still did on-device detection.
    // No longer strictly necessary now that this task is just capture +
    // JPEG encode + send (much lighter than inference was), but core 0
    // still has more headroom (only wake_word_feed_task, lighter still,
    // shares it), so left as-is.
    // WithCaps (PSRAM stack), checked -- same fix, same reasoning, as
    // orchestrator_task's in main.c: found by whole-codebase review that
    // this xTaskCreate() call was never checked, on a board already found
    // to run internal SRAM down to ~2KB free at times. face_track_task's
    // own state is task-local (gaze offset, timing) -- its JPEG buffers
    // already come from esp_camera's own PSRAM frame buffers (see this
    // file's camera_config_t), not this stack.
    BaseType_t task_created = xTaskCreatePinnedToCoreWithCaps(face_track_task, "face_track", 4096, NULL, 4, NULL, 0,
                                                                MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(face_track) failed: %d", (int)task_created);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "camera_face_track initialized (OV2640, QVGA, %dms poll, server-side detection)", FACE_TRACK_POLL_MS);
    return ESP_OK;
}

bool camera_face_track_get_offset(float *dx, float *dy)
{
    int64_t last_seen = atomic_load(&s_last_seen_us);
    if (last_seen == 0 || esp_timer_get_time() - last_seen > (int64_t)FACE_TRACK_STALE_MS * 1000) {
        return false;
    }
    *dx = atomic_load(&s_face_dx);
    *dy = atomic_load(&s_face_dy);
    return true;
}

void camera_face_track_set_remote_position(bool found, float dx, float dy)
{
    if (!found) {
        return; // let camera_face_track_get_offset()'s own staleness window handle "no face" -- see its header comment
    }
    atomic_store(&s_face_dx, dx);
    atomic_store(&s_face_dy, dy);
    atomic_store(&s_last_seen_us, esp_timer_get_time());
}
