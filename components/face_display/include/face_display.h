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

// Renders a deterministic action result (matches orchestrator_face_ops_t's
// show_action contract): `name` is "dice_roll" (result a decimal digit
// string "1".."6") or "coin_flip" (result "testa" or "croce"). Plays a
// short rolling/flipping flourish, settles on the result, holds it, then
// returns -- this call BLOCKS the calling task for its whole duration
// (~8s total: 3s roll + 5s hold, see face_display.c's ACTION_* constants),
// same trade-off
// as face_display_show()'s existing blocking animation. Unlike
// face_display_show()/the gaze-offset overlay, does not touch
// s_current_pose: the next real face_display_show() call cross-fades from
// whatever expression was showing BEFORE this action, not from the
// die/coin graphic, so the transition off this screen is an abrupt cut.
// Returns ESP_ERR_INVALID_STATE if no display is attached,
// ESP_ERR_INVALID_ARG if `name` isn't recognized.
esp_err_t face_display_show_action(const char *name, const char *result);

// Draws a row of musical notes scrolling right-to-left, replacing the eyes
// entirely -- for HARO_STATE_PLAYING_MUSIC. Unlike face_display_show_action()
// above, this does NOT block: call it repeatedly (e.g. every ~100ms) from
// main.c's own polling loop with an increasing `scroll_offset` (any
// monotonically increasing/decreasing int; only its value mod ~42 matters)
// to animate the scroll -- one call is one static frame. Same "does not
// touch s_current_pose" contract as the other overlays: the next real
// face_display_show() cuts back to the last real expression, not a
// cross-fade from this. Returns ESP_ERR_INVALID_STATE if no display is
// attached.
esp_err_t face_display_set_music_notes(int scroll_offset);

// Draws `text` scrolling right-to-left along a single horizontal line,
// replacing the eyes entirely -- a generic primitive used for both the
// server-unreachable diagnostic screen (main.c) and the boot-time
// wake-word reminder (main.c's app_main(), before orchestrator_task
// starts). Same non-blocking, call-repeatedly-with-an-increasing-
// scroll_offset contract as face_display_set_music_notes() above (one
// call is one static frame), and the same "does not touch s_current_pose"
// overlay contract as the rest of this file's one-off screens: the next
// real face_display_show() cuts back to the last real expression, not a
// cross-fade from this. Uses font5x7 (5x7 px per glyph, 1px gap) -- only
// the characters font5x7_glyph() actually supports render meaningfully;
// anything else renders blank. Returns ESP_ERR_INVALID_STATE if no
// display is attached.
esp_err_t face_display_set_scrolling_text(const char *text, int scroll_offset);

// Boot-time WiFi status, called from wifi_provisioning.c. Same "replaces
// the eyes entirely, one-off, doesn't touch s_current_pose" contract as
// face_display_show_action() -- not a mood, so not part of
// face_display_show()'s cross-fading expression system. Draws a
// conventional wifi-signal glyph (arcs over a dot) and returns
// immediately -- call this right before starting a scan/connect attempt;
// it stays on screen for as long as that attempt itself takes, no separate
// hold needed. Returns ESP_ERR_INVALID_STATE if no display is attached.
esp_err_t face_display_show_wifi_searching(void);

// Draws a thumbs-up and holds it ~1.5s before returning (see
// face_display.c's WIFI_CONNECTED_HOLD_MS) -- call this once a
// scan-matched or newly-provisioned network actually connects. Same
// ESP_ERR_INVALID_STATE condition as face_display_show_wifi_searching().
// There's no third "entered AP/provisioning mode" function here: call
// face_display_show(EXPR_SETUP) for that instead -- it's a genuinely
// indefinite wait (until someone provisions a new network via the app),
// which fits the normal interruptible mood system better than a one-shot
// graphic.
esp_err_t face_display_show_wifi_connected(void);

// Draws a speaker icon plus a 10-segment volume bar (`level` of the 10
// filled, the rest hollow), replacing the eyes entirely -- for main.c's
// KEY1/KEY3 volume buttons. Same "replaces the eyes entirely, one-off
// overlay, doesn't touch s_current_pose" contract as the functions above,
// but unlike them, does NOT block or hold: draws once and returns
// immediately, since there's nothing to animate -- the caller (main.c)
// owns the "show for a few seconds then return to normal" timing itself,
// the same way it already owns face_display_set_scrolling_text()'s/
// face_display_set_music_notes()'s repeated-call timing. `level` is
// clamped to [1, 10]. Returns ESP_ERR_INVALID_STATE if no display is
// attached.
esp_err_t face_display_set_volume_icon(int level);

// KEY2 info screens (main.c): same draw-once, return-immediately overlay
// contract as face_display_set_volume_icon() -- main.c decides when to
// redraw and when to go back to the eyes.
//
// Network page: connected SSID, RSSI in dBm, `level` (0-10, clamped) as
// text plus a 10-segment bar, and last ping round-trip. `ssid` NULL/empty
// means "not connected" (rssi/level ignored, shown as "--"); `ping_ms` < 0
// means no reply yet / timed out ("ping: --").
esp_err_t face_display_set_network_info(const char *ssid, int rssi_dbm, int level, int ping_ms);

// Up to ~5 lines of font5x7 text, each centered horizontally, the block
// centered vertically. Lines wider than the display (21 glyphs) clip.
esp_err_t face_display_set_text_lines(const char *const *lines, int count);

#ifdef __cplusplus
}
#endif
