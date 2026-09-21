#include "servo.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "servo";

#define SERVO_PAN_GPIO 4
#define SERVO_TILT_GPIO 5
// Both channels share one timer: they need identical frequency/resolution
// anyway (both are plain 50Hz hobby-servo PWM), so a second timer would
// just be redundant configuration, not independent behavior.
#define SERVO_LEDC_TIMER LEDC_TIMER_0
#define SERVO_PAN_LEDC_CHANNEL LEDC_CHANNEL_0
#define SERVO_TILT_LEDC_CHANNEL LEDC_CHANNEL_1
#define SERVO_LEDC_MODE LEDC_LOW_SPEED_MODE // ESP32-S3 only has low-speed mode (no SOC_LEDC_SUPPORT_HS_MODE)
#define SERVO_LEDC_RESOLUTION LEDC_TIMER_14_BIT // enum value == bit count (verified in hal/ledc_types.h)
#define SERVO_PWM_FREQ_HZ 50 // standard hobby-servo PWM rate (20ms period)
#define SERVO_PERIOD_US (1000000 / SERVO_PWM_FREQ_HZ)

// Conservative, widely-documented safe pulse-width range for a 0-180deg
// SG90 sweep. Wider ranges (e.g. 500-2500us) exist in some datasheets and
// give a fuller sweep, but can buzz/strain the gears against the end
// stops on SG90 clones -- not verified against this specific unit's
// datasheet, so staying inside the conservative range.
#define SERVO_PULSE_MIN_US 1000
#define SERVO_PULSE_MAX_US 2000

#define SERVO_CENTER_DEG 90 // assumed mechanical center -- adjust to match how each horn is physically mounted

static bool s_initialized;

static esp_err_t configure_channel(int gpio_num, ledc_channel_t channel)
{
    ledc_channel_config_t channel_conf = {
        .gpio_num = gpio_num,
        .speed_mode = SERVO_LEDC_MODE,
        .channel = channel,
        .timer_sel = SERVO_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_conf);
}

static esp_err_t set_angle(ledc_channel_t channel, int angle_deg)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (angle_deg < 0) {
        angle_deg = 0;
    } else if (angle_deg > 180) {
        angle_deg = 180;
    }

    uint32_t pulse_us = SERVO_PULSE_MIN_US +
        (uint32_t)((SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * angle_deg / 180);
    uint32_t max_duty = (1u << SERVO_LEDC_RESOLUTION) - 1;
    uint32_t duty = (uint32_t)((uint64_t)pulse_us * max_duty / SERVO_PERIOD_US);

    esp_err_t err = ledc_set_duty(SERVO_LEDC_MODE, channel, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(SERVO_LEDC_MODE, channel);
}

esp_err_t servo_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_LEDC_RESOLUTION,
        .timer_num = SERVO_LEDC_TIMER,
        .freq_hz = SERVO_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_channel(SERVO_PAN_GPIO, SERVO_PAN_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pan channel config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = configure_channel(SERVO_TILT_GPIO, SERVO_TILT_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tilt channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "servo initialized (pan GPIO%d, tilt GPIO%d)", SERVO_PAN_GPIO, SERVO_TILT_GPIO);

    esp_err_t pan_err = set_angle(SERVO_PAN_LEDC_CHANNEL, SERVO_CENTER_DEG);
    esp_err_t tilt_err = set_angle(SERVO_TILT_LEDC_CHANNEL, SERVO_CENTER_DEG);
    return (pan_err != ESP_OK) ? pan_err : tilt_err;
}

esp_err_t servo_set_pan_angle(int angle_deg)
{
    return set_angle(SERVO_PAN_LEDC_CHANNEL, angle_deg);
}

esp_err_t servo_set_tilt_angle(int angle_deg)
{
    return set_angle(SERVO_TILT_LEDC_CHANNEL, angle_deg);
}
