#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal 5x7 bitmap font: lowercase a-z (uppercase A-Z folds onto the
// same glyphs), digits, space and a handful of punctuation (. : - _ /) --
// what face_display's text screens (server-unreachable error, wake-word
// reminder, KEY2 network-info/wake-word screens) need, not a
// general-purpose ASCII font. Deliberately its own component, separate from face_display: it has
// no ESP-IDF hardware dependency (no esp_lcd/i2c includes), so it builds
// and is testable on the "linux" host target even though the rest of
// face_display isn't (see this component's CMakeLists.txt and test/).
#define FONT5X7_WIDTH 5
#define FONT5X7_HEIGHT 7

// Returns 5 column bytes for `c` (space and any unsupported character
// return a blank glyph -- never NULL). Column-major, bit 0 = top row:
// column byte `cols[i]`'s bit b (0..6) is set iff pixel (i, b) of the
// glyph is lit. Caller owns nothing; the returned pointer is into static
// font data valid for the process lifetime.
const uint8_t *font5x7_glyph(char c);

#ifdef __cplusplus
}
#endif
