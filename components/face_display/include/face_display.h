#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EXPR_IDLE, EXPR_LISTENING, EXPR_THINKING, EXPR_SPEAKING_HAPPY, EXPR_SPEAKING_SAD,
    EXPR_SPEAKING_CONFUSED, EXPR_SPEAKING_NEUTRAL, EXPR_ERROR, EXPR_SETUP,
} face_expression_t;

// Initializes the external I2C bus (GPIO8=SDA, GPIO9=SCL, separate from the
// internal codec/TCA9555 bus) and the SSD1306 panel driver.
esp_err_t face_display_init(void);

// Pure drawing function: renders `expression` into `framebuffer`, a
// caller-owned buffer of WIDTH*HEIGHT/8 bytes (128*64/8 = 1024) laid out in
// SSD1306 GDDRAM page/column order -- see face_display.c for the exact
// packing. Does no I2C/esp_lcd calls and touches no static state, so it is
// usable and testable outside of an ESP-IDF build.
void face_display_render(face_expression_t expression, uint8_t *framebuffer);

// Thin wrapper: renders `expression` into the component's internal
// framebuffer and pushes it to the panel via esp_lcd_panel_draw_bitmap.
esp_err_t face_display_show(face_expression_t expression);

#ifdef __cplusplus
}
#endif
