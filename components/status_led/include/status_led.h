#pragma once
#include "esp_err.h"
#include "face_display.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the 7-LED WS2812 chain on GPIO38 (silkscreen "RGB_LED",
// confirmed against the board's official pinout diagram -- a single-wire
// chain on the back of the board, distinct from the front-facing SSD1306).
// Turns all LEDs off (not ambient, not any expression color) until the
// first status_led_set_expression() or status_led_set_idle_ambient() call.
esp_err_t status_led_init(void);

// Solid color for `expression`, matching face_display's mood, and stops
// any in-progress ambient breathing. Intended to be called from the same
// place face_display_show() is called for a REAL state change (main.c's
// orchestrator face.show callback) -- NOT from main.c's idle-fidget cycle,
// so idle fidgets (EXPR_THINKING/EXPR_BORED/EXPR_LOOKING_*) don't interrupt
// the idle ambient animation status_led_set_idle_ambient() drives. See
// status_led.c's color table for the exact mapping.
void status_led_set_expression(face_expression_t expression);

// Enables/disables a slow, smooth breathing animation (own FreeRTOS task,
// ~4s full cycle) on a calm idle color. Meant to track orchestrator's real
// haro_state_t == HARO_STATE_IDLE, not face_display's current expression --
// call with `true` on entering idle, `false` on leaving it. Disabling does
// not itself change what's on the LEDs; pair it with a
// status_led_set_expression() call for the state being entered, the same
// way main.c's orchestrator_task already pairs its own idle-tracking with
// a face_display_show() call.
void status_led_set_idle_ambient(bool enabled);

#ifdef __cplusplus
}
#endif
