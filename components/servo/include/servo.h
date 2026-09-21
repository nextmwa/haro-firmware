#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Two SG90 micro servos, PWM-driven via the LEDC peripheral: pan (left/
// right, GPIO4) and tilt (up/down, GPIO5). Both physically move whatever
// the servo horns are mounted to (the OLED "head"), intended to be driven
// by the same offset already moving the eyes on screen (see main.c's
// eye-liveliness block) rather than independently.
//
// GPIO4/GPIO5: confirmed free on this board's external header (photographed
// pinout, 2026-09-14) -- GPIO10/GPIO11 are audio_pipeline's I2C bus,
// GPIO19/GPIO20 are the ESP32-S3's fixed native-USB D-/D+ pins (this is
// how the board is flashed/monitored over /dev/cu.usbmodem*), neither
// reusable. Wiring for each servo: signal -> its GPIO, VCC -> the header's
// 5V pin, GND -> the header's GND. Two servos sharing that one 5V pin may
// exceed what the board's onboard regulator can supply under simultaneous
// movement (not verified for this board) -- if resets/brownouts show up
// once both are wired, power the servos from a separate 5V supply instead.
//
// The tilt servo does not need to be physically present for this to work:
// servo_set_tilt_angle() just drives a PWM signal on GPIO5 that goes
// nowhere until it's wired up.
esp_err_t servo_init(void);

// Moves the pan (left/right) servo to `angle_deg`, clamped to [0, 180].
// 90 is assumed as mechanical center -- adjust SERVO_CENTER_DEG in servo.c
// if the physical horn mounting puts center somewhere else. Non-blocking:
// just updates the PWM duty cycle, the servo moves on its own afterward.
esp_err_t servo_set_pan_angle(int angle_deg);

// Same contract as servo_set_pan_angle(), for the tilt (up/down) servo.
esp_err_t servo_set_tilt_angle(int angle_deg);

#ifdef __cplusplus
}
#endif
