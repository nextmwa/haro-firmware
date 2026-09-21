#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <math.h>

// GPIO38 = silkscreen "RGB_LED", 7x WS2812B-0807 chained on a single data
// line (verified against the board's official pinout diagram, same source
// used for face_display's I2C pins and wake_word's camera GPIOs elsewhere
// in this project).
#define STATUS_LED_GPIO 38
#define STATUS_LED_COUNT 7
// 10MHz RMT tick resolution: matches led_strip's own reference example
// (examples/led_strip_rmt_ws2812/main/led_strip_rmt_ws2812_main.c) --
// WS2812 needs a high-resolution RMT clock to hit its ~1.25us bit timing.
#define STATUS_LED_RMT_RES_HZ (10 * 1000 * 1000)

static const char *TAG = "status_led";

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_mutex;
static volatile bool s_ambient_enabled;
static uint8_t s_ambient_r, s_ambient_g, s_ambient_b;

// Idle ambient: a calm, cool blue -- distinct from every mood color below,
// so it's unambiguous at a glance that Haro is idle vs. actively reacting.
#define AMBIENT_R 30
#define AMBIENT_G 90
#define AMBIENT_B 200

// Breathing envelope spans the full [0, 1] brightness range -- fades all
// the way out, not just dim -- and slower than a first pass at this (was a
// 4s cycle at floor 0.12): found on real hardware that not reaching true
// off read as "stuck dim" rather than "breathing", and 4s felt hurried for
// an ambient idle cue.
#define BREATH_FLOOR 0.0
#define BREATH_SPAN  1.0
#define BREATH_STEP_DELAY_MS 30
#define BREATH_CYCLE_MS 8000.0
// 2*pi phase advance per step, sized so BREATH_STEP_DELAY_MS * steps-per-
// cycle ~= BREATH_CYCLE_MS.
#define BREATH_PHASE_STEP (2.0 * M_PI / (BREATH_CYCLE_MS / BREATH_STEP_DELAY_MS))
// Raising the raw (symmetric) sine envelope to a power > 1 leaves both ends
// (0 and 1) fixed but pulls every value in between down toward 0 -- the
// curve spends more of the cycle sitting near "off" and less near "max
// brightness", without breaking the sine's continuity/smoothness (no
// segment boundaries, still a single continuous function of phase). Found
// on real hardware: asked for specifically -- "the off part should be a
// little longer than the max-brightness part".
#define BREATH_BIAS_EXPONENT 2.2

static void set_solid_locked(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < STATUS_LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}

static void breathing_task(void *arg)
{
    double phase = 0.0;
    while (true) {
        if (s_ambient_enabled) {
            double raw = (sin(phase) + 1.0) / 2.0; // 0..1, symmetric
            double envelope = pow(raw, BREATH_BIAS_EXPONENT); // biased toward 0 -- see BREATH_BIAS_EXPONENT
            double brightness = BREATH_FLOOR + envelope * BREATH_SPAN;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            // Re-check after taking the mutex: status_led_set_expression()
            // may have disabled ambient and written a solid color while
            // this task was waiting for the lock, and this step's stale
            // envelope value must not clobber that.
            if (s_ambient_enabled) {
                set_solid_locked((uint8_t)(s_ambient_r * brightness),
                                  (uint8_t)(s_ambient_g * brightness),
                                  (uint8_t)(s_ambient_b * brightness));
            }
            xSemaphoreGive(s_mutex);
            phase += BREATH_PHASE_STEP;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BREATH_STEP_DELAY_MS));
    }
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        // Found on real hardware: with the "standard" GRB wire order below,
        // requesting pure red (255,0,0) rendered as green on this board's
        // specific LEDs -- i.e. this chain actually expects RGB order, not
        // every WS2812(-labelled) chip agrees on wire order, and clones in
        // particular vary. RGB confirmed correct against the same
        // real-hardware red-vs-green test.
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STATUS_LED_RMT_RES_HZ,
        .mem_block_symbols = 0, // let the driver pick a sane default for 7 LEDs
        .flags = { .with_dma = false }, // DMA only pays off for long strips; 7 LEDs doesn't need it
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = led_strip_clear(s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip_clear failed: %s", esp_err_to_name(err));
    }

    // WithCaps (PSRAM stack), checked -- same fix, same reasoning, as
    // orchestrator_task's in main.c: found by whole-codebase review that
    // this xTaskCreate() call was never checked, on a board already found
    // to run internal SRAM down to ~2KB free at times. breathing_task's
    // own state is a handful of floats/doubles (sin/pow) plus the mutex
    // above -- nothing here needs internal-only memory.
    BaseType_t task_created = xTaskCreateWithCaps(breathing_task, "status_led_breathe", 2560, NULL, 3, NULL,
                                                   MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps(status_led_breathe) failed: %d", (int)task_created);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "status_led initialized (%d LEDs on GPIO%d)", STATUS_LED_COUNT, STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_idle_ambient(bool enabled)
{
    s_ambient_r = AMBIENT_R;
    s_ambient_g = AMBIENT_G;
    s_ambient_b = AMBIENT_B;
    s_ambient_enabled = enabled;
}

void status_led_set_expression(face_expression_t expression)
{
    if (s_strip == NULL) {
        return;
    }
    // Every value here is deliberate, not a placeholder: colors chosen to
    // be visually distinct from each other and from the idle ambient blue.
    // EXPR_IDLE/EXPR_BORED/EXPR_LOOKING_LEFT/EXPR_LOOKING_RIGHT are main.c's
    // idle-fidget-only expressions (see status_led.h) and are never passed
    // here in normal operation, but still get a reasonable fallback (dim
    // white) rather than undefined behavior if that ever changes.
    uint8_t r, g, b;
    switch (expression) {
    case EXPR_LISTENING:         r = 0;   g = 200; b = 255; break; // cyan: alert
    case EXPR_THINKING:          r = 140; g = 0;   b = 220; break; // purple: processing
    case EXPR_SPEAKING_HAPPY:    r = 120; g = 255; b = 40;  break; // warm yellow-green
    case EXPR_SPEAKING_SAD:      r = 20;  g = 40;  b = 200; break; // deep blue
    case EXPR_SPEAKING_CONFUSED: r = 255; g = 130; b = 0;   break; // orange
    case EXPR_SPEAKING_NEUTRAL:  r = 200; g = 200; b = 200; break; // white
    case EXPR_ERROR:              r = 255; g = 0;   b = 0;   break; // red
    case EXPR_SETUP:              r = 0;   g = 100; b = 255; break; // blue
    case EXPR_ANGRY:              r = 255; g = 20;  b = 0;   break; // red-orange
    case EXPR_DISGUSTED:          r = 100; g = 200; b = 40;  break; // sickly green
    case EXPR_SURPRISED:          r = 255; g = 220; b = 0;   break; // bright yellow
    case EXPR_FEARFUL:            r = 180; g = 120; b = 255; break; // pale purple
    case EXPR_IDLE:
    case EXPR_BORED:
    case EXPR_LOOKING_LEFT:
    case EXPR_LOOKING_RIGHT:
    default:                       r = 40;  g = 40;  b = 40;  break; // dim white fallback
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ambient_enabled = false;
    set_solid_locked(r, g, b);
    xSemaphoreGive(s_mutex);
}
