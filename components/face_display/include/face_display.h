#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// New values are always appended, never inserted: orchestrator.c keeps its
// own local mirror of this enum (deliberately decoupled from this header,
// see orchestrator.c's file comment) and passes bare int ordinals across
// its face.show callback. orchestrator.c's mirror now assigns each entry an
// explicit `= N` matching this header exactly, so a future addition here
// only needs a matching explicit-valued entry there -- not perfect ordinal
// bookkeeping -- but appending here still costs nothing and avoids the
// question entirely. EXPR_BORED/EXPR_LOOKING_LEFT/EXPR_LOOKING_RIGHT have no
// orchestrator-side entry at all (main.c's idle animation calls
// face_display_show() with them directly, never through orchestrator).
typedef enum {
    EXPR_IDLE, EXPR_LISTENING, EXPR_THINKING, EXPR_SPEAKING_HAPPY, EXPR_SPEAKING_SAD,
    EXPR_SPEAKING_CONFUSED, EXPR_SPEAKING_NEUTRAL, EXPR_ERROR, EXPR_SETUP,
    EXPR_BORED, EXPR_LOOKING_LEFT, EXPR_LOOKING_RIGHT,
    EXPR_ANGRY, EXPR_DISGUSTED, EXPR_SURPRISED, EXPR_FEARFUL,
} face_expression_t;

// Initializes the SSD1306 panel driver on `i2c_bus`. Found on real hardware:
// this board's external 18-pin header does NOT expose a separate I2C bus --
// its silkscreen-labeled "SDA"/"SCL" pins are physically GPIO11/GPIO10, the
// SAME pins audio_pipeline.c already drives for the onboard ES8311/ES7210/
// TCA9555 codecs (a design-spec assumption of a dedicated GPIO8/GPIO9 bus
// turned out to be an unverified placeholder, not the real pinout). Pass the
// bus handle from audio_pipeline_get_i2c_bus() -- call this only after
// audio_pipeline_init() has succeeded.
esp_err_t face_display_init(i2c_master_bus_handle_t i2c_bus);

// Animates from whatever is currently on screen to `expression` (~200ms,
// interpolating eye geometry frame-by-frame -- see face_display.c's pose
// model) and pushes each frame via esp_lcd_panel_draw_bitmap. The very
// first call after face_display_init() snaps directly to `expression`
// instead, since there's no previous pose to animate from.
esp_err_t face_display_show(face_expression_t expression);

// Nudges both eyes of whatever expression is CURRENTLY on screen by
// (dx_px, dy_px) and redraws immediately (no animation -- this is meant
// for continuous, responsive gaze tracking, called repeatedly as a
// tracked face moves, not a mood change). Purely cosmetic: does not
// change what face_display_show()'s next call animates FROM, so a real
// expression change after tracking still starts from the untracked base
// pose. Call with (0, 0) when tracking is lost to recenter the eyes.
// Returns ESP_ERR_INVALID_STATE if no display is attached or
// face_display_show() hasn't been called yet (nothing to nudge).
esp_err_t face_display_set_gaze_offset(int dx_px, int dy_px);

// Scales both eyes' current height by `openness` (1.0 = fully open/normal,
// 0.0 = fully closed) and redraws immediately -- same "transient overlay on
// top of whatever's showing" contract as face_display_set_gaze_offset()
// above (does not touch what face_display_show()'s next call animates
// from). Call in a short sequence (e.g. 1.0 -> 0.1 -> 1.0 over ~150ms) to
// animate a blink. Returns ESP_ERR_INVALID_STATE under the same conditions
// as face_display_set_gaze_offset().
esp_err_t face_display_set_blink(float openness);

#ifdef __cplusplus
}
#endif
