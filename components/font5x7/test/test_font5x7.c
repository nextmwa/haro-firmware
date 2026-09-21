// Decodes every glyph back into the same ASCII-art form as the comment
// above its entry in font5x7.c and prints it -- run on the "linux" host
// target and read the stdout by eye against those comments. This is the
// only real check available: there's no simulator for the actual SSD1306
// hardware, so a wrong bit pattern would otherwise only be discoverable by
// flashing real hardware and looking at a garbled display.
#include "font5x7.h"
#include "unity.h"
#include <stdio.h>

static void print_glyph(char c)
{
    const uint8_t *cols = font5x7_glyph(c);
    printf("glyph '%c':\n", c);
    for (int row = 0; row < FONT5X7_HEIGHT; row++) {
        for (int col = 0; col < FONT5X7_WIDTH; col++) {
            putchar((cols[col] & (1u << row)) ? '#' : '.');
        }
        putchar('\n');
    }
}

TEST_CASE("every character used by the error screen prints a legible glyph", "[font5x7]")
{
    const char *chars = " 0123456789abegilnorsuv.:";
    for (const char *p = chars; *p != '\0'; p++) {
        print_glyph(*p);
    }
}

TEST_CASE("every character used by the boot-time wake-word reminder prints a legible glyph", "[font5x7]")
{
    // h,y,k,d,w added specifically for "hey kira"/"wall" -- not needed by
    // the error screen's "server non raggiungibile <ip:port>" text above.
    const char *chars = "hykdw";
    for (const char *p = chars; *p != '\0'; p++) {
        print_glyph(*p);
    }
}

TEST_CASE("an unsupported character falls back to a blank glyph, not NULL", "[font5x7]")
{
    const uint8_t *cols = font5x7_glyph('Z'); // not in the supported set
    TEST_ASSERT_NOT_NULL(cols);
    for (int i = 0; i < FONT5X7_WIDTH; i++) {
        TEST_ASSERT_EQUAL_UINT8(0x00, cols[i]);
    }
}

TEST_CASE("space is blank", "[font5x7]")
{
    const uint8_t *cols = font5x7_glyph(' ');
    for (int i = 0; i < FONT5X7_WIDTH; i++) {
        TEST_ASSERT_EQUAL_UINT8(0x00, cols[i]);
    }
}
